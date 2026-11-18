#pragma once
#include "index_engine/table/updatable_neighbor_table.h"
#include "index_engine/table/sharded_reloadable.h"
#include "flatbuffers/flatbuffers.h"
#include <charconv>
#include <limits>
#include <unordered_map>
#include <unordered_set>
namespace index_engine {
struct Record {uint64_t id=0,generation=0;std::string value;};
struct RecordFormat {
 using struct_type=Record;using flatbuffers_type=flatbuffers::Table;using ValueType=Record;
 static uint64_t Id(const Record* r){return r->id;}
 static void StructFromFlatbuffers(Record* out,const flatbuffers_type* in){
  out->id=in->GetField<uint64_t>(4,0);out->generation=in->GetField<uint64_t>(6,0);
  auto* text=in->GetPointer<const flatbuffers::String*>(8);out->value=text?text->str():"";
 }
};
// Each shard owns its table and allocator.
// One pool per shard avoids making the single-writer pool shared across loaders.
template<class T>struct OwnedTable {
 typename T::allocator_type allocator;
 T table;
 explicit OwnedTable(base::PolymorphismEpochController* epoch):table(epoch){
  allocator.Init(4096);table.set_allocator(&allocator);
 }
 bool Load(const DirBased& dir,const ShardMeta& meta){return table.Load(dir,meta);}
 const Record* Seek(uint64_t hash){return table.Seek(hash);}
 uint32_t Size()const{return table.Size();}
 template<class Visitor>void Visit(const Visitor& v){table.Visit(v);}
};
using Bucket=OwnedTable<ImmutableSnapshotTable<RecordFormat>>;
using Neighbor=OwnedTable<NeighborPointerTable<RecordFormat>>;
using BucketPool=OwnedTable<UpdatableSnapshotTable<RecordFormat>>;
using NeighborPool=OwnedTable<UpdatableNeighborPointerTable<RecordFormat>>;
inline uint64_t ParseUnsigned(std::string_view text){
 uint64_t value=0;
 auto parsed=std::from_chars(text.data(),text.data()+text.size(),value);
 if(text.empty()||parsed.ec!=std::errc()||parsed.ptr!=text.data()+text.size())
  throw std::invalid_argument("expected an unsigned 64-bit integer");
 return value;
}
struct MemoryReturnSettings {
 MemoryReturnPolicy policy{MemoryReturnPolicy::None};
 size_t every{1};
};
inline MemoryReturnSettings ParseMemoryReturnSettings(int argc,char** argv,int first){
 MemoryReturnSettings result;bool seen_policy=false,seen_every=false;
 for(int i=first;i<argc;i+=2){
  if(i+1>=argc)throw std::invalid_argument("missing memory-return option value");
  const std::string_view flag=argv[i];
  if(flag=="--memory-return"&&!seen_policy){
   result.policy=ParseMemoryReturnPolicy(argv[i+1]);seen_policy=true;
  }else if(flag=="--trim-every"&&!seen_every){
   auto count=ParseUnsigned(argv[i+1]);
   if(count>std::numeric_limits<size_t>::max())throw std::invalid_argument("trim-every exceeds platform size");
   result.every=static_cast<size_t>(count);seen_every=true;
  }else throw std::invalid_argument("unknown or duplicate memory-return option");
 }
 MemoryReturnSchedule check;check.Configure(result.policy,result.every);
 return result;
}
inline std::vector<Record> ReadTSV(const std::filesystem::path& path){
 std::ifstream in(path);if(!in)throw std::runtime_error("input unavailable");
 std::vector<Record> records;std::string line;
 while(std::getline(in,line)){
  auto a=line.find('\t'),b=line.find('\t',a+1),c=line.find('\t',b+1);
  if(a==std::string::npos||b==std::string::npos)throw std::runtime_error("expected id, generation and value TSV columns");
  records.push_back({ParseUnsigned(line.substr(0,a)),ParseUnsigned(line.substr(a+1,b-a-1)),line.substr(b+1,c==std::string::npos?c:c-b-1)});
 }
 if(in.bad())throw std::runtime_error("input read failed");
 return records;
}
// Public-data adapter emits the header framing and 16-byte key/offset nodes.
// Records are ordered by bucket for the contiguous-bucket loader.
inline SnapshotMeta Build(const std::vector<Record>& records,const std::filesystem::path& dir,size_t shards){
 if(!IsPowerOfTwo(shards)||shards>1024)throw std::invalid_argument("invalid shards");
 if(std::filesystem::exists(dir))throw std::runtime_error("output already exists");
 {
  std::unordered_set<uint64_t> ids;
  for(const auto& record:records)if(!ids.insert(record.id).second)
   throw std::invalid_argument("duplicate record id");
 }
 std::filesystem::create_directories(dir);
 std::vector<std::vector<const Record*>> groups(shards);
 for(auto& r:records)groups[ShardHashFunc(HashValue(r.id),shards)].push_back(&r);
 SnapshotMeta meta;meta.set_items_total_count(records.size());meta.set_timestamp(records.empty()?0:records.front().generation);
 for(size_t s=0;s<shards;++s){
  auto& items=groups[s];uint64_t buckets=butil::find_power2(static_cast<uint64_t>(std::ceil(items.size()/FLAGS_hash_refactor)));
  std::sort(items.begin(),items.end(),[&](auto* a,auto* b){auto x=HashValue(a->id),y=HashValue(b->id);return (x&(buckets-1))!=(y&(buckets-1))?(x&(buckets-1))<(y&(buckets-1)):x<y;});
  struct Node {uint64_t key,offset;};std::vector<Node> index;std::string data;
  for(auto* record:items){
   flatbuffers::FlatBufferBuilder fb;auto text=fb.CreateString(record->value);auto start=fb.StartTable();
   fb.AddElement<uint64_t>(4,record->id,0);fb.AddElement<uint64_t>(6,record->generation,0);fb.AddOffset(8,text);
   fb.Finish(flatbuffers::Offset<void>(fb.EndTable(start)));
   data.resize((data.size()+7)&~size_t(7),0);
   index.push_back({HashValue(record->id),data.size()+flatbuffers::ReadScalar<flatbuffers::uoffset_t>(fb.GetBufferPointer())});
   data.append(reinterpret_cast<const char*>(fb.GetBufferPointer()),fb.GetSize());
  }
  auto* shard=meta.add_shard_info_list();shard->set_dir_name(std::to_string(s));shard->set_items_total_count(items.size());
  shard->set_index_file_name("index.bin");shard->set_data_file_name("data.bin");
  auto path=dir/std::to_string(s);std::filesystem::create_directory(path);
  TableHeader header;header.set_version(0);header.set_item_count(items.size());header.set_bucket_count(buckets);
  auto raw=header.SerializeAsString();uint32_t size=raw.size();
  {std::ofstream out(path/"index.bin",std::ios::binary);out.write(reinterpret_cast<const char*>(&size),4);out.write(raw.data(),raw.size());out.write(reinterpret_cast<const char*>(index.data()),index.size()*sizeof(Node));if(!out)throw std::runtime_error("index write");}
  {std::ofstream out(path/"data.bin",std::ios::binary);out.write(data.data(),data.size());if(!out)throw std::runtime_error("data write");}
  shard->set_index_file_size(std::filesystem::file_size(path/"index.bin"));shard->set_data_file_size(data.size());
 }
 {std::ofstream out(dir/"meta.pb",std::ios::binary);if(!meta.SerializeToOstream(&out))throw std::runtime_error("meta write");}
 return meta;
}
inline SnapshotMeta Meta(const std::filesystem::path& dir){
 std::ifstream in(dir/"meta.pb",std::ios::binary);SnapshotMeta meta;
 if(!meta.ParseFromIstream(&in)||!IsPowerOfTwo(meta.shard_info_list_size()))throw std::runtime_error("invalid meta");
 for(auto& s:meta.shard_info_list()){
  auto path=dir/s.dir_name();
  if(std::filesystem::file_size(path/s.index_file_name())!=uint64_t(s.index_file_size())||std::filesystem::file_size(path/s.data_file_name())!=uint64_t(s.data_file_size()))throw std::runtime_error("size mismatch");
 }
 return meta;
}
}
