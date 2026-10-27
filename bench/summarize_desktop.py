#!/usr/bin/env python3
"""Validate and summarize a run_desktop.py result directory."""
import argparse
import csv
import json
import pathlib
import statistics

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--input', required=True)
a = p.parse_args()
root = pathlib.Path(a.input)
repo = pathlib.Path(__file__).resolve().parents[1]
m = json.loads((root/'manifest.json').read_text())
rows = []
pressure = {}
for case in m['pressure_cases']:
    d = json.loads((root/case['file']).read_text())
    pressure[case['name']] = d
    for t in sorted({r['threads'] for r in d['results']}):
        for strategy in sorted({r['strategy'] for r in d['results']}):
            group = [r for r in d['results'] if r['threads']==t and r['strategy']==strategy]
            assert len(group)==d['rounds']
            for r in group:
                assert r['checksums_verified'] and r['latency_samples']>0
                assert r['batch_p50_ns']<=r['batch_p95_ns']<=r['batch_p99_ns']
                assert abs(r['keys_per_second']/r['batch_requests_per_second']-d['batch'])<.001
            med = lambda k: statistics.median(r[k] for r in group)
            rows.append(dict(case=case['name'],threads=t,strategy=strategy,batch=d['batch'],
                             mkeys_s=med('keys_per_second')/1e6,min_mkeys_s=min(r['keys_per_second'] for r in group)/1e6,
                             max_mkeys_s=max(r['keys_per_second'] for r in group)/1e6,batch_qps=med('batch_requests_per_second'),
                             p50_us=med('batch_p50_ns')/1000,p95_us=med('batch_p95_ns')/1000,p99_us=med('batch_p99_ns')/1000))
with (root/'pressure-summary.csv').open('w',newline='') as f:
    w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
standard = json.loads((root/'standard.json').read_text())
duke = json.loads((repo/'bench/results/duke-x86-inline-2026-10-24.json').read_text())
for name in ('standard','inline','inline-scale'):
    d=json.loads((root/(name+'.json')).read_text())
    for c in d['cases']:
        r=c['report']['results']
        assert len({x['checksum'] for x in r})==1
        assert all(x['batch_p50_ns']<=x['batch_p95_ns']<=x['batch_p99_ns'] for x in r)
def rate(d,name,s):
    return statistics.median(next(r['keys_per_second'] for r in c['report']['results'] if r['strategy']==s and r['window']==(1 if s=='scalar' else 32)) for c in d['cases'] if c['name']==name)/1e6
def row(case,t,s):return next(r for r in rows if r['case']==case and r['threads']==t and r['strategy']==s)
lines=['# Desktop benchmarks and concurrency pressure test','',
       'Measured October 27, 2026 on an Intel Core Ultra 9 285K (8 performance cores + 16 efficiency cores), approximately 64 GB RAM and 36 MiB L3. This is a CPU query engine; the GPU is unused.','',
       'Windows 11/Cygwin, GCC 14.4.0, CMake Release: `-O3 -DNDEBUG -std=gnu++20 -msse4.1 -mtune=native`. Native comparison backend: `sse4.1`. Default builds retain SSE2 or the portable/NEON paths. No `-march=native` is used; that broader flag failed earlier local correctness checks.','',
       'All 12 CTest groups passed in SSE4.1, SSE2, and forced-portable builds (36 group passes). See the three test logs.','',
       '## Single-thread comparison with Duke','',
       'Throughput is million keys/s. Windows use 32; both suites run three independent processes per workload, with three rounds per process. Desktop runs are pinned to performance-core logical CPU 10. The engine code is optimized relative to Duke; OS, compiler, CPU, RAM and ISA also differ. Ratios therefore describe complete tested configurations, not isolated hardware gains.','',
       '| Workload | Duke Scalar | Desktop Scalar | Duke AMAC | Desktop AMAC | Duke SIMD | Desktop SIMD |',
       '|---|---:|---:|---:|---:|---:|---:|']
for name in dict.fromkeys(c['name'] for c in standard['cases']):
    vals=[rate(d,name,s) for s in ('scalar','amac','simd') for d in (duke,standard)]
    lines.append('| '+name+' | '+' | '.join(f'{v:.2f}' for v in vals)+' |')
