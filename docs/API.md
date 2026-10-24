# API reference

```cpp
#include <kvquery/kvquery.hpp>
```

The CMake library target is `kvquery`. The public namespace is `kvquery`.

## Records and snapshots

```cpp
struct Record {
    uint64_t key;
    std::string value;
};

static std::shared_ptr<const Snapshot>
Snapshot::build(std::span<const Record> records, BuildOptions options = {});

static std::shared_ptr<const Snapshot>
Snapshot::load(const std::filesystem::path& path, LoadOptions options = {});

void Snapshot::save(const std::filesystem::path& path) const;
```

`Record::value` contains opaque bytes, including embedded zero bytes. All `uint64_t` keys are supported. Build copies input values into snapshot-owned storage; the input records may be released after success. Duplicate data keys are rejected.

A snapshot is immutable after construction. Copy its shared pointer to share the same data with other callers. Build or load another snapshot to prepare a refreshed dataset. The application controls where new requests obtain their handles.

Save publishes a new file and rejects an existing destination. The parent directory must exist. Load parses and validates records, then reconstructs the index. File-format version 1 uses little-endian integers and checksums; details are in [Architecture](ARCHITECTURE.md#6-持久化与加载).

## BuildOptions and LoadOptions

| Member | Type | Default | Meaning |
|---|---|---:|---|
| `load_factor` | `double` | `0.8` | Finite target strictly between zero and one |
| `max_records` | `uint64_t` | `10'000'000` | Input record count limit |
| `max_value_bytes` | `uint64_t` | `512ULL << 20` | Owned value region, including reserved byte and extended lengths |
| `max_index_bytes` | `uint64_t` | `512ULL << 20` | Logical node-array budget for every build attempt |

`LoadOptions` contains `max_file_bytes` (default `512ULL << 20`) and a nested `BuildOptions build`. File, value and index limits are separate. A large collection may need a larger file limit because serialized records include 16 bytes of metadata per value.

```cpp
kvquery::BuildOptions build;
build.load_factor = 0.75;
build.max_index_bytes = 256ULL << 20;
auto snapshot = kvquery::Snapshot::build(records, build);

kvquery::LoadOptions load;
load.max_file_bytes = 1ULL << 30;
load.build = build;
auto restored = kvquery::Snapshot::load("dataset-v2.kwv", load);
```

Build and load use temporary records/metadata as well as the final snapshot. These limits describe components rather than total process RSS.

## Queries

```cpp
enum class Strategy { scalar, prefetch, amac, simd };

struct QueryOptions {
    Strategy strategy = Strategy::scalar;
    size_t window = 32;
};

BatchResult Snapshot::query(std::span<const uint64_t> keys,
                            QueryOptions options = {}) const;
```

The input span must remain valid for the duration of the synchronous call. Empty input returns an empty batch. Results preserve input order and repeated query positions. All strategies validate a window from 1 through 256; scalar otherwise ignores it.

For prefetch, the window is input lookahead distance. For AMAC and SIMD, it is the maximum active-state count. AMAC accepts any supported window, including non-powers of two. SIMD compares pairs of full 64-bit candidate keys and retains divergent chain positions in a residual window; single active lanes use direct comparisons. Query work stays on the calling thread. To use several CPU threads, invoke const queries from the application's own workers.

```cpp
std::array<uint64_t, 5> keys{7, 8, 0, 7, 9};
auto result = snapshot->query(keys, {kvquery::Strategy::prefetch, 32});
```

## BatchResult and lifetime

```cpp
size_t BatchResult::size() const noexcept;
const std::optional<std::string_view>& BatchResult::operator[](size_t i) const;
std::span<const std::optional<std::string_view>> BatchResult::values() const noexcept;
```

`operator[]` performs bounds checking. Each engaged view points into immutable snapshot storage. A missing key produces `nullopt`; a stored empty value produces an engaged zero-length view.

```cpp
std::vector<kvquery::Record> data{{7, "hello"}, {0, ""}};
auto snapshot = kvquery::Snapshot::build(data);
std::array<uint64_t, 3> keys{7, 8, 0};
auto result = snapshot->query(keys);
snapshot.reset();
// result[0] remains "hello"; result[1] is missing; result[2] is present and empty.
std::string retained = std::string(*result[0]);
result = kvquery::BatchResult{};
// retained owns its bytes independently.
```

A result batch keeps its snapshot alive. Borrowed views copied out of a batch require that batch or another snapshot owner to remain alive. Destroying the last owner releases the complete snapshot.

## Stats

```cpp
Stats Snapshot::stats() const noexcept;
const char* strategy_name(Strategy) noexcept;
const char* simd_backend() noexcept; // "neon", "sse2", or "portable"
```

| Field | Meaning |
|---|---|
| `records` | Stored records |
| `buckets` | Node count of the successful index |
| `index_bytes` | `buckets * sizeof(FlatNode)` |
| `value_bytes` | Logical value-region bytes, including prefixes |
| `direct` | Direct home-slot inserts in the successful build |
| `collisions` | Same-chain inserts in the successful build |
| `relocations` | Inserts requiring lodger relocation in the successful build |
| `build_attempts` | Initial build plus any retries |

The byte counts describe owned logical arrays. They exclude allocator metadata, snapshot objects, caller-owned records and temporary build storage.

## Errors

Public operations throw standard C++ exceptions. Principal categories are:

- `invalid_argument`: malformed configuration, unsupported strategy/window, duplicate keys.
- `length_error`: count, length, capacity or resource limit violations; exhausted relative-link neighborhood after bounded retries.
- `runtime_error`: file opening, format, checksum or read failures.
- `system_error` / `filesystem_error`: filesystem and POSIX failures with underlying codes.
- `bad_alloc`: allocation failure.

Applications can catch `std::exception` at a request or load boundary, translate it to their status type, and retain the old snapshot when a new load fails. Error messages are diagnostic text.

## CLI

```text
kvquery build INPUT.tsv OUTPUT.kwv
kvquery inspect SNAPSHOT.kwv
kvquery query SNAPSHOT.kwv [--strategy scalar|prefetch|amac|simd] [--window N] KEY...
```

Build accepts unsigned decimal keys, a first-tab separator and text values. Blank lines are skipped and CRLF normalized. The CLI applies a 512 MiB TSV size limit. Inspect prints JSON statistics. Query prints one line per key with `FOUND` and hex value bytes or `MISSING`.

Exit status is 0 on success, 1 on a reported operation error, and 2 for command/usage errors. Numeric overflow and negative keys are rejected.

## SIMD build selection

Baseline ARM64 NEON or x86 SSE2 is selected at compile time. No AVX flags or new CPU requirements are added. Other targets use the portable two-key helper. Configure with `-DKVQUERY_FORCE_PORTABLE_SIMD=ON` to test the fallback explicitly. `simd_backend()` reports the backend compiled into the library; a window of 1 uses direct single-lane comparisons.
