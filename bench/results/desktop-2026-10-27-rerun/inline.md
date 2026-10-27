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
| value-0 | 41.62 | 42.11 | 1.012× | 1.006× | 87.51 | 88.11 | 1.002× [0.981–1.007] | 20,000,001 → 20,000,001 |
| value-1 | 40.41 | 42.32 | 1.047× | 1.084× | 80.09 | 88.03 | 1.099× [1.095–1.117] | 21,000,001 → 20,000,001 |
| value-2 | 38.55 | 41.15 | 1.036× | 1.138× | 70.20 | 85.93 | 1.223× [1.201–1.224] | 22,000,001 → 20,000,001 |
| value-3 | 33.10 | 40.31 | 1.127× | 1.677× | 48.71 | 73.06 | 1.500× [1.284–3.842] | 23,000,001 → 20,000,001 |
| value-4 | 34.60 | 41.84 | 1.209× | 1.378× | 63.60 | 85.82 | 1.349× [1.343–1.351] | 24,000,001 → 20,000,001 |
| value-5 | 36.52 | 43.18 | 1.205× | 1.404× | 59.86 | 85.86 | 1.434× [1.365–1.457] | 25,000,001 → 20,000,001 |
| value-6 | 35.72 | 44.04 | 1.235× | 1.350× | 61.56 | 87.13 | 1.415× [1.405–1.446] | 26,000,001 → 20,000,001 |
| value-7 | 35.68 | 36.60 | 1.014× | 0.999× | 58.94 | 59.33 | 1.009× [0.979–1.073] | 27,000,001 → 27,000,001 |
| value-8 | 34.46 | 34.49 | 1.003× | 1.010× | 58.09 | 57.68 | 0.994× [0.980–1.011] | 28,000,001 → 28,000,001 |
| value-32 | 30.58 | 31.76 | 1.039× | 0.998× | 49.55 | 49.02 | 0.990× [0.989–0.992] | 52,000,001 → 52,000,001 |
| wide-control | 36.87 | 36.63 | 0.994× | 1.002× | 47.19 | 47.89 | 1.016× [0.985–1.027] | 69,000,001 → 69,000,001 |
| mixed-6-32 | 31.51 | 29.37 | 0.923× | 0.880× | 50.73 | 46.69 | 0.952× [0.890–0.974] | 39,000,001 → 36,000,001 |
| small-six | 103.62 | 104.22 | 1.006× | 0.986× | 104.96 | 106.88 | 1.018× [1.011–1.022] | 260,001 → 200,001 |
| single-six | 3.33 | 4.65 | 1.402× | 1.420× | 3.41 | 4.79 | 1.392× [1.376–1.403] | 26,000,001 → 20,000,001 |
| low-hit-six | 38.82 | 42.68 | 1.099× | 1.185× | 70.00 | 83.10 | 1.180× [1.104–1.189] | 26,000,001 → 20,000,001 |
| hot-six | 56.32 | 73.65 | 1.308× | 1.401× | 95.67 | 137.99 | 1.429× [1.275–1.442] | 26,000,001 → 20,000,001 |

Logical bytes count the successful node array and external value region;
they exclude allocator metadata, input records, map baseline, result arrays,
and temporary construction storage. Inlined payload occupies existing node
bytes. The raw JSON includes every window, batch P50/P95/P99, input seed,
inline counts, source hashes, binary hash, and CPU affinity.
