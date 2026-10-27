#include <tsl/robin_map.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <kvquery/kvquery.hpp>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#if defined(__CYGWIN__)
#define NOMINMAX
#include <windows.h>
#else
#include <sched.h>
#endif
using Clock = std::chrono::steady_clock;
using Values = std::vector<std::optional<std::string>>;
uint64_t mix(uint64_t k) {
  k ^= k >> 33;
  k *= 0xff51afd7ed558ccdULL;
  k ^= k >> 33;
  k *= 0xc4ceb9fe1a85ec53ULL;
  k ^= k >> 33;
  return k;
}
struct Hash {
  size_t operator()(uint64_t x) const noexcept {
    return static_cast<size_t>(mix(x));
  }
};
using Map = std::unordered_map<uint64_t, std::string, Hash>;
using Robin = tsl::robin_map<uint64_t, std::string, Hash>;
using Views = std::vector<std::optional<std::string_view>>;
std::string value(uint64_t key, size_t n) {
  std::string s(n, '\0');
  for (size_t j = 0; j < n; ++j)
    s[j] = static_cast<char>(mix(key / 2 + j) & 255);
  return s;
}
Values expected(std::span<const uint64_t> keys, size_t n) {
  Values v(keys.size());
  for (size_t i = 0; i < keys.size(); ++i)
    if (!(keys[i] & 1)) v[i] = value(keys[i], n);
  return v;
}
template <class Range>
uint64_t consume(const Range& v) {
  uint64_t h = 1;
  for (const auto& s : v) {
    uint64_t x = 0;
    if (s) {
      x = s->size() + 1;
      if (!s->empty())
        x ^= uint64_t(static_cast<unsigned char>(s->front())) << 32 |
             uint64_t(static_cast<unsigned char>(s->back())) << 40;
    }
    h = (h * 1099511628211ULL) ^ x;
  }
  return h;
}
uint64_t percentile(std::vector<uint64_t> v, double q) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[static_cast<size_t>(std::ceil(q * v.size())) - 1];
}
void pin() {
#if defined(__CYGWIN__)
  if (!SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR(1) << 10))
    throw std::runtime_error("pin CPU10 failed");
#else
  cpu_set_t s;
  CPU_ZERO(&s);
  CPU_SET(10, &s);
  if (sched_setaffinity(0, sizeof(s), &s))
    throw std::runtime_error("pin failed");
