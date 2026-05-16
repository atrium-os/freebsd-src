#!/bin/sh
# skew.sh: three groups of N spinners at nice -5, 0, +5 for T seconds.
# Emit per-group CPU totals; ratio shows how much the scheduler honors
# nice for proportional share.
#
# usage: skew.sh [n=4] [duration=10] [cpu=]
#   cpu=N pins all work to one CPU.

set -eu

N=${1:-4}
T=${2:-10}
CPU=${3:-}

DIR=$(dirname "$0")
SPIN="$DIR/spin"
[ -x "$SPIN" ] || { echo "spin not built; run 'make' in $DIR"; exit 1; }

SCHED=$(sysctl -n kern.sched.name)
echo "=== skew.sh: scheduler=$SCHED N=$N per-group T=${T}s ==="

out_lo=$(mktemp -t skew.XXXXXX)
out_zero=$(mktemp -t skew.XXXXXX)
out_hi=$(mktemp -t skew.XXXXXX)

if [ -n "$CPU" ]; then
    cpuset -l "$CPU" "$SPIN" "$N" "$T" -5 > "$out_lo" &
    cpuset -l "$CPU" "$SPIN" "$N" "$T"  0 > "$out_zero" &
    cpuset -l "$CPU" "$SPIN" "$N" "$T" +5 > "$out_hi" &
else
    "$SPIN" "$N" "$T" -5 > "$out_lo" &
    "$SPIN" "$N" "$T"  0 > "$out_zero" &
    "$SPIN" "$N" "$T" +5 > "$out_hi" &
fi
wait

mean_of() {
    awk '/^summary:/ {
        for (i = 1; i <= NF; i++) if ($i ~ /^mean=/) { sub("mean=","",$i); print $i; exit }
    }' "$1"
}

m_lo=$(mean_of "$out_lo")
m_zero=$(mean_of "$out_zero")
m_hi=$(mean_of "$out_hi")

echo "group nice=-5 mean cpu/child = ${m_lo}s"
echo "group nice= 0 mean cpu/child = ${m_zero}s"
echo "group nice=+5 mean cpu/child = ${m_hi}s"
echo "ratio (lo/zero : 1 : hi/zero) = $(echo "$m_lo $m_zero $m_hi" | \
    awk '{printf "%.2f : 1.00 : %.2f\n", $1/$2, $3/$2}')"

rm -f "$out_lo" "$out_zero" "$out_hi"
