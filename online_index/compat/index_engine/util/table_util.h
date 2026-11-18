#pragma once
#include "runtime.h"
#include "index_engine/util/dir_based.h"
#include "index_engine/util/epoch_controller.h"
namespace index_engine {
namespace util {template<class T>std::string type_name(){return typeid(T).name();}}
template <typename T>
struct VersionedValue {
  VersionedValue(int32_t partition, int64_t offset)
      : partition(partition),
        offset(offset) {}
  VersionedValue(int32_t partition, int64_t offset, const T& value)
      : value(value),
        partition(partition),
        offset(offset) {}

  T value;
  // TODO: compact partition and offset metadata.
  int32_t partition{0};
  int64_t offset{0};
};

template <class Value, class Allocator>
class BatchValueGarbageCollector : public base::GarbageCollector {
 public:
  static BatchValueGarbageCollector* Build(std::vector<Value*>& retired_values, Allocator* allocator) {
    auto* gc = butil::get_object<BatchValueGarbageCollector>();
    gc->values.swap(retired_values);
    gc->allocator = allocator;
    return gc;
  }

  void Recycle() override {
    // LOG_EVERY_SECOND(INFO) << "BatchValueGarbageCollector recycle: " << values.size();
    for (auto* v: values) {
      allocator->Release(v);
    }
    values.clear();
    allocator = nullptr;
    butil::return_object(this);
  }

  std::vector<Value*> values;
  Allocator* allocator{nullptr};
};

class AbslHash {
 public:
  uint64_t operator()(uint64_t key) const {
    auto kMul = 0x9ddfea08eb382d6;
    key *= kMul;
    return static_cast<uint64_t>(key ^ (key >> (sizeof(key) * 8 / 2)));
  }

  uint64_t operator()(const std::string& key) const {
    absl::Hash<std::string> hash;
    return hash(key);
  }
};

inline uint64_t ShardHashFunc(uint64_t key, int shard_cnt) {
  return (key >> 48) & (shard_cnt - 1);
}

inline uint64_t FastIntHash(uint64_t key) {
  key ^= key >> 33;
  key *= 0xff51afd7ed558ccd;
  key ^= key >> 33;
  key *= 0xc4ceb9fe1a85ec53;
  key ^= key >> 33;
  return key;
}


inline uint64_t HashValue(uint64_t key){return FastIntHash(key);}
inline bool IsPowerOfTwo(uint64_t n){return n && !(n&(n-1));}
inline double GetPercent(uint64_t n,uint64_t d){return d?100.0*n/d:0;}
template<class T>std::string ProtoStringify(const T& value){return value.ShortDebugString();}
template<class T>std::string StringifyPartitions(const T&){return "";}
template<class T>struct ObjectCounter {
 struct Counter {std::atomic<int64_t> v{0};void operator<<(int n){v.fetch_add(n);}int64_t get_value()const{return v.load();}};
 inline static Counter counter;
 ObjectCounter(){counter<<1;}ObjectCounter(const ObjectCounter&){counter<<1;}
 ~ObjectCounter(){counter<<-1;}
};
} // namespace index_engine
