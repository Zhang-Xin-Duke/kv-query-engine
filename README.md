# High-Performance KV Query Engine for Read-Heavy Workloads

[中文](README.zh-CN.md) · [Architecture](docs/ARCHITECTURE.md) · [API](docs/API.md) · [Benchmarks](docs/BENCHMARKS.md) · [License](LICENSE)

## Introduction

An AI request often begins with a batch of IDs.

A retrieval pipeline identifies relevant chunks and then fetches their documents. A feature service turns entity IDs into model inputs. An embedding lookup maps discrete IDs to vectors. These reads prepare the data that downstream computation consumes; examples appear in the official documentation for [LangChain retrieval](https://reference.langchain.com/python/langchain-classic/retrievers/multi_vector/MultiVectorRetriever), [Feast online features](https://docs.feast.dev/getting-started/components/online-store), and [PyTorch embedding tables](https://docs.pytorch.org/tutorials/intermediate/torchrec_intro_tutorial.html).

Consider a search returning 64 document IDs. Before constructing the final context, the application must retrieve the corresponding records and preserve their association with those IDs. Even with resident data, each lookup must locate a bucket, follow any collision chain, and access the value. Dependent memory accesses can limit progress, while independent lookups within the batch create opportunities to prefetch and interleave work.

This C++20 engine brings together a compact immutable index, configurable batch execution, snapshot persistence, and explicit result ownership. It is suitable for read-heavy reference data such as document metadata, materialized features, entity attributes, catalogs, and rules. Applications encode records as `uint64_t` keys and opaque value bytes.

## Features

- **16-byte index nodes:** packed value offsets, lengths, and relative collision links.
- **Four execution strategies:** scalar, lookahead prefetch, AMAC, and SIMD batch probing, selected per call.
- **Ordered results:** duplicate keys retain their positions; missing and empty values are distinct.
- **Immutable snapshots:** concurrent readers share stable index and value storage.
- **Owned result lifetime:** a result batch retains the snapshot behind its value views.
- **Checked persistence:** versioned little-endian records, length checks, resource budgets, and checksums.
- **Independent tooling:** CMake library, CLI, correctness tests, sanitizers, Linux CI, and a benchmark suite.

## Design

### 1. Resolve collisions during construction

When two keys map to the same bucket, the builder searches nearby slots in both directions. If a new key's home bucket is occupied by a node belonging to another collision chain, the builder moves that node, repairs the chain, and places the new key at home. Finding space, moving records, and repairing links happen before publication; lookups then compare keys and follow relative links.

This favors a simple read path over cheap insertion. Relative links constrain placement, so bounded rebuild attempts expand the table and change its hash seed when a neighborhood is exhausted.

### 2. Keep index nodes compact and values contiguous

A 16-byte index node stores a 64-bit key and a packed word containing a 36-bit value offset, an 18-bit length, and a 10-bit signed relative link. Variable-length values live in a separate contiguous byte region. Following the index does not require reading entire values, and short relative links replace a separate next-node pointer.

The builder calculates the value storage budget and reserves the buffer before copying records. This keeps node size fixed and avoids repeatedly growing the value region during construction.

### 3. Share immutable snapshots across readers

The index and values remain unchanged after construction. Every query owns its scheduling state and result positions, so readers do not coordinate index mutations. Each batch reads one snapshot throughout execution.

Applications refresh data by building or loading another snapshot and choosing when new requests use it. Existing results retain their original snapshot. This gives reads stable addresses while leaving refresh scheduling and overlapping snapshot memory under application control.

### 4. Compare multiple keys with SIMD and retain unfinished probes

SIMD batch probing gathers two candidate keys and compares them with a 128-bit vector instruction. Unfinished queries retain their input and collision-chain positions in a residual-state window. Completed positions immediately accept new queries; the remaining probes drain with the same scheduling rules. Original input positions preserve output order and duplicates even when probes finish at different times.

ARM64 uses NEON; x86 uses SSE2, comparing both 32-bit halves of each full 64-bit key. Other targets use a portable fallback. Odd or draining single lanes use direct comparisons. Hashing, noncontiguous node loads, chain transitions and value-view construction remain scalar. `kvquery::simd_backend()` reports the compiled backend.

| Strategy | Execution | Window |
|---|---|---|
| Scalar | Complete one lookup before starting the next | Validated, otherwise unused |
| Prefetch | Prefetch future home buckets ahead of scalar lookup | Input lookahead distance |
| AMAC | Advance independent lookup states one node at a time and refill completed states | Active state count |
| SIMD | Compare two candidate keys per vector and retain/refill unfinished lookups | Residual state count |

All strategies run on the calling thread. Windows from 1 through 256, including non-powers of two, allow workload-specific tuning. Prefetch, AMAC and SIMD add bookkeeping to create opportunities for overlapping access or comparison. Scalar remains the default; [measured results](docs/SIMD_EVALUATION.md) guide the choice.

### Value views and ownership

Results use `optional<string_view>` in input order. `nullopt` represents a miss; an engaged empty view represents a stored empty value. Returning views avoids copying complete payloads. One shared snapshot owner per result batch keeps the referenced bytes alive; retaining a result also retains that complete snapshot.

### Portable records on disk

Snapshot files contain keys, lengths, and value bytes. Loading validates the file and reconstructs the index. The defined byte format is independent of C++ object layout and hash placement, with parsing and rebuilding performed at startup.

Saving writes an exclusive temporary sibling, syncs the file, and publishes it with a same-filesystem hard link. Existing destinations are preserved. Versioned filenames and snapshot handles support application-managed refreshes.

See [Architecture](docs/ARCHITECTURE.md) for layout, construction cases, query state machines, file format, resource budgets and failure behavior.

## Build and quick start

Requirements: a 64-bit POSIX host, a GCC or Clang compiler with C++20 support, CMake 3.20+, and Python 3 for integration tests.

```sh
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j
ctest --test-dir build/release --output-on-failure

demo_dir="$(mktemp -d "${TMPDIR:-/tmp}/kvquery-demo.XXXXXX")"
printf '0\t\n7\thello\n9\tworld\n' > "$demo_dir/input.tsv"
build/release/kvquery build "$demo_dir/input.tsv" "$demo_dir/data.kwv"
build/release/kvquery inspect "$demo_dir/data.kwv"
build/release/kvquery query "$demo_dir/data.kwv" --strategy simd --window 32 7 8 0 7
```

```text
7	FOUND	68656c6c6f
8	MISSING
0	FOUND	
7	FOUND	68656c6c6f
```

The first tab separates a decimal key from its text value. Empty lines are skipped, CRLF is normalized, and duplicate keys are rejected. Query values are printed as hex. The library supports binary values.

## C++ integration

```cpp
#include <kvquery/kvquery.hpp>
#include <array>
#include <iostream>
#include <vector>

int main() {
    std::vector<kvquery::Record> records{{7, "hello"}, {0, ""}};
    auto snapshot = kvquery::Snapshot::build(records);
    std::array<uint64_t, 4> keys{7, 8, 0, 7};
    auto result = snapshot->query(keys, {kvquery::Strategy::simd, 32});
    snapshot.reset();
    for (size_t i = 0; i < result.size(); ++i) {
        if (result[i]) std::cout << keys[i] << ": " << *result[i] << '\n';
        else std::cout << keys[i] << ": missing\n";
    }
}
```

Link the CMake target `kvquery`. Keep either the result batch or its source snapshot alive while using value views. [API reference](docs/API.md) covers options, budgets, persistence, and errors.

## Performance evaluation

The benchmark design follows the NeighborHash paper and its public reproduction artifact [[1]](#reference-1), [[2]](#reference-2).

The benchmark compares `std::unordered_map`, scalar index lookup, lookahead prefetch, AMAC, and SIMD over identical inputs. Workloads vary table size, batch size, value length, hit rate, and access distribution. Reports include throughput and batch P50/P95/P99 wall latency, together with commands, compiler, hardware, and source hashes.

The [SIMD evaluation](docs/SIMD_EVALUATION.md) records the current implementation and its measured trade-offs. The [benchmark guide](docs/BENCHMARKS.md) includes the reference design's published server evaluation and explains how to run the current engine's server suite. Reference results are organized by their source implementation and experiment configuration.

```sh
build/release/kvquery_bench --records 1000000 --batch 256 --queries 262144 --rounds 3
python3 bench/run_suite.py --output bench/results/server-suite.json
python3 bench/summarize.py --input bench/results/server-suite.json --output bench/results/server-report.md
```

## Validation

Ten CTest groups cover edge values, forced collisions and relocation, randomized differential lookup, result ownership, concurrent access, persistence failures, golden file bytes, SIMD lane/tail handling, CLI integration, and benchmark output validation. Linux CI builds GCC Release, Clang Debug, and Clang ASan/UBSan, and forced-portable Release configurations.

```sh
cmake -S . -B build/asan -DKVQUERY_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build/asan -j
ctest --test-dir build/asan --output-on-failure
```

ThreadSanitizer is available in a separate build through `-DKVQUERY_TSAN=ON`. See [Validation](docs/VALIDATION.md).

## Source and license

The engine adapts an existing neighbor-hash index and AMAC implementation. [PROVENANCE](docs/PROVENANCE.md) records the source and project changes.

Original project contributions use the **PolyForm Noncommercial License 1.0.0**. Commercial use is not licensed. See [LICENSE](LICENSE) and [NOTICE](NOTICE).

## References

<a id="reference-1"></a>
[1] Q. Zhang, Z. Teng, D. Wu, and J. Wang, “An Enhanced Batch Query Architecture in Real-time Recommendation,” in *Proceedings of the 33rd ACM International Conference on Information and Knowledge Management (CIKM ’24)*, 2024, pp. 5078–5085, doi: [10.1145/3627673.3680034](https://doi.org/10.1145/3627673.3680034).

<a id="reference-2"></a>
[2] slow-steppers, “NeighborHash,” GitHub repository, [Online]. Available: [github.com/slow-steppers/NeighborHash](https://github.com/slow-steppers/NeighborHash). Accessed: Oct. 24, 2026.
