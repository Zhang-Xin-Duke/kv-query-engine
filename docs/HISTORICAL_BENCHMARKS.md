# Historical Duke server benchmarks

Recorded October 24, 2026. These results retain their original hardware, protocol and source artifacts. The main README presents the two desktop comparison suites.

## Performance evaluation

The benchmark design follows the NeighborHash paper and its public reproduction artifact [[1]](https://github.com/Zhang-Xin-Duke/kv-query-engine/blob/41f8981/README.md#reference-1), [[2]](https://github.com/Zhang-Xin-Duke/kv-query-engine/blob/41f8981/README.md#reference-2).

All results below were measured on the **Duke Linux course server** on October 24, 2026: Intel Xeon E5-2440 v2 (8 cores / 16 logical CPUs), approximately 16 GB RAM, Ubuntu 22.04.5, GCC 11.4.0, and `-std=c++20 -O3 -DNDEBUG`. The single-threaded benchmark is pinned to CPU 0; the native SIMD backend is SSE2. No `-march=native` flags are used.

Each workload runs in **three independent processes, three rounds per process**. The table reports median throughput in **million keys per second**. Prefetch, AMAC and SIMD use a fixed window of 32 here; all tested windows and batch P50/P95/P99 remain in the raw results.


![Throughput of Map, Scalar, AMAC and SIMD on eight server workloads](../docs/figures/executor-throughput.svg)

*Server process medians with min/max whiskers. All throughput bars start at zero; the complete table also includes Prefetch.*

<details>
<summary>Full standard-workload throughput table</summary>

| Workload | unordered_map | Scalar | Prefetch/32 | AMAC/32 | SIMD/32 |
|---|---:|---:|---:|---:|---:|
| small-uniform | 17.78 | 32.97 | 31.40 | 33.50 | 32.85 |
| large-single | 1.59 | 2.77 | 2.96 | 2.96 | 2.89 |
| large-batch32 | 6.13 | 14.26 | 15.52 | 15.68 | 15.75 |
| large-batch256 | 6.23 | 15.33 | 18.96 | 21.49 | 21.15 |
| large-batch1024 | 5.86 | 15.30 | 19.30 | 21.98 | 22.13 |
| large-low-hit | 5.25 | 19.62 | 24.64 | 28.22 | 28.03 |
| large-hot | 9.25 | 31.77 | 36.98 | 39.19 | 38.54 |
| wide-values | 7.24 | 17.18 | 19.25 | 20.76 | 20.64 |

</details>

For `large-batch1024` (one million records, 32-byte values, batches of 1,024 keys, 90% target hit rate), SIMD/32 reaches **22.13 Mkeys/s**: **1.45× Scalar** and **3.78× `std::unordered_map`**. AMAC/32 reaches 21.98 Mkeys/s in the same workload. These 32/256-byte workloads use external values even with inlining enabled; they measure the final query implementation. Batch size, working set and hit distribution guide executor selection.

All methods process identical inputs and return input-ordered value views. Full result equality is checked outside the timed sections. Timing includes result allocation, lookup, ownership, value length/end-byte consumption, and result destruction. These are in-process batch-lookup measurements.

- [Raw server results and source/binary hashes](../bench/results/duke-x86-inline-2026-10-24.json)
- [Full throughput and batch P99 report](../bench/results/duke-x86-inline-2026-10-24.md)
- [Recorded server environment](../bench/results/duke-x86-inline-2026-10-24.environment.txt)
- [Workloads and measurement protocol](../docs/BENCHMARKS.md)
- [SIMD implementation and analysis](../docs/SIMD_EVALUATION.md)

Reproduce the complete build, correctness checks and 24-process suite on a Linux host with GCC, Python 3, `ar`, and `taskset`:

```sh
bash bench/run_linux.sh bench/results/linux-suite 0
```

A CMake build can also run the suite through `bench/run_suite.py`; its `--build-description` option records the actual compilation setup.

### Small-value inlining: paired comparison

Both layouts run in the same process with identical records and query traces,
rotating and alternating measurement order. The table uses one million records,
batches of 256 keys, a 90% hit target and SIMD/32. Rates are process medians in
Mkeys/s; ratios are medians of matched per-process ratios. Logical storage counts
index nodes plus the external value region, excluding allocator overhead and
other process memory.


![Paired inline throughput ratios, including the mixed-value regression](../docs/figures/inline-tradeoffs.svg)

*The 1.0 line is equal throughput. Small-value gains and the mixed-value cost are both shown.*

<details>
<summary>Exact paired ratios and logical-storage table</summary>

| Value distribution | External-only | Inlining enabled | Paired ratio | Logical storage MB, before → after |
|---|---:|---:|---:|---:|
| 1 byte | 29.34 | 31.82 | 1.083× | 21.00 → 20.00 |
| 2 bytes | 27.84 | 32.21 | 1.157× | 22.00 → 20.00 |
| 4 bytes | 25.47 | 31.76 | 1.244× | 24.00 → 20.00 |
| 6 bytes | 24.26 | 31.78 | 1.310× | 26.00 → 20.00 |
| 7 bytes (control) | 24.06 | 23.89 | 0.993× | 27.00 → 27.00 |
| 32 bytes (control) | 21.15 | 20.94 | 0.987× | 52.00 → 52.00 |
| 50% 6-byte + 50% 32-byte | 22.27 | 21.44 | 0.960× | 39.00 → 36.00 |

</details>

For six-byte values, inlining improves throughput by **31.0%** and reduces logical
storage by **23.1%**, keeping every node at 16 bytes. Scalar improves by 23.6% and
AMAC/32 by 28.1% in the same case. The gain depends on value access and working-set
size: the cache-resident six-byte case improves SIMD by 2.2%, and a 30%-hit case by
9.7%. The full report retains every length from zero through six and all controls.

The half-small/half-large case reduces storage by 7.7% but lowers SIMD throughput
by 4.0% (Scalar by 6.9%). Mixed layouts add a representation decision to successful
lookups; this tradeoff should be measured for the application's value distribution.
The external-only build option provides a direct alternative:

```cpp
kvquery::BuildOptions options;
options.inline_small_values = false;
auto snapshot = kvquery::Snapshot::build(records, options);
```

- [Paired raw results, hashes and all windows](../bench/results/duke-inline-2026-10-24.json)
- [All 16 cases, process ranges and storage counts](../bench/results/duke-inline-2026-10-24.md)
- [Inline layout, lifetime and implementation details](../docs/INLINE_EVALUATION.md)

```sh
bash bench/run_linux.sh bench/results/linux-inline 0 inline
```

### Larger working sets

The scaling test holds value size at six bytes, batch size at 256, target hit rate
at 90%, and **1,048,576 queries per round** across all three sizes. It uses the same
server, binary, CPU affinity and paired measurement protocol. Each case runs in
three independent processes with three rounds. Throughput is in Mkeys/s; gains
are matched per-process ratio medians.


![SIMD throughput and logical storage at one, four and eight million records](../docs/figures/inline-scaling.svg)

*The same server binary measures both layouts. Logical storage counts index and external-value bytes.*

<details>
<summary>Exact scaling results</summary>

| Records | SIMD/32 external | SIMD/32 inline | SIMD gain | AMAC/32 gain | Logical storage MB, before → after |
|---|---:|---:|---:|---:|---:|
| 1,000,000 | 25.07 | 31.94 | +27.4% | +27.0% | 26 → 20 |
| 4,000,000 | 20.08 | 28.78 | +41.8% | +43.3% | 104 → 80 |
| 8,000,000 | 18.17 | 26.57 | +46.0% | +48.2% | 208 → 160 |

</details>

The relative SIMD gain rises from 27.4% at one million records to 46.0% at eight
million; AMAC/32 reaches 48.2%. Inlined logical storage at eight million records is
160 MB versus 208 MB externally. Absolute throughput falls as the dataset grows,
while inlining reduces the loss relative to external-only storage. The index
arrays span roughly 20, 80 and 160 MB on a server with a 20 MiB L3 cache. This
trend is consistent with avoiding external-value accesses becoming more valuable
as the working set expands. These are single-threaded working-set measurements.

- [Scaling raw results and source/binary hashes](../bench/results/duke-inline-scale-2026-10-24.json)
- [Scaling process ranges and full strategy results](../bench/results/duke-inline-scale-2026-10-24.md)

```sh
bash bench/run_linux.sh bench/results/linux-inline-scale 0 inline-scale
```

