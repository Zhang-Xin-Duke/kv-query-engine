# Benchmark report

Recorded: 2026-10-24T18:31:37.775315+00:00.
Host: Intel(R) Xeon(R) CPU E5-2440 v2 @ 1.90GHz, Linux 6.8.0-138-generic, x86_64.
Compiler: `11.4.0`.
Build: Direct g++ -std=c++20 -O3 -DNDEBUG; no -march=native.
Raw input: `bench/results/duke-x86-inline-2026-10-24.json`.

## Median throughput

Units: million keys per second. Each row aggregates 3 independent processes.
Prefetch, AMAC and SIMD (when available) use the same fixed window 32 in this table.

| Workload | Records | Batch | Value bytes | Hit target | Map | Scalar | Prefetch/32 | AMAC/32 | SIMD/32 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| small-uniform | 10,000 | 256 | 32 | 90% | 17.78 | 32.97 | 31.40 | 33.50 | 32.85 |
| large-single | 1,000,000 | 1 | 32 | 90% | 1.59 | 2.77 | 2.96 | 2.96 | 2.89 |
| large-batch32 | 1,000,000 | 32 | 32 | 90% | 6.13 | 14.26 | 15.52 | 15.68 | 15.75 |
| large-batch256 | 1,000,000 | 256 | 32 | 90% | 6.23 | 15.33 | 18.96 | 21.49 | 21.15 |
| large-batch1024 | 1,000,000 | 1024 | 32 | 90% | 5.86 | 15.30 | 19.30 | 21.98 | 22.13 |
| large-low-hit | 1,000,000 | 256 | 32 | 30% | 5.25 | 19.62 | 24.64 | 28.22 | 28.03 |
| large-hot | 1,000,000 | 256 | 32 | 90% | 9.25 | 31.77 | 36.98 | 39.19 | 38.54 |
| wide-values | 250,000 | 256 | 256 | 90% | 7.24 | 17.18 | 19.25 | 20.76 | 20.64 |

## Process variation

| Workload | Strategy | Throughput min / median / max (Mkeys/s) | Batch P99 min / median / max (µs) |
| --- | --- | ---: | ---: |
| small-uniform | unordered_map/1 | 17.36 / 17.78 / 18.23 | 26.62 / 28.10 / 28.51 |
| small-uniform | scalar/1 | 32.61 / 32.97 / 34.54 | 8.48 / 14.22 / 20.68 |
| small-uniform | prefetch/32 | 30.73 / 31.40 / 32.61 | 9.03 / 13.83 / 14.01 |
| small-uniform | amac/32 | 32.55 / 33.50 / 34.50 | 11.82 / 14.34 / 20.20 |
| small-uniform | simd/32 | 31.78 / 32.85 / 33.55 | 14.80 / 20.91 / 20.95 |
| large-single | unordered_map/1 | 1.54 / 1.59 / 1.63 | 0.91 / 0.92 / 0.94 |
| large-single | scalar/1 | 2.58 / 2.77 / 2.79 | 0.63 / 0.63 / 0.65 |
| large-single | prefetch/32 | 2.69 / 2.96 / 2.99 | 0.59 / 0.59 / 0.62 |
| large-single | amac/32 | 2.71 / 2.96 / 2.97 | 0.59 / 0.59 / 0.62 |
| large-single | simd/32 | 2.55 / 2.89 / 2.92 | 0.60 / 0.61 / 0.65 |
| large-batch32 | unordered_map/1 | 6.09 / 6.13 / 6.17 | 7.66 / 7.79 / 8.57 |
| large-batch32 | scalar/1 | 14.21 / 14.26 / 14.46 | 2.92 / 2.97 / 2.98 |
| large-batch32 | prefetch/32 | 15.32 / 15.52 / 15.63 | 2.62 / 2.67 / 2.68 |
| large-batch32 | amac/32 | 15.54 / 15.68 / 15.84 | 2.55 / 2.58 / 3.44 |
| large-batch32 | simd/32 | 15.74 / 15.75 / 15.78 | 2.52 / 2.52 / 2.60 |
| large-batch256 | unordered_map/1 | 6.21 / 6.23 / 6.26 | 56.79 / 64.26 / 65.71 |
| large-batch256 | scalar/1 | 15.31 / 15.33 / 15.33 | 23.42 / 23.52 / 28.02 |
| large-batch256 | prefetch/32 | 18.93 / 18.96 / 18.96 | 18.96 / 19.28 / 19.33 |
| large-batch256 | amac/32 | 21.31 / 21.49 / 21.67 | 17.23 / 17.51 / 19.21 |
| large-batch256 | simd/32 | 21.02 / 21.15 / 21.23 | 19.45 / 24.24 / 25.20 |
| large-batch1024 | unordered_map/1 | 5.82 / 5.86 / 5.88 | 213.88 / 214.46 / 214.91 |
| large-batch1024 | scalar/1 | 15.29 / 15.30 / 15.34 | 84.26 / 86.92 / 89.46 |
| large-batch1024 | prefetch/32 | 18.66 / 19.30 / 19.34 | 68.24 / 69.28 / 91.92 |
| large-batch1024 | amac/32 | 21.90 / 21.98 / 22.17 | 63.06 / 65.62 / 66.31 |
| large-batch1024 | simd/32 | 22.12 / 22.13 / 22.18 | 62.17 / 62.60 / 65.10 |
| large-low-hit | unordered_map/1 | 5.22 / 5.25 / 5.26 | 63.56 / 65.25 / 66.87 |
| large-low-hit | scalar/1 | 19.30 / 19.62 / 19.73 | 19.78 / 21.59 / 24.20 |
| large-low-hit | prefetch/32 | 24.37 / 24.64 / 24.77 | 15.37 / 15.41 / 23.15 |
| large-low-hit | amac/32 | 28.16 / 28.22 / 28.51 | 14.09 / 14.33 / 15.14 |
| large-low-hit | simd/32 | 27.82 / 28.03 / 28.48 | 13.88 / 14.89 / 21.84 |
| large-hot | unordered_map/1 | 9.25 / 9.25 / 9.32 | 35.26 / 37.26 / 40.67 |
| large-hot | scalar/1 | 31.59 / 31.77 / 32.13 | 11.76 / 13.50 / 13.71 |
| large-hot | prefetch/32 | 36.94 / 36.98 / 37.14 | 11.71 / 11.89 / 12.39 |
| large-hot | amac/32 | 38.70 / 39.19 / 39.53 | 7.46 / 10.85 / 11.86 |
| large-hot | simd/32 | 38.24 / 38.54 / 38.77 | 7.52 / 7.63 / 8.28 |
| wide-values | unordered_map/1 | 7.17 / 7.24 / 7.24 | 49.46 / 50.50 / 53.75 |
| wide-values | scalar/1 | 17.09 / 17.18 / 17.27 | 21.80 / 23.62 / 26.98 |
| wide-values | prefetch/32 | 19.23 / 19.25 / 19.38 | 19.00 / 19.40 / 20.41 |
| wide-values | amac/32 | 20.60 / 20.76 / 20.87 | 18.00 / 18.20 / 23.45 |
| wide-values | simd/32 | 20.50 / 20.64 / 20.66 | 18.61 / 18.77 / 19.04 |

Percentiles describe complete batch calls. The raw JSON retains all windows, commands, checksums and source/binary hashes.

Compiled SIMD backend: sse2.
