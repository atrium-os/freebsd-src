#!/bin/sh
# bench_throughput.sh: aggregate iterations across N spinners for T
# seconds, normalised to single-spinner rate.  Iterations are the
# ground-truth work metric (rusage CPU time is tick-biased under
# contention on FreeBSD).
#
# A 4-CPU system should reach ~4.0x single-spinner throughput for
# N >= 4 and saturate from there.

set -eu
T=${1:-5}
DIR=$(dirname "$0")
SPIN="$DIR/../spin"
SCHED=$(sysctl -n kern.sched.name)

[ -x "$SPIN" ] || { echo "spin not built; cd .. && make"; exit 1; }

# Measure single-spinner baseline first.
out=$(mktemp -t bt.XXXXXX)
"$SPIN" 1 "$T" 0 > "$out" 2>&1
base=$(awk '/^aggregate:/ { for(i=1;i<=NF;i++) if($i ~ /^total_iters=/) { sub("total_iters=","",$i); print $i; exit } }' "$out")
rm -f "$out"

for N in 1 2 4 8 16 32; do
    out=$(mktemp -t bt.XXXXXX)
    "$SPIN" "$N" "$T" 0 > "$out" 2>&1
    total=$(awk '/^aggregate:/ { for(i=1;i<=NF;i++) if($i ~ /^total_iters=/) { sub("total_iters=","",$i); print $i; exit } }' "$out")
    spread=$(awk '/^summary:/ { for(i=1;i<=NF;i++) if($i ~ /\(/){ gsub("[()%]","",$i); printf "%.1f", $i; exit } }' "$out")
    ratio=$(echo "$total $base" | awk '{ if($2>0) printf "%.2f", $1/$2; else print "n/a" }')
    printf "%-12s N=%-3d T=%-2ds total_iters=%-12s vs-1-spinner=%-6sx spread=%s%%\n" \
        "$SCHED" "$N" "$T" "$total" "$ratio" "$spread"
    rm -f "$out"
done
