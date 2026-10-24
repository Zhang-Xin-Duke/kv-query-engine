# Small-value inlining

## Lookup representation

The index remains a contiguous array of 16-byte nodes. Each node contains a full
64-bit key and one packed word. The upper ten bits of that word retain the signed
relative collision link. The remaining 54 bits have two representations:

```text
external: [ chain:10 | direct length / sentinel:18 | byte offset:36 ]
inline:   [ chain:10 | tag 111:3 | length 0..6:3   | payload:48     ]
```

After matching a key, an inline result views the payload in the node already
visited. An external result views `value_region_base + offset`. Both retain the
snapshot through the result batch. The optimization removes the dependent
external-value access when the caller consumes a small value. It also eliminates
that value's bytes from the external region without increasing any node.

Inline values include empty byte strings and arbitrary binary data. A nonzero tag
distinguishes an empty value from an empty slot, including for key zero. The
builder copies packed payloads when relocating collision nodes; queries only
receive views after construction has completed. The payload's address remains
stable throughout the immutable snapshot's lifetime. Character access and an
endian-dependent byte offset avoid aliasing and unaligned integer access.

## Choices and costs

The six-byte limit leaves room for both a length and a discriminator while keeping
all key and link bits. Eight payload bytes would require wider nodes or reduced
key/link information. External direct lengths now stop at 229375; larger values
use the existing four-byte length prefix and all-ones sentinel. This reserves the
tag space while continuing to represent large values.

Every successful lookup checks the representation. Larger values therefore need
control measurements as well as small-value tests. Inlining is enabled by default;
set `BuildOptions::inline_small_values=false` to build the external-only control.
The in-memory tag never appears in the version-1 snapshot file. Saving writes
logical key/length/value records; loading can choose either memory layout.

`Stats::inline_records` counts inline entries, including empty values.
`inline_value_bytes` counts payload inside existing node bytes. `value_bytes`
counts only the external region, including its reserved byte and length prefixes.
Total logical storage is `index_bytes + value_bytes`; inline bytes must not be
added again. Temporary build records and allocator overhead are separate.

## Reproduce the comparison

```sh
bash bench/run_linux.sh bench/results/linux-inline 0 inline
```

The paired suite runs both layouts in the same process with identical inputs and
full-byte verification outside timing. It measures the map baseline and both
layouts of Scalar, Prefetch, AMAC and SIMD at windows 8/32/64. Alternating method
order reduces ordering effects. The [benchmark protocol](BENCHMARKS.md) describes
the 16 cases, three processes per case and three rounds per process.

Timing includes lookup, result allocation, ownership, length/end-byte consumption
and destruction. It does not include construction, file loading or network work.
The external-only control uses the same final executable and query code, so this
comparison measures layout choice within the new implementation. It does not
isolate the new layout check's cost against an older executable.

## Validation

The `inline` CTest group checks every payload length, all 256 repeated byte values,
both signed link endpoints, forced lodger relocation, mixed small/large values,
direct/extended-length boundaries, all execution strategies and varied windows,
copied/moved result ownership, resource budgets and both save/load directions.
Existing randomized and concurrent-reader tests also exercise inline records.
The independent version-1 golden file remains unchanged.

## Duke server results

Measured source revision: `14311dee25ecf9fdd398afb1cb365e28454a60f5`.
Intel Xeon E5-2440 v2, GCC 11.4.0, `-std=c++20 -O3 -DNDEBUG`, CPU 0, SSE2.
Three independent processes per workload, three rounds per process.

| Value distribution | External-only | Inlining enabled | Paired ratio | Logical storage MB, before → after |
|---|---:|---:|---:|---:|
| 1 byte | 29.34 | 31.82 | 1.083× | 21.00 → 20.00 |
| 2 bytes | 27.84 | 32.21 | 1.157× | 22.00 → 20.00 |
| 4 bytes | 25.47 | 31.76 | 1.244× | 24.00 → 20.00 |
| 6 bytes | 24.26 | 31.78 | 1.310× | 26.00 → 20.00 |
| 7 bytes (control) | 24.06 | 23.89 | 0.993× | 27.00 → 27.00 |
| 32 bytes (control) | 21.15 | 20.94 | 0.987× | 52.00 → 52.00 |
| 50% 6-byte + 50% 32-byte | 22.27 | 21.44 | 0.960× | 39.00 → 36.00 |

