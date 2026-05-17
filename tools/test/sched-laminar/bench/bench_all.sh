#!/bin/sh
# bench_all.sh: drive every bench_*.sh, tag each line with current
# scheduler, and emit a single combined report.  Intended workflow:
#
#   1. Boot under Laminar          (kern.sched.name=Laminar)
#   2. ./bench_all.sh > laminar.txt
#   3. Reboot under ULE            (unset kern.sched.name in loader)
#   4. ./bench_all.sh > ule.txt
#   5. diff -u ule.txt laminar.txt (or just read side by side)
#
# Each sub-bench prefixes every line with its scheduler name, so
# the two output files diff cleanly with the scheduler tag in the
# left column.

set -eu

DIR=$(dirname "$0")
SCHED=$(sysctl -n kern.sched.name)

echo "=================================================================="
echo "  Scheduler: $SCHED"
echo "  Uptime:    $(uptime)"
echo "  Date:      $(date)"
echo "  Build:     $(uname -v | head -c 70)..."
echo "=================================================================="
echo

for b in throughput fair skew burst latency ipc; do
    name="$DIR/bench_${b}.sh"
    [ -x "$name" ] || { echo "[skip $b: $name not executable]"; continue; }
    echo "--- bench_${b} ---"
    "$name" 2>&1 || echo "(bench_${b} failed)"
    echo
done

echo "=================================================================="
echo "  END  ($SCHED)"
echo "=================================================================="
