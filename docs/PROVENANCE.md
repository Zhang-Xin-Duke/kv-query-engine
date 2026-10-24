# Source provenance and attribution

KVQueryEngine adapts a supplied neighbor-hash implementation into an independent
C++20 batch-query engine. This record identifies the source and the project's changes.

Original KVQueryEngine contributions are licensed under [PolyForm Noncommercial 1.0.0](../LICENSE).
Inherited portions retain their original rights, as recorded in [NOTICE](../NOTICE).
The supplied source checkout has no attached license grant. NOTICE records the scope of the project license.

## Recorded source

Source project: `athena/kfc-peta-lite` (local user-supplied checkout).
Source commit: `94fb5a1bd7358dc46c64c29d68ddcede6a963a83`.

| Relative source path | SHA-256 of supplied file |
| --- | --- |
| `pite/immutable_table/immutable_neighbor_hash_table.h` | `bab8137ee038eb1f9de5a7c1ae45d488a042d9daa1f4ceebfd952ae9b80ec667` |
| `pite/table/raw_hash_map.h` | `f0e39b877fc644cbc2bb8ce5c77e48a8ffaba9eadb4b6e9ef1ec7923c5254733` |

## What is reused

- `include/kvquery/detail/neighbor_index.hpp` retains/adapts FlatNode's 16-byte
  layout, the bucket mixer, relative links, bidirectional neighborhood search,
  home-chain splicing and lodger relocation from the immutable table header.
  Helper function bodies were extracted directly, with dependency and safety changes.
- `src/kvquery.cpp` adapts the AMAC state advance/refill loop from
  `raw_hash_map.h` to immutable byte values, per-input result positions and a portable
  prefetch intrinsic. AMAC and NeighborHash are existing techniques.

## Project additions and changes

Standalone C++20/CMake packaging; unsigned link encoding; full modular wrapping for
small tables; reserved value offset so zero keys/empty values are representable;
explicit duplicate rejection; bounded rebuild retries with a new mixer seed and larger table; ordered batch results retaining
snapshot ownership; checked portable persistence; CLI; tests and synthetic benchmarks.

The optional SIMD executor and `detail/simd_probe.hpp` add native two-key NEON/SSE2
comparison masks, residual-state scheduling, backend reporting and a forced portable
fallback. This implementation follows the existing multi-query probing idea while
retaining the supplied index layout; no SIMD implementation body was copied from
the reference artifact.
The repository contains standalone code, tests, and documentation. Source configuration and business data remain outside this project.

Small-value inlining is an independently implemented extension of the packed node:
six payload bytes, a length and a tag reuse its existing 54 data bits. The supplied
project's `docs/next.md` describes the general idea of storing small values directly
in a `NeighborValueTable`; no corresponding implementation was found in that
checkout. This project adds the mixed inline/external encoding, endian-aware views,
cross-layout persistence, accounting, tests and paired benchmark implementation.

## Background references

- [NeighborHash public artifact](https://github.com/slow-steppers/NeighborHash)
- [An Enhanced Batch Query Architecture in Real-time Recommendation](https://arxiv.org/abs/2409.00400)
- [AMAC: Asynchronous Memory Access Chaining](https://www.vldb.org/pvldb/vol9/p252-kocberber.pdf)

The public artifact and the supplied checkout are separate source records.
