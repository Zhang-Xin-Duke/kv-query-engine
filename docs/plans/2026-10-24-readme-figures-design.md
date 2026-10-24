# README figures

Make the English README explain the component visually before expanding the
implementation detail. Review the supplied documentation's images as background,
then draw current-project diagrams with English labels and accurate boundaries.

The source images cover separate index/data files, pointer-based buckets,
neighbor links, distributed ingestion and epoch-based refresh. The current engine
has different packed fields, standalone inputs and shared snapshot ownership.
Redraw relevant concepts rather than copying diagrams with obsolete labels or
unimplemented components. Record inspected originals and source anchors in a
figure provenance note.

Add diagrams for the complete input/query/output path, construction-time lodger
relocation, external versus inline value layout, snapshot ownership, and the SIMD
residual window. Add two quantitative figures from committed server JSON: standard
executor throughput and larger-working-set inline comparison, including storage.
Keep diagrams readable at GitHub's content width; use accessible SVG descriptions,
consistent colors, explicit arrow labels and short captions. Include the mixed
value regression in the inline findings instead of displaying only gains.

Generate assets reproducibly, retain SVG plus PNG previews, and keep figure code
separate from executable benchmark source/hashes. Link the English README and
Chinese companion to the same figures. Verify source/labels, published metric
values, SVG validity, local links and visual rendering, then commit and push.
