#!/bin/sh
# bench_burst.sh: B background spinners + S short-sleep/burst
# threads.  The spinners measure baseline throughput; the bursters
# create wake events that exercise the wake-rebase + IPC-affinity
# paths.  Reports spinner iters mean+spread and total burst count.

set -eu
DIR=$(dirname "$0")
SPIN="$DIR/../spin"
SCHED=$(sysctl -n kern.sched.name)

T=10
B=4
S=4

out_bg=$(mktemp -t bb.XXXXXX)

burst_pids=""
counters=""
for i in $(seq 1 "$S"); do
    cnt=$(mktemp -t bbc.XXXXXX)
    counters="$counters $cnt"
    ( end=$(($(date +%s) + T))
      while [ "$(date +%s)" -lt "$end" ]; do
          sleep 0.005
          deadline=$(date +%s%N)
          deadline=$((deadline + 1000000))
          while [ "$(date +%s%N)" -lt "$deadline" ]; do :; done
          printf '.' >> "$cnt"
      done ) &
    burst_pids="$burst_pids $!"
done

"$SPIN" "$B" "$T" 0 > "$out_bg" 2>&1
for p in $burst_pids; do
    kill "$p" 2>/dev/null || true
done
wait 2>/dev/null

mean=$(awk '/^summary:/ { for(i=1;i<=NF;i++) if($i ~ /^mean=/){ sub("mean=","",$i); printf "%.0f", $i; exit } }' "$out_bg")
spread=$(awk '/^summary:/ { for(i=1;i<=NF;i++) if($i ~ /\(/){ gsub("[()%]","",$i); printf "%.1f", $i; exit } }' "$out_bg")
total_wakes=0
for c in $counters; do
    n=$(wc -c < "$c" | tr -d ' ')
    total_wakes=$((total_wakes + n))
    rm -f "$c"
done
wake_rate=$(echo "$total_wakes $T" | awk '{ printf "%.1f", $1/$2 }')
printf "%-12s B=%d S=%d T=%ds spinner_iters_mean=%-10s spread=%s%% burst_wakes=%d (%.1f/s)\n" \
    "$SCHED" "$B" "$S" "$T" "$mean" "$spread" "$total_wakes" "$wake_rate"
rm -f "$out_bg"
