#!/bin/sh
# fair.sh: N CPU-bound spinners for T seconds; emit per-child CPU
# time and spread.  A fair scheduler keeps all values close.
#
# usage: fair.sh [n=8] [duration=10] [cpu=]
#   cpu=N pins all work to one CPU (single-CPU run is most legible).

set -eu

N=${1:-8}
T=${2:-10}
CPU=${3:-}

DIR=$(dirname "$0")
SPIN="$DIR/spin"
[ -x "$SPIN" ] || { echo "spin not built; run 'make' in $DIR"; exit 1; }

SCHED=$(sysctl -n kern.sched.name)
echo "=== fair.sh: scheduler=$SCHED N=$N T=${T}s ==="
if [ -n "$CPU" ]; then
    echo "(pinned to cpu $CPU)"
    exec cpuset -l "$CPU" "$SPIN" "$N" "$T"
fi
exec "$SPIN" "$N" "$T"
