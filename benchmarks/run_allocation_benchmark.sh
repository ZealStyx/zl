#!/usr/bin/env bash
#
# benchmarks/run_allocation_benchmark.sh - Phase 0 allocation benchmark
# driver (docs/memory-domains.md §9/§10).
#
# Runs benchmarks/AllocationBenchmark.zl once per workload configuration,
# each in its own process so the per-run peak RSS (ru_maxrss of the child)
# is the memory cost of exactly that workload. Collects the benchmark's
# machine-readable `bench ...` lines plus the child's peak RSS and prints a
# table, and writes the raw numbers to a JSON file for the before/after
# record (default: benchmarks/results/allocation_<tag>.json).
#
# Usage:
#   benchmarks/run_allocation_benchmark.sh [path/to/zl_language] [tag]
#
# With no binary argument it looks for ../build/zl_language. The tag names
# the result file (e.g. "before", "after") and is recorded in the JSON.
#
# No thresholds are hard-coded: performance numbers are host-dependent
# (see this directory's README). A regression is a semantic failure, not a
# wall-time number.

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

ZL=""
if [[ $# -ge 1 ]]; then
    ZL="$1"
elif [[ -x "$ROOT/build/zl_language" ]]; then
    ZL="$ROOT/build/zl_language"
fi

TAG="${2:-run}"
OUT="${ZL_BENCH_OUT:-$SCRIPT_DIR/results/allocation_${TAG}.json}"

if [[ -z "$ZL" || ! -x "$ZL" ]]; then
    echo "error: cannot find the zl_language runtime (build it, or pass the path)" >&2
    exit 2
fi
command -v python3 >/dev/null || { echo "error: python3 is required to drive the benchmark" >&2; exit 2; }

CONFIGS=(
    one_keep_100000
    eight_keep_100000
    one_keep_1000000
    eight_keep_1000000
    one_churn_1000000
    eight_churn_1000000
)

PYBIN="$(command -v python3)"
BENCH="$SCRIPT_DIR/AllocationBenchmark.zl"

"$PYBIN" - "$ZL" "$BENCH" "$OUT" "$TAG" "${CONFIGS[@]}" <<'PYEOF'
import json
import os
import re
import subprocess
import sys

zl, bench, out, tag, configs = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5:]

line_re = re.compile(
    r"^bench config=(?P<config>\S+) phase=(?P<phase>\S+) n=(?P<n>\d+)"
    r" wall_ms=(?P<wall_ms>\d+) alloc=(?P<alloc>\d+) collections=(?P<collections>\d+)"
    r" collect_ms=(?P<collect_ms>\d+) reused=(?P<reused>\d+)"
    r" tracked=(?P<tracked>\d+) peak=(?P<peak>\d+)$"
)

results = []
for cfg in configs:
    env = dict(os.environ, ZL_BENCH_CONFIG=cfg)
    proc = subprocess.Popen([zl, bench], env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    stdout, stderr = proc.stdout.read(), proc.stderr.read()
    # Reap the child through wait4 so ru_maxrss is THIS child's peak (the
    # aggregate RUSAGE_CHILDREN view is a high-water mark over all children
    # and would leak the biggest workload into every later row).
    _pid, _status, rusage = os.wait4(proc.pid, 0)
    rc = os.waitstatus_to_exitcode(_status)
    if rc != 0:
        sys.stderr.write(f"error: {cfg} exited {rc}\n{stderr}\n")
        sys.exit(1)
    # Peak RSS of exactly this workload (kB on Linux).
    peak_rss_kb = rusage.ru_maxrss
    for line in stdout.splitlines():
        m = line_re.match(line)
        if not m:
            continue
        row = m.groupdict()
        for key in ("n", "wall_ms", "alloc", "collections", "collect_ms", "reused", "tracked", "peak"):
            row[key] = int(row[key])
        row["config"] = cfg
        row["phase_rss_kb"] = peak_rss_kb
        # Derived, for the report table:
        row["throughput_per_s"] = int(row["alloc"] * 1000 / row["wall_ms"]) if row["wall_ms"] > 0 else 0
        row["collect_share_pct"] = round(100.0 * row["collect_ms"] / row["wall_ms"], 1) if row["wall_ms"] > 0 else 0.0
        row["fresh"] = row["alloc"] - row["reused"]
        results.append(row)

results.sort(key=lambda r: (r["config"], r["phase"]))
os.makedirs(os.path.dirname(out), exist_ok=True)
with open(out, "w") as f:
    json.dump({"tag": tag, "binary": zl, "rows": results}, f, indent=2)
    f.write("\n")

print(f"{'config':<20} {'phase':<6} {'n':>9} {'wall_ms':>9} {'alloc/s':>10} {'collect%':>9} {'collections':>11} {'reused':>8} {'peak_rss_kb':>12}")
for r in results:
    print(f"{r['config']:<20} {r['phase']:<6} {r['n']:>9} {r['wall_ms']:>9} {r['throughput_per_s']:>10}"
          f" {r['collect_share_pct']:>8.1f}% {r['collections']:>11} {r['reused']:>8} {r['phase_rss_kb']:>12}")
print(f"wrote {out}")
PYEOF
