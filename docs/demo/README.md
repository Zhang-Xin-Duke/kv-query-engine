# Extended runtime demo

The main README contains a 125-second rendered replay of actual CLI output and fresh benchmark summaries. Playback is paced for readability, and executable/temporary path prefixes are shortened. All measurements are in [recording.json](recording.json); [pressure.json](pressure.json) retains the concurrent executable output. Binary and measured source hashes are recorded.

## Chapters

1. Ordered batch lookup: hit, miss, stored empty value and duplicate positions.
2. Product catalog: stored Desk/Chair/Lamp records, reordered IDs and an absent ID.
3. Snapshot persistence: a fresh CLI process inspects and queries the saved catalog.
4. Cache-resident data: 10K records, 32-byte values, batches of 256.
5. Large batches: 1M records, 32-byte values, batches of 1,024.
6. Miss-heavy traffic: 1M records, 32-byte values, 30% target hits.
7. Wide records: 250K records, 256-byte values, batches of 256.
8. Small values: 1M six-byte records, batches of 256, inlining enabled.
9. Concurrent pressure: 16M six-byte records, batches of 256, eight workers.

## Matched comparison protocol

All five map comparisons run on logical CPU 10, a performance core. Each workload uses three independent processes with adjacent seeds, three rounds per process, and 1,048,576 query keys per round. The same process runs `std::unordered_map` and all four engine strategies on identical inputs. The map uses the same initial mixing function and a maximum load factor of 0.8. All methods allocate ordered optional views, consume value length/endpoint bytes, and destroy results in timing. Full-byte equality checks run outside timing; timed checksums must match. Windows below are 32. Rates and P99 are process medians; ratios are ratios of those medians.

These are complete engine-path comparisons, including different storage layouts and ownership costs; they do not isolate SIMD instructions or the hash algorithm. The map is read-only during measurement. Synthetic fixed-size workloads do not establish production service capacity.

### cache

10,000 records, 32-byte values, batch 256, target hit rate 90%.

| Method | Mkeys/s | Batch P99 µs |
|---|---:|---:|
| unordered_map | 76.18 | 4.60 |
| scalar | 100.70 | 2.90 |
| prefetch | 94.34 | 3.00 |
| amac | 102.94 | 2.80 |
| simd | 104.18 | 2.80 |

### batch

1,000,000 records, 32-byte values, batch 1024, target hit rate 90%.

| Method | Mkeys/s | Batch P99 µs |
|---|---:|---:|
| unordered_map | 18.51 | 70.40 |
| scalar | 36.24 | 37.10 |
| prefetch | 49.84 | 25.70 |
| amac | 57.79 | 22.30 |
| simd | 56.86 | 23.00 |

### misses

1,000,000 records, 32-byte values, batch 256, target hit rate 30%.

| Method | Mkeys/s | Batch P99 µs |
|---|---:|---:|
| unordered_map | 17.20 | 19.50 |
| scalar | 44.44 | 8.10 |
| prefetch | 58.08 | 5.00 |
| amac | 64.26 | 4.50 |
| simd | 69.78 | 4.20 |

### wide

250,000 records, 256-byte values, batch 256, target hit rate 90%.

| Method | Mkeys/s | Batch P99 µs |
|---|---:|---:|
| unordered_map | 21.52 | 17.30 |
| scalar | 41.76 | 9.60 |
| prefetch | 47.31 | 6.10 |
| amac | 51.13 | 5.50 |
| simd | 50.83 | 5.60 |

### inline

1,000,000 records, 6-byte values, batch 256, target hit rate 90%.

| Method | Mkeys/s | Batch P99 µs |
|---|---:|---:|
| unordered_map | 19.85 | 16.90 |
| scalar | 63.16 | 7.40 |
| prefetch | 82.36 | 3.80 |
| amac | 93.41 | 3.10 |
| simd | 96.31 | 3.00 |

## Reproduce

Build with the [documented compiler flags](../../bench/results/desktop-2026-10-27-rerun/README.md#provenance-and-reproduction). Use the [quick start](../../README.md#build-and-quick-start) for the initial CLI example. The catalog input contains `101\tDesk`, `202\tChair`, `303\tLamp` (replace `\t` with actual tabs), and queries request `202 999 101 202`. A separate `inspect`/`query` process reloads the saved file.

The exact arguments and seeds for every comparison process are in each `comparisons[].commands` entry in [recording.json](recording.json); use `build/tuned/` as the executable prefix and pin the process to an allowed core on your host. For the large-batch case:

```sh
build/tuned/kvquery_bench --records 1000000 --batch 1024 \
  --queries 1048576 --value-bytes 32 --hit-rate 0.9 \
  --distribution uniform --seed 20261021 --rounds 3
```

For concurrent pressure, set `KVQUERY_BENCH_THREADS=8`, `KVQUERY_BENCH_STRATEGY=amac`, and `KVQUERY_BENCH_CPUS` to eight allowed cores. The recorded order was `10,11,12,13,22,23,0,1`, identified as performance cores on this host, then run:

```sh
build/tuned/kvquery_concurrency_bench 16000000 6 256 3 1
```

The pressure chapter is one three-second round; its rate is not a single-thread map comparison. For repeated rounds, run-to-run variation, longer pressure and single-key concurrency limitations, see the [full evaluation](../../bench/results/desktop-2026-10-27-rerun/README.md).