lines+=['','Full single-thread tables: [standard](standard.md), [paired inline](inline.md), [1M/4M/8M scaling](inline-scale.md). The matching JSON files retain all windows, checksums, batch P50/P95/P99, seeds and source/binary hashes.','',
        '## Concurrent pressure and saturation','',
        'Every pressure case uses **16 million records**, 90% target hits, uniform independent per-worker traces of 1,048,576 keys, and one shared immutable snapshot. Logical storage is about 832 MB for 32-byte external values and 320 MB for six-byte inlined values; allocator overhead, input records and traces are additional memory. Build budgets are explicitly raised for the 16M-record case.','',
        'The sweep uses 1/2/4/8/16/24/32 workers. CPU order places the eight performance cores first; adding 16/24 workers includes efficiency cores. At 32 workers, the performance cores each have two pinned workers. Every worker uses window 32. These are heterogeneous-core placement results, not identical-core scaling.','',
        'Each configuration runs three **2-second timed rounds in the same process** with rotated strategy order, worker warmups and a synchronized start. Pressure cases do not use three independent processes. Latency is sampled once per 64 calls and includes result allocation/destruction and endpoint-byte consumption. P99 below is the median of the three round P99s, each pooling all workers.','',
        'Batch requests/s = keys/s divided by batch size. This is closed-loop, in-process capacity, with no transport, request queue, serialization or full-value copy. Call percentiles exclude queueing delay and do not establish an end-to-end service SLA. The amortized timer differs from the earlier single-thread benchmark, so use comparisons within each harness.']
for case in ('external-batch256','inline-batch256'):
    lines+=['','### '+case,'','Units: million keys/s, except the request-rate and latency columns.','',
            '| Workers | Scalar | AMAC | SIMD | SIMD batch requests/s | SIMD P99 µs |','|---:|---:|---:|---:|---:|---:|']
    for t in sorted({r['threads'] for r in pressure[case]['results']}):
        rs=[row(case,t,s) for s in ('scalar','amac','simd')];c=rs[2]
        lines.append(f'| {t} | '+ ' | '.join(f'{r["mkeys_s"]:.2f}' for r in rs)+f' | {c["batch_qps"]:,.0f} | {c["p99_us"]:.2f} |')
    best=max((r for r in rows if r['case']==case),key=lambda r:r['mkeys_s'])
    lines+=['',f'Highest median tested: **{best["mkeys_s"]:.2f} Mkeys/s**, {best["batch_qps"]:,.0f} batch requests/s, {best["strategy"]}/{best["threads"]} workers; batch P99 {best["p99_us"]:.2f} µs.']
for case in ('external-batch1','external-batch1024'):
    lines+=['','### '+case,'','| Workers | Strategy | Mkeys/s | Batch requests/s | P50 µs | P95 µs | P99 µs |','|---:|---|---:|---:|---:|---:|---:|']
    for r in rows:
        if r['case']==case:lines.append(f'| {r["threads"]} | {r["strategy"]} | {r["mkeys_s"]:.2f} | {r["batch_qps"]:,.0f} | {r["p50_us"]:.2f} | {r["p95_us"]:.2f} | {r["p99_us"]:.2f} |')
lines+=['','## Longer pressure runs','',
        'Three **20-second timed rounds** per selected configuration follow the sweeps. They are separate processes from the short sweeps. These longer runs provide a limited stability check, not an hours-long thermal or availability test.','',
        '| Case | Strategy / workers | Mkeys/s min / median / max | Batch requests/s median | Batch P99 µs median |','|---|---|---:|---:|---:|']
for case in ('external-soak','inline-soak'):
    r=next(r for r in rows if r['case']==case)
    lines.append(f'| {case} | {r["strategy"]} / {r["threads"]} | {r["min_mkeys_s"]:.2f} / {r["mkeys_s"]:.2f} / {r["max_mkeys_s"]:.2f} | {r["batch_qps"]:,.0f} | {r["p99_us"]:.2f} |')
