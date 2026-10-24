#!/usr/bin/env python3
"""Run deterministic synthetic workloads sequentially; preserve every raw result."""
import argparse
import datetime
import hashlib
import json
import os
import pathlib
import platform
import statistics
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("--binary", default="build/release/kvquery_bench")
parser.add_argument("--output", default="bench/results/server-suite.json")
parser.add_argument("--repeats", type=int, default=3)
parser.add_argument("--build-description", default="CMake Release, default optimization; no -march=native")
args = parser.parse_args()
if not 1 <= args.repeats <= 20:
    parser.error("repeats must be in [1,20]")
workloads = [
    ("small-uniform", 10_000, 256, 262144, 32, .9, "uniform"),
    ("large-single", 1_000_000, 1, 65536, 32, .9, "uniform"),
    ("large-batch32", 1_000_000, 32, 262144, 32, .9, "uniform"),
    ("large-batch256", 1_000_000, 256, 262144, 32, .9, "uniform"),
    ("large-batch1024", 1_000_000, 1024, 262144, 32, .9, "uniform"),
    ("large-low-hit", 1_000_000, 256, 262144, 32, .3, "uniform"),
    ("large-hot", 1_000_000, 256, 262144, 32, .9, "hot"),
    ("wide-values", 250_000, 256, 262144, 256, .9, "uniform"),
]
model = platform.processor()
if platform.system() == "Darwin":
    model = subprocess.check_output(["sysctl", "-n", "machdep.cpu.brand_string"], text=True).strip()
elif platform.system() == "Linux":
    cpuinfo = pathlib.Path("/proc/cpuinfo")
    if cpuinfo.exists():
        model = next((line.split(":", 1)[1].strip() for line in cpuinfo.read_text().splitlines()
                      if line.startswith("model name")), model)
source_files = [pathlib.Path("CMakeLists.txt"), pathlib.Path("src/kvquery.cpp"),
                pathlib.Path("bench/kvquery_bench.cpp"), pathlib.Path("bench/run_suite.py"),
                *sorted(pathlib.Path("include").rglob("*.hpp"))]
source_hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in source_files}
binary_hash = hashlib.sha256(pathlib.Path(args.binary).read_bytes()).hexdigest()
report = {"source_sha256": source_hashes, "binary_sha256": binary_hash, "created_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
          "host": {"os": platform.system(), "release": platform.release(),
                   "arch": platform.machine(), "cpu": model},
          "build": args.build_description,
          "cpu_affinity": sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else None,
          "repeats": args.repeats, "cases": []}
for name, records, batch, queries, value_bytes, hit, distribution in workloads:
    for repeat in range(args.repeats):
        command = [args.binary, "--records", str(records), "--batch", str(batch),
                   "--queries", str(queries), "--value-bytes", str(value_bytes),
                   "--hit-rate", str(hit), "--distribution", distribution,
                   "--seed", str(20261021 + repeat), "--rounds", "3"]
        raw = subprocess.check_output(command, text=True)
        result = json.loads(raw)
        report["cases"].append({"name": name, "repeat": repeat, "command": command, "report": result})
        print(f"{name} repeat {repeat + 1}/{args.repeats} complete", flush=True)
assert source_hashes == {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in source_files}, "source changed during benchmark"
assert binary_hash == hashlib.sha256(pathlib.Path(args.binary).read_bytes()).hexdigest(), "binary changed during benchmark"
output = pathlib.Path(args.output)
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text(json.dumps(report, indent=2) + "\n")
for name, *_ in workloads:
    cases = [case["report"] for case in report["cases"] if case["name"] == name]
    rows = []
    for index, first in enumerate(cases[0]["results"]):
        rates = [case["results"][index]["keys_per_second"] for case in cases]
        rows.append((statistics.median(rates), first["strategy"], first["window"]))
    best = max(rows)
    scalar = next(row[0] for row in rows if row[1] == "scalar")
    base = next(row[0] for row in rows if row[1] == "unordered_map")
    print(f"{name}: best={best[1]}/{best[2]} {best[0]/1e6:.2f} Mkeys/s; "
          f"vs scalar={best[0]/scalar:.2f}x; vs map={best[0]/base:.2f}x")
print(f"saved {output}")
