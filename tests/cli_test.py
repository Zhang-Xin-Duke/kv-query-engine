import json
import pathlib
import subprocess
import sys
import tempfile

exe = sys.argv[1]
def run(*args, ok=True):
    result = subprocess.run([exe, *map(str, args)], capture_output=True, text=True)
    assert (result.returncode == 0) == ok, (args, result.stdout, result.stderr)
    return result.stdout

with tempfile.TemporaryDirectory(prefix="kvquery-cli-") as tmp:
    root = pathlib.Path(tmp)
    source, snapshot = root / "input.tsv", root / "sample.kwv"
    source.write_text("0\t\n7\thello\n18446744073709551615\tworld\n")
    assert run("build", source, snapshot) == "saved 3 records\n"
    assert json.loads(run("inspect", snapshot))["records"] == 3
    expected = "7\tFOUND\t68656c6c6f\n8\tMISSING\n0\tFOUND\t\n7\tFOUND\t68656c6c6f\n18446744073709551615\tFOUND\t776f726c64\n"
    for strategy in ("scalar", "prefetch", "amac"):
        assert run("query", snapshot, "--strategy", strategy, "--window", 3,
                   7, 8, 0, 7, 18446744073709551615) == expected
    old = snapshot.read_bytes()
    run("build", source, snapshot, ok=False)
    assert snapshot.read_bytes() == old
    run("query", snapshot, "--window", 0, 1, ok=False)
    run("query", snapshot, "--strategy", "unknown", 1, ok=False)
    run("query", snapshot, -1, ok=False)
    run("query", snapshot, 18446744073709551616, ok=False)
    source.write_text("1\ta\n1\tb\n")
    run("build", source, root / "duplicate.kwv", ok=False)
    source.write_text("missing tab\n")
    run("build", source, root / "invalid.kwv", ok=False)
    assert not list(root.glob("*.tmp.*"))
print("CLI roundtrip and errors passed")
