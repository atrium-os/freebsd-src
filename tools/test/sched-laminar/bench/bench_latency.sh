#!/bin/sh
# bench_latency.sh: wake-from-sleep latency under load.  W watchdogs
# each sleep for 1ms then measure wake delta; B spinners create
# contention.  Reports p50/p90/p99/max in microseconds.

set -eu
DIR=$(dirname "$0")
WAKELAT="$DIR/wakelat"
SCHED=$(sysctl -n kern.sched.name)

[ -x "$WAKELAT" ] || { echo "wakelat not built; cd bench && make"; exit 1; }

T=5

for cfg in "0 1" "0 4" "4 1" "4 4" "8 4"; do
    set -- $cfg
    nspin=$1; nwatch=$2
    out=$("$WAKELAT" "$nspin" "$nwatch" "$T" 1000)
    # extract just the wake_latency line
    lat=$(echo "$out" | awk '/wake_latency_us/ {sub("^wake_latency_us ","");print}')
    printf "%-12s spin=%d watch=%d T=%ds %s\n" \
        "$SCHED" "$nspin" "$nwatch" "$T" "$lat"
done
