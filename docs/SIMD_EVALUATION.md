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

Measured source revision: `f61ff21e6f2fc3ad7a06c2d407e70346c4dafa44`.
Recorded on October 24, 2026 on the Duke Linux course server: Intel Xeon E5-2440 v2,
8 cores / 16 logical CPUs, approximately 16 GB RAM, Ubuntu 22.04.5, GCC 11.4.0,
`-std=c++20 -O3 -DNDEBUG`, pinned to CPU 0. Native backend: SSE2.

The complete [protocol](BENCHMARKS.md) uses eight workloads, three independent
processes and three rounds per process. The table reports process medians in
million keys per second, with a fixed window of 32 for AMAC and SIMD.

| Workload | unordered_map | Scalar | AMAC/32 | SIMD/32 | SIMD / Scalar |
|---|---:|---:|---:|---:|---:|
| small-uniform | 18.65 | 36.17 | 35.36 | 34.80 | 0.96× |
| large-single | 1.61 | 2.81 | 2.84 | 2.92 | 1.04× |
| large-batch32 | 6.21 | 14.75 | 16.05 | 15.92 | 1.08× |
| large-batch256 | 6.35 | 15.78 | 21.66 | 21.56 | 1.37× |
| large-batch1024 | 5.92 | 15.94 | 22.56 | 22.58 | 1.42× |
| large-low-hit | 5.26 | 19.89 | 28.42 | 28.39 | 1.43× |
| large-hot | 9.25 | 32.63 | 40.23 | 39.44 | 1.21× |
| wide-values | 7.32 | 17.66 | 20.84 | 20.87 | 1.18× |

## Findings

For `large-batch1024`, SIMD/32 reaches 22.58 Mkeys/s versus 15.94 for Scalar and
5.92 for `std::unordered_map`: approximately 1.42× and 3.81× throughput. AMAC/32
reaches 22.56 Mkeys/s, so the run supports similar performance for the two
interleaved executors in that workload. It does not establish a meaningful
advantage for SIMD over AMAC.

SIMD/32 improves on Scalar in the other multi-key large-working-set cases as well.
For the small dataset, Scalar is faster. A single-key batch never fills a vector
pair and uses a direct comparison; that row does not measure vector-comparison
acceleration. Scalar remains the default, with other strategies selected per call.

The executor combines prefetching, interleaved state progression and SIMD key
comparison. These measurements evaluate their combined query path. A separate
ablation would be needed to isolate the vector comparison's contribution.

## Correctness and backend evidence

Native NEON, forced-portable and ASan/UBSan local builds passed all ten CTest groups.
The server's native SSE2 build passed the same eight C++ groups and two Python
integration checks. GCC disassembly contains `pcmpeqd` and `pmovmskb`; NEON object
inspection contains `cmeq.2d`. Tests check full 64-bit equality, empty/zero keys,
colliding misses, duplicate queries, irregular windows, partial batches, retained
results and concurrent readers.

Linux CI covers GCC Release, Clang Debug, Clang ASan/UBSan and forced-portable GCC
Release. Each configuration runs all ten CTest groups.

## Reproduction and data

```sh
bash bench/run_linux.sh bench/results/linux-suite 0
```

- [Raw server results and source/binary hashes](../bench/results/duke-x86-2026-10-24.json)
- [Complete throughput and batch P99 report](../bench/results/duke-x86-2026-10-24.md)
- [Recorded environment](../bench/results/duke-x86-2026-10-24.environment.txt)
- [Server test output](../bench/results/duke-x86-2026-10-24.tests.txt)
