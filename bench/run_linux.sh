#!/usr/bin/env bash
set -euo pipefail

# Run from the repository root. Uses existing Linux tools without installing packages.
output_prefix=${1:-bench/results/linux-suite}
cpu=${2:-0}
cxx_binary=${CXX:-g++}
mkdir -p build/server "$(dirname "$output_prefix")"

{
  date -u +'%Y-%m-%dT%H:%M:%SZ'
  uname -sr
  lscpu
  free -m
  "$cxx_binary" --version
  printf 'Benchmark affinity: CPU %s\n' "$cpu"
  printf 'Flags: -std=c++20 -O3 -DNDEBUG; no -march=native\n'
  grep -E '^(pswpin|pswpout|pgmajfault) ' /proc/vmstat
} > "${output_prefix}.environment.txt"

"$cxx_binary" -std=c++20 -O3 -DNDEBUG -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
  -Iinclude -c src/kvquery.cpp -o build/server/kvquery.o
ar rcs build/server/libkvquery.a build/server/kvquery.o
"$cxx_binary" -std=c++20 -O3 -DNDEBUG -Iinclude tests/kvquery_test.cpp \
  build/server/libkvquery.a -pthread -o build/server/kvquery_test
"$cxx_binary" -std=c++20 -O3 -DNDEBUG -Iinclude tools/kvquery_cli.cpp \
  build/server/libkvquery.a -o build/server/kvquery
"$cxx_binary" -std=c++20 -O3 -DNDEBUG -Iinclude bench/kvquery_bench.cpp \
  build/server/libkvquery.a -o build/server/kvquery_bench

{
  build/server/kvquery_test all
  python3 tests/cli_test.py build/server/kvquery
  python3 tests/benchmark_test.py build/server/kvquery_bench
} | tee "${output_prefix}.tests.txt"

taskset -c "$cpu" python3 bench/run_suite.py --binary build/server/kvquery_bench \
  --output "${output_prefix}.json" --repeats 3 \
  --build-description "Direct ${cxx_binary} -std=c++20 -O3 -DNDEBUG; no -march=native"
python3 bench/summarize.py --input "${output_prefix}.json" --output "${output_prefix}.md"
{
  printf '\nAfter benchmark:\n'
  free -m
  grep -E '^(pswpin|pswpout|pgmajfault) ' /proc/vmstat
} >> "${output_prefix}.environment.txt"
