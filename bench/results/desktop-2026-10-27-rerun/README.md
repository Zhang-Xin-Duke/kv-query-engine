# Desktop benchmarks and concurrency pressure test

Measured October 27, 2026 on an Intel Core Ultra 9 285K (8 performance cores + 16 efficiency cores), approximately 64 GB RAM and 36 MiB L3. This is a CPU query engine; the GPU is unused.

Windows 11/Cygwin, GCC 14.4.0, CMake Release: `-O3 -DNDEBUG -std=gnu++20 -msse4.1 -mtune=native`. Native comparison backend: `sse4.1`. Default builds retain SSE2 or the portable/NEON paths. No `-march=native` is used; that broader flag failed earlier local correctness checks.

All 12 CTest groups passed in SSE4.1, SSE2, and forced-portable builds (36 group passes). See the three test logs.

## Single-thread comparison with Duke

Throughput is million keys/s. Windows use 32; both suites run three independent processes per workload, with three rounds per process. Desktop runs are pinned to performance-core logical CPU 10. The engine code is optimized relative to Duke; OS, compiler, CPU, RAM and ISA also differ. Ratios therefore describe complete tested configurations, not isolated hardware gains.

| Workload | Duke Scalar | Desktop Scalar | Duke AMAC | Desktop AMAC | Duke SIMD | Desktop SIMD |
|---|---:|---:|---:|---:|---:|---:|
| small-uniform | 32.97 | 100.18 | 33.50 | 102.48 | 32.85 | 104.42 |
| large-single | 2.77 | 3.06 | 2.96 | 3.17 | 2.89 | 3.13 |
| large-batch32 | 14.26 | 26.46 | 15.68 | 33.67 | 15.75 | 33.41 |
| large-batch256 | 15.33 | 31.99 | 21.49 | 54.03 | 21.15 | 53.77 |
| large-batch1024 | 15.30 | 31.29 | 21.98 | 55.74 | 22.13 | 55.36 |
| large-low-hit | 19.62 | 39.15 | 28.22 | 64.01 | 28.03 | 71.30 |
| large-hot | 31.77 | 63.40 | 39.19 | 119.02 | 38.54 | 99.42 |
| wide-values | 17.18 | 36.14 | 20.76 | 47.86 | 20.64 | 48.26 |

Full single-thread tables: [standard](standard.md), [paired inline](inline.md), [1M/4M/8M scaling](inline-scale.md). The matching JSON files retain all windows, checksums, batch P50/P95/P99, seeds and source/binary hashes.

## Concurrent pressure and saturation

Every pressure case uses **16 million records**, 90% target hits, uniform independent per-worker traces of 1,048,576 keys, and one shared immutable snapshot. Logical storage is about 832 MB for 32-byte external values and 320 MB for six-byte inlined values; allocator overhead, input records and traces are additional memory. Build budgets are explicitly raised for the 16M-record case.

The sweep uses 1/2/4/8/16/24/32 workers. CPU order places the eight performance cores first; adding 16/24 workers includes efficiency cores. At 32 workers, the performance cores each have two pinned workers. Every worker uses window 32. These are heterogeneous-core placement results, not identical-core scaling.

Each configuration runs three **2-second timed rounds in the same process** with rotated strategy order, worker warmups and a synchronized start. Pressure cases do not use three independent processes. Latency is sampled once per 64 calls and includes result allocation/destruction and endpoint-byte consumption. P99 below is the median of the three round P99s, each pooling all workers.

Batch requests/s = keys/s divided by batch size. This is closed-loop, in-process capacity, with no transport, request queue, serialization or full-value copy. Call percentiles exclude queueing delay and do not establish an end-to-end service SLA. The amortized timer differs from the earlier single-thread benchmark, so use comparisons within each harness.

### external-batch256

Units: million keys/s, except the request-rate and latency columns.