Six-byte SIMD throughput improves by 31.0% (paired process range 29.7–33.8%).
Scalar improves by 23.6% and AMAC/32 by 28.1%. At one million six-byte records,
logical storage falls from 26,000,001 to 20,000,001 bytes (23.1%). A smaller
external region and the eliminated dependent value access are both part of this
layout change; the experiment does not isolate their individual contributions.

The 50/50 mixed case is slower: SIMD loses 4.0% and Scalar 6.9%, while storage
falls by 7.7%. A predictable layout in uniform-size records and an unpredictable
layout in mixed records are a plausible explanation; no hardware-counter
measurement was taken to attribute the regression specifically to branch misses.
Above-threshold controls remain close between enabled/disabled layouts: SIMD
paired ratios are 0.993× at seven bytes and 0.987× at 32 bytes. Both use the same
external representation and code path in the final executable.

An additional field-selection implementation was checked in a targeted pilot; it
did not remove the mixed-case regression or consistently improve absolute rates.
The published final suite uses the simpler tagged view path at the revision above.

- [Full raw paired suite](../bench/results/duke-inline-2026-10-24.json)
- [All 16 cases and process ranges](../bench/results/duke-inline-2026-10-24.md)
- [Environment and swap counters](../bench/results/duke-inline-2026-10-24.environment.txt)
- [Server correctness checks](../bench/results/duke-inline-2026-10-24.tests.txt)

The published standard eight-case suite was rerun from the same revision and
binary. Both suites recorded CPU affinity `[0]`, clean tracked source files and
matching source/binary hashes. Global swap-in, swap-out and major-fault counters
remained unchanged during each suite.

## Scaling beyond the last-level cache

The additional suite measures 1/4/8 million records, six-byte values, batches of
256 keys, 90% target hits and 1,048,576 queries per round. Each size uses three
processes with three rounds. It measures revision
`a407f3c0c5f6f0cb4fd130d92f183fdbf8e8ca7e`; the engine and benchmark executable are
unchanged from the first suite. Both report the same binary SHA-256:
`a91a320a2461930d8b7e2373cc70b49deaac9c125d7ef2c0ad4b30677a882e45`.

| Records | SIMD/32 external | SIMD/32 inline | SIMD gain | AMAC/32 gain | Logical storage MB, before → after |
|---|---:|---:|---:|---:|---:|
| 1,000,000 | 25.07 | 31.94 | +27.4% | +27.0% | 26 → 20 |
| 4,000,000 | 20.08 | 28.78 | +41.8% | +43.3% | 104 → 80 |
| 8,000,000 | 18.17 | 26.57 | +46.0% | +48.2% | 208 → 160 |

SIMD paired gains are 27.4%, 41.8% and 46.0%; process ranges are 26.1–27.5%,
41.0–44.4% and 45.8–47.5%. AMAC/32 gains are 27.0%, 43.3% and 48.2%.
Scalar gains remain about 30–32%, illustrating executor-dependent scaling.
The index arrays are approximately 20/80/160 MB, compared with the host's 20 MiB L3.
Avoiding external-value accesses and reducing the working set are plausible
contributors; the comparison does not isolate either one. Higher relative speedup
does not mean larger datasets have higher absolute throughput.

The scaling series uses more queries than the original 262,144-query paired
suite. Compare sizes within this scaling series to assess the working-set trend.
Execution remains single-threaded on CPU 0; concurrent saturation is a separate
measurement. No global swap or major-fault counter changed during the run.

- [Scaling raw JSON](../bench/results/duke-inline-scale-2026-10-24.json)
- [Scaling full report and process ranges](../bench/results/duke-inline-scale-2026-10-24.md)
- [Environment](../bench/results/duke-inline-scale-2026-10-24.environment.txt)
- [Server checks](../bench/results/duke-inline-scale-2026-10-24.tests.txt)
