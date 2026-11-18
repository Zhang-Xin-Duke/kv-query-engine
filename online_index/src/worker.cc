// Persistent engine process for the version-pinned replica service.
// stdin is a private tab-delimited control channel; stdout is JSON lines.
#include "engine.h"
#ifdef ENGINE_HAVE_KVQUERY
#include "compact_shard.h"
#include <optional>
#endif

namespace {
using namespace index_engine;

std::string Quote(std::string_view input) {
  std::string out = "\"";
  const char* hex = "0123456789abcdef";
  for (unsigned char c : input) {
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if (c < 32) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
    else out += c;
  }
  return out + '"';
}

struct Guard {
  base::PolymorphismEpochController& epoch;
  uint64_t handle;
  explicit Guard(base::PolymorphismEpochController& e) : epoch(e), handle(e.Acquire()) {}
  ~Guard() { epoch.Release(handle); }
};

const Record* RecordOf(const Record* record) { return record; }
const Record* RecordOf(const VersionedValue<Record>* record) { return &record->value; }

template<class T>
void Validate(ShardedReloadable<T>& table, const SnapshotMeta& meta, uint64_t generation) {
  uint64_t count = 0;
  table.ForEach([&](uint64_t hash, auto* stored) {
    const auto* record = RecordOf(stored);
    if (record->generation != generation || HashValue(record->id) != hash)
      throw std::runtime_error("snapshot record identity/generation mismatch");
    ++count;
  });
  if (count != static_cast<uint64_t>(meta.items_total_count()))
    throw std::runtime_error("snapshot record count mismatch");
}

#ifdef ENGINE_HAVE_KVQUERY
void Validate(ShardedReloadable<CompactShard>& table, const SnapshotMeta& meta, uint64_t generation) {
  uint64_t count = 0;
  table.ForEachTable([&](const CompactShard* shard) {
    shard->ValidateGeneration(generation);
    count += shard->Size();
  });
  if (count != static_cast<uint64_t>(meta.items_total_count()))
    throw std::runtime_error("snapshot record count mismatch");
}

kvquery::Strategy Strategy(const std::string& name) {
  if (name == "scalar") return kvquery::Strategy::scalar;
  if (name == "prefetch") return kvquery::Strategy::prefetch;
  if (name == "amac") return kvquery::Strategy::amac;
  if (name == "simd") return kvquery::Strategy::simd;
  throw std::invalid_argument("unknown query strategy");
}
#endif

template<class T>
void EmitReady(ShardedReloadable<T>& table, uint64_t generation) {
  std::cout << "{\"ok\":true,\"generation\":" << generation;
#ifdef ENGINE_HAVE_KVQUERY
  if constexpr (std::is_same_v<T, CompactShard>) {
    uint64_t records = 0, inline_records = 0, index_bytes = 0, value_bytes = 0;
    table.ForEachTable([&](const CompactShard* shard) {
      const auto stats = shard->Stats();
      records += stats.records;
      inline_records += stats.inline_records;
      index_bytes += stats.index_bytes;
      value_bytes += stats.value_bytes;
    });
    std::cout << ",\"compact_stats\":{\"records\":" << records
              << ",\"inline_records\":" << inline_records
              << ",\"index_bytes\":" << index_bytes
              << ",\"value_bytes\":" << value_bytes << '}';
  }
#endif
  std::cout << '}' << std::endl;
}

template<class T>
int Run(const std::string& directory, uint64_t generation, MemoryReturnSettings settings,
        const std::string& strategy, size_t window) {
  base::PolymorphismEpochController epoch(1024, base::GarbageCollectorRecycler());
  ShardedReloadable<T> table(directory, &epoch);
  table.set_memory_return_policy(settings.policy, settings.every);
  auto meta = Meta(directory);
#ifdef ENGINE_HAVE_KVQUERY
  std::unordered_map<std::string, size_t> compact_positions;
  size_t compact_shard_count = 0;
  auto set_layout = [&](const SnapshotMeta& layout) {
    if constexpr (std::is_same_v<T, CompactShard>) {
      compact_positions.clear();
      compact_shard_count = static_cast<size_t>(layout.shard_info_list_size());
      for (size_t i = 0; i < compact_shard_count; ++i) {
        if (!compact_positions.emplace(layout.shard_info_list(static_cast<int>(i)).dir_name(), i).second)
          throw std::runtime_error("duplicate shard directory");
      }
    }
  };
  if constexpr (std::is_same_v<T, CompactShard>) {
    table.set_on_shard_create([&](CompactShard* shard) {
      shard->SetLayout(compact_shard_count, &compact_positions);
    });
    set_layout(meta);
  }
#endif
  if (!table.Load(meta)) throw std::runtime_error("initial load failed");
  Validate(table, meta, generation);
  bool ready = true;
  EmitReady(table, generation);
  std::string line;
  while (std::getline(std::cin, line)) {
    try {
      std::istringstream in(line);
      std::string op, token;
      std::getline(in, op, '\t');
      std::getline(in, token, '\t');
      const auto target = ParseUnsigned(token);
      if (op == "REFRESH") {
        std::string next;
        std::getline(in, next);
        if (!ready || target <= generation) throw std::runtime_error("invalid refresh state/generation");
        // Any failure after this point poisons this worker. Restart to recover.
        ready = false;
        auto next_meta = Meta(next);
#ifdef ENGINE_HAVE_KVQUERY
        if constexpr (std::is_same_v<T, CompactShard>) set_layout(next_meta);
#endif
        table.ResetDir(next);
        if (!table.Reload(next_meta)) throw std::runtime_error("refresh failed");
        Validate(table, next_meta, target);
        generation = target;
        ready = true;
        EmitReady(table, generation);
      } else if (op == "BATCH") {
        if (!ready || target != generation) throw std::runtime_error("generation unavailable");
        Guard guard(epoch);
        std::ostringstream out;
        out << "{\"ok\":true,\"generation\":" << generation << ",\"records\":[";
#ifdef ENGINE_HAVE_KVQUERY
        if constexpr (std::is_same_v<T, CompactShard>) {
          std::vector<uint64_t> keys;
          while (std::getline(in, token, '\t')) {
            if (keys.size() >= 1024) throw std::runtime_error("batch too large");
            keys.push_back(ParseUnsigned(token));
          }
          struct Group { const CompactShard* shard; std::vector<uint64_t> keys; std::vector<size_t> positions; };
          std::vector<Group> groups;
          std::unordered_map<const CompactShard*, size_t> group_index;
          for (size_t i = 0; i < keys.size(); ++i) {
            const auto* shard = table.GetShard(HashValue(keys[i]));
            auto [it, inserted] = group_index.emplace(shard, groups.size());
            if (inserted) groups.push_back({shard, {}, {}});
            groups[it->second].keys.push_back(keys[i]);
            groups[it->second].positions.push_back(i);
          }
          std::vector<kvquery::BatchResult> owners;
          std::vector<std::optional<std::string_view>> values(keys.size());
          owners.reserve(groups.size());
          for (const auto& group : groups) {
            owners.push_back(group.shard->Query(group.keys, {Strategy(strategy), window}));
            const auto& result = owners.back();
            for (size_t i = 0; i < group.keys.size(); ++i)
              values[group.positions[i]] = result[i];
          }
          for (size_t i = 0; i < keys.size(); ++i) {
            if (i) out << ',';
            out << "{\"key\":" << keys[i] << ",\"generation\":" << generation;
            if (values[i]) out << ",\"found\":true,\"id\":" << keys[i] << ",\"value\":" << Quote(*values[i]);
            else out << ",\"found\":false";
            out << '}';
          }
          out << "]}";
          // Result owners and the epoch guard both survive serialization.
          std::cout << out.str() << std::endl;
        } else
#endif
        {
        size_t count = 0;
        while (std::getline(in, token, '\t')) {
          if (++count > 1024) throw std::runtime_error("batch too large");
          const auto key = ParseUnsigned(token), hash = HashValue(key);
          const auto* r = table.GetShard(hash)->Seek(hash);
          if (count > 1) out << ',';
          out << "{\"key\":" << key << ",\"generation\":" << generation;
          if (r) {
            if (r->id != key || r->generation != generation)
              throw std::runtime_error("record identity/generation mismatch");
            out << ",\"found\":true,\"id\":" << r->id << ",\"value\":" << Quote(r->value);
          } else out << ",\"found\":false";
          out << '}';
        }
        out << "]}";
        // Serialization completes before releasing the epoch guard.
        std::cout << out.str() << std::endl;
        }
      } else throw std::runtime_error("unknown operation");
    } catch (const std::exception& e) {
      std::cout << "{\"ok\":false,\"error\":" << Quote(e.what()) << "}" << std::endl;
    }
  }
  return 0;
}
}

