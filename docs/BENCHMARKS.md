# Benchmark protocol and server results

The benchmark design follows the NeighborHash paper and public reproduction
artifact; academic references are listed as [1] and [2] in the
[README references](../README.md#references). The measurements in this repository's
README and current reports use the Duke Linux course server.

## Server and build

- Intel Xeon E5-2440 v2, 8 physical cores / 16 logical CPUs, approximately 16 GB RAM.
- Ubuntu 22.04.5 LTS, Linux 6.8.0-138-generic, x86-64; GCC 11.4.0.
- `-std=c++20 -O3 -DNDEBUG`, no `-march=native`; native backend `sse2`.
- Each benchmark process is single-threaded and pinned to CPU 0. Server capacity
  describes the host, not the number of cores consumed by one test.

The direct GCC runner builds the library, CLI, test executable and benchmark using
existing server tools. It executes the same nine C++ test groups plus two Python
integration checks as CTest before timing.

## Comparable query paths

Each batch is evaluated by eleven configurations:

- `std::unordered_map<uint64_t, std::string, Hash>` with the same initial 64-bit
  mixing function as the compact index, and a target maximum load factor of 0.8.
- Compact-index Scalar.
- Prefetch, AMAC and SIMD with windows 8, 32 and 64.

All paths allocate input-ordered optional value views. The benchmark compares full
result bytes before timing, and consumes each value's length and first/last byte
inside timing. All methods must produce matching checksums. Timed calls include
result allocation/destruction, lookup, value-view construction and ownership.
Query preparation, file loading, network transport and full payload copies are
outside this measurement.

`steady_clock` measures complete batch-call wall time. Throughput divides total
queried keys by accumulated call time. P50/P95/P99 describe batch calls, not
individual keys.

## Workload matrix

| Workload | Records | Keys per batch | Value bytes | Hit target | Distribution |
|---|---:|---:|---:|---:|---|
| small-uniform | 10,000 | 256 | 32 | 90% | uniform |
| large-single | 1,000,000 | 1 | 32 | 90% | uniform |
| large-batch32 | 1,000,000 | 32 | 32 | 90% | uniform |
| large-batch256 | 1,000,000 | 256 | 32 | 90% | uniform |
| large-batch1024 | 1,000,000 | 1024 | 32 | 90% | uniform |
| large-low-hit | 1,000,000 | 256 | 32 | 30% | uniform |
| large-hot | 1,000,000 | 256 | 32 | 90% | hot |
| wide-values | 250,000 | 256 | 256 | 90% | uniform |

Stored keys are even; absent requests use odd keys. Hit requests select existing
records, and the report records actual hit rate. In the hot distribution, 90% of
requests choose a key from the first 1% of records.

Each workload uses three independent processes with adjacent fixed seeds, each
running three rounds. Methods share identical query inputs and rotate execution
order across rounds. Full result checks and warm-up precede the timed sections.
The full suite comprises 24 benchmark processes and 264 configuration reports.

## Reproduce

Run from the repository root:

```sh
bash bench/run_linux.sh bench/results/linux-suite 0
```

The final argument chooses the allowed logical CPU. The runner records environment
and global swap/major-fault counters before and after the run. In the published
server run, those counters did not increase. The server is shared; this does not
provide exclusive hardware isolation.

An existing CMake Release binary can use the same suite:

```sh
taskset -c 0 python3 bench/run_suite.py --binary build/release/kvquery_bench \
  --output bench/results/linux-suite.json --repeats 3 \
  --build-description 'CMake Release, default optimization; no -march=native'
python3 bench/summarize.py --input bench/results/linux-suite.json \
  --output bench/results/linux-suite.md
```

`run_suite.py` records actual CPU affinity, commands, seeds, compiler, hardware,
source hashes and binary hash. Source or binary changes during a run fail its
integrity checks. `--build-description` records the actual build process.

## Reports

## Paired small-value comparison

```sh
bash bench/run_linux.sh bench/results/linux-inline 0 inline
```

The inline suite compares **both layouts in the same process**, with the same
record order, keys, load factor and query trace. The external-only control sets
`BuildOptions::inline_small_values=false`; default snapshots inline 0–6 bytes.
The benchmark retains one unordered_map baseline and two copies of every engine
strategy/window, giving 21 measurements per process. Both snapshots are resident
throughout measurement. Method order rotates and reverses on alternate rounds.

Sixteen cases cover every inline length (0–6), external controls (7, 8, 32, 256),
an equal mix of 6/32-byte values, a cache-resident table, single-key batches, 30%
hits and a hot-key distribution. Three independent processes per case each run
three rounds. All byte contents are validated before timing. Ratios use matched
per-process inline/external measurements, and summaries show process ranges.
Reported memory is logical index plus external-region bytes, not process RSS.

`bench/summarize_inline.py` generates the paired report from the full raw JSON.

## Published reports

The summary uses per-workload process medians and fixed window 32 for comparisons.
Raw JSON retains every window and per-process percentile. Speedups compare methods
within the same workload; they are not averaged across incompatible batch sizes.

- [Raw server JSON](../bench/results/duke-x86-2026-10-24.json)
- [Complete throughput and P99 report](../bench/results/duke-x86-2026-10-24.md)
- [Environment and swap-counter record](../bench/results/duke-x86-2026-10-24.environment.txt)
- [Server correctness checks](../bench/results/duke-x86-2026-10-24.tests.txt)
- [SIMD analysis](SIMD_EVALUATION.md)
