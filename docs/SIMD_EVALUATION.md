# SIMD batch probing evaluation

## Implementation

`Strategy::simd` groups two independent candidate keys into a 128-bit comparison.
It retains each unfinished lookup's chain and input positions in a residual-state
window, prefetches the next node, and refills completed positions immediately.
ARM64 uses NEON; x86 uses SSE2; a forced-portable build tests equivalent semantics.
An odd or draining single lane compares directly without padded index reads.

The vector operation compares complete keys. Hashing, noncontiguous node loads,
chain transitions and result views remain scalar. Compared with a Swiss-table
metadata scan, this path preserves the existing 16-byte index and file format.

## Method

Recorded on 2026-10-24 on an Apple M1 Pro, ARM64 macOS, with AppleClang 21 and
CMake Release defaults. No target-specific `-march=native` flags were added.
Disassembly of `Snapshot::query` contains the NEON `cmeq.2d` instruction.

Each of eight workloads runs in three independent processes, with three rounds
per process and rotating strategy order. All eleven methods process identical
input and return input-ordered value views; full equality is verified outside the
measurement. Timed sections include result allocation, lookup, view ownership,
value length/end-byte consumption and result destruction. These numbers describe
batch-call throughput, not network service throughput or disk reads.

The table uses the process median and a fixed window of 32 for AMAC and SIMD.
Other windows, batch percentiles and all process samples remain in the raw JSON.

Units: million keys per second.

| Workload | unordered_map | Scalar | AMAC/32 | SIMD/32 | SIMD / Scalar |
|---|---:|---:|---:|---:|---:|
| small-uniform | 51.18 | 64.51 | 70.96 | 60.39 | 0.94× |
| large-single | 2.11 | 3.57 | 4.01 | 3.97 | 1.11× |
| large-batch32 | 14.58 | 26.76 | 24.53 | 21.76 | 0.81× |
| large-batch256 | 17.21 | 29.77 | 27.15 | 22.08 | 0.74× |
| large-batch1024 | 17.08 | 31.54 | 28.50 | 23.29 | 0.74× |
| large-low-hit | 14.95 | 31.33 | 32.15 | 30.45 | 0.97× |
| large-hot | 19.06 | 53.49 | 58.74 | 50.31 | 0.94× |
| wide-values | 22.53 | 35.54 | 35.56 | 29.64 | 0.83× |

## Findings

For the seven multi-key workloads in this run, SIMD/32 is slower than Scalar.
The single-key workload cannot fill a vector pair and uses direct comparisons;
its result does not measure vector-comparison acceleration. SIMD remains an
explicit strategy and Scalar remains the default.

This implementation performs two scalar candidate loads before comparing two
keys. Gathering keys, extracting the lane mask and updating independent chain
states require additional instructions. These are plausible sources of overhead;
this evaluation does not isolate their individual costs. A native x86 performance
study and hardware counters would be needed to make an architecture-wide claim.

## Reproduction and evidence

```sh
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j
ctest --test-dir build/release --output-on-failure
python3 bench/run_suite.py --output bench/results/simd-neon-2026-10-24.json --repeats 3
python3 bench/summarize.py --input bench/results/simd-neon-2026-10-24.json --output bench/results/simd-neon-2026-10-24.md
```

- [Raw results and source/binary hashes](../bench/results/simd-neon-2026-10-24.json)
- [Complete throughput and batch P99 report](../bench/results/simd-neon-2026-10-24.md)
- [Correctness and sanitizer coverage](VALIDATION.md)

The recorded source hashes identify the measured implementation independently of
later README, evaluation-text or CI changes. Native NEON, forced-portable and
ASan/UBSan builds each passed all ten local CTest groups. GitHub CI exercises
native SSE2 in GCC/Clang builds and the forced-portable implementation separately.
