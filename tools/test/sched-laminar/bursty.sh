#!/bin/sh
# bursty.sh: B short-burst + C long-burst threads alongside S background
# spinners.  Bursty threads alternate sleep + brief run; long-sleepers
# arrive with a stale (low) vruntime and would dominate without LAG_CAP.
# Emit per-class totals; compare with kern.sched.lag_cap tuning on Laminar.
#
# usage: bursty.sh [duration=10] [cpu=]

set -eu

T=${1:-10}
CPU=${2:-}

DIR=$(dirname "$0")
SPIN="$DIR/spin"
[ -x "$SPIN" ] || { echo "spin not built; run 'make' in $DIR"; exit 1; }

SCHED=$(sysctl -n kern.sched.name)
LAG=$(sysctl -nq kern.sched.lag_cap 2>/dev/null || echo "(n/a)")
echo "=== bursty.sh: scheduler=$SCHED T=${T}s lag_cap=$LAG ==="

PIN=""
[ -n "$CPU" ] && PIN="cpuset -l $CPU"

# Background pressure: 4 spinners.
out_bg=$(mktemp -t bursty.XXXXXX)
$PIN "$SPIN" 4 "$T" > "$out_bg" &
bg_pid=$!

# Short bursts: sleep 200ms then burst 50ms in a loop.  Each burst
# child runs a 50ms tight loop.  We don't measure these with rusage;
# the goal is just to create wake events that exercise lag-cap.
short_pids=""
for i in 1 2 3 4; do
    ( while :; do
        sleep 0.2
        # burst ~50ms
        deadline=$(date +%s%N)
        deadline=$((deadline + 50000000))
        while [ "$(date +%s%N)" -lt "$deadline" ]; do :; done
      done ) &
    short_pids="$short_pids $!"
done

# Long bursts: sleep 1s then burst 200ms in a loop.
long_pids=""
for i in 1 2; do
    ( while :; do
        sleep 1
        deadline=$(date +%s%N)
        deadline=$((deadline + 200000000))
        while [ "$(date +%s%N)" -lt "$deadline" ]; do :; done
      done ) &
    long_pids="$long_pids $!"
done

# Wait for the background spinner test to finish, then stop bursters.
wait "$bg_pid"
for p in $short_pids $long_pids; do
    kill "$p" 2>/dev/null || true
done

echo "background spinner outcome:"
grep '^summary:' "$out_bg"
echo "background per-child mean low spread => bursters did not starve background"
rm -f "$out_bg"
