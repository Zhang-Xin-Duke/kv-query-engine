# Small-value inlining: Windows/Cygwin desktop comparison

CPU: Intel Core Ultra 9 285K. Build: Windows 11 Cygwin x86_64; GCC 14.4.0; CMake Release -O3 -DNDEBUG -std=gnu++20 -msse4.1 -mtune=native; no -march=native.

3 independent processes, three rounds each. Both layouts are
resident in the same process and receive identical records and query traces.
Layout order alternates across rounds. Throughput includes result allocation,
lookup, length/end-byte consumption, ownership, and destruction. Full-byte
equality is checked before timing. Values are returned as views in both layouts.

Rates below are process medians in Mkeys/s; ratios are medians of paired
per-process inline/external ratios. Ranges show all three process ratios.

| Case | Scalar ext. | Scalar inline | Ratio | AMAC/32 ratio | SIMD/32 ext. | SIMD/32 inline | Ratio [min–max] | Logical bytes ext. → inline |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| scale-1m-six | 42.24 | 51.17 | 1.229× | 1.349× | 70.60 | 89.81 | 1.294× [1.267–1.317] | 26,000,001 → 20,000,001 |
| scale-4m-six | 28.56 | 37.44 | 1.305× | 1.601× | 51.19 | 82.61 | 1.614× [1.593–1.624] | 104,000,001 → 80,000,001 |
| scale-8m-six | 27.34 | 34.33 | 1.257× | 1.623× | 48.30 | 80.21 | 1.661× [1.649–1.661] | 208,000,001 → 160,000,001 |

Logical bytes count the successful node array and external value region;
they exclude allocator metadata, input records, map baseline, result arrays,
and temporary construction storage. Inlined payload occupies existing node
bytes. The raw JSON includes every window, batch P50/P95/P99, input seed,
inline counts, source hashes, binary hash, and CPU affinity.
