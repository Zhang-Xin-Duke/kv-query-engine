#include <algorithm>
#include <barrier>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <kvquery/kvquery.hpp>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#if defined(__CYGWIN__)
#define NOMINMAX
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#endif
using namespace kvquery;
using Clock = std::chrono::steady_clock;
std::vector<unsigned> number_list(const char* value) {
  std::vector<unsigned> result;
  std::istringstream input(value);
  std::string item;
  while (std::getline(input, item, ',')) {
    size_t used = 0;
    const auto n = std::stoul(item, &used);
    if (used != item.size() || n > 65535)
      throw std::invalid_argument("invalid numeric list");
    result.push_back(static_cast<unsigned>(n));
  }
  if (result.empty()) throw std::invalid_argument("empty numeric list");
  return result;
}
std::vector<unsigned> cpu_order() {
  std::vector<unsigned> allowed;
#if defined(__CYGWIN__)
  DWORD_PTR process_mask = 0, system_mask = 0;
  if (!GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask))
    throw std::runtime_error("cannot read process affinity");
  for (unsigned i = 0; i < sizeof(process_mask) * 8; ++i)
    if (process_mask & (DWORD_PTR(1) << i)) allowed.push_back(i);
#else
  cpu_set_t mask;
  if (sched_getaffinity(0, sizeof(mask), &mask))
    throw std::runtime_error("cannot read process affinity");
  for (unsigned i = 0; i < CPU_SETSIZE; ++i)
    if (CPU_ISSET(i, &mask)) allowed.push_back(i);
#endif
  if (const auto value = std::getenv("KVQUERY_BENCH_CPUS")) {
    const auto selected = number_list(value);
    for (unsigned cpu : selected)
      if (std::find(allowed.begin(), allowed.end(), cpu) == allowed.end())
        throw std::invalid_argument(
            "requested CPU is outside allowed affinity");
    return selected;
  }
  if (allowed.empty()) throw std::runtime_error("no allowed CPUs");
  return allowed;
}
void pin(unsigned cpu) {
#if defined(__CYGWIN__)
  if (!SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR(1) << cpu))
    throw std::runtime_error("thread affinity failed");
#else
  cpu_set_t mask;
  CPU_ZERO(&mask);
  CPU_SET(cpu, &mask);
  if (pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask))
    throw std::runtime_error("thread affinity failed");
