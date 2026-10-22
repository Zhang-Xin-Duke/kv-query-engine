# High-Performance KV Query Engine for Read-Heavy Workloads

[中文](README.md) · [Architecture](docs/ARCHITECTURE.md) · [API](docs/API.md) · [Benchmarks](docs/BENCHMARKS.md) · [License](LICENSE)

## Introduction

An AI request often begins with a batch of IDs.

A retrieval pipeline identifies relevant chunks and then fetches their documents. A feature service turns entity IDs into model inputs. An embedding lookup maps discrete IDs to vectors. These reads prepare the data that downstream computation consumes; examples appear in the official documentation for [LangChain retrieval](https://reference.langchain.com/python/langchain-classic/retrievers/multi_vector/MultiVectorRetriever), [Feast online features](https://docs.feast.dev/getting-started/components/online-store), and [PyTorch embedding tables](https://docs.pytorch.org/tutorials/intermediate/torchrec_intro_tutorial.html).

Consider a search returning 64 document IDs. Before constructing the final context, the application must retrieve the corresponding records and preserve their association with those IDs. Even with resident data, each lookup must locate a bucket, follow any collision chain, and access the value. Dependent memory accesses can limit progress, while independent lookups within the batch create opportunities to prefetch and interleave work.

This C++20 engine brings together a compact immutable index, configurable batch execution, snapshot persistence, and explicit result ownership. It is suitable for read-heavy reference data such as document metadata, materialized features, entity attributes, catalogs, and rules. Applications encode records as `uint64_t` keys and opaque value bytes.

## Features

- **16-byte index nodes:** packed value offsets, lengths, and relative collision links.
- **Three execution strategies:** scalar, lookahead prefetch, and AMAC, selected per call.
- **Ordered results:** duplicate keys retain their positions; missing and empty values are distinct.
- **Immutable snapshots:** concurrent readers share stable index and value storage.
- **Owned result lifetime:** a result batch retains the snapshot behind its value views.
- **Checked persistence:** versioned little-endian records, length checks, resource budgets, and checksums.
- **Independent tooling:** CMake library, CLI, correctness tests, sanitizers, Linux CI, and a benchmark suite.

## Design

### Stable data for a read-heavy path

Records are validated and organized before a snapshot becomes available to readers. Its index and value addresses then remain stable, and each query call owns its scheduling state. A batch reads one snapshot throughout execution.

Applications refresh data by building or loading another snapshot and choosing when new requests use it. Existing results retain their original data. This keeps read execution simple and moves preparation work into the build phase; the application manages refresh scheduling and overlapping snapshot memory.

### A compact index with nearby collision nodes

Each node contains a 64-bit key and a 64-bit packed word: a 36-bit value offset, an 18-bit length, and a 10-bit signed relative link. Values live in a separate contiguous byte region. This preserves fixed-size index nodes while supporting variable-length records.

Collision insertion searches nearby slots in both directions. When a key's home bucket holds a node from another chain, the builder relocates that node, repairs its links, and places the new host at home. Construction does more work to establish a simple lookup path. Short links also constrain placement; bounded rebuild attempts expand the table and change its hash seed when a neighborhood is exhausted.

### Choose how a batch accesses memory

| Strategy | Execution | Window |
|---|---|---|
| Scalar | Complete one lookup before starting the next | Validated, otherwise unused |
| Prefetch | Prefetch future home buckets ahead of scalar lookup | Input lookahead distance |
| AMAC | Advance independent lookup states one node at a time and refill completed states | Active state count |

Prefetch and AMAC spend extra instructions to create opportunities for overlapping memory access. Scalar has less bookkeeping and is the default. Windows from 1 through 256, including non-powers of two, allow applications to select an execution plan for their workloads. AMAC interleaves initial fill, normal execution, and tail drain on one CPU thread.

### Views with ownership

Results use `optional<string_view>` and preserve input positions. `nullopt` represents a miss; an engaged empty view represents a stored empty value. Returning views avoids copying entire values. The result batch retains a shared snapshot, trading reference-count updates and snapshot retention for a straightforward lifetime contract. Applications can copy a small subset of fields when they want to release a large result batch early.

### Portable records on disk

Snapshot files contain keys, lengths, and value bytes. Loading validates the file and reconstructs the in-memory index. A defined byte format keeps persistence independent of C++ object layout and hash placement, at the cost of parsing and rebuilding during startup.

Saving writes an exclusive temporary sibling, syncs the file, and publishes it with a same-filesystem hard link. Existing destinations are preserved. Versioned filenames and snapshot handles provide the building blocks for application-managed refreshes.

See [Architecture](docs/ARCHITECTURE.md) for the complete layout, construction cases, state machine, file format, and failure behavior.

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
build/release/kvquery query "$demo_dir/data.kwv" --strategy amac --window 32 7 8 0 7
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
    auto result = snapshot->query(keys, {kvquery::Strategy::amac, 32});
    snapshot.reset();
    for (size_t i = 0; i < result.size(); ++i) {
        if (result[i]) std::cout << keys[i] << ": " << *result[i] << '\n';
        else std::cout << keys[i] << ": missing\n";
    }
}
```

Link the CMake target `kvquery`. Keep either the result batch or its source snapshot alive while using value views. [API reference](docs/API.md) covers options, budgets, persistence, and errors.

## Performance evaluation

The benchmark compares `std::unordered_map`, scalar index lookup, lookahead prefetch, and AMAC over identical inputs. Workloads vary table size, batch size, value length, hit rate, and access distribution. Reports include throughput and batch P50/P95/P99 wall latency, together with commands, compiler, hardware, and source hashes.

The [benchmark guide](docs/BENCHMARKS.md) includes the reference design's published server evaluation and explains how to run the current engine's server suite. Reference results are organized by their source implementation and experiment configuration.

```sh
build/release/kvquery_bench --records 1000000 --batch 256 --queries 262144 --rounds 3
python3 bench/run_suite.py --output bench/results/server-suite.json
python3 bench/summarize.py --input bench/results/server-suite.json --output bench/results/server-report.md
```

## Validation

Nine CTest groups cover edge values, forced collisions and relocation, randomized differential lookup, result ownership, concurrent access, persistence failures, golden file bytes, CLI integration, and benchmark output validation. Linux CI builds GCC Release, Clang Debug, and Clang ASan/UBSan configurations.

```sh
cmake -S . -B build/asan -DKVQUERY_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build/asan -j
ctest --test-dir build/asan --output-on-failure
```

ThreadSanitizer is available in a separate build through `-DKVQUERY_TSAN=ON`. See [Validation](docs/VALIDATION.md).

## Source and license

The engine adapts an existing neighbor-hash index and AMAC implementation. [PROVENANCE](docs/PROVENANCE.md) records the source and project changes.

Original project contributions use the **PolyForm Noncommercial License 1.0.0**. Commercial use is not licensed. See [LICENSE](LICENSE) and [NOTICE](NOTICE).
