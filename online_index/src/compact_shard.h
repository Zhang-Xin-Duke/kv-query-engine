#pragma once

// Adapter for the optional, separately supplied KVQueryEngine dependency.
// Existing disk snapshots are decoded one shard at a time; each replacement
// remains owned by ShardedReloadable and uses its existing epoch reclamation.
#include "engine.h"
#include <kvquery/kvquery.hpp>
#include <optional>

namespace index_engine {
class CompactShard {
 public:
  void SetLayout(size_t shard_count, const std::unordered_map<std::string, size_t>* positions) {
    shard_count_ = shard_count;
    positions_ = positions;
  }
  bool Load(const DirBased& directory, const ShardMeta& meta) {
    try {
      IndexAndDataReader<Record> reader;
      using Node = typename IndexAndDataReader<Record>::Node;
      std::unique_ptr<Node[]> index;
      std::string data;
      if (!reader.ReadIndexAndData(directory, meta, data, index)) return false;
      if (!positions_ || !shard_count_ || positions_->find(meta.dir_name()) == positions_->end())
        throw std::runtime_error("missing shard placement");
      const auto expected_shard = positions_->at(meta.dir_name());
      std::vector<kvquery::Record> records;
      records.reserve(reader.record_count());
      for (uint32_t i = 0; i < reader.record_count(); ++i) {
        const auto offset = index[i].opaque.offset;
        if (offset >= data.size()) throw std::runtime_error("record offset outside data file");
        auto* fb = reinterpret_cast<const flatbuffers::Table*>(data.data() + offset);
        flatbuffers::Verifier verifier(reinterpret_cast<const uint8_t*>(data.data()), data.size());
        if (!fb->VerifyTableStart(verifier) ||
            !fb->VerifyField<uint64_t>(verifier, 4, alignof(uint64_t)) ||
            !fb->VerifyField<uint64_t>(verifier, 6, alignof(uint64_t)) ||
            !fb->VerifyOffset(verifier, 8) ||
            !verifier.VerifyString(fb->GetPointer<const flatbuffers::String*>(8)) ||
            !verifier.EndTable())
          throw std::runtime_error("invalid encoded record");
        Record record;
        RecordFormat::StructFromFlatbuffers(&record, fb);
        if (HashValue(record.id) != index[i].key)
          throw std::runtime_error("record hash mismatch");
        if (((index[i].key >> 48) & (shard_count_ - 1)) != expected_shard)
          throw std::runtime_error("record belongs to another shard");
        if (generation_ && *generation_ != record.generation)
          throw std::runtime_error("mixed generations in shard");
        generation_ = record.generation;
        records.push_back({record.id, std::move(record.value)});
      }
      kvquery::BuildOptions options;
      options.max_records = std::max<uint64_t>(options.max_records, records.size());
      options.max_value_bytes = std::max<uint64_t>(options.max_value_bytes, data.size() + 1);
      options.max_index_bytes = std::max<uint64_t>(options.max_index_bytes, records.size() * 128ULL + 64);
      snapshot_ = kvquery::Snapshot::build(records, options);
      return true;
    } catch (const std::exception& error) {
      LITE_LOG(ERROR) << "compact shard load failed: " << error.what();
      return false;
    }
  }

  void ValidateGeneration(uint64_t expected) const {
    if (generation_ && *generation_ != expected)
      throw std::runtime_error("snapshot generation mismatch");
  }
  uint64_t Size() const { return snapshot_->stats().records; }
  kvquery::Stats Stats() const { return snapshot_->stats(); }
  kvquery::BatchResult Query(const std::vector<uint64_t>& keys, kvquery::QueryOptions options) const {
    return snapshot_->query(keys, options);
  }

 private:
  size_t shard_count_ = 0;
  const std::unordered_map<std::string, size_t>* positions_ = nullptr;
  std::optional<uint64_t> generation_;
  std::shared_ptr<const kvquery::Snapshot> snapshot_;
};
}  // namespace index_engine
