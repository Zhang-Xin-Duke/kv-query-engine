# Small-value inlining: server comparison

CPU: Intel(R) Xeon(R) CPU E5-2440 v2 @ 1.90GHz. Build: Direct g++ -std=c++20 -O3 -DNDEBUG; no -march=native.

3 independent processes, three rounds each. Both layouts are
resident in the same process and receive identical records and query traces.
Layout order alternates across rounds. Throughput includes result allocation,
lookup, length/end-byte consumption, ownership, and destruction. Full-byte
equality is checked before timing. Values are returned as views in both layouts.

Rates below are process medians in Mkeys/s; ratios are medians of paired
per-process inline/external ratios. Ranges show all three process ratios.

| Case | Scalar ext. | Scalar inline | Ratio | AMAC/32 ratio | SIMD/32 ext. | SIMD/32 inline | Ratio [min–max] | Logical bytes ext. → inline |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| scale-1m-six | 18.30 | 23.89 | 1.304× | 1.270× | 25.07 | 31.94 | 1.274× [1.261–1.275] | 26,000,001 → 20,000,001 |
| scale-4m-six | 13.90 | 18.00 | 1.298× | 1.433× | 20.08 | 28.78 | 1.418× [1.410–1.444] | 104,000,001 → 80,000,001 |
| scale-8m-six | 12.83 | 16.93 | 1.323× | 1.482× | 18.17 | 26.57 | 1.460× [1.458–1.475] | 208,000,001 → 160,000,001 |

Logical bytes count the successful node array and external value region;
they exclude allocator metadata, input records, map baseline, result arrays,
and temporary construction storage. Inlined payload occupies existing node
bytes. The raw JSON includes every window, batch P50/P95/P99, input seed,
inline counts, source hashes, binary hash, and CPU affinity.