#endif
}
uint64_t token(uint64_t key, size_t bytes) {
  if (key & 1) return 0;
  const uint64_t b = ((key / 2) * 131 + 17) & 255;
  return (bytes + 1) ^ (b << 32) ^ (b << 40);
}
uint64_t consume(const BatchResult& result) {
  uint64_t h = 1;
  for (const auto& v : result.values()) {
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
struct Trace {
  std::vector<uint64_t> keys, expected;
};
struct alignas(64) Result {
  uint64_t batches = 0, sum = 0, expected = 0;
  std::vector<uint64_t> latency;
  bool failed = false;
};
uint64_t percentile(std::vector<uint64_t> v, double q) {
  std::sort(v.begin(), v.end());
  return v.empty() ? 0 : v[static_cast<size_t>(std::ceil(q * v.size())) - 1];
}
int main(int argc, char** argv) {
  try {
    const auto cpus = cpu_order();
    uint64_t n = argc > 1 ? std::stoull(argv[1]) : 16000000;
    size_t bytes = argc > 2 ? std::stoull(argv[2]) : 32;
    size_t batch = argc > 3 ? std::stoull(argv[3]) : 256;
    double seconds = argc > 4 ? std::stod(argv[4]) : 2;
    unsigned rounds = argc > 5 ? std::stoul(argv[5]) : 3;
    std::string mode = argc > 6 ? argv[6] : "full";
    bool smoke = mode == "smoke";
    if (!n || n > 32000000 || !batch || (batch & (batch - 1)) || batch > 4096 ||
        bytes == 0 || bytes > 256 || !std::isfinite(seconds) || seconds <= 0 ||
        seconds > 3600 || rounds == 0 || rounds > 20)
      throw std::runtime_error("invalid arguments");
    std::vector<Record> records;
    records.reserve(n);
    for (uint64_t i = 0; i < n; ++i)
      records.push_back(
          {2 * i, std::string(bytes, static_cast<char>((i * 131 + 17) & 255))});
    BuildOptions options;
    options.max_records = 32000000;
    options.max_index_bytes = 2ULL << 30;
    options.max_value_bytes = 8ULL << 30;
    auto snapshot = Snapshot::build(records, options);
    std::vector<Record>().swap(records);
    const auto stats = snapshot->stats();
    std::vector<unsigned> levels =
        smoke ? std::vector<unsigned>{1, 2}
              : (mode == "focused"
                     ? std::vector<unsigned>{1, 8, 24}
                     : std::vector<unsigned>{1, 2, 4, 8, 16, 24, 32});
    if (!smoke)
      if (const auto value = std::getenv("KVQUERY_BENCH_THREADS"))
        levels = number_list(value);
    for (unsigned t : levels)
      if (!t || t > 128)
        throw std::invalid_argument("threads must be in [1,128]");
    std::vector<Strategy> strategies{Strategy::scalar, Strategy::amac,
                                     Strategy::simd};
    if (const auto selected = std::getenv("KVQUERY_BENCH_STRATEGY")) {
      auto it =
          std::find_if(strategies.begin(), strategies.end(), [&](Strategy s) {
            return std::string(strategy_name(s)) == selected;
          });
      if (it == strategies.end())
        throw std::invalid_argument("unknown strategy");
      strategies = {*it};
    }
    const unsigned maxthreads = *std::max_element(levels.begin(), levels.end());
    const size_t trace_size = smoke ? 4096 : 1048576;
    std::vector<Trace> traces(maxthreads);
    for (unsigned t = 0; t < maxthreads; ++t) {
      auto& trace = traces[t];
      std::mt19937_64 rng(20261027 + t);
      trace.keys.reserve(trace_size);
      for (size_t i = 0; i < trace_size; ++i)
        trace.keys.push_back(2 * (rng() % n) + (rng() % 10 == 0));
      for (size_t p = 0; p < trace_size; p += batch) {
        uint64_t h = 1;
        for (size_t j = p; j < std::min(p + batch, trace_size); ++j)
          h = (h * 1099511628211ULL) ^ token(trace.keys[j], bytes);
        trace.expected.push_back(h);
      }
      for (auto strategy : {Strategy::scalar, Strategy::amac, Strategy::simd}) {
        auto input = std::span<const uint64_t>(trace.keys)
                         .first(std::min(batch, trace_size));
        auto r = snapshot->query(input, {strategy, 32});
        if (consume(r) != trace.expected.front())
          throw std::runtime_error("initial verification failed");
        for (size_t j = 0; j < input.size(); ++j) {
          if (bool(r[j]) != !(input[j] & 1))
            throw std::runtime_error("hit verification failed");
          if (r[j] &&
              *r[j] !=
                  std::string(bytes, static_cast<char>(
                                         ((input[j] / 2) * 131 + 17) & 255)))
            throw std::runtime_error("full-byte verification failed");
        }
      }
    }
    std::cout << std::setprecision(12) << "{\"records\":" << n
              << ",\"value_bytes\":" << bytes << ",\"batch\":" << batch
              << ",\"seconds_target\":" << seconds << ",\"rounds\":" << rounds
              << ",\"trace_keys_per_worker\":" << trace_size
              << ",\"latency_sample_every\":64,\"index_bytes\":"
              << stats.index_bytes
              << ",\"value_storage_bytes\":" << stats.value_bytes
              << ",\"simd_backend\":\"" << simd_backend()
              << "\",\"compiler\":\"" << __VERSION__ << "\",\"cpu_order\":[";
    for (size_t i = 0; i < cpus.size(); ++i)
      std::cout << (i ? "," : "") << cpus[i];
    std::cout << "],\"results\":[\n";
    bool first = true;
    for (unsigned nt : levels)
      for (unsigned round = 0; round < rounds; ++round) {
        for (size_t m = 0; m < strategies.size(); ++m) {
          auto strategy = strategies[(m + round) % strategies.size()];
          std::barrier ready(static_cast<ptrdiff_t>(nt + 1)),
              go(static_cast<ptrdiff_t>(nt + 1));
          Clock::time_point deadline;
          std::vector<Result> results(nt);
          std::vector<std::thread> workers;
          for (unsigned t = 0; t < nt; ++t)
            workers.emplace_back([&, t] {
              auto& r = results[t];
              const auto& tr = traces[t];
              r.latency.reserve(200000);
              try {
                pin(cpus[t % cpus.size()]);
                for (size_t w = 0; w < 128; ++w) {
                  size_t p = (w * batch) % trace_size;
                  auto v = snapshot->query(
                      std::span<const uint64_t>(tr.keys).subspan(
                          p, std::min(batch, trace_size - p)),
                      {strategy, 32});
                  if (consume(v) != tr.expected[p / batch])
                    throw std::runtime_error("warmup checksum");
                }
              } catch (...) {
                r.failed = true;
              }
              // All workers finish warmup before the common deadline is
              // published.
              ready.arrive_and_wait();
              go.arrive_and_wait();
              if (r.failed) return;
              size_t p = 0;
              try {
                while (true) {
                  const auto input = std::span<const uint64_t>(tr.keys).subspan(
                      p, std::min(batch, trace_size - p));
                  uint64_t h;
                  // Sample one call in 64; avoid timing overhead on every small
                  // request.
                  if ((r.batches & 63) == (t & 63)) {
                    auto start = Clock::now();
                    {
                      auto v = snapshot->query(input, {strategy, 32});
                      h = consume(v);
                    }
                    r.latency.push_back(static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            Clock::now() - start)
                            .count()));
                  } else {
                    auto v = snapshot->query(input, {strategy, 32});
                    h = consume(v);
                  }
                  r.sum += h;
                  r.expected += tr.expected[p / batch];
                  ++r.batches;
                  p += input.size();
                  if (p == trace_size) p = 0;
                  if ((r.batches & 63) == 0 && Clock::now() >= deadline) break;
                }
              } catch (...) {
                r.failed = true;
              }
            });
          ready.arrive_and_wait();
          const auto start = Clock::now();
          deadline = start + std::chrono::duration_cast<Clock::duration>(
                                 std::chrono::duration<double>(seconds));
          go.arrive_and_wait();
          for (auto& w : workers) w.join();
          const double elapsed =
              std::chrono::duration<double>(Clock::now() - start).count();
          uint64_t batches = 0;
          std::vector<uint64_t> lat;
          uint64_t minb = UINT64_MAX, maxb = 0;
          for (const auto& r : results) {
            if (r.failed || r.sum != r.expected || !r.batches)
              throw std::runtime_error("timed verification failed");
            batches += r.batches;
            minb = std::min(minb, r.batches);
            maxb = std::max(maxb, r.batches);
            lat.insert(lat.end(), r.latency.begin(), r.latency.end());
          }
          if (!first) std::cout << ",\n";
          first = false;
          std::cout << "{\"threads\":" << nt << ",\"round\":" << round
                    << ",\"strategy\":\"" << strategy_name(strategy)
                    << "\",\"elapsed_seconds\":" << elapsed
                    << ",\"batches\":" << batches << ",\"keys_per_second\":"
                    << double(batches) * batch / elapsed
                    << ",\"batch_requests_per_second\":" << batches / elapsed
                    << ",\"batch_p50_ns\":" << percentile(lat, .5)
                    << ",\"batch_p95_ns\":" << percentile(lat, .95)
                    << ",\"batch_p99_ns\":" << percentile(lat, .99)
                    << ",\"latency_samples\":" << lat.size()
                    << ",\"min_worker_batches\":" << minb
                    << ",\"max_worker_batches\":" << maxb
                    << ",\"checksums_verified\":true}" << std::flush;
          std::cerr << n << " records/" << bytes << " bytes batch=" << batch
                    << " threads=" << nt << " " << strategy_name(strategy)
                    << " round=" << round + 1 << " "
                    << double(batches) * batch / elapsed / 1e6 << " Mkeys/s\n";
        }
      }
    std::cout << "\n]}\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
