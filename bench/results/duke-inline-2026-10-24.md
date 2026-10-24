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
| value-0 | 21.32 | 22.19 | 1.040× | 1.005× | 32.64 | 33.14 | 1.024× [0.996–1.025] | 20,000,001 → 20,000,001 |
| value-1 | 19.47 | 21.26 | 1.092× | 1.071× | 29.34 | 31.82 | 1.083× [1.080–1.098] | 21,000,001 → 20,000,001 |
| value-2 | 18.62 | 21.22 | 1.147× | 1.143× | 27.84 | 32.21 | 1.157× [1.152–1.160] | 22,000,001 → 20,000,001 |
| value-3 | 17.79 | 21.13 | 1.196× | 1.210× | 26.27 | 31.91 | 1.215× [1.214–1.215] | 23,000,001 → 20,000,001 |
| value-4 | 17.41 | 20.95 | 1.204× | 1.249× | 25.47 | 31.76 | 1.244× [1.243–1.269] | 24,000,001 → 20,000,001 |
| value-5 | 17.10 | 20.96 | 1.227× | 1.292× | 24.77 | 31.90 | 1.288× [1.261–1.295] | 25,000,001 → 20,000,001 |
| value-6 | 16.99 | 20.99 | 1.236× | 1.281× | 24.26 | 31.78 | 1.310× [1.297–1.338] | 26,000,001 → 20,000,001 |
| value-7 | 16.74 | 16.62 | 0.990× | 0.993× | 24.06 | 23.89 | 0.993× [0.984–0.993] | 27,000,001 → 27,000,001 |
| value-8 | 16.41 | 16.44 | 1.006× | 1.001× | 23.38 | 23.73 | 1.012× [1.009–1.019] | 28,000,001 → 28,000,001 |
| value-32 | 15.07 | 15.06 | 0.999× | 0.998× | 21.15 | 20.94 | 0.987× [0.987–1.007] | 52,000,001 → 52,000,001 |
| wide-control | 16.91 | 17.39 | 1.025× | 1.000× | 20.65 | 20.51 | 0.993× [0.992–0.993] | 69,000,001 → 69,000,001 |
| mixed-6-32 | 15.79 | 14.72 | 0.931× | 0.933× | 22.27 | 21.44 | 0.960× [0.958–0.963] | 39,000,001 → 36,000,001 |
| small-six | 36.93 | 39.16 | 1.062× | 1.026× | 35.50 | 36.28 | 1.022× [1.007–1.044] | 260,001 → 200,001 |
| single-six | 3.85 | 5.49 | 1.425× | 1.255× | 4.48 | 5.60 | 1.250× [1.225–1.267] | 26,000,001 → 20,000,001 |
| low-hit-six | 19.77 | 21.35 | 1.080× | 1.094× | 29.24 | 31.90 | 1.097× [1.044–1.147] | 26,000,001 → 20,000,001 |
| hot-six | 34.84 | 40.83 | 1.170× | 1.101× | 41.12 | 44.75 | 1.079× [1.078–1.108] | 26,000,001 → 20,000,001 |

Logical bytes count the successful node array and external value region;
they exclude allocator metadata, input records, map baseline, result arrays,
and temporary construction storage. Inlined payload occupies existing node
bytes. The raw JSON includes every window, batch P50/P95/P99, input seed,
inline counts, source hashes, binary hash, and CPU affinity.
