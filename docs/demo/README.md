# Runtime demo

The animated terminal replay in the main README renders output from commands executed on October 27, 2026. Playback is paced for readability, and local executable/temporary-directory prefixes are shortened. The captured text and measured values are in [recording.json](recording.json); [pressure.json](pressure.json) is the benchmark executable's output. The recording includes the measured binary SHA-256.

## Lookup demonstration

The input contains key 0 with an empty value, key 7 with `hello`, and key 9 with `world`. After building the snapshot, the CLI queries `7 8 0 7` using SIMD/window 32. An independent expected-output check verifies the complete printed result. Missing key 8 and stored empty key 0 remain distinct; repeated key 7 keeps its position. Values print as hexadecimal bytes.

Use the [build and quick start](../../README.md#build-and-quick-start) commands to reproduce this lookup.

## Fresh pressure demonstration

This is one 3-second AMAC/window-32 round with 16M six-byte records, batches of 256 keys, 90% target hits and eight pinned workers. To select only this configuration, set `KVQUERY_BENCH_THREADS=8` and `KVQUERY_BENCH_STRATEGY=amac`, select eight allowed CPU IDs with `KVQUERY_BENCH_CPUS`, then run:

```sh
build/tuned/kvquery_concurrency_bench 16000000 6 256 3 1
```

The recorded host used logical CPUs `10,11,12,13,22,23,0,1`, identified as performance cores. These IDs are host-specific. The build uses GCC 14.4.0, CMake Release, `-msse4.1 -mtune=native`. Full timed-batch checksums are verified; sampled batch P99 excludes queueing and transport.

For repeated measurements, method definitions, variation and concurrency regressions, see the [full pressure evaluation](../../bench/results/desktop-2026-10-27-rerun/README.md).
