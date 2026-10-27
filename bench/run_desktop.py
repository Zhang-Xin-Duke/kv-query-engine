#!/usr/bin/env python3
"""Record single-thread suites and closed-loop concurrent pressure on this host."""
import argparse
import datetime
import hashlib
import json
import os
import pathlib
import platform
import subprocess
import sys

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--binary-dir', default='build/tuned')
p.add_argument('--output', required=True)
p.add_argument('--cpus', required=True, help='allowed logical CPUs in worker placement order')
p.add_argument('--cpu-name', default=platform.processor())
p.add_argument('--seconds', type=float, default=2)
p.add_argument('--rounds', type=int, default=3)
p.add_argument('--soak-seconds', type=float, default=20)
p.add_argument('--records', type=int, default=16_000_000)
p.add_argument('--build-description', required=True)
a = p.parse_args()
cpus = [int(x) for x in a.cpus.split(',')]
if not cpus or min(cpus) < 0 or not 0 < a.seconds <= 3600 or not 0 < a.soak_seconds <= 3600 or not 1 <= a.rounds <= 20:
    p.error('invalid timing, CPU, or round options')
repo = pathlib.Path(__file__).resolve().parents[1]
os.chdir(repo)
out = pathlib.Path(a.output).resolve()
out.mkdir(parents=True, exist_ok=True)
bin_dir = pathlib.Path(a.binary_dir)
suffix = '.exe' if sys.platform.startswith(('cygwin', 'win')) else ''
bench = bin_dir / ('kvquery_bench' + suffix)
concurrent = bin_dir / ('kvquery_concurrency_bench' + suffix)
build = a.build_description
source_files = [pathlib.Path('CMakeLists.txt'), *pathlib.Path('src').glob('*.cpp'),
                *pathlib.Path('include').rglob('*.hpp'), *pathlib.Path('bench').glob('*.cpp'),
                *pathlib.Path('bench').glob('*.py')]
hashes = {str(f): hashlib.sha256(f.read_bytes()).hexdigest() for f in source_files}
manifest = dict(created_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                source_revision=subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip(),
                source_dirty=bool(subprocess.check_output(['git', 'status', '--porcelain', '--untracked-files=no'], text=True).strip()),
                source_sha256=hashes, binary_sha256={str(f): hashlib.sha256(f.read_bytes()).hexdigest() for f in [bench, concurrent]},
                host=dict(os=platform.platform(), cpu=a.cpu_name), build=build, cpu_order=cpus,
                hit_target=.9, distribution='uniform', pressure_cases=[], single_thread_suites=[],
                notes='Closed-loop in-process calls. Three rounds within one pressure process per case; single-thread suites use three independent processes. Latency samples one batch call in 64; excludes queueing/network.')
for suite in ('standard', 'inline', 'inline-scale'):
    path = out / (suite + '.json')
    affinity_runner = "import os,runpy,sys;cpu=int(sys.argv.pop(1));os.sched_setaffinity(0,{cpu});sys.argv[0]='bench/run_suite.py';runpy.run_path(sys.argv[0],run_name='__main__')"
    cmd = [sys.executable, '-c', affinity_runner, str(cpus[0]), '--binary', str(bench),
           '--output', str(path), '--suite', suite, '--build-description', build]
    print('Starting single-thread ' + suite, flush=True)
    subprocess.run(cmd, check=True)
    d = json.loads(path.read_text())
    d['host']['cpu'] = a.cpu_name
    path.write_text(json.dumps(d, indent=2) + '\n')
    summary = 'bench/summarize.py' if suite == 'standard' else 'bench/summarize_inline.py'
    subprocess.run([sys.executable, summary, '--input', str(path), '--output', str(path.with_suffix('.md'))], check=True)
    manifest['single_thread_suites'].append(dict(name=suite, command=cmd, file=path.name))
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
cases = [('external-batch256',32,256,'full',None,None,a.seconds),
         ('inline-batch256',6,256,'full',None,None,a.seconds),
         ('external-batch1',32,1,'focused',None,None,a.seconds),
         ('external-batch1024',32,1024,'focused',None,None,a.seconds),
         ('external-soak',32,256,'full','24','simd',a.soak_seconds),
         ('inline-soak',6,256,'full','8','amac',a.soak_seconds)]
for name, value_bytes, batch, mode, threads, strategy, seconds in cases:
    env = dict(os.environ, KVQUERY_BENCH_CPUS=a.cpus)
    for key in ('KVQUERY_BENCH_THREADS', 'KVQUERY_BENCH_STRATEGY'):
        env.pop(key, None)
    if threads: env['KVQUERY_BENCH_THREADS'] = threads
    if strategy: env['KVQUERY_BENCH_STRATEGY'] = strategy
    cmd = [str(concurrent), str(a.records), str(value_bytes), str(batch), str(seconds), str(a.rounds), mode]
    print('Starting pressure ' + name, flush=True)
    path = out / (name + '.json')
    with path.open('w') as f, path.with_suffix('.log').open('w') as log:
        subprocess.run(cmd, stdout=f, stderr=log, env=env, check=True)
    d = json.loads(path.read_text())
    assert all(r['checksums_verified'] for r in d['results'])
    manifest['pressure_cases'].append(dict(name=name, command=cmd, environment={k:env[k] for k in ('KVQUERY_BENCH_CPUS','KVQUERY_BENCH_THREADS','KVQUERY_BENCH_STRATEGY') if k in env}, file=path.name, measurements=len(d['results'])))
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print('Completed pressure ' + name, flush=True)
assert hashes == {str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in source_files}, 'source changed'
assert manifest['binary_sha256'] == {str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in [bench, concurrent]}, 'binary changed'
print('All suites complete; source and binary hashes unchanged', flush=True)
