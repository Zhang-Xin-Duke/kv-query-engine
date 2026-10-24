#!/usr/bin/env python3
"""Summarize matched inline/external measurements; retain all raw observations."""
import argparse
import json
import pathlib
import statistics


def summarize(suite):
    lines = ["# Small-value inlining: server comparison", "",
             f"CPU: {suite['host']['cpu']}. Build: {suite['build']}.", "",
             f"{suite['repeats']} independent processes, three rounds each. Both layouts are",
             "resident in the same process and receive identical records and query traces.",
             "Layout order alternates across rounds. Throughput includes result allocation,",
             "lookup, length/end-byte consumption, ownership, and destruction. Full-byte",
             "equality is checked before timing. Values are returned as views in both layouts.", "",
             "Rates below are process medians in Mkeys/s; ratios are medians of paired",
             "per-process inline/external ratios. Ranges show all three process ratios.", "",
             "| Case | Scalar ext. | Scalar inline | Ratio | AMAC/32 ratio | SIMD/32 ext. | SIMD/32 inline | Ratio [min–max] | Logical bytes ext. → inline |",
             "|---|---:|---:|---:|---:|---:|---:|---:|---:|"]
    names = list(dict.fromkeys(c['name'] for c in suite['cases']))
    for name in names:
        cases = [c['report'] for c in suite['cases'] if c['name'] == name]
        for case in cases:
            assert case['compare_inline']
            assert len(case['results']) == 21
            assert len({r['checksum'] for r in case['results']}) == 1
            keys = {(r['strategy'], r['window'], r['layout']) for r in case['results']}
            assert len(keys) == 21

        def rates(strategy, window, layout):
            return [next(r['keys_per_second'] for r in c['results']
                         if (r['strategy'], r['window'], r['layout']) ==
                         (strategy, window, layout)) for c in cases]

        def pair(strategy, window):
            ext, inline = rates(strategy, window, 'external'), rates(strategy, window, 'inline')
            ratios = [i / e for e, i in zip(ext, inline)]
            return statistics.median(ext) / 1e6, statistics.median(inline) / 1e6, ratios

        se, si, sr = pair('scalar', 1)
        _, _, ar = pair('amac', 32)
        ve, vi, vr = pair('simd', 32)
        first = cases[0]
        before = first['index_bytes'] + first['external_control_value_bytes']
        after = first['index_bytes'] + first['value_storage_bytes']
        lines.append(f"| {name} | {se:.2f} | {si:.2f} | {statistics.median(sr):.3f}× | "
                     f"{statistics.median(ar):.3f}× | {ve:.2f} | {vi:.2f} | "
                     f"{statistics.median(vr):.3f}× [{min(vr):.3f}–{max(vr):.3f}] | "
                     f"{before:,} → {after:,} |")
    lines += ["", "Logical bytes count the successful node array and external value region;",
              "they exclude allocator metadata, input records, map baseline, result arrays,",
              "and temporary construction storage. Inlined payload occupies existing node",
              "bytes. The raw JSON includes every window, batch P50/P95/P99, input seed,",
              "inline counts, source hashes, binary hash, and CPU affinity.", ""]
    return '\n'.join(lines)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--input', required=True)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    report = json.loads(pathlib.Path(args.input).read_text())
    pathlib.Path(args.output).write_text(summarize(report))
