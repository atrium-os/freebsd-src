#!/bin/sh
# pace-sweep.sh — the display frame-pacing measurement (runs IN the VM).
#
# The scheduler doc's #1 axis: frame-time / frame-miss under load. Same
# apples-to-apples control as the audio L6 story — the SAME periodic workload
# (a 60Hz frame grid, t=16200us) with the deadline lane ON vs OFF. The metronome
# binary does work_us of "render" per frame and must have it ready by the next
# vblank; a miss is a frame not ready on time (a dropped/judder frame). Lane
# misses are KERNEL-counted (robust); plain misses are userspace-counted but
# under load are dominated by real starvation, not HVF noise (the L6 caveat).
#
# Unlike audio (tiny 2.7ms period, miss under any load), a frame's 16.2ms period
# has slack, so timeshare copes with LIGHT frames and only janks once a frame's
# CPU cost approaches its fair share under load. So we sweep FRAME COST (work_us)
# — the display analog of audio's buffer-depth sweep — at two load levels.
#
# frescod's broker role (sponsoring this same lane anchored to the REAL vblank)
# is separately gated (J.2b: 606 frames / 0 misses / 16 spinners); this measures
# the scheduling contrast the broker rides on.
#
# Emits/append `RESULT work=<us> spin=<n> mode=<plain|lane> rep=<r> misses=<m>`.
M=/tmp/metronome
OUT=/mnt/host/freebsd-src/usr/src/tools/test/sched-laminar/bench/pace-results.txt
T=16200            # ~60Hz frame period
N=${N:-150}        # frame attempts per run
REPS=${REPS:-3}
MARGIN=${MARGIN:-1500}   # lane reservation Q = work + margin (must keep Q/T < 0.75)
WORK_LIST=${WORK_LIST:-"2000 4000 6000 8000"}
SPIN_LIST=${SPIN_LIST:-"4 8 16"}

cleanup() { pkill -9 metronome 2>/dev/null; }
trap cleanup EXIT HUP INT TERM
sysctl kern.sched.deadline_enable=1 >/dev/null 2>&1

: > "$OUT"
echo "pace sweep: t=$T N=$N reps=$REPS margin=$MARGIN work='$WORK_LIST' spin='$SPIN_LIST'" | tee -a "$OUT"
for work in $WORK_LIST; do
  for spin in $SPIN_LIST; do
    for mode in plain lane; do
      if [ "$mode" = "plain" ]; then q=0; else q=$((work+MARGIN)); fi
      r=1
      while [ $r -le $REPS ]; do
        out=$(timeout -k 2 40 $M $q $T $work $N $spin 2>/dev/null)
        pkill -9 metronome 2>/dev/null
        pm=$(echo "$out" | sed -n "s/.*periods=\([0-9]*\) misses=\([0-9]*\).*/\1 \2/p")
        [ -z "$pm" ] && pm="0 refused"   # lane: admission refused (Q/T > cap)
        set -- $pm
        echo "RESULT work=$work spin=$spin mode=$mode rep=$r periods=$1 misses=$2" | tee -a "$OUT"
        r=$((r+1))
      done
    done
  done
done
echo "SWEEP_DONE" | tee -a "$OUT"
