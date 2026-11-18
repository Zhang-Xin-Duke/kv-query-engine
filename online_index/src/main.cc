#include "engine.h"

namespace {
using namespace index_engine;

class ReadGuard {
 public:
  explicit ReadGuard(base::PolymorphismEpochController& epoch)
      : epoch_(epoch), handle_(epoch.Acquire()) {}
  ~ReadGuard() { epoch_.Release(handle_); }
  ReadGuard(const ReadGuard&) = delete;
  ReadGuard& operator=(const ReadGuard&) = delete;

 private:
  base::PolymorphismEpochController& epoch_;
  uint64_t handle_;
};

std::string JsonString(const std::string& input) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string result = "\"";
  for (unsigned char c : input) {
    if (c == '"' || c == '\\') {
      result += '\\';
      result += static_cast<char>(c);
    } else if (c < 0x20) {
      result += "\\u00";
      result += hex[c >> 4];
      result += hex[c & 15];
    } else {
      result += static_cast<char>(c);
    }
  }
  return result + '"';
}

template <class T>
void PrintRecord(ShardedReloadable<T>& table,
                 base::PolymorphismEpochController& epoch,
                 uint64_t key, const char* phase) {
  ReadGuard guard(epoch);
  const auto hash = HashValue(key);
  const auto* record = table.GetShard(hash)->Seek(hash);
  std::cout << "{\"phase\":" << JsonString(phase) << ",\"key\":" << key;
  if (record) {
    std::cout << ",\"found\":true,\"id\":" << record->id
              << ",\"generation\":" << record->generation
              << ",\"value\":" << JsonString(record->value);
  } else {
    std::cout << ",\"found\":false,\"status\":\"not_found\"";
  }
  std::cout << "}\n";
}

template <class T>
int Serve(const std::string& directory, uint64_t key,
          const std::string* replacement, MemoryReturnSettings settings) {
  const auto initial = Meta(directory);
  SnapshotMeta next;
  if (replacement) next = Meta(*replacement);

  // The controller outlives the table and every reader that borrows its records.
  base::PolymorphismEpochController epoch(1024, base::GarbageCollectorRecycler());
  ShardedReloadable<T> table(directory, &epoch);
  table.set_memory_return_policy(settings.policy, settings.every);
  if (!table.Load(initial)) throw std::runtime_error("snapshot load failed");
  PrintRecord(table, epoch, key, replacement ? "before" : "query");
  if (replacement) {
    // PrintRecord releases its guard before this synchronous reload starts.
    table.ResetDir(*replacement);
    if (!table.Reload(next)) throw std::runtime_error("snapshot refresh failed");
    PrintRecord(table, epoch, key, "after");
  }
  return 0;
}

int Dispatch(const std::string& backend, const std::string& directory,
             uint64_t key, const std::string* replacement,
             MemoryReturnSettings settings = {}) {
  if (backend == "bucket") return Serve<Bucket>(directory, key, replacement, settings);
  if (backend == "neighbor") return Serve<Neighbor>(directory, key, replacement, settings);
  if (backend == "bucket-pool") return Serve<BucketPool>(directory, key, replacement, settings);
  if (backend == "neighbor-pool") return Serve<NeighborPool>(directory, key, replacement, settings);
  throw std::invalid_argument("unknown backend: " + backend);
}

void Usage() {
  std::cerr << "Usage:\n"
            << "  online_index build INPUT_TSV OUTPUT_DIR SHARDS\n"
            << "  online_index query BACKEND SNAPSHOT_DIR KEY\n"
            << "  online_index refresh BACKEND OLD_DIR NEW_DIR KEY [--memory-return none|trim|reclaim] [--trim-every N]\n"
            << "BACKEND: bucket | neighbor | bucket-pool | neighbor-pool\n";
}
}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "--help") {
      Usage();
      return 0;
    }
    if (argc == 5 && std::string(argv[1]) == "build") {
      const auto shards = index_engine::ParseUnsigned(argv[4]);
      if (!index_engine::IsPowerOfTwo(shards) || shards > 1024)
        throw std::invalid_argument("SHARDS must be a power of two between 1 and 1024");
      const auto meta = index_engine::Build(index_engine::ReadTSV(argv[2]), argv[3], shards);
      std::cout << "{\"status\":\"built\",\"records\":" << meta.items_total_count()
                << ",\"shards\":" << meta.shard_info_list_size() << "}\n";
      return 0;
    }
    if (argc == 5 && std::string(argv[1]) == "query")
      return Dispatch(argv[2], argv[3], index_engine::ParseUnsigned(argv[4]), nullptr);
    if (argc >= 6 && std::string(argv[1]) == "refresh") {
      const auto settings = ParseMemoryReturnSettings(argc, argv, 6);
      const std::string replacement = argv[4];
      return Dispatch(argv[2], argv[3], index_engine::ParseUnsigned(argv[5]), &replacement, settings);
    }
    Usage();
    return 2;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}
