#!/bin/sh
# bench_ipc.sh: K pipe-pair processes ping-pong tokens for T
# seconds.  Higher round-trip rate / lower mean latency means
# better wake-then-run latency (and, for K > 1, less migration
# churn since IPC-affinity should keep pairs co-located).
#
# We also do an "IPC + background load" variant -- harder for the
# scheduler because it must keep IPC pairs together while still
# spreading background work.

set -eu
DIR=$(dirname "$0")
PINGPONG="$DIR/pingpong"
SPIN="$DIR/../spin"
SCHED=$(sysctl -n kern.sched.name)

[ -x "$PINGPONG" ] || { echo "pingpong not built; cd bench && make"; exit 1; }

T=5

# Single pair, no background.
out=$(mktemp -t bi.XXXXXX)
"$PINGPONG" "$T" > "$out" 2>&1
rt=$(awk '{ for(i=1;i<=NF;i++) if($i ~ /^rt_mean_us=/) { sub("rt_mean_us=","",$i); printf "%.2f",$i; exit } }' "$out")
n=$(awk '{ for(i=1;i<=NF;i++) if($i ~ /^round_trips=/) { sub("round_trips=","",$i); print $i; exit } }' "$out")
printf "%-12s pairs=1   bg_spinners=0 T=%ds round_trips=%-10s rt_mean_us=%s\n" \
    "$SCHED" "$T" "$n" "$rt"
rm -f "$out"

# 4 pairs, no background -- if the box has 4 CPUs, each pair gets a CPU.
outs=""
for i in 1 2 3 4; do
    o=$(mktemp -t bi.XXXXXX)
    outs="$outs $o"
    "$PINGPONG" "$T" > "$o" 2>&1 &
done
wait
sum_rt=0; sum_n=0
for o in $outs; do
    rt=$(awk '{ for(i=1;i<=NF;i++) if($i ~ /^rt_mean_us=/) { sub("rt_mean_us=","",$i); print $i; exit } }' "$o")
    n=$(awk '{ for(i=1;i<=NF;i++) if($i ~ /^round_trips=/) { sub("round_trips=","",$i); print $i; exit } }' "$o")
    sum_rt=$(echo "$sum_rt $rt" | awk '{ printf "%.2f", $1+$2 }')
    sum_n=$((sum_n + n))
    rm -f "$o"
done
mean_rt=$(echo "$sum_rt" | awk '{ printf "%.2f", $1/4 }')
printf "%-12s pairs=4   bg_spinners=0 T=%ds round_trips=%-10s rt_mean_us=%s (per-pair mean)\n" \
    "$SCHED" "$T" "$sum_n" "$mean_rt"

# 4 pairs + 4 background spinners -- contention.
outs=""
for i in 1 2 3 4; do
    o=$(mktemp -t bi.XXXXXX)
    outs="$outs $o"
    "$PINGPONG" "$T" > "$o" 2>&1 &
done
"$SPIN" 4 "$T" 0 > /dev/null 2>&1
wait
sum_rt=0; sum_n=0
for o in $outs; do
    rt=$(awk '{ for(i=1;i<=NF;i++) if($i ~ /^rt_mean_us=/) { sub("rt_mean_us=","",$i); print $i; exit } }' "$o")
    n=$(awk '{ for(i=1;i<=NF;i++) if($i ~ /^round_trips=/) { sub("round_trips=","",$i); print $i; exit } }' "$o")
    sum_rt=$(echo "$sum_rt $rt" | awk '{ printf "%.2f", $1+$2 }')
    sum_n=$((sum_n + n))
    rm -f "$o"
done
mean_rt=$(echo "$sum_rt" | awk '{ printf "%.2f", $1/4 }')
printf "%-12s pairs=4   bg_spinners=4 T=%ds round_trips=%-10s rt_mean_us=%s\n" \
    "$SCHED" "$T" "$sum_n" "$mean_rt"
