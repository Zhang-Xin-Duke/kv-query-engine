# Small-value inlining

Keep the 16-byte index node and the ordered, snapshot-owned string-view API. Store
0..6-byte values in the node's 54 data bits: a 48-bit byte payload, three length
bits, and a three-bit tag. The ten relative-link bits remain unchanged. Empty
values have a nonzero tag, so key zero and missing entries remain distinguishable.
Views point into stable, immutable nodes after construction; moving collision
nodes during construction copies the payload and does not publish views.

External values retain 36-bit offsets. Direct lengths are below 229376; larger
values use the existing all-ones length sentinel and a four-byte length prefix.
The inline tag reserves length codes, not extra node storage. Endian-aware
character access exposes the payload without an unaligned typed read. Snapshot
files continue storing logical key/length/value records; the in-memory tag is not
serialized. Both load modes can read the same version-1 file.

`BuildOptions::inline_small_values` enables inlining by default and permits an
external-only control. `Stats` reports inlined record and byte counts. The value
budget applies to external storage, including its reserved byte and length
prefixes; index limits continue bounding node storage.

Alternatives considered: widening every node would penalize large-value tables;
an extra side array would retain the second memory access; four-byte inlining
would preserve more direct-length codes but cover fewer small records. Six-byte
inlining uses the available payload while preserving all 64 key bits and links.

Validation covers binary bytes and every inline length, zero keys/empty values,
mixed inline/external collision chains and relocation, overflow-length boundaries,
all query strategies, retained/moved results, concurrent readers, and save/load
across layout choices. Run release, sanitizer, and portable-backend checks.

Measure inline and external-only snapshots in the same process using identical
records and query traces. Alternate layout order across rounds, retain full-byte
correctness checks outside timing, and include scalar/AMAC/SIMD and unordered_map.
Use the selected Duke server, CPU 0, unchanged compiler flags, repeated processes,
0..6-byte cases, above-threshold controls, mixed lengths, hot and low-hit cases.
Publish all results, including regressions; refresh the standard README suite
against the final implementation as well.