#endif
}
int main(int argc, char** argv) {
  try {
    if (argc != 7)
      throw std::runtime_error(
          "records bytes batch seconds seed output-prefix");
    const uint64_t n = std::stoull(argv[1]);
    const size_t bytes = std::stoull(argv[2]), batch = std::stoull(argv[3]);
    const double seconds = std::stod(argv[4]);
    const uint64_t seed = std::stoull(argv[5]);
    const std::string prefix = argv[6];
    if (!n || n > 16000000 || !batch || batch > 1024 || (1048576 % batch) ||
        bytes > 256 || !std::isfinite(seconds) || seconds <= 0 ||
        seconds > 3600)
      throw std::runtime_error("invalid arguments");
    pin();
    std::vector<kvquery::Record> records;
    records.reserve(n);
    for (uint64_t i = 0; i < n; ++i)
      records.push_back({2 * i, value(2 * i, bytes)});
    auto begin = Clock::now();
    kvquery::BuildOptions options;
    options.max_records = 16000000;
    options.max_index_bytes = 1ULL << 30;
    options.max_value_bytes = 1ULL << 30;
    auto snap = kvquery::Snapshot::build(records, options);
    const double kvbuild =
        std::chrono::duration<double>(Clock::now() - begin).count();
    begin = Clock::now();
    Map map;
    map.max_load_factor(.8f);
    map.reserve(n);
    for (const auto& r : records) map.emplace(r.key, r.value);
    const double mapbuild =
        std::chrono::duration<double>(Clock::now() - begin).count();
    begin = Clock::now();
    Robin robin;
    robin.max_load_factor(.8f);
    robin.reserve(n);
    for (const auto& r : records) robin.emplace(r.key, r.value);
    const double robinbuild =
        std::chrono::duration<double>(Clock::now() - begin).count();
    std::vector<kvquery::Record>().swap(records);
    std::mt19937_64 rng(seed);
    std::vector<uint64_t> keys;
    keys.reserve(1048576);
    uint64_t hits = 0;
    for (size_t i = 0; i < 1048576; ++i) {
      bool hit = rng() % 10 != 0;
      keys.push_back(2 * (rng() % n) + (hit ? 0 : 1));
      hits += hit;
    }
    std::vector<uint64_t> checks;
    for (size_t p = 0; p < keys.size(); p += batch)
      checks.push_back(consume(
          expected(std::span<const uint64_t>(keys).subspan(p, batch), bytes)));
    auto map_query = [&](const auto& table, std::span<const uint64_t> input) {
      Views out(input.size());
      for (size_t i = 0; i < input.size(); ++i) {
        auto it = table.find(input[i]);
        if (it != table.end()) out[i] = std::string_view(it->second);
      }
      return out;
    };
    auto dispatch = [&](unsigned method, std::span<const uint64_t> input,
                        const auto& use) {
      if (method == 0) {
        auto out = map_query(map, input);
        return use(std::span<const std::optional<std::string_view>>(out));
      }
      if (method == 1) {
        auto out = map_query(robin, input);
        return use(std::span<const std::optional<std::string_view>>(out));
      }
      auto strategy = method == 2   ? kvquery::Strategy::scalar
                      : method == 3 ? kvquery::Strategy::amac
                                    : kvquery::Strategy::simd;
      auto out = snap->query(input, {strategy, 32});
      return use(out.values());
    };
    const std::array<const char*, 5> names{"std_unordered_map", "tsl_robin_map",
                                           "kvquery_scalar", "kvquery_amac32",
                                           "kvquery_simd32"};
    for (unsigned m = 0; m < 5; ++m)
      for (size_t p = 0; p < keys.size(); p += batch) {
        auto input = std::span<const uint64_t>(keys).subspan(p, batch);
        auto want = expected(input, bytes);
        bool good = dispatch(m, input, [&](auto values) {
          return std::equal(values.begin(), values.end(), want.begin(),
                            [](const auto& a, const auto& b) {
                              return a.has_value() == b.has_value() &&
                                     (!a || *a == *b);
                            });
        });
        if (!good)
          throw std::runtime_error("full trace byte verification failed");
      }
    const auto st = snap->stats();
    std::cout << std::setprecision(12) << "{\"records\":" << n
              << ",\"value_bytes\":" << bytes << ",\"batch\":" << batch
              << ",\"seed\":" << seed << ",\"seconds_target\":" << seconds
              << ",\"trace_keys\":" << keys.size()
              << ",\"actual_hit_rate\":" << double(hits) / keys.size()
              << ",\"cpu\":10,\"compiler\":\"" << __VERSION__
              << "\",\"simd_backend\":\"" << kvquery::simd_backend()
              << "\",\"kvquery_logical_bytes\":"
              << st.index_bytes + st.value_bytes
              << ",\"kvquery_buckets\":" << st.buckets
              << ",\"kvquery_build_attempts\":" << st.build_attempts
              << ",\"map_bucket_count\":" << map.bucket_count()
              << ",\"map_load_factor\":" << map.load_factor()
              << ",\"robin_bucket_count\":" << robin.bucket_count()
              << ",\"robin_load_factor\":" << robin.load_factor()
              << ",\"build_seconds\":{\"kvquery\":" << kvbuild
              << ",\"std_map\":" << mapbuild << ",\"robin_map\":" << robinbuild
              << "},\"results\":[\n";
    bool first = true;
    for (unsigned order = 0; order < 5; ++order) {
      const unsigned offset = seed % 2 ? order : 4 - order;
      const unsigned method = static_cast<unsigned>((offset + seed) % 5);
      for (size_t p = 0; p < std::min<size_t>(keys.size(), batch * 256);
           p += batch) {
        auto h =
            dispatch(method, std::span<const uint64_t>(keys).subspan(p, batch),
                     [](auto v) { return consume(v); });
        if (h != checks[p / batch]) throw std::runtime_error("warmup failed");
      }
      std::vector<uint64_t> latency;
      latency.reserve(1000000);
      std::ofstream lf(prefix + "." + names[method] + ".latency.csv"),
          tf(prefix + "." + names[method] + ".timeseries.csv");
      lf << "batch_index,call_ns\n";
      tf << "elapsed_seconds,interval_seconds,batches,keys_per_second\n";
      uint64_t batches = 0, sum = 0, expected_sum = 0, last_batches = 0;
      size_t p = 0;
      begin = Clock::now();
      auto last = begin;
      double elapsed = 0;
      while (true) {
        auto input = std::span<const uint64_t>(keys).subspan(p, batch);
        uint64_t h;
        if ((batches & 63) == 0) {
          auto t = Clock::now();
          {
            h = dispatch(method, input, [](auto v) { return consume(v); });
          }
          latency.push_back(static_cast<uint64_t>(
              std::chrono::duration_cast<std::chrono::nanoseconds>(
                  Clock::now() - t)
                  .count()));
        } else {
          h = dispatch(method, input, [](auto v) { return consume(v); });
        }
        sum += h;
        expected_sum += checks[p / batch];
        ++batches;
        p += batch;
        if (p == keys.size()) p = 0;
        if ((batches & 63) == 0) {
          auto now = Clock::now();
          elapsed = std::chrono::duration<double>(now - begin).count();
          double interval = std::chrono::duration<double>(now - last).count();
          if (interval >= 1 || elapsed >= seconds) {
            tf << std::setprecision(12) << elapsed << ',' << interval << ','
               << batches - last_batches << ','
               << double(batches - last_batches) * batch / interval << '\n';
            last = now;
            last_batches = batches;
          }
          if (elapsed >= seconds) break;
        }
      }
      if (sum != expected_sum)
        throw std::runtime_error("timed checksum mismatch");
      for (size_t i = 0; i < latency.size(); ++i)
        lf << i * 64 << ',' << latency[i] << '\n';
      if (!lf || !tf) throw std::runtime_error("sample write failed");
      if (!first) std::cout << ",\n";
      first = false;
      std::cout << "{\"method\":\"" << names[method]
                << "\",\"elapsed_seconds\":" << elapsed
                << ",\"batches\":" << batches
                << ",\"keys_per_second\":" << double(batches) * batch / elapsed
                << ",\"batch_requests_per_second\":" << batches / elapsed
                << ",\"batch_p50_ns\":" << percentile(latency, .5)
                << ",\"batch_p95_ns\":" << percentile(latency, .95)
                << ",\"batch_p99_ns\":" << percentile(latency, .99)
                << ",\"latency_samples\":" << latency.size()
                << ",\"checksum\":" << sum
                << ",\"expected_checksum\":" << expected_sum
                << ",\"checksums_verified\":true,\"full_trace_byte_"
                   "verification\":true}"
                << std::flush;
      std::cerr << n << "/" << bytes << " " << names[method] << " "
                << double(batches) * batch / elapsed / 1e6 << " Mkeys/s\n";
    }
    std::cout << "\n]}\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
