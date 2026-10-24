# SIMD batch probing evaluation

## Implementation

`Strategy::simd` compares two independent candidate keys with a 128-bit vector.
Each unfinished lookup retains its chain position and original input position in
a residual-state window. Completed positions refill immediately; chain advancement
prefetches the next node before other states are processed. Odd or draining single
lanes compare directly.

ARM64 uses NEON `vceqq_u64`. x86 SSE2 compares four 32-bit halves and requires both
halves of each full 64-bit key to match. A portable helper provides the same lane
mask on other targets and can be selected explicitly for testing. Hashing,
noncontiguous candidate loads, chain transitions and result construction remain
scalar; the existing 16-byte index and file format are preserved.

## Server experiment

Measured source revision: `14311dee25ecf9fdd398afb1cb365e28454a60f5`.
Recorded on October 24, 2026 on the Duke Linux course server: Intel Xeon E5-2440 v2,
8 cores / 16 logical CPUs, approximately 16 GB RAM, Ubuntu 22.04.5, GCC 11.4.0,
`-std=c++20 -O3 -DNDEBUG`, pinned to CPU 0. Native backend: SSE2.

The complete [protocol](BENCHMARKS.md) uses eight workloads, three independent
processes and three rounds per process. The table reports process medians in
million keys per second, with a fixed window of 32 for AMAC and SIMD.

| Workload | unordered_map | Scalar | AMAC/32 | SIMD/32 | SIMD / Scalar |
|---|---:|---:|---:|---:|---:|
| small-uniform | 17.78 | 32.97 | 33.50 | 32.85 | 1.00× |
| large-single | 1.59 | 2.77 | 2.96 | 2.89 | 1.04× |
| large-batch32 | 6.13 | 14.26 | 15.68 | 15.75 | 1.10× |
| large-batch256 | 6.23 | 15.33 | 21.49 | 21.15 | 1.38× |
| large-batch1024 | 5.86 | 15.30 | 21.98 | 22.13 | 1.45× |
| large-low-hit | 5.25 | 19.62 | 28.22 | 28.03 | 1.43× |
| large-hot | 9.25 | 31.77 | 39.19 | 38.54 | 1.21× |
| wide-values | 7.24 | 17.18 | 20.76 | 20.64 | 1.20× |

## Findings

For `large-batch1024`, SIMD/32 reaches 22.13 Mkeys/s versus 15.30 for Scalar and
5.86 for `std::unordered_map`: approximately 1.45× and 3.78× throughput. AMAC/32
reaches 21.98 Mkeys/s, providing similar performance in this workload.

These standard workloads use 32/256-byte external values. The separate
[paired inlining experiment](INLINE_EVALUATION.md) covers small and mixed values.
For the small dataset SIMD and Scalar are close; a single-key batch uses a direct
comparison and does not measure vector-comparison acceleration. Scalar remains
the default, with other strategies selected per call.

The executor combines prefetching, interleaved state progression and SIMD key
comparison. These measurements evaluate their combined query path. A separate
ablation would be needed to isolate the vector comparison's contribution.

## Correctness and backend evidence

Native NEON, forced-portable and ASan/UBSan local builds passed all eleven CTest groups.
The server's native SSE2 build passed the same nine C++ groups and two Python
integration checks. GCC disassembly contains `pcmpeqd` and `pmovmskb`; NEON object
inspection contains `cmeq.2d`. Tests check full 64-bit equality, empty/zero keys,
colliding misses, duplicate queries, irregular windows, partial batches, retained
results and concurrent readers.

Linux CI covers GCC Release, Clang Debug, Clang ASan/UBSan and forced-portable GCC
Release. Each configuration runs all eleven CTest groups.

## Reproduction and data

```sh
bash bench/run_linux.sh bench/results/linux-suite 0
```

- [Raw server results and source/binary hashes](../bench/results/duke-x86-inline-2026-10-24.json)
- [Complete throughput and batch P99 report](../bench/results/duke-x86-inline-2026-10-24.md)
- [Recorded environment](../bench/results/duke-x86-inline-2026-10-24.environment.txt)
- [Server test output](../bench/results/duke-x86-inline-2026-10-24.tests.txt)
