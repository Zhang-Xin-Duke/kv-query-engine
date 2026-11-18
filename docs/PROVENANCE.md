# Component rights and implementation

KVQueryEngine's original contributions are licensed under
[PolyForm Noncommercial 1.0.0](../LICENSE). The license applies only to
contributions the licensor owns; third-party rights remain with their respective
holders. See [NOTICE](../NOTICE) for the copyright notice.

## Implementation

- `include/kvquery/detail/neighbor_index.hpp` implements a 16-byte node layout,
  bucket mixing, relative links, bidirectional neighborhood search, chain
  splicing and node relocation.
- `src/kvquery.cpp` implements scalar, prefetch, AMAC and SIMD batch lookup,
  maintaining input positions and snapshot ownership.
- `online_index` provides partitioned snapshot loading, epoch-based reclamation,
  an online worker, replica services and client-side hot-key controls.
- Small-value inlining stores up to six bytes in the packed node and supports
  mixed inline and external values.

## Reference methods

- [NeighborHash public artifact](https://github.com/slow-steppers/NeighborHash)
- [An Enhanced Batch Query Architecture in Real-time Recommendation](https://arxiv.org/abs/2409.00400)
- [AMAC: Asynchronous Memory Access Chaining](https://www.vldb.org/pvldb/vol9/p252-kocberber.pdf)
