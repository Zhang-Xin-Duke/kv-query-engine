#include "kvquery/kvquery.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <system_error>

#include "kvquery/detail/neighbor_index.hpp"

namespace kvquery {
using Index = detail::NeighborIndex;
namespace {
void validate_options(const BuildOptions& o) {
  if (!std::isfinite(o.load_factor) || o.load_factor <= 0 || o.load_factor >= 1)
    throw std::invalid_argument("load_factor must be finite and between 0 and 1");
  if (o.max_index_bytes < sizeof(Index::FlatNode) || !o.max_value_bytes)
    throw std::invalid_argument("resource limits cannot fit an empty snapshot");
}
uint32_t u32le(const char* p) {
  uint32_t v = 0;
  for (unsigned i = 0; i < 4; ++i) v |= uint32_t(static_cast<unsigned char>(p[i])) << (i * 8);
  return v;
}
void append32(std::vector<char>& out, uint32_t v) {
  for (unsigned i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (i * 8)) & 255));
}
void put64(char* p, uint64_t v) {
  for (unsigned i = 0; i < 8; ++i) p[i] = static_cast<char>((v >> (i * 8)) & 255);
}
uint64_t get64(const char* p) {
  uint64_t v = 0;
  for (unsigned i = 0; i < 8; ++i) v |= uint64_t(static_cast<unsigned char>(p[i])) << (i * 8);
  return v;
}
uint64_t hash_bytes(uint64_t h, const char* p, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    h ^= static_cast<unsigned char>(p[i]);
    h *= 1099511628211ULL;
  }
  return h;
}
constexpr uint64_t fnv_basis = 14695981039346656037ULL;
constexpr std::array<char, 8> magic{'K', 'W', 'V', '0', '0', '1', '\r', '\n'};
constexpr size_t header_bytes = 40;
struct TempFile {
  std::filesystem::path path;
  int fd = -1;
  ~TempFile() {
    if (fd >= 0) ::close(fd);
    if (!path.empty()) {
      std::error_code ec;
      std::filesystem::remove(path, ec);
    }
  }
  void write(const char* p, size_t size) {
    while (size) {
      const auto n = ::write(fd, p, std::min<size_t>(size, 1ULL << 30));
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) throw std::system_error(errno, std::generic_category(), "snapshot write");
      p += n;
      size -= static_cast<size_t>(n);
    }
  }
};
}  // namespace
struct Snapshot::Impl {
  std::unique_ptr<Index> index;
  // Offset 0 is reserved so packed data 0 means empty, including for key 0.
  std::vector<char> values{0};
  uint64_t records = 0, attempts = 0;
  std::string_view view(const Index::FlatNode& n) const noexcept {
    const char* data = values.data() + n.ValueOffset();
    const uint32_t len =
        n.ValueLength() == Index::kValueLenSentinel ? u32le(data - 4) : n.ValueLength();
    return {data, len};
  }
};
Snapshot::Snapshot(std::unique_ptr<Impl> p) : impl_(std::move(p)) {}
Snapshot::~Snapshot() = default;
const char* strategy_name(Strategy s) noexcept {
  switch (s) {
    case Strategy::scalar:
      return "scalar";
    case Strategy::prefetch:
      return "prefetch";
    case Strategy::amac:
      return "amac";
  }
  return "invalid";
}
std::shared_ptr<const Snapshot> Snapshot::build(std::span<const Record> records,
                                                BuildOptions options) {
  validate_options(options);
  if (records.size() > options.max_records || records.size() > UINT32_MAX)
    throw std::length_error("record count exceeds limit");
  const long double estimate = static_cast<long double>(records.size()) / options.load_factor + 1;
  const uint64_t max_buckets =
      std::min<uint64_t>(UINT32_MAX, options.max_index_bytes / sizeof(Index::FlatNode));
  if (estimate > static_cast<long double>(max_buckets))
    throw std::length_error("index exceeds limit");
  uint64_t capacity = std::max<uint64_t>(1, static_cast<uint64_t>(estimate));
  auto p = std::make_unique<Impl>();
  // Validate the whole byte budget before constructing value storage.
  uint64_t total = 1;
  for (const auto& r : records) {
    const uint64_t n = r.value.size();
    const uint64_t extra = n + (n >= Index::kValueLenSentinel ? 4 : 0);
    if (n > UINT32_MAX || total > options.max_value_bytes ||
        extra > options.max_value_bytes - total || total + extra > Index::kOffsetMask)
      throw std::length_error("value storage exceeds limit");
    total += extra;
  }
  p->values.reserve(static_cast<size_t>(total));
  std::vector<Index::Entry> entries;
  entries.reserve(records.size());
  for (const auto& r : records) {
    const auto size = static_cast<uint32_t>(r.value.size());
    if (size >= Index::kValueLenSentinel) append32(p->values, size);
    const auto offset = static_cast<uint64_t>(p->values.size());
    p->values.insert(p->values.end(), r.value.begin(), r.value.end());
    entries.push_back({r.key, offset | (uint64_t(std::min(size, Index::kValueLenSentinel)) << 36)});
  }
  for (unsigned attempt = 0;; ++attempt) {
    ++p->attempts;
    try {
      p->index = std::make_unique<Index>(entries, static_cast<uint32_t>(capacity),
                                         attempt ? 0x9e3779b97f4a7c15ULL * attempt : 0);
      break;
    } catch (const std::length_error&) {
      if (attempt == 3 || capacity > max_buckets / 2) throw;
      capacity *= 2;
    }
  }
  p->records = records.size();
  return std::shared_ptr<const Snapshot>(new Snapshot(std::move(p)));
}
Stats Snapshot::stats() const noexcept {
  const auto s = impl_->index->stats();
  return {impl_->records,
          impl_->index->capacity(),
          impl_->index->capacity() * sizeof(Index::FlatNode),
          impl_->values.size(),
          s.direct,
          s.collisions,
          s.relocations,
          impl_->attempts};
}
BatchResult Snapshot::query(std::span<const uint64_t> keys, QueryOptions options) const {
  if (options.window < 1 || options.window > 256)
    throw std::invalid_argument("window must be in [1,256]");
  if (options.strategy != Strategy::scalar && options.strategy != Strategy::prefetch &&
      options.strategy != Strategy::amac)
    throw std::invalid_argument("invalid query strategy");
  BatchResult out;
  out.owner_ = shared_from_this();
  out.values_.resize(keys.size());
  const auto& idx = *impl_->index;
  const auto store = [&](size_t i, const Index::FlatNode* n) {
    if (n) out.values_[i] = impl_->view(*n);
  };
  if (options.strategy == Strategy::scalar) {
    for (size_t i = 0; i < keys.size(); ++i) store(i, idx.find(keys[i]));
  } else if (options.strategy == Strategy::prefetch) {
    const size_t ahead = options.window;
    for (size_t i = 0; i < std::min(ahead, keys.size()); ++i) idx.prefetch(idx.bucket(keys[i]));
    for (size_t i = 0; i < keys.size(); ++i) {
      if (ahead < keys.size() - i) idx.prefetch(idx.bucket(keys[i + ahead]));
      store(i, idx.find(keys[i]));
    }
  } else {
    // Adapted from supplied raw_hash_map.h::amac_find: independent lookup states
    // advance one link per visit; completed states refill from remaining input.
    // Input positions preserve duplicate keys and order; portable prefetch also
    // covers initial fill. Continue round-robin during drain, including short batches.
    struct State {
      size_t input;
      uint32_t bucket;
      bool active;
    };
    std::array<State, 256> states;
    const size_t width = std::min(options.window, keys.size());
    size_t next_input = 0, active = width;
    const auto assign = [&](State& s) {
      s = {next_input, idx.bucket(keys[next_input]), true};
      ++next_input;
      idx.prefetch(s.bucket);
    };
    for (size_t i = 0; i < width; ++i) assign(states[i]);
    while (active)
      for (size_t i = 0; i < width; ++i) {
        auto& s = states[i];
        if (!s.active) continue;
        const auto& n = idx.node(s.bucket);
        if (!n.IsEmpty() && n.key == keys[s.input])
          store(s.input, &n);
        else if (!n.IsEmpty() && n.ChainLink()) {
          s.bucket = idx.next(s.bucket, n.ChainLink());
          idx.prefetch(s.bucket);
          continue;
        }
        if (next_input < keys.size())
          assign(s);
        else {
          s.active = false;
          --active;
        }
      }
  }
  return out;
}
void Snapshot::save(const std::filesystem::path& path) const {
  if (path.empty() || path.filename().empty())
    throw std::invalid_argument("empty snapshot filename");
  if (std::filesystem::exists(path)) throw std::runtime_error("destination already exists");
  std::array<char, header_bytes> header{};
  std::copy(magic.begin(), magic.end(), header.begin());
  put64(header.data() + 8, 1);
  put64(header.data() + 16, impl_->records);
  uint64_t payload = 0;
  const auto visit = [&](auto&& fn) {
    for (uint32_t i = 0; i < impl_->index->capacity(); ++i) {
      const auto& n = impl_->index->node(i);
      if (n.IsEmpty()) continue;
      const auto v = impl_->view(n);
      std::array<char, 16> record{};
      put64(record.data(), n.key);
      put64(record.data() + 8, v.size());
      fn(record.data(), record.size());
      fn(v.data(), v.size());
    }
  };
  visit([&](const char*, size_t n) { payload += n; });
  put64(header.data() + 24, payload);
  uint64_t checksum = hash_bytes(fnv_basis, header.data(), 32);
  visit([&](const char* p, size_t n) { checksum = hash_bytes(checksum, p, n); });
  put64(header.data() + 32, checksum);
  static std::atomic<uint64_t> sequence{0};
  TempFile tmp;
  for (unsigned attempt = 0; attempt < 100; ++attempt) {
    auto candidate = path;
    candidate += ".tmp." + std::to_string(::getpid()) + "." + std::to_string(sequence.fetch_add(1));
    const int fd = ::open(candidate.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) {
      tmp.path = std::move(candidate);
      tmp.fd = fd;
      break;
    }
    if (errno != EEXIST)
      throw std::system_error(errno, std::generic_category(), "create temporary snapshot");
  }
  if (tmp.fd < 0) throw std::runtime_error("cannot create unique temporary snapshot");
  tmp.write(header.data(), header.size());
  visit([&](const char* p, size_t n) { tmp.write(p, n); });
  if (::fsync(tmp.fd) != 0)
    throw std::system_error(errno, std::generic_category(), "snapshot fsync");
  // Atomic no-clobber publication on the same filesystem. Unlike rename(), link()
  // fails if another writer created the destination after our existence check.
  if (::link(tmp.path.c_str(), path.c_str()) != 0)
    throw std::system_error(errno, std::generic_category(), "publish snapshot");
  // TempFile removes the staging name. Directory fsync / crash durability is not promised.
}
std::shared_ptr<const Snapshot> Snapshot::load(const std::filesystem::path& path,
                                               LoadOptions options) {
  validate_options(options.build);
  // Open once and read through this descriptor: metadata and payload refer to one inode.
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) throw std::runtime_error("cannot open snapshot");
  const auto end = in.tellg();
  if (end < 0 || static_cast<uint64_t>(end) < header_bytes ||
      static_cast<uint64_t>(end) > options.max_file_bytes)
    throw std::length_error("snapshot file size outside limits");
  const uint64_t bytes = static_cast<uint64_t>(end);
  in.seekg(0);
  std::array<char, header_bytes> header{};
  if (!in.read(header.data(), header.size())) throw std::runtime_error("truncated header");
  if (!std::equal(magic.begin(), magic.end(), header.begin()) || get64(header.data() + 8) != 1)
    throw std::runtime_error("unsupported snapshot magic/version");
  const uint64_t count = get64(header.data() + 16), payload = get64(header.data() + 24);
  if (payload != bytes - header_bytes || count > options.build.max_records || count > UINT32_MAX ||
      count > payload / 16)
    throw std::length_error("invalid snapshot count/length");
  // Reject impossible resource budgets before allocating record storage.
  const long double buckets = static_cast<long double>(count) / options.build.load_factor + 1;
  if (buckets > static_cast<long double>(options.build.max_index_bytes / sizeof(Index::FlatNode)))
    throw std::length_error("index exceeds limit");
  uint64_t checksum = hash_bytes(fnv_basis, header.data(), 32), remaining = payload,
           value_bytes = 1;
  std::vector<Record> records;
  records.reserve(static_cast<size_t>(count));
  for (uint64_t i = 0; i < count; ++i) {
    std::array<char, 16> record{};
    if (remaining < record.size() || !in.read(record.data(), record.size()))
      throw std::runtime_error("truncated record");
    remaining -= record.size();
    checksum = hash_bytes(checksum, record.data(), record.size());
    const uint64_t len = get64(record.data() + 8), key = get64(record.data());
    if (len > remaining || len > UINT32_MAX) throw std::length_error("invalid value length");
    const uint64_t extra = len + (len >= Index::kValueLenSentinel ? 4 : 0);
    if (value_bytes > options.build.max_value_bytes ||
        extra > options.build.max_value_bytes - value_bytes ||
        value_bytes + extra > Index::kOffsetMask)
      throw std::length_error("value storage exceeds limit");
    value_bytes += extra;
    Record r{key, std::string(static_cast<size_t>(len), '\0')};
    if (!in.read(r.value.data(), static_cast<std::streamsize>(len)))
      throw std::runtime_error("truncated value");
    checksum = hash_bytes(checksum, r.value.data(), r.value.size());
    remaining -= len;
    records.push_back(std::move(r));
  }
  if (remaining || in.peek() != std::char_traits<char>::eof())
    throw std::runtime_error("trailing snapshot data");
  if (checksum != get64(header.data() + 32)) throw std::runtime_error("snapshot checksum mismatch");
  return build(records, options.build);
}
}  // namespace kvquery
