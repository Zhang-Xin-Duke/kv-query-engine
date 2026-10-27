"""Validate concurrency scheduling, sampled latency and benchmark JSON."""
import json
import os
import subprocess
import sys

exe = sys.argv[1]
env = dict(os.environ)
for key in ('KVQUERY_BENCH_CPUS', 'KVQUERY_BENCH_THREADS', 'KVQUERY_BENCH_STRATEGY'):
    env.pop(key, None)
for batch in (1, 256):
    p = subprocess.run([exe, '1000', '6', str(batch), '0.05', '1', 'smoke'],
                       text=True, capture_output=True, env=env, check=True)
    d = json.loads(p.stdout)
    assert len(d['results']) == 6
    assert d['cpu_order'] and d['records'] == 1000
    assert {(r['threads'], r['strategy']) for r in d['results']} == {
        (t, s) for t in (1, 2) for s in ('scalar', 'amac', 'simd')}
    for r in d['results']:
        assert r['checksums_verified'] and r['latency_samples'] > 0
        assert 0 <= r['batch_p50_ns'] <= r['batch_p95_ns'] <= r['batch_p99_ns']
        assert abs(r['keys_per_second'] / r['batch_requests_per_second'] - batch) < .001
bad_env = dict(env, KVQUERY_BENCH_CPUS='65535')
assert subprocess.run([exe, '1000', '6', '256', '.05', '1', 'smoke'],
                      env=bad_env, capture_output=True).returncode != 0
print('concurrency benchmark verification passed')
