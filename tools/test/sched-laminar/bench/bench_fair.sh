#!/bin/sh
# bench_fair.sh: same-nice spinners; spread of iteration counts
# measures within-CPU fairness.  Longer T gives the balancer time
# to converge.

set -eu
DIR=$(dirname "$0")
SPIN="$DIR/../spin"
SCHED=$(sysctl -n kern.sched.name)

for N in 4 8 16; do
    for T in 5 15; do
        out=$(mktemp -t bf.XXXXXX)
        "$SPIN" "$N" "$T" 0 > "$out" 2>&1
        mean=$(awk '/^summary:/ { for(i=1;i<=NF;i++) if($i ~ /^mean=/){ sub("mean=","",$i); printf "%.0f", $i; exit } }' "$out")
        spread=$(awk '/^summary:/ { for(i=1;i<=NF;i++) if($i ~ /\(/){ gsub("[()%]","",$i); printf "%.1f", $i; exit } }' "$out")
        printf "%-12s N=%-3d T=%-3ds mean_iters=%-12s spread=%s%% (lower=fairer)\n" \
            "$SCHED" "$N" "$T" "$mean" "$spread"
        rm -f "$out"
    done
done