int main(int argc, char** argv) {
  try {
    if (argc < 4) throw std::invalid_argument("usage: online_index_worker BACKEND SNAPSHOT_DIR GENERATION [--memory-return none|trim|reclaim] [--trim-every N]");
    std::vector<char*> memory_args(argv, argv + 4);
    std::string strategy = "scalar";
    size_t window = 32;
    bool seen_strategy = false, seen_window = false;
    for (int i = 4; i < argc; i += 2) {
      if (i + 1 >= argc) throw std::invalid_argument("missing worker option value");
      std::string_view flag(argv[i]);
      if (flag == "--query-strategy" && !seen_strategy) {
        strategy = argv[i + 1]; seen_strategy = true;
      } else if (flag == "--query-window" && !seen_window) {
        auto parsed = index_engine::ParseUnsigned(argv[i + 1]);
        if (parsed < 1 || parsed > 256) throw std::invalid_argument("query window must be 1..256");
        window = static_cast<size_t>(parsed); seen_window = true;
      } else {
        memory_args.push_back(argv[i]); memory_args.push_back(argv[i + 1]);
      }
    }
    const auto settings = ParseMemoryReturnSettings(static_cast<int>(memory_args.size()), memory_args.data(), 4);
    auto generation = index_engine::ParseUnsigned(argv[3]);
    std::string backend = argv[1];
    if (backend != "compact" && (seen_strategy || seen_window))
      throw std::invalid_argument("query strategy options require compact backend");
    if (backend == "bucket") return Run<Bucket>(argv[2], generation, settings, strategy, window);
    if (backend == "neighbor") return Run<Neighbor>(argv[2], generation, settings, strategy, window);
    if (backend == "bucket-pool") return Run<BucketPool>(argv[2], generation, settings, strategy, window);
    if (backend == "neighbor-pool") return Run<NeighborPool>(argv[2], generation, settings, strategy, window);
#ifdef ENGINE_HAVE_KVQUERY
    if (backend == "compact") {
      Strategy(strategy);  // Reject invalid choices before loading the snapshot.
      return Run<CompactShard>(argv[2], generation, settings, strategy, window);
    }
#endif
    throw std::invalid_argument("unknown backend");
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
