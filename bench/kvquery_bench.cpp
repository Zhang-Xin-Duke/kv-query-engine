#include <sys/utsname.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <random>
#include <stdexcept>
#include <unordered_map>

#include "kvquery/kvquery.hpp"

using namespace kvquery;
using Clock = std::chrono::steady_clock;
namespace {
uint64_t integer(std::string_view s) {
  uint64_t n = 0;
  const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
  if (ec != std::errc{} || p != s.data() + s.size()) throw std::invalid_argument("invalid integer");
  return n;
}
uint64_t mix(uint64_t k) {
  k ^= k >> 33;
  k *= 0xff51afd7ed558ccdULL;
  k ^= k >> 33;
  k *= 0xc4ceb9fe1a85ec53ULL;
  k ^= k >> 33;
  return k;
}
struct Hash {
  size_t operator()(uint64_t k) const noexcept { return static_cast<size_t>(mix(k)); }
};
using Map = std::unordered_map<uint64_t, std::string, Hash>;
uint64_t consume(std::span<const std::optional<std::string_view>> result) {
  uint64_t h = 1;
  for (const auto& v : result) {
    uint64_t x = 0;
    if (v) {
      x = v->size() + 1;
      if (!v->empty())
        x ^= uint64_t(static_cast<unsigned char>(v->front())) << 32 |
             uint64_t(static_cast<unsigned char>(v->back())) << 40;
    }
    h = (h * 1099511628211ULL) ^ x;
  }
  return h;
}
std::vector<std::optional<std::string_view>> map_query(const Map& map,
                                                       std::span<const uint64_t> keys) {
  std::vector<std::optional<std::string_view>> out(keys.size());
  for (size_t i = 0; i < keys.size(); ++i) {
    const auto it = map.find(keys[i]);
    if (it != map.end()) out[i] = it->second;
  }
  return out;
}
struct Method {
  std::string name;
  QueryOptions options;
};
struct Measurement {
  Method method;
  uint64_t elapsed = 0, checksum = 0;
  std::vector<uint64_t> ns;
};
uint64_t percentile(std::vector<uint64_t> values, double q) {
  std::sort(values.begin(), values.end());
  return values[static_cast<size_t>(std::ceil(q * static_cast<double>(values.size()))) - 1];
}
std::string escaped(std::string_view s) {
  std::string out;
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    if (static_cast<unsigned char>(c) >= 32) out += c;
  }
  return out;
}
}  // namespace
int main(int argc, char** argv) {
  try {
    uint64_t count = 100000, batch = 256, queries = 262144, value_size = 32, seed = 20261021,
             rounds = 3;
    double hit_rate = .9;
    std::string distribution = "uniform";
    for (int i = 1; i < argc; i += 2) {
      if (i + 1 >= argc) throw std::invalid_argument("each argument requires a value");
      const std::string_view key = argv[i], value = argv[i + 1];
      if (key == "--records")
        count = integer(value);
      else if (key == "--batch")
        batch = integer(value);
      else if (key == "--queries")
        queries = integer(value);
      else if (key == "--value-bytes")
        value_size = integer(value);
      else if (key == "--seed")
        seed = integer(value);
      else if (key == "--rounds")
        rounds = integer(value);
      else if (key == "--distribution")
        distribution = value;
      else if (key == "--hit-rate") {
        size_t end = 0;
        hit_rate = std::stod(std::string(value), &end);
        if (end != value.size()) throw std::invalid_argument("invalid hit rate");
      } else
        throw std::invalid_argument("unknown benchmark option");
    }
    if (!count || count > 10'000'000 || !batch || batch > 65536 || !queries ||
        queries > 10'000'000 || !rounds || rounds > 50 || value_size > 1048576 ||
        !std::isfinite(hit_rate) || hit_rate < 0 || hit_rate > 1 ||
        (distribution != "uniform" && distribution != "hot"))
      throw std::invalid_argument("benchmark option out of bounds");
    if (value_size && count > ((512ULL << 20) / value_size))
      throw std::length_error("benchmark value budget exceeded");
    std::mt19937_64 rng(seed);
    std::vector<Record> records;
    records.reserve(static_cast<size_t>(count));
    Map map;
    map.max_load_factor(.8F);
    map.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; ++i) {
      std::string value(static_cast<size_t>(value_size), '\0');
      for (size_t j = 0; j < value.size(); ++j) value[j] = static_cast<char>(mix(i + j) & 255);
      records.push_back({i * 2, std::move(value)});
      map.emplace(records.back().key, records.back().value);
    }
    const auto begin_build = Clock::now();
    const auto snapshot = Snapshot::build(records);
    const auto build_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin_build).count();
    std::vector<Record>().swap(records);
    std::vector<uint64_t> keys;
    keys.reserve(static_cast<size_t>(queries));
    uint64_t hits = 0;
    for (uint64_t i = 0; i < queries; ++i) {
      const bool hit = static_cast<double>(rng() >> 11) / 9007199254740992.0 < hit_rate;
      uint64_t bound = count;
      // 90% of lookups choose from the first 1% of keys for the hot distribution.
      if (distribution == "hot" && rng() % 10 < 9) bound = std::max<uint64_t>(1, count / 100);
      keys.push_back(2 * (rng() % bound) + (hit ? 0 : 1));
      hits += hit;
    }
    std::vector<Method> methods{{"unordered_map", {Strategy::scalar, 1}},
                                {"scalar", {Strategy::scalar, 1}}};
    for (auto strategy : {Strategy::prefetch, Strategy::amac})
      for (size_t window : {8, 32, 64})
        methods.push_back({strategy_name(strategy), {strategy, window}});
    std::vector<Measurement> results;
    for (const auto& m : methods) results.push_back({m, 0, 0, {}});
    // Full result equality validation is outside all timed sections.
    for (size_t pos = 0; pos < keys.size(); pos += static_cast<size_t>(batch)) {
      const auto input = std::span<const uint64_t>(keys).subspan(
          pos, std::min<size_t>(static_cast<size_t>(batch), keys.size() - pos));
      const auto expected = map_query(map, input);
      for (const auto& m : methods)
        if (m.name != "unordered_map") {
          auto actual = snapshot->query(input, m.options);
          if (!std::equal(actual.values().begin(), actual.values().end(), expected.begin()))
            throw std::runtime_error("benchmark verification failed");
        }
    }
    // Warm every strategy; rotate measurement order across rounds to reduce order bias.
    for (const auto& m : methods) {
      auto input = std::span<const uint64_t>(keys).first(
          std::min<size_t>(keys.size(), static_cast<size_t>(batch)));
      if (m.name == "unordered_map") {
        auto out = map_query(map, input);
        if (consume(out) == 0) std::cerr << "";
      } else {
        auto out = snapshot->query(input, m.options);
        if (consume(out.values()) == 0) std::cerr << "";
      }
    }
    for (uint64_t round = 0; round < rounds; ++round)
      for (size_t step = 0; step < methods.size(); ++step) {
        const size_t index = (step + static_cast<size_t>(round)) % methods.size();
        auto& result = results[index];
        for (size_t pos = 0; pos < keys.size(); pos += static_cast<size_t>(batch)) {
          const auto input = std::span<const uint64_t>(keys).subspan(
              pos, std::min<size_t>(static_cast<size_t>(batch), keys.size() - pos));
          uint64_t check = 0;
          const auto start = Clock::now();
          if (result.method.name == "unordered_map") {
            const auto out = map_query(map, input);
            check = consume(out);
          } else {
            const auto out = snapshot->query(input, result.method.options);
            check = consume(out.values());
          }
          const auto ns = static_cast<uint64_t>(
              std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
          result.elapsed += ns;
          result.ns.push_back(ns);
          result.checksum += check;
        }
      }
    for (const auto& r : results)
      if (r.checksum != results.front().checksum)
        throw std::runtime_error("timed checksum mismatch");
    const auto stats = snapshot->stats();
    struct utsname host{};
    ::uname(&host);
    std::cout << std::setprecision(12) << "{\n  \"schema_version\":1,\n  \"compiler\":\""
              << escaped(__VERSION__) << "\",\n  \"os\":\"" << escaped(host.sysname)
              << "\",\n  \"arch\":\"" << escaped(host.machine) << "\",\n  \"records\":" << count
              << ",\n  \"batch\":" << batch << ",\n  \"queries_per_round\":" << queries
              << ",\n  \"rounds\":" << rounds << ",\n  \"seed\":" << seed
              << ",\n  \"value_bytes\":" << value_size << ",\n  \"requested_hit_rate\":" << hit_rate
              << ",\n  \"actual_hit_rate\":"
              << static_cast<double>(hits) / static_cast<double>(queries)
              << ",\n  \"distribution\":\"" << distribution
              << "\",\n  \"index_bytes\":" << stats.index_bytes
              << ",\n  \"value_storage_bytes\":" << stats.value_bytes
              << ",\n  \"build_ns\":" << build_ns << ",\n  \"results\":[\n";
    for (size_t i = 0; i < results.size(); ++i) {
      const auto& r = results[i];
      std::cout << "    {\"strategy\":\"" << r.method.name
                << "\",\"window\":" << r.method.options.window << ",\"keys_per_second\":"
                << static_cast<double>(queries * rounds) * 1e9 / static_cast<double>(r.elapsed)
                << ",\"batch_p50_ns\":" << percentile(r.ns, .5)
                << ",\"batch_p95_ns\":" << percentile(r.ns, .95)
                << ",\"batch_p99_ns\":" << percentile(r.ns, .99) << ",\"samples\":" << r.ns.size()
                << ",\"checksum\":" << r.checksum << "}";
      std::cout << (i + 1 == results.size() ? "\n" : ",\n");
    }
    std::cout << "  ]\n}\n";
  } catch (const std::exception& e) {
    std::cerr << "kvquery_bench: " << e.what() << '\n';
    return 1;
  }
}
