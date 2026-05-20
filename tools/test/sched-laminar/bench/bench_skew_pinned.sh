#!/bin/sh
# bench_skew_pinned.sh: skew measurement with placement isolated.
#
# Uses skew_pinned to pin each child to a specific CPU (parent-side
# before fork) so every CPU gets identical nice mix.  Removes
# placement-distribution variance from the measurement, leaving only
# the picker's nice-weighting behaviour.
#
# Compare against bench_skew.sh (unpinned, which mixes placement
# variance into the result).  When pinned skew passes target ratio
# but unpinned doesn't, the residual is purely placement noise.
#
# Built and run from the same directory as skew_pinned.

set -eu
DIR=$(dirname "$0")
SP="$DIR/skew_pinned"
SCHED=$(sysctl -n kern.sched.name)

[ -x "$SP" ] || { echo "skew_pinned not built; cd bench && cc -O2 -o skew_pinned skew_pinned.c"; exit 1; }

T=15

out=$(mktemp -t bsp.XXXXXX)
"$SP" "$T" > "$out" 2>&1

m_lo=$(awk '/^nice -5:/ { print $3 }' "$out")
m_z=$(awk '/^nice  0:/ { print $3 }' "$out")
m_hi=$(awk '/^nice \+5:/ { print $3 }' "$out")
ratio=$(awk '/^ratio = / { print $3 }' "$out")

printf "%-12s pinned-mix   nice-5_iters=%-10s 0_iters=%-10s +5_iters=%-10s ratio=%s (target 3.05:1.00:0.33)\n" \
    "$SCHED" "$m_lo" "$m_z" "$m_hi" "$ratio"

rm -f "$out"