| Workers | Scalar | AMAC | SIMD | SIMD batch requests/s | SIMD P99 µs |
|---:|---:|---:|---:|---:|---:|
| 1 | 26.20 | 45.12 | 44.95 | 175,581 | 6.90 |
| 2 | 49.66 | 85.37 | 83.99 | 328,082 | 7.60 |
| 4 | 96.08 | 162.46 | 157.74 | 616,181 | 8.40 |
| 8 | 158.80 | 251.75 | 253.56 | 990,479 | 11.80 |
| 16 | 260.49 | 276.84 | 266.62 | 1,041,471 | 27.60 |
| 24 | 263.61 | 298.34 | 296.70 | 1,158,992 | 54.30 |
| 32 | 263.04 | 288.12 | 282.18 | 1,102,252 | 230.10 |

Highest median tested: **298.34 Mkeys/s**, 1,165,377 batch requests/s, amac/24 workers; batch P99 51.20 µs.

### inline-batch256

Units: million keys/s, except the request-rate and latency columns.

| Workers | Scalar | AMAC | SIMD | SIMD batch requests/s | SIMD P99 µs |
|---:|---:|---:|---:|---:|---:|
| 1 | 33.57 | 81.14 | 80.95 | 316,200 | 3.60 |
| 2 | 67.32 | 150.68 | 149.63 | 584,501 | 4.50 |
| 4 | 131.12 | 285.81 | 280.04 | 1,093,902 | 5.50 |
| 8 | 216.59 | 404.94 | 399.48 | 1,560,488 | 10.10 |
| 16 | 369.45 | 355.04 | 355.42 | 1,388,366 | 74.40 |
| 24 | 314.72 | 275.42 | 272.38 | 1,063,971 | 158.20 |
| 32 | 309.61 | 273.09 | 271.32 | 1,059,849 | 235.90 |

Highest median tested: **404.94 Mkeys/s**, 1,581,787 batch requests/s, amac/8 workers; batch P99 10.50 µs.

### external-batch1

| Workers | Strategy | Mkeys/s | Batch requests/s | P50 µs | P95 µs | P99 µs |
|---:|---|---:|---:|---:|---:|---:|
| 1 | amac | 0.07 | 70,581 | 0.50 | 0.80 | 0.90 |
| 1 | scalar | 0.62 | 624,999 | 0.40 | 0.60 | 0.70 |
| 1 | simd | 0.04 | 40,957 | 0.50 | 0.70 | 0.90 |
| 8 | amac | 1.61 | 1,614,436 | 1.80 | 11.30 | 58.90 |
| 8 | scalar | 1.61 | 1,613,554 | 1.80 | 11.20 | 57.90 |
| 8 | simd | 1.62 | 1,623,689 | 1.80 | 11.50 | 56.70 |
| 24 | amac | 0.98 | 984,604 | 7.30 | 68.30 | 182.00 |
| 24 | scalar | 0.87 | 868,921 | 7.20 | 70.50 | 209.40 |
| 24 | simd | 1.00 | 1,004,198 | 5.90 | 58.70 | 164.90 |

### external-batch1024

| Workers | Strategy | Mkeys/s | Batch requests/s | P50 µs | P95 µs | P99 µs |
|---:|---|---:|---:|---:|---:|---:|
| 1 | amac | 49.40 | 48,242 | 20.70 | 21.50 | 24.80 |
| 1 | scalar | 25.82 | 25,214 | 38.40 | 41.50 | 45.70 |
| 1 | simd | 48.94 | 47,789 | 20.90 | 21.70 | 25.00 |
| 8 | amac | 286.99 | 280,260 | 24.50 | 26.90 | 78.30 |
| 8 | scalar | 168.00 | 164,062 | 43.00 | 46.80 | 254.30 |
| 8 | simd | 286.88 | 280,157 | 24.90 | 27.30 | 234.00 |
| 24 | amac | 366.37 | 357,780 | 77.80 | 89.40 | 221.60 |
| 24 | scalar | 276.17 | 269,701 | 83.10 | 92.80 | 247.70 |
| 24 | simd | 365.94 | 357,361 | 77.20 | 88.00 | 246.40 |

## Longer pressure runs

Three **20-second timed rounds** per selected configuration follow the sweeps. They are separate processes from the short sweeps. These longer runs provide a limited stability check, not an hours-long thermal or availability test.

| Case | Strategy / workers | Mkeys/s min / median / max | Batch requests/s median | Batch P99 µs median |
|---|---|---:|---:|---:|
| external-soak | simd / 24 | 286.77 / 287.26 / 291.62 | 1,122,121 | 59.60 |
| inline-soak | amac / 8 | 397.95 / 398.61 / 401.37 | 1,557,089 | 10.80 |

