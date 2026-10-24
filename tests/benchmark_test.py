import json
import pathlib
import subprocess
import sys
import tempfile

exe = sys.argv[1]
for distribution in ("uniform", "hot"):
    result = subprocess.run([exe, "--records", "1000", "--queries", "4096",
                             "--batch", "33", "--rounds", "2", "--distribution", distribution],
                            text=True, capture_output=True, check=True)
    report = json.loads(result.stdout)
    assert len(report["results"]) == 11
    assert report["simd_backend"] in ("neon", "sse2", "portable")
    assert {r["window"] for r in report["results"] if r["strategy"] == "simd"} == {8, 32, 64}
    assert len({r["checksum"] for r in report["results"]}) == 1
    assert all(r["samples"] == 250 for r in report["results"])
    assert all(r["keys_per_second"] > 0 for r in report["results"])
    assert all(r["batch_p50_ns"] <= r["batch_p95_ns"] <= r["batch_p99_ns"] for r in report["results"])
    with tempfile.TemporaryDirectory(prefix="kvquery-summary-") as tmp:
        root = pathlib.Path(tmp)
        fixture = {"created_utc": "fixture", "host": {"cpu": "fixture", "os": "fixture",
                   "release": "fixture", "arch": "fixture"}, "build": "fixture", "repeats": 1,
                   "cases": [{"name": "fixture", "report": report}]}
        source, output = root / "suite.json", root / "summary.md"
        source.write_text(json.dumps(fixture))
        summarize = pathlib.Path(__file__).resolve().parents[1] / "bench" / "summarize.py"
        subprocess.run([sys.executable, str(summarize), "--input", str(source),
                        "--output", str(output)], check=True, capture_output=True)
        assert "SIMD/32" in output.read_text()
        assert report["simd_backend"] in output.read_text()
        # Older eight-method result files remain readable.
        report["results"] = [r for r in report["results"] if r["strategy"] != "simd"]
        source.write_text(json.dumps(fixture))
        subprocess.run([sys.executable, str(summarize), "--input", str(source),
                        "--output", str(output)], check=True, capture_output=True)
        assert "SIMD/32" not in output.read_text()
for args in (("--batch", "0"), ("--hit-rate", "nan"), ("--hit-rate", "0.9junk"),
             ("--distribution", "unknown"), ("--records", "-1")):
    assert subprocess.run([exe, *args], capture_output=True).returncode != 0
print("benchmark validation and JSON schema passed")
