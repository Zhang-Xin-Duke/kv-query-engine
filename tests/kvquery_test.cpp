#include "kvquery/kvquery.hpp"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <stdexcept>
#include <thread>

#include "kvquery/detail/neighbor_index.hpp"

using namespace kvquery;
#define CHECK(x)                                                                        \
  do {                                                                                  \
    if (!(x))                                                                           \
      throw std::runtime_error(std::string(__func__) + ":" + std::to_string(__LINE__) + \
                               " check failed: " #x);                                   \
  } while (false)
template <class F>
void rejects(F&& f) {
  bool failed = false;
  try {
    f();
  } catch (const std::exception&) {
    failed = true;
  }
  CHECK(failed);
}
static const std::vector<Strategy> strategies{Strategy::scalar, Strategy::prefetch, Strategy::amac};
void compare(const std::shared_ptr<const Snapshot>& s, const std::vector<uint64_t>& keys,
             const std::map<uint64_t, std::string>& ref) {
  for (auto strategy : strategies)
    for (size_t window : {1, 2, 3, 8, 32, 256}) {
      const auto r = s->query(keys, {strategy, window});
      CHECK(r.size() == keys.size());
      for (size_t i = 0; i < keys.size(); ++i) {
        const auto it = ref.find(keys[i]);
        CHECK(r[i].has_value() == (it != ref.end()));
        if (it != ref.end()) CHECK(*r[i] == it->second);
      }
    }
}
void edge_cases() {
  std::vector<Record> records{{0, ""},
                              {UINT64_MAX, std::string("a\0b", 3)},
                              {42, std::string(262143, 'x')},
                              {43, std::string(300000, 'z')}};
  auto s = Snapshot::build(records);
  std::map<uint64_t, std::string> ref;
  for (const auto& r : records) ref.emplace(r.key, r.value);
  compare(s, {0, 42, 43, 1, UINT64_MAX, 0, 42}, ref);
  compare(s, {}, ref);
  auto empty = Snapshot::build({});
  compare(empty, {0, 1, UINT64_MAX}, {});
  CHECK(s->stats().records == records.size());
  rejects([&] { s->query({}, {Strategy::amac, 0}); });
  rejects([&] { s->query({}, {Strategy::amac, 257}); });
  rejects([&] { s->query({}, {static_cast<Strategy>(99), 1}); });
  rejects([&] { Snapshot::build(std::vector<Record>{{1, "a"}, {1, "b"}}); });
  for (double lf : {0.0, 1.0, -1.0, std::numeric_limits<double>::quiet_NaN()}) {
    BuildOptions o;
    o.load_factor = lf;
    rejects([&] { Snapshot::build(records, o); });
  }
  BuildOptions o;
  o.max_records = 1;
  rejects([&] { Snapshot::build(records, o); });
  o = {};
  o.max_value_bytes = 2;
  rejects([&] { Snapshot::build(records, o); });
  o = {};
  o.max_index_bytes = 8;
  rejects([&] { Snapshot::build(records, o); });
}
void collision_and_relocation() {
  using Index = detail::NeighborIndex;
  // All first keys hash to slot 0 at the exact capacity used below.
  Index hasher({}, 16);
  std::vector<Index::Entry> entries;
  for (uint64_t k = 0; entries.size() < 8; ++k)
    if (hasher.bucket(k) == 0) entries.push_back({k, 1});
  Index collisions(entries, 16);
  CHECK(collisions.stats().collisions == 7);
  for (auto e : entries) CHECK(collisions.find(e.key) != nullptr);
  // Insertion 2 becomes a lodger; insert a host for its actual position to force relocation.
  Index two(std::span<const Index::Entry>(entries.data(), 2), 16);
  uint32_t lodger = two.next(0, two.node(0).ChainLink());
  uint64_t host = 1;
  while (hasher.bucket(host) != lodger) ++host;
  auto forced = std::vector<Index::Entry>{entries[0], entries[1], {host, 2}};
  Index relocated(forced, 16);
  CHECK(relocated.stats().relocations == 1);
  for (auto e : forced) CHECK(relocated.find(e.key)->ValueOffset() == e.packed);
  for (uint32_t n = 1; n < 32; ++n) {
    std::vector<Index::Entry> tiny;
    for (uint64_t k = 0; k < n; ++k) tiny.push_back({k, k + 1});
    Index t(tiny, n);
    for (auto e : tiny) CHECK(t.find(e.key)->ValueOffset() == e.packed);
    CHECK(!t.find(999999));
  }
  // Exercise both signed endpoints independently of table topology.
  Index::FlatNode node;
  node.SetData(0, 1);
  for (int off : {-512, -511, -1, 0, 1, 511}) {
    node.SetChainLink(off);
    CHECK(node.ChainLink() == off);
    CHECK(node.ValueOffset() == 1);
  }
}
void randomized() {
  std::mt19937_64 rng(20261021);
  for (size_t n : {1, 2, 3, 10, 100, 1000, 10000}) {
    std::vector<Record> records;
    std::map<uint64_t, std::string> ref;
    while (records.size() < n) {
      uint64_t key = rng();
      std::string value(static_cast<size_t>(rng() % 128), '\0');
      for (char& c : value) c = static_cast<char>(rng());
      if (ref.emplace(key, value).second) records.push_back({key, value});
    }
    for (double lf : {0.4, 0.8, 0.95}) {
      BuildOptions o;
      o.load_factor = lf;
      auto s = Snapshot::build(records, o);
      std::vector<uint64_t> keys;
      for (const auto& r : records) keys.push_back(r.key);
      for (size_t i = 0; i < n; ++i) keys.push_back(rng());
      std::shuffle(keys.begin(), keys.end(), rng);
      compare(s, keys, ref);
    }
  }
}
void lifetime_and_concurrent_reads() {
  std::vector<Record> records;
  for (uint64_t k = 0; k < 1000; ++k) records.push_back({k, std::to_string(k)});
  auto s = Snapshot::build(records);
  std::weak_ptr<const Snapshot> weak = s;
  std::vector<uint64_t> keys{0, 999, 1000, 1, 1};
  auto held = s->query(keys);
  std::atomic<bool> ok{true};
  std::vector<std::thread> threads;
  for (int t = 0; t < 6; ++t)
    threads.emplace_back([s, &keys, &ok] {
      for (int i = 0; i < 500; ++i) {
        auto r = s->query(keys, {Strategy::amac, 3});
        if (!r[0] || *r[0] != "0" || r[2] || *r[4] != "1") ok = false;
      }
    });
  s.reset();
  for (auto& t : threads) t.join();
  threads.clear();
  CHECK(ok.load());
  CHECK(!weak.expired());
  CHECK(*held[1] == "999");
  held = BatchResult{};
  CHECK(weak.expired());
}
struct TestDir {
  std::filesystem::path path;
  TestDir() {
    std::string pattern =
        (std::filesystem::temp_directory_path() / "kvquery-test-XXXXXX").string();
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back(0);
    const auto p = ::mkdtemp(name.data());
    if (!p) throw std::runtime_error("mkdtemp failed");
    path = p;
  }
  ~TestDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};
std::vector<char> read_bytes(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), {}};
}
void write_bytes(const std::filesystem::path& p, const std::vector<char>& bytes) {
  std::ofstream out(p, std::ios::binary);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!out) throw std::runtime_error("test file write failed");
}
void set64(std::vector<char>& bytes, size_t offset, uint64_t v) {
  for (unsigned i = 0; i < 8; ++i) bytes.at(offset + i) = static_cast<char>((v >> (i * 8)) & 255);
}
void checksum(std::vector<char>& bytes) {
  uint64_t h = 14695981039346656037ULL;
  for (size_t i = 0; i < bytes.size(); ++i)
    if (i < 32 || i >= 40) {
      h ^= static_cast<unsigned char>(bytes[i]);
      h *= 1099511628211ULL;
    }
  set64(bytes, 32, h);
}
void persistence() {
  TestDir dir;
  auto path = dir.path / "sample.kwv";
  std::vector<Record> records{
      {0, ""}, {7, std::string("a\0b", 3)}, {UINT64_MAX, std::string(300000, 'z')}};
  auto s = Snapshot::build(records);
  s->save(path);
  const auto original = read_bytes(path);
  auto loaded = Snapshot::load(path);
  std::map<uint64_t, std::string> ref;
  for (const auto& r : records) ref.emplace(r.key, r.value);
  compare(loaded, {0, 7, 8, UINT64_MAX, 7}, ref);
  rejects([&] { s->save(path); });
  CHECK(read_bytes(path) == original);
  rejects([&] { s->save(dir.path / "missing" / "out.kwv"); });
  auto empty = Snapshot::build({});
  empty->save(dir.path / "empty.kwv");
  compare(Snapshot::load(dir.path / "empty.kwv"), {0, 1}, {});
  const auto corrupt = dir.path / "corrupt.kwv";
  auto bad = [&](std::vector<char> b) {
    write_bytes(corrupt, b);
    rejects([&] { Snapshot::load(corrupt); });
  };
  for (size_t n : {0, 1, 8, 39, 40, 55, 56, 100})
    bad({original.begin(), original.begin() + static_cast<ptrdiff_t>(n)});
  for (size_t offset : {0, 8, 16, 24, 32, 48, 100}) {
    auto b = original;
    b[offset] ^= 1;
    bad(b);
  }
  auto b = original;
  b.push_back(0);
  bad(b);
  b = original;
  set64(b, 16, UINT64_MAX);
  checksum(b);
  bad(b);
  b = original;
  set64(b, 48, UINT64_MAX);
  checksum(b);
  bad(b);
  // Duplicate keys with a valid checksum must still be rejected during index build.
  b = original;
  size_t first = 40;
  uint64_t first_key = 0, first_len = 0;
  for (unsigned i = 0; i < 8; ++i) {
    first_key |= uint64_t(static_cast<unsigned char>(b[first + i])) << (8 * i);
    first_len |= uint64_t(static_cast<unsigned char>(b[first + 8 + i])) << (8 * i);
  }
  set64(b, first + 16 + static_cast<size_t>(first_len), first_key);
  checksum(b);
  bad(b);
  LoadOptions o;
  o.max_file_bytes = 40;
  rejects([&] { Snapshot::load(path, o); });
  o = {};
  o.build.max_records = 1;
  rejects([&] { Snapshot::load(path, o); });
  o = {};
  o.build.max_value_bytes = 2;
  rejects([&] { Snapshot::load(path, o); });
  o = {};
  o.build.max_index_bytes = 16;
  rejects([&] { Snapshot::load(path, o); });
  rejects([&] { Snapshot::load(dir.path / "absent.kwv"); });
  // Racing writers: a single complete file wins, and no temporary names remain.
  auto raced = dir.path / "raced.kwv";
  std::atomic<int> wins = 0;
  std::vector<std::thread> writers;
  for (int t = 0; t < 4; ++t)
    writers.emplace_back([&] {
      try {
        s->save(raced);
        ++wins;
      } catch (const std::exception&) {
      }
    });
  for (auto& t : writers) t.join();
  CHECK(wins == 1);
  compare(Snapshot::load(raced), {0, 7, 8, UINT64_MAX}, ref);
  for (const auto& entry : std::filesystem::directory_iterator(dir.path))
    CHECK(entry.path().string().find(".tmp.") == std::string::npos);
}
void format_golden() {
  TestDir dir;
  // An independent byte-level fixture for the documented little-endian format.
  std::vector<char> bytes(40, 0);
  const std::string magic = "KWV001\r\n";
  std::copy(magic.begin(), magic.end(), bytes.begin());
  set64(bytes, 8, 1);
  set64(bytes, 16, 1);
  set64(bytes, 24, 19);
  bytes.resize(59, 0);
  set64(bytes, 40, 42);
  set64(bytes, 48, 3);
  bytes[56] = 'a';
  bytes[57] = 'b';
  bytes[58] = 'c';
  checksum(bytes);
  const auto fixture = dir.path / "golden.kwv";
  write_bytes(fixture, bytes);
  auto loaded = Snapshot::load(fixture);
  compare(loaded, {42, 0, 43}, {{42, "abc"}});
  const auto saved = dir.path / "saved.kwv";
  Snapshot::build(std::vector<Record>{{42, "abc"}})->save(saved);
  CHECK(read_bytes(saved) == bytes);
}
void neighborhood_retry() {
  using Index = detail::NeighborIndex;
  constexpr uint32_t count = 1024;
  // Derive the exact first-attempt capacity, then exhaust its relative-link neighborhood.
  BuildOptions o;
  const auto buckets = static_cast<uint32_t>(static_cast<long double>(count) / o.load_factor + 1);
  Index hasher({}, buckets);
  std::vector<Record> records;
  for (uint64_t k = 0; records.size() < count; ++k)
    if (hasher.bucket(k) == 0) records.push_back({k, std::to_string(k)});
  auto s = Snapshot::build(records);
  CHECK(s->stats().build_attempts > 1);
  std::vector<uint64_t> keys;
  std::map<uint64_t, std::string> ref;
  for (const auto& r : records) {
    keys.push_back(r.key);
    ref.emplace(r.key, r.value);
  }
  compare(s, keys, ref);
  o.max_index_bytes = uint64_t(buckets) * 16;
  rejects([&] { Snapshot::build(records, o); });
}
int main(int argc, char** argv) {
  try {
    const std::string group = argc > 1 ? argv[1] : "all";
    const auto run = [&](const char* name, auto&& fn) {
      if (group == "all" || group == name) {
        fn();
        std::cout << name << " passed\n";
      }
    };
    run("edges", edge_cases);
    run("collisions", collision_and_relocation);
    run("randomized", randomized);
    run("concurrent", lifetime_and_concurrent_reads);
    run("persistence", persistence);
    run("retry", neighborhood_retry);
    run("format", format_golden);
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
