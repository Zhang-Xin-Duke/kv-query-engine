# Partitioned refresh, replicas, and hot keys

This optional module combines the repository's compact C++20 batch-query engine
with a partitioned snapshot loader and a small HTTP replica/client prototype.
The original in-process `kvquery::Snapshot` API remains available without the
online module or its Protobuf, Abseil and FlatBuffers dependencies.

```text
snapshot builder → partitioned files → replica A: shards → compact batch index
                                   ↘ replica B: shards → compact batch index
client: version check → local cache / coalescing → round-robin replica request
```

## Build

```bash
cmake -S . -B build/online -DCMAKE_BUILD_TYPE=Release \
  -DKVQUERY_ONLINE_INDEX=ON -DBUILD_TESTING=OFF
cmake --build build/online --parallel 2
```

The default build omits this module. With `KVQUERY_ONLINE_INDEX=ON`, CMake links
the worker to this repository's `kvquery` target and downloads pinned public
dependencies by default. C++20 and Python 3.9+ on POSIX are required for the
combined worker and client. Source files and rights notices remain subject to
[NOTICE](../NOTICE) and [LICENSE](../LICENSE).

## Prepare and query a snapshot

The builder accepts a TSV file with `uint64 key`, `uint64 generation`, and value
in each row, separated by tabs. For a generation-1 snapshot, every row must use
generation 1. The source dataset and its permissions are chosen by the operator;
the repository does not include a workload dataset.

```bash
build/online/online_index/online_index build records-v1.tsv snapshot-v1 8

# Start two replicas, in separate terminals.
python3 online_index/distributed/replica.py --name A \
  --worker build/online/online_index/online_index_worker --backend compact \
  --query-strategy simd --query-window 32 --snapshot snapshot-v1 --generation 1 --port 9101
python3 online_index/distributed/replica.py --name B \
  --worker build/online/online_index/online_index_worker --backend compact \
  --query-strategy simd --query-window 32 --snapshot snapshot-v1 --generation 1 --port 9102

# Repeat a user-supplied key within one long-lived client process.
python3 online_index/distributed/client.py hot \
  --replicas http://127.0.0.1:9101 http://127.0.0.1:9102 \
  --generation 1 --keys 0 0 5 --iterations 20 --capacity 64 --ttl 10
```

Use keys that exist in your input. `hot` prints the last result and process-local
counts; a new CLI invocation starts a new cache. Applications can also import
`HotKeyClient` from `online_index.distributed.hotkey` and reuse the object.

## Refresh and version boundary

The snapshot builder uses the existing partitioned file format. The compact
adapter decodes each shard and builds a `kvquery::Snapshot` for that shard. On
replacement, the replica stops admitting requests, drains admitted batches, and
replaces shards sequentially using the N+1 refresh and epoch reclamation logic.
It checks record generation, key hash, shard membership and total count before
announcing READY. Each batch is pinned to one generation and artifact digest;
failed attempts are discarded instead of combining partial results.

```bash
build/online/online_index/online_index build records-v2.tsv snapshot-v2 8
python3 online_index/distributed/client.py rollout \
  --replicas http://127.0.0.1:9101 http://127.0.0.1:9102 \
  --generation 2 --snapshot snapshot-v2 --step
```

Use a shared path accessible to the coordinator and every replica; snapshot
distribution is not implemented here. Rollout uses one coordinator host and a
local advisory lock. It does not offer leader election, consensus, simultaneous
global cutover or latest-value linearizability. A failed reload makes the
replica FAILED; restart from a valid snapshot. Long readers can delay reclamation.

## Hot-key controls

- A bounded TTL/LRU cache is scoped by generation and snapshot digest. Only
  positive results are cached. Status is still checked on every client batch.
- Duplicate keys in a batch are queried once. Concurrent requests for the same
  uncached key share one in-flight lookup, subject to a bounded in-flight limit.
- READY replicas for the requested artifact are selected round-robin; failed
  endpoints enter a short client-side cooldown. The number of attempts is bounded.
- Each replica rejects batches above `--max-active`. Its C++ worker processes
  commands serially; the HTTP wrapper is a protocol prototype, not a production
  throughput server or an exhaustive connection-level overload defense.
- The client can report a bounded window of locally observed key counts. LRU
  eviction makes this approximate; there is no global detector or automatic
  scaling policy.

For replicas on different hosts, use the scheduler to place them on distinct
failure domains. `client.py placement --replicas URL_A URL_B` checks that all
listed replicas are READY and report distinct domains; `--failure-domain`
overrides a replica's host-name default. This audit does not schedule or move
replicas. A single surviving replica can still serve a matching version even
though the placement audit reports lost redundancy.

## Network scope

The service binds to `127.0.0.1` by default. A non-loopback `--listen` address
requires `--require-auth` and a nonempty `INDEX_ENGINE_TOKEN` on both replica
and client processes. The protocol uses plaintext HTTP: bearer credentials and
values are not encrypted in transit. Use a protected transport such as TLS for
an untrusted network, and restrict the refresh endpoint to trusted operators.
The prototype does not itself implement TLS or operator/user authorization
separation.

## Validation scope

Real MovieLens-1M snapshots were used locally for correctness: sampled hits,
misses and duplicates matched the existing `neighbor-pool` backend before and
after refresh for scalar, prefetch, AMAC and SIMD. A separate 100-record check
used real rating fields as short values; all 100 were inlined and returned
correctly. The prior standalone-query benchmarks do not measure the HTTP layer,
the compact-shard adapter or its refresh memory. The adapter builds temporary
decoded records, so its refresh RSS and latency must be measured independently.

### Existing refresh-backend measurements

An earlier NCShare experiment on the included `neighbor-pool` backend used real
MovieLens-32M snapshots, eight shards, one replacement loader and four fresh
processes per condition. The table reports median process peak RSS and refresh
time; both controls used the `reclaim` page-return policy after safe reclamation.

| Refresh strategy | Reclamation interval | Peak RSS | Refresh time |
| --- | ---: | ---: | ---: |
| N+1 | every 2 old shards | 10.092 GiB | 40.252 s |
| Whole-table double buffer | once after the old table | 15.324 GiB | 36.587 s |

For that workload, N+1 used 34.14% less peak RSS and took about 10.0% longer.
The controlled whole-table comparison was run in a local benchmark harness,
not through the HTTP worker. These numbers do not establish the compact backend's
refresh cost. `reclaim` is an optional glibc-specific page-return policy; the
default `none` policy avoids its synchronous work, and process RSS depends on
allocator behavior and input size.

### Two-node hot-key check

One NCShare run used two distinct shared compute nodes, one replica per node,
with two requested CPU cores and 4 GiB per node. Both replicas loaded the same
MovieLens-1M snapshot and used the compact SIMD worker with window 17. A serial
client sent 200 single-key batches per trial. The records were real; the access
sequence was controlled: 80% of requests used one key and the rest cycled among
four other keys. This is a demonstration workload, not a production trace.

| Client cache | Backend batch RPC attempts per trial | End-to-end elapsed time |
| --- | ---: | ---: |
| Disabled | 200, 200 | 3.890 s, 3.902 s |
| 64 entries, 60 s TTL | 5, 5 | 2.599 s, 2.607 s |

Trials ran in the order disabled, enabled, enabled, disabled. Each client batch
also checked both replicas' status, including cache hits; those status requests
are not counted in the backend batch RPC column. The results show the effect in
this small controlled run, not an established throughput or tail-latency gain.
The same run checked even replica selection, a version transition and service
after one replica process was terminated. It built the consolidated repository's
worker and query engine together inside the NCShare allocation.
