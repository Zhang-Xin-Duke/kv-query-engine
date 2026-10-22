#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kvquery {
struct Record {
  uint64_t key;
  std::string value;
};
enum class Strategy { scalar, prefetch, amac };
const char* strategy_name(Strategy) noexcept;
struct QueryOptions {
  Strategy strategy = Strategy::scalar;
  size_t window = 32;
};
struct BuildOptions {
  double load_factor = 0.8;
  uint64_t max_records = 10'000'000;
  uint64_t max_value_bytes = 512ULL << 20;
  uint64_t max_index_bytes = 512ULL << 20;
};
struct LoadOptions {
  uint64_t max_file_bytes = 512ULL << 20;
  BuildOptions build;
};
struct Stats {
  uint64_t records, buckets, index_bytes, value_bytes;
  uint64_t direct, collisions, relocations, build_attempts;
};
class Snapshot;
class BatchResult {
 public:
  std::span<const std::optional<std::string_view>> values() const noexcept { return values_; }
  size_t size() const noexcept { return values_.size(); }
  const std::optional<std::string_view>& operator[](size_t i) const { return values_.at(i); }

 private:
  friend class Snapshot;
  std::shared_ptr<const Snapshot> owner_;
  std::vector<std::optional<std::string_view>> values_;
};
// Immutable after construction. Concurrent const queries and save calls are safe.
// Views remain valid while their BatchResult or its originating Snapshot survives.
class Snapshot : public std::enable_shared_from_this<Snapshot> {
 public:
  static std::shared_ptr<const Snapshot> build(std::span<const Record>, BuildOptions = {});
  static std::shared_ptr<const Snapshot> load(const std::filesystem::path&, LoadOptions = {});
  ~Snapshot();
  Snapshot(const Snapshot&) = delete;
  Snapshot& operator=(const Snapshot&) = delete;
  BatchResult query(std::span<const uint64_t>, QueryOptions = {}) const;
  Stats stats() const noexcept;
  // Atomic publication, refuses an existing destination. POSIX local filesystems.
  // Payload and metadata checksum is accidental-corruption detection, not authentication.
  void save(const std::filesystem::path&) const;

 private:
  struct Impl;
  explicit Snapshot(std::unique_ptr<Impl>);
  std::unique_ptr<Impl> impl_;
};
}  // namespace kvquery
