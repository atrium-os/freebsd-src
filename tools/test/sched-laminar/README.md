# Laminar scheduler stress / proxy tests

Three small reproducible tests that exercise the scheduler along the
axes the Laminar design cares about.  Each prints comparable summary
numbers so you can run it once under `kern.sched.name=ULE` and once
under `=Laminar` and contrast.

## How to switch scheduler

`kern.sched.name` is a loader-time tunable on FreeBSD (you cannot
change the active scheduler on a live kernel).  Edit
`/boot/loader.conf`:

```
kern.sched.name="Laminar"   # or "ULE"
```

then reboot.  Confirm with `sysctl kern.sched.name`.

## Tests

### `fair.sh` — proportional fairness under uniform load

Launches N CPU-bound spinners (default 8) and lets them run for T
seconds (default 10).  Measures per-process CPU time consumed.  A
fair scheduler should give each spinner the same number of seconds
(within ~few %).

Prints: min, max, mean, max-min spread.  Lower spread = better.

### `bursty.sh` — wake-rebase / latency on alternating sleep+burst

Launches B short-burst threads (sleep 200ms, run 50ms, repeat) and
C long-burst threads (sleep 1s, run 200ms, repeat) alongside S
background spinners.  Measures the bursters' aggregate CPU + the
ratio of CPU each class consumed vs the share their nominal
duty-cycle implies.

A scheduler with no wake re-base will let long-sleepers dominate
briefly on wake (low vruntime); LAG_CAP should bound this.

### `skew.sh` — cross-class behavior (nice-skewed timeshare)

Launches N=10 spinners at nice 0, nice -5, and nice +5.  Measures
per-class CPU.  ULE's interactivity scoring and Laminar's vruntime
weighting will weight these differently; the test simply emits the
ratios for comparison.

## Caveats

- Single-CPU runs are most legible; pin via `cpuset -l 0`.  See
  comments in each script.
- All numbers are **proxy** measurements (process accounting via
  `wait4`/`getrusage`), not formal scheduler benchmarks.  Use them
  to spot regressions and gross differences, not to publish.
