# README figures: sources and reproduction

All README diagrams use English labels and describe the current KVQueryEngine.
SVG is used in the READMEs for crisp scaling; each figure also has a PNG preview.
Every SVG contains an accessible title and description. White backgrounds retain
contrast in both light and dark GitHub themes.

## Original documentation reviewed

The supplied `kfc-peta-lite/docs/images` illustrations provided useful visual
models for packed buckets, separate index/data storage, neighbor links and reader
lifetimes. They describe a broader system and a different bucket representation.
The README assets are newly drawn from the current code, with the relevant ideas
carried over and current field sizes and ownership made explicit. Original image
files were reviewed locally and are not copied into this repository.

| Original illustration | Useful concept | Treatment in this project |
|---|---|---|
| `dms2.0-overview.drawio.png` | Input → build → serving layers | Redrawn as the current standalone build/query component; external ingestion and FlatBuffers are omitted. |
| `dms2.0-index_data_pair.drawio.png` | Separate key/offset index and value data | Informed the external-region explanation; current memory layout and file records are drawn from this engine. |
| `index_data_pair.png` | Index/data addressing | Reviewed as a second rendering of the same format; the public diagram uses current node fields. |
| `dms2.0-bucket.drawio.png` | Packed bucket fields | Original 16-bit link / 48-bit pointer replaced by this engine’s 10-bit link and mixed data encoding. |
| `dms2.0-neighbor_map.drawio.png` | Neighbor links inside a contiguous node array | Informed the relative-link view; the A/B/C relocation sequence follows the current builder. |
| `dms2.0-pointer_value.drawio.png` | Specialized compact-value planning | Reviewed; current engine preserves all 64 key bits and uses a separate inline tag. |
| `dms2.0-epoch.drawio.png` | Readers retaining old data during replacement | Redrawn with current shared snapshot ownership, rather than epoch slots or an updater. |
| `overview.png` | Offline sources and application adapters | Reviewed; the standalone diagram uses Record input and current library entrypoints. |

The supplied implementation’s source record is in [PROVENANCE](../PROVENANCE.md).
The inspected illustrations have the following SHA-256 identifiers:

```text
5e6e741303a22ae6e756c60f7628005ce0cbfa4d83415a400b7fc8602b37cf43  dms2.0-overview.drawio.png
ba5da0f2614d6f3add8b81ad8e61d00e386805781dbedd131a696399132b4bc5  dms2.0-index_data_pair.drawio.png
88faa97e39b25e9915c7a770ed95b4dac7db8df7c770f0757339341433956cfe  index_data_pair.png
1c8496a0201dd877993a5ef57eaa85c2c00c31e55078a86337b74610b544aabe  dms2.0-bucket.drawio.png
aa175ba0689f157458d440dafd8700c082d2e6e52f8d13bec6e7e2ff1df64a70  dms2.0-neighbor_map.drawio.png
09646fafb850c6b33bf8c1fd70d3844503d263f41125d66491ccf50d54dceb10  dms2.0-pointer_value.drawio.png
326b714cc134f0b452661184bce6ed977ecd163f150632b20bc798280258a84c  dms2.0-epoch.drawio.png
896bff15e4e72ba75350509bac9bf25a22407e12ede5e9c1087fbe3086e8c7fd  overview.png
```

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
