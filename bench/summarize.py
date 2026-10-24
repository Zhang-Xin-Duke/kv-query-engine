#!/usr/bin/env python3
"""Summarize a recorded server suite with fixed strategy/window comparisons."""
import argparse
import json
import pathlib
import statistics

parser = argparse.ArgumentParser()
parser.add_argument("--input", default="bench/results/server-suite.json")
parser.add_argument("--output", default="bench/results/server-report.md")
args = parser.parse_args()
report = json.loads(pathlib.Path(args.input).read_text())
cases = report["cases"]
if not cases:
    raise ValueError("the suite contains no recorded cases")
methods = [("unordered_map", 1), ("scalar", 1), ("prefetch", 32), ("amac", 32)]
simd_present = [any(row["strategy"] == "simd" and row["window"] == 32
                    for row in case["report"]["results"]) for case in cases]
if any(simd_present) and not all(simd_present):
    raise ValueError("mixed legacy and SIMD result sets")
if all(simd_present):
    methods.append(("simd", 32))
names = list(dict.fromkeys(case["name"] for case in cases))

def rows(name, strategy, window):
    return [next(row for row in case["report"]["results"]
                 if row["strategy"] == strategy and row["window"] == window)
            for case in cases if case["name"] == name]

def rate(name, strategy, window):
    return statistics.median(row["keys_per_second"] for row in rows(name, strategy, window)) / 1e6

host = report["host"]
text = f'''# Benchmark report

Recorded: {report['created_utc']}.
Host: {host['cpu']}, {host['os']} {host['release']}, {host['arch']}.
Compiler: `{cases[0]['report']['compiler']}`.
Build: {report['build']}.
Raw input: `{args.input}`.

## Median throughput

Units: million keys per second. Each row aggregates {report['repeats']} independent processes.
Prefetch, AMAC and SIMD (when available) use the same fixed window 32 in this table.

'''
labels = ["Map", "Scalar", "Prefetch/32", "AMAC/32"]
if all(simd_present):
    labels.append("SIMD/32")
text += "| Workload | Records | Batch | Value bytes | Hit target | " + " | ".join(labels) + " |\n"
text += "| --- | ---: | ---: | ---: | ---: | " + " | ".join(["---:"] * len(methods)) + " |\n"
for name in names:
    data = next(case["report"] for case in cases if case["name"] == name)
    values = [rate(name, s, w) for s, w in methods]
    text += (f"| {name} | {data['records']:,} | {data['batch']} | {data['value_bytes']} | "
             f"{data['requested_hit_rate']:.0%} | " + " | ".join(f"{v:.2f}" for v in values) + " |\n")
text += "\n## Process variation\n\n"
text += "| Workload | Strategy | Throughput min / median / max (Mkeys/s) | Batch P99 min / median / max (µs) |\n"
text += "| --- | --- | ---: | ---: |\n"
for name in names:
    for strategy, window in methods:
        data = rows(name, strategy, window)
        rates = [r["keys_per_second"] / 1e6 for r in data]
        p99 = [r["batch_p99_ns"] / 1000 for r in data]
        text += (f"| {name} | {strategy}/{window} | {min(rates):.2f} / {statistics.median(rates):.2f} / {max(rates):.2f} | "
                 f"{min(p99):.2f} / {statistics.median(p99):.2f} / {max(p99):.2f} |\n")
text += "\nPercentiles describe complete batch calls. The raw JSON retains all windows, commands, checksums and source/binary hashes.\n"
backends = sorted({case["report"].get("simd_backend", "not recorded") for case in cases})
text += "\nCompiled SIMD backend: " + ", ".join(backends) + ".\n"
output = pathlib.Path(args.output)
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text(text)
print(f"saved {output}")
