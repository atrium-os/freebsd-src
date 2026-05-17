#!/bin/sh
# bench_skew.sh: 3 groups of N spinners at nice -5, 0, +5; reports
# per-group iteration mean and lo:zero:hi ratio.  Target ratio
# under CFS-style nice weighting is 3.05 : 1.00 : 0.33 (~10% per
# nice unit).  ULE uses interactivity heuristics so its ratio
# tends to be flatter unless the threads register as
# "non-interactive" first.

set -eu
DIR=$(dirname "$0")
SPIN="$DIR/../spin"
SCHED=$(sysctl -n kern.sched.name)

T=8
N=3

for cpu in "" 0; do
    if [ -z "$cpu" ]; then label="all-cpus";  PIN="";
    else                     label="cpu0-only"; PIN="cpuset -l 0"; fi

    out_lo=$(mktemp -t bs.XXXXXX)
    out_z=$(mktemp -t bs.XXXXXX)
    out_hi=$(mktemp -t bs.XXXXXX)
    $PIN "$SPIN" "$N" "$T" -5 > "$out_lo" &
    $PIN "$SPIN" "$N" "$T" 0  > "$out_z"  &
    $PIN "$SPIN" "$N" "$T" +5 > "$out_hi" &
    wait
    m() { awk '/^summary:/ { for(i=1;i<=NF;i++) if($i ~ /^mean=/){ sub("mean=","",$i); printf "%.0f", $i; exit } }' "$1"; }
    m_lo=$(m "$out_lo"); m_z=$(m "$out_z"); m_hi=$(m "$out_hi")
    ratio=$(echo "$m_lo $m_z $m_hi" | awk '{ if($2>0) printf "%.2f:1.00:%.2f", $1/$2, $3/$2; else print "n/a"; }')
    printf "%-12s %-12s nice-5_iters=%-10s 0_iters=%-10s +5_iters=%-10s ratio=%s (target 3.05:1.00:0.33)\n" \
        "$SCHED" "$label" "$m_lo" "$m_z" "$m_hi" "$ratio"
    rm -f "$out_lo" "$out_z" "$out_hi"
done