## Single-key variability and confirmation

The short sweep showed large single-worker variation. All rounds are retained, including slow rounds. A follow-up uses three 5-second rounds per strategy with one worker and the same 16M-record, 32-byte, batch-1 workload. This is a separate process after the soaks; the data below supplements the original sweep.

| Case | Strategy | Mkeys/s min / median / max | Sampled P99 µs median |
|---|---|---:|---:|
| external-batch1 | amac | 0.052 / 0.071 / 1.826 | 0.90 |
| external-batch1 | scalar | 0.088 / 0.625 / 2.779 | 0.70 |
| external-batch1 | simd | 0.036 / 0.041 / 0.911 | 0.90 |
| single-key-confirmation | amac | 3.171 / 3.197 / 3.228 | 0.60 |
| single-key-confirmation | scalar | 3.142 / 3.183 / 3.230 | 0.60 |
| single-key-confirmation | simd | 3.092 / 3.179 / 3.194 | 0.60 |

A low sampled P99 can coexist with poor wall-time throughput: one-in-64 sampling and the 99th percentile can miss rare long pauses or scheduling stalls. Their cause was not isolated. This shared desktop was not controlled as an exclusive benchmark host. Single-key capacity and tail latency need a dedicated, longer investigation.

## Interpretation and limits

- Large batched requests scale to high aggregate throughput, but worker count and value layout need tuning. Adding workers beyond the measured peak can reduce throughput and increase tail latency.
- Single-key calls expose the high-concurrency limitation. Per-call vector allocation and shared snapshot ownership/reference counting are plausible contention costs; no hardware counters or ownership/allocation ablations were collected to attribute the bottleneck.
- A reusable query context/output buffer that retains ownership across calls is a useful next experiment. It must preserve the API lifetime guarantees; no improvement from such a change is claimed here.
- The fixed-size, uniform synthetic workload, one host and short sweeps limit generalization. Longer soak, open-loop offered-load tests, real value distributions and a network service are needed for deployment capacity/SLA decisions.
- Every timed batch checksum was verified; initial full-byte checks and warmups run outside timing. P50 ≤ P95 ≤ P99 and request-rate units were also validated. See [pressure-summary.csv](pressure-summary.csv) for ranges and all percentile summaries.

## Provenance and reproduction

Measured local source commit: `6cb0c02cdd76d3685848758cf52e01b94c4db622`. The equivalent published source commit is [`b745ea3`](https://github.com/Zhang-Xin-Duke/kv-query-engine/commit/b745ea3ea25fa01a99924631dc576266305205a8); both have Git tree `5ee41d780632b19fad3defb0de0b1781b3659640`. The local and connector-created commits have different author/committer metadata. Source content is identical. Working-tree hashes preserve the measured local line endings; `source_sha256_lf` additionally normalizes CRLF to LF for comparison with GitHub blobs. Source/binary SHA-256 hashes are in [manifest.json](manifest.json). Absolute workspace prefixes in published commands/logs are normalized to repository-relative paths; numeric results and hashes are unchanged.

```sh
cmake -S . -B build/tuned -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS="-msse4.1 -mtune=native"
cmake --build build/tuned -j8
ctest --test-dir build/tuned --output-on-failure
python3 bench/run_desktop.py --binary-dir build/tuned \
  --output bench/results/my-desktop-run \
  --cpus 10,11,12,13,22,23,0,1,2,3,4,5,6,7,8,9,14,15,16,17,18,19,20,21 \
  --cpu-name "Intel Core Ultra 9 285K" \
  --build-description "Windows/Cygwin GCC 14.4.0 Release -O3 -DNDEBUG -std=gnu++20 -msse4.1 -mtune=native"
python3 bench/summarize_desktop.py --input bench/results/my-desktop-run
```

Choose CPU IDs allowed on your host and verify topology before reusing the order above. The Cygwin affinity code targets the current Windows processor group. Linux uses its allowed CPU mask. Explicit SSE4.1 builds require compatible hardware. The supplied soak defaults select 24-worker SIMD and 8-worker AMAC; change the runner cases for other host topologies.
