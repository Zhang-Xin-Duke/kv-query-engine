import json
import subprocess
import sys

exe = sys.argv[1]
for distribution in ("uniform", "hot"):
    result = subprocess.run([exe, "--records", "1000", "--queries", "4096",
                             "--batch", "33", "--rounds", "2", "--distribution", distribution],
                            text=True, capture_output=True, check=True)
    report = json.loads(result.stdout)
    assert len(report["results"]) == 8
    assert len({r["checksum"] for r in report["results"]}) == 1
    assert all(r["samples"] == 250 for r in report["results"])
    assert all(r["keys_per_second"] > 0 for r in report["results"])
    assert all(r["batch_p50_ns"] <= r["batch_p95_ns"] <= r["batch_p99_ns"] for r in report["results"])
for args in (("--batch", "0"), ("--hit-rate", "nan"), ("--hit-rate", "0.9junk"),
             ("--distribution", "unknown"), ("--records", "-1")):
    assert subprocess.run([exe, *args], capture_output=True).returncode != 0
print("benchmark validation and JSON schema passed")
