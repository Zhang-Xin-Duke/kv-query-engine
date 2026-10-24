# SIMD batch probing design and implementation plan

**Goal:** Add an optional, tested SIMD query executor and publish it to GitHub.

**Architecture:** Keep the immutable 16-byte neighbor index and ordered value-view
API. Compare two independent candidate keys per 128-bit vector, retaining unfinished
collision-chain lookups in a bounded residual-state window. Refill completed states
from the input and continue the same schedule during drain.

**Tech stack:** C++20, AArch64 NEON, x86 SSE2, CMake, CTest, Python benchmark tools.

## Design choices

The chosen approach vectorizes independent query probes without changing index or
file layout. NEON and SSE2 cover baseline 64-bit ARM and x86 systems. Noncontiguous
nodes are loaded individually; SIMD compares the gathered full 64-bit keys. This
does not require AVX2 gather, AVX-512, or a new target CPU requirement.

A metadata-group / Swiss-table approach would change the index and its collision
semantics. An AVX2-only gather executor would exclude the current ARM machine and
require runtime dispatch on x86. These remain separate future experiments.

SSE2 compares both 32-bit halves and requires both halves to match for a 64-bit
lane. NEON uses 64-bit equality. A portable two-lane helper preserves semantics on
other targets, and a CMake option forces this path in tests. Backend identity is
reported by the library and benchmark. Scalar remains the default.

Each lookup state owns an input index and current bucket. Only initialized active
lanes read index memory. Empty nodes cannot match key zero; partial vector groups
and exhausted states never read past the input. Every result is written to its
original input position, preserving duplicates and snapshot ownership.

## Implementation steps

1. Add SIMD differential tests (including false half-key matches, collisions,
   odd/short batches, irregular windows, refill/drain and concurrent readers).
   Verify the tests fail before adding the strategy.
2. Add the portable SIMD helper and `Strategy::simd`, residual-window executor,
   backend reporting, CLI selection and CMake fallback switch.
3. Extend benchmark verification, strategy windows and summaries. Preserve all
   existing timing and result-consumption semantics; record native backend.
4. Update README, API, architecture, provenance and validation docs. Explain that
   only key comparisons are vectorized; hashing/gather/chain transitions are scalar.
5. Run native Release, ASan/UBSan and forced-portable tests, inspect native vector
   instructions, and run the complete multi-process benchmark suite.
6. Review the diff, commit, push, and verify x86 native plus portable GitHub CI.

## Acceptance

SIMD results equal Scalar / the reference map for the test matrix. No out-of-bounds
tail access or sanitizer errors. ARM NEON and x86 SSE2 are exercised on their native
hosts; the fallback is explicitly tested. Report measured throughput without
assuming SIMD improves every workload. Do not change the resume's existing metric
to attribute it to the new executor.
