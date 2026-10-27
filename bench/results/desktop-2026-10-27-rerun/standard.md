# Benchmark report

Recorded: 2026-10-27T14:48:04.207104+00:00.
Host: Intel Core Ultra 9 285K, CYGWIN_NT-10.0-26200 3.6.12-1.x86_64, x86_64.
Compiler: `14.4.0`.
Build: Windows 11 Cygwin x86_64; GCC 14.4.0; CMake Release -O3 -DNDEBUG -std=gnu++20 -msse4.1 -mtune=native; no -march=native.
Raw input: `./bench/results/desktop-2026-10-27-rerun/standard.json`.

## Median throughput

Units: million keys per second. Each row aggregates 3 independent processes.
Prefetch, AMAC and SIMD (when available) use the same fixed window 32 in this table.

| Workload | Records | Batch | Value bytes | Hit target | Map | Scalar | Prefetch/32 | AMAC/32 | SIMD/32 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| small-uniform | 10,000 | 256 | 32 | 90% | 73.12 | 100.18 | 94.36 | 102.48 | 104.42 |
| large-single | 1,000,000 | 1 | 32 | 90% | 1.89 | 3.06 | 3.13 | 3.17 | 3.13 |
| large-batch32 | 1,000,000 | 32 | 32 | 90% | 13.66 | 26.46 | 33.72 | 33.67 | 33.41 |
| large-batch256 | 1,000,000 | 256 | 32 | 90% | 16.26 | 31.99 | 47.08 | 54.03 | 53.77 |
| large-batch1024 | 1,000,000 | 1024 | 32 | 90% | 16.17 | 31.29 | 47.15 | 55.74 | 55.36 |
| large-low-hit | 1,000,000 | 256 | 32 | 30% | 15.36 | 39.15 | 57.01 | 64.01 | 71.30 |
| large-hot | 1,000,000 | 256 | 32 | 90% | 22.22 | 63.40 | 104.73 | 119.02 | 99.42 |
| wide-values | 250,000 | 256 | 256 | 90% | 18.13 | 36.14 | 44.96 | 47.86 | 48.26 |

## Process variation

| Workload | Strategy | Throughput min / median / max (Mkeys/s) | Batch P99 min / median / max (µs) |
| --- | --- | ---: | ---: |
| small-uniform | unordered_map/1 | 72.70 / 73.12 / 73.95 | 8.10 / 8.30 / 8.80 |
| small-uniform | scalar/1 | 99.44 / 100.18 / 100.38 | 3.00 / 3.30 / 3.50 |
| small-uniform | prefetch/32 | 94.00 / 94.36 / 94.44 | 3.00 / 3.10 / 3.60 |
| small-uniform | amac/32 | 102.45 / 102.48 / 102.82 | 2.80 / 2.90 / 3.20 |
| small-uniform | simd/32 | 103.55 / 104.42 / 104.43 | 2.80 / 2.80 / 2.80 |
| large-single | unordered_map/1 | 1.80 / 1.89 / 1.95 | 0.90 / 1.00 / 1.10 |
| large-single | scalar/1 | 2.97 / 3.06 / 3.12 | 0.50 / 0.60 / 0.60 |
| large-single | prefetch/32 | 3.12 / 3.13 / 3.27 | 0.50 / 0.50 / 0.50 |
| large-single | amac/32 | 3.14 / 3.17 / 3.37 | 0.50 / 0.50 / 0.50 |
| large-single | simd/32 | 3.09 / 3.13 / 3.24 | 0.50 / 0.50 / 0.50 |
| large-batch32 | unordered_map/1 | 13.14 / 13.66 / 13.98 | 3.40 / 3.40 / 3.90 |
| large-batch32 | scalar/1 | 26.30 / 26.46 / 28.02 | 1.60 / 1.70 / 1.70 |
| large-batch32 | prefetch/32 | 33.70 / 33.72 / 34.68 | 1.30 / 1.40 / 1.70 |
| large-batch32 | amac/32 | 32.32 / 33.67 / 33.84 | 1.20 / 1.30 / 1.30 |
| large-batch32 | simd/32 | 33.40 / 33.41 / 33.82 | 1.30 / 1.30 / 1.30 |
| large-batch256 | unordered_map/1 | 16.19 / 16.26 / 16.50 | 20.50 / 20.60 / 20.70 |
| large-batch256 | scalar/1 | 31.59 / 31.99 / 32.85 | 10.40 / 10.80 / 10.80 |
| large-batch256 | prefetch/32 | 46.78 / 47.08 / 47.92 | 6.10 / 6.30 / 6.40 |
| large-batch256 | amac/32 | 52.23 / 54.03 / 54.33 | 5.30 / 5.30 / 5.60 |
| large-batch256 | simd/32 | 53.00 / 53.77 / 54.08 | 5.30 / 5.40 / 5.40 |
| large-batch1024 | unordered_map/1 | 15.90 / 16.17 / 16.42 | 78.20 / 79.30 / 87.70 |
| large-batch1024 | scalar/1 | 31.14 / 31.29 / 32.83 | 40.60 / 41.60 / 43.10 |
| large-batch1024 | prefetch/32 | 45.62 / 47.15 / 48.84 | 25.90 / 26.60 / 27.30 |
| large-batch1024 | amac/32 | 54.53 / 55.74 / 57.31 | 23.60 / 23.80 / 23.90 |
| large-batch1024 | simd/32 | 54.48 / 55.36 / 55.75 | 23.30 / 24.00 / 24.30 |
| large-low-hit | unordered_map/1 | 15.26 / 15.36 / 15.45 | 22.60 / 22.70 / 23.10 |
| large-low-hit | scalar/1 | 38.83 / 39.15 / 39.98 | 8.70 / 8.90 / 8.90 |
| large-low-hit | prefetch/32 | 56.05 / 57.01 / 57.33 | 5.20 / 5.40 / 5.40 |
| large-low-hit | amac/32 | 62.65 / 64.01 / 65.58 | 4.50 / 4.50 / 4.70 |
| large-low-hit | simd/32 | 65.73 / 71.30 / 71.89 | 4.10 / 4.20 / 6.80 |
| large-hot | unordered_map/1 | 18.20 / 22.22 / 22.39 | 20.50 / 20.60 / 31.50 |
| large-hot | scalar/1 | 51.18 / 63.40 / 65.33 | 7.50 / 7.90 / 11.90 |
| large-hot | prefetch/32 | 82.20 / 104.73 / 119.28 | 2.90 / 3.60 / 6.50 |
| large-hot | amac/32 | 93.92 / 119.02 / 135.10 | 2.30 / 2.70 / 6.70 |
| large-hot | simd/32 | 95.17 / 99.42 / 119.35 | 3.00 / 3.30 / 6.50 |
| wide-values | unordered_map/1 | 16.57 / 18.13 / 19.04 | 20.90 / 21.20 / 22.90 |
| wide-values | scalar/1 | 35.50 / 36.14 / 36.75 | 10.60 / 11.60 / 12.40 |
| wide-values | prefetch/32 | 44.75 / 44.96 / 45.11 | 6.60 / 6.60 / 6.70 |
| wide-values | amac/32 | 47.19 / 47.86 / 48.86 | 6.40 / 6.90 / 7.30 |
| wide-values | simd/32 | 47.17 / 48.26 / 48.94 | 6.30 / 6.50 / 7.60 |

Percentiles describe complete batch calls. The raw JSON retains all windows, commands, checksums and source/binary hashes.

Compiled SIMD backend: sse4.1.
