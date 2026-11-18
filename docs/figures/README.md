# README figures: sources and reproduction

All README diagrams use English labels and describe the current KVQueryEngine.
SVG is used in the READMEs for crisp scaling; each figure also has a PNG preview.
Every SVG contains an accessible title and description. White backgrounds retain
contrast in both light and dark GitHub themes.

## Figure design

The README figures illustrate the current implementation: packed nodes,
separate index and value storage, relative links and snapshot ownership.
Component rights are described in [PROVENANCE](../PROVENANCE.md).

## Current figures

| Figure | Current implementation or measurement source |
|---|---|
| [Component flow](overview.svg) | `Snapshot::build`, `load`, `query`, and ordered `BatchResult` values |
| [Collision relocation](collision-relocation.svg) | `NeighborIndex::Insert_`: move the lodger, repair its chain, place the host |
| [External / inline layout](value-layout.svg) | `FlatNode`, `PackInline`, and `Snapshot::Impl::view`; 16 bytes in both cases |
| [Snapshot ownership](snapshot-ownership.svg) | `BatchResult::owner_`; application-managed snapshot selection |
| [SIMD residual window](simd-window.svg) | Pair comparison, state advancement, immediate refill and ordered result positions |
| [Executor throughput](executor-throughput.svg) | `duke-x86-inline-2026-10-24.json`; all eight workloads, four displayed executors |
| [Inline tradeoffs](inline-tradeoffs.svg) | `duke-inline-2026-10-24.json`; paired SIMD/32 process ratios and min/max |
| [Working-set scaling](inline-scaling.svg) | `duke-inline-scale-2026-10-24.json`; SIMD/32 throughput and logical storage |

The diagram keys and state positions are illustrative, and the collision example
follows a valid builder sequence. Layout field widths are schematic and labeled
with their exact bit counts. The SIMD illustration uses four states on one calling
thread; it does not imply four threads or a four-lane hardware comparison.

Charts read the committed server JSON directly. Throughput is the median of
per-process rates, ratio labels are medians of matched per-process ratios, and
whiskers are process min/max rather than confidence intervals. Throughput and
storage axes start at zero. Ratio bars use equal throughput (1.0) as their baseline.
Logical storage is node-array bytes plus external value-region bytes; inlined
payload is already part of node storage. Both gains and the mixed-value regression
are shown. The complete numeric tables remain expandable in the READMEs.

## Regenerate

From the repository root, with Python 3:

```sh
python3 -m venv /tmp/kvquery-figures
/tmp/kvquery-figures/bin/pip install -r docs/figures/requirements.txt
/tmp/kvquery-figures/bin/python docs/figures/generate.py
```

The optional plotting environment is separate from the C++ build. The generator
writes eight SVGs, eight PNG previews and [manifest.json](manifest.json). It reads
existing measurements and does not execute or alter benchmark workloads.

The new illustrations and generation script are original project contributions
under the project’s [noncommercial license](../../LICENSE).