if 'single-key-confirmation' in pressure:
    lines+=['','## Single-key variability and confirmation','','The short sweep showed large single-worker variation. All rounds are retained, including slow rounds. A follow-up uses three 5-second rounds per strategy with one worker and the same 16M-record, 32-byte, batch-1 workload. This is a separate process after the soaks; the data below supplements the original sweep.','','| Case | Strategy | Mkeys/s min / median / max | Sampled P99 µs median |','|---|---|---:|---:|']
    for case in ('external-batch1','single-key-confirmation'):
        for r in rows:
            if r['case']==case and r['threads']==1:
                lines.append(f'| {case} | {r["strategy"]} | {r["min_mkeys_s"]:.3f} / {r["mkeys_s"]:.3f} / {r["max_mkeys_s"]:.3f} | {r["p99_us"]:.2f} |')
    lines+=['','A low sampled P99 can coexist with poor wall-time throughput: one-in-64 sampling and the 99th percentile can miss rare long pauses or scheduling stalls. Their cause was not isolated. This shared desktop was not controlled as an exclusive benchmark host. Single-key capacity and tail latency need a dedicated, longer investigation.']
lines+=['','## Interpretation and limits','',
        '- Large batched requests scale to high aggregate throughput, but worker count and value layout need tuning. Adding workers beyond the measured peak can reduce throughput and increase tail latency.',
        '- Single-key calls expose the high-concurrency limitation. Per-call vector allocation and shared snapshot ownership/reference counting are plausible contention costs; no hardware counters or ownership/allocation ablations were collected to attribute the bottleneck.',
        '- A reusable query context/output buffer that retains ownership across calls is a useful next experiment. It must preserve the API lifetime guarantees; no improvement from such a change is claimed here.',
        '- The fixed-size, uniform synthetic workload, one host and short sweeps limit generalization. Longer soak, open-loop offered-load tests, real value distributions and a network service are needed for deployment capacity/SLA decisions.',
        '- Every timed batch checksum was verified; initial full-byte checks and warmups run outside timing. P50 ≤ P95 ≤ P99 and request-rate units were also validated. See [pressure-summary.csv](pressure-summary.csv) for ranges and all percentile summaries.','',
        '## Provenance and reproduction','',
        'Measured local source commit: `'+m['source_revision']+'`. The equivalent published source commit is [`b745ea3`](https://github.com/Zhang-Xin-Duke/kv-query-engine/commit/b745ea3ea25fa01a99924631dc576266305205a8); both have Git tree `5ee41d780632b19fad3defb0de0b1781b3659640`. The local and connector-created commits have different author/committer metadata. Source content is identical. Working-tree hashes preserve the measured local line endings; `source_sha256_lf` additionally normalizes CRLF to LF for comparison with GitHub blobs. Source/binary SHA-256 hashes are in [manifest.json](manifest.json). Absolute workspace prefixes in published commands/logs are normalized to repository-relative paths; numeric results and hashes are unchanged.','',
        '```sh','cmake -S . -B build/tuned -DCMAKE_BUILD_TYPE=Release \\','  -DCMAKE_CXX_FLAGS="-msse4.1 -mtune=native"','cmake --build build/tuned -j8','ctest --test-dir build/tuned --output-on-failure','python3 bench/run_desktop.py --binary-dir build/tuned \\','  --output bench/results/my-desktop-run \\','  --cpus 10,11,12,13,22,23,0,1,2,3,4,5,6,7,8,9,14,15,16,17,18,19,20,21 \\','  --cpu-name "Intel Core Ultra 9 285K" \\','  --build-description "Windows/Cygwin GCC 14.4.0 Release -O3 -DNDEBUG -std=gnu++20 -msse4.1 -mtune=native"','python3 bench/summarize_desktop.py --input bench/results/my-desktop-run','```','',
        'Choose CPU IDs allowed on your host and verify topology before reusing the order above. The Cygwin affinity code targets the current Windows processor group. Linux uses its allowed CPU mask. Explicit SSE4.1 builds require compatible hardware. The supplied soak defaults select 24-worker SIMD and 8-worker AMAC; change the runner cases for other host topologies.']
(root/'README.md').write_text('\n'.join(lines)+'\n',encoding='utf-8')
validation=dict(single_thread_processes=sum(len(json.loads((root/(s+'.json')).read_text())['cases']) for s in ('standard','inline','inline-scale')),
                pressure_measurements=sum(len(d['results']) for d in pressure.values()),all_checksums_verified=True,percentiles_ordered=True,request_rate_units_valid=True)
(root/'validation.json').write_text(json.dumps(validation,indent=2)+'\n')
print(json.dumps(validation))
