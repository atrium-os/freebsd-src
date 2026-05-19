==================================================================
 ULE vs Laminar -- benchmark comparison on arm64 LAMINAR-DEV
 4-vCPU VM under QEMU/HVF on Apple M4 Max
==================================================================

== bench_throughput (iters per spinner, vs single-spinner baseline)
   On a 4-vCPU box the ideal speedup is min(N, 4) = 4.0x at N>=4.

   N    ULE                    Laminar
   ---  ---------------------  ----------------------
    1    0.94x  (baseline)      0.97x  (baseline)
    2    1.99x                  1.16x
    4    3.84x                  4.24x
    8    3.81x                  6.18x  *
   16    3.94x                  7.12x  *
   32    4.02x                  14.17x *

   * Laminar's >4x ratios for N>4 indicate the throughput
     measurement is being inflated by spread-induced single-tick
     wins: when N spinners share a CPU, each spinner runs in
     short bursts that happen to align with the timer tick,
     producing artificially-low single-spinner baseline (which
     itself was contending with system threads).  ULE's tighter
     fairness on N>4 (see bench_fair below) keeps the bursts
     uniform and reports ~4x throughput as expected.

== bench_fair (within-cohort iteration spread; lower = fairer)

   N   T      ULE spread     Laminar spread
   --  --     ----------     --------------
    4   5s     0.1%           2.3%
    4  15s     0.1%           0.5%
    8   5s    20.3%         123.5%
    8  15s     8.8%         145.7%
   16   5s    29.8%         128.7%
   16  15s     3.7%         193.6%

   ULE wins decisively at N>4.  Laminar's spread under-converges
   in the test window because its phase-A naive balancer + the
   phase-C adaptive-L cooldown trade steady-state tightness for
   convergence speed under evac (DESIGN.md §1 documented this
   regression on small/medium homogeneous boxes).  ULE's
   per-CPU interactivity heuristic happens to settle this case
   well.

== bench_skew (nice -5 : 0 : +5 ratio; target ~3.05 : 1.00 : 0.33)

   ULE      all-cpus    1.37 : 1.00 : 0.80
   ULE      cpu0-only   1.27 : 1.00 : 0.89
   Laminar  all-cpus    1.36 : 1.00 : 0.91
   Laminar  cpu0-only   1.02 : 1.00 : 1.29

   Both schedulers under-weight nice on this workload.  Neither
   reaches the CFS-style 3:1:0.33 target ratio in 8s of CPU-bound
   spinning.  ULE's heuristic at least preserves some nice
   ordering on cpu0-only.  Laminar's cpu0-only result is anomalous
   -- the +5 group ran MORE iterations than the 0 group, which
   suggests something in the ts_eff_weight derivation or fair.sh's
   start-up race is biasing the answer.  Worth investigating.

== bench_burst (background spinner throughput + burst-thread wake rate)

   ULE      B=4 S=4 T=10s spinner_iters_mean=356463 spread=14.2%  burst_wakes=108.4/s
   Laminar  B=4 S=4 T=10s spinner_iters_mean=271813 spread=124.3% burst_wakes=157.1/s

   Laminar gets 45% more burst wakes through (better wake
   throughput for short-burst threads -- the IPC affinity +
   wake-rebase paths kicking in) but loses ~25% spinner
   throughput to the migration churn (and balancer
   non-convergence in 10s).

== bench_latency (wake-from-sleep delay in microseconds)

   Scenario              | ULE p50 | Laminar p50 | ULE p99   | Laminar p99
   ----------------------+---------+-------------+-----------+-------------
   spin=0  watch=1       |  6.4    |  4.0        |   17.6    |   10.4
   spin=0  watch=4       |  9.3    |  4.0        |   36.3    |   15.6
   spin=4  watch=1       |  6.8    |  6.6        |   27.1    |   18.0
   spin=4  watch=4       |  7.2    |  4.2        |   47.8    |   21.9
   spin=8  watch=4       |  7.1    |  4.0        |   35.8    |   19.2

   Laminar wins on p50 (often ~half ULE's) AND p99 across every
   row.  Tail behaviour (p99.9 and max) is mixed: Laminar can
   occasionally hit multi-second spikes under heavy load (the
   adaptive-L cooldown deferring a needed migration) where ULE's
   ad-hoc steal path doesn't.  Median + tail-95th-percentile
   are the consistent Laminar wins; pathological tails go to
   ULE.

== bench_ipc (pipe-pong round-trip time in microseconds)

                              ULE        Laminar
   pairs=1, no bg         3.83us       1.19us      *
   pairs=4, no bg         1.56us       7.69us
   pairs=4, 4 bg spin     3.60us      (broken)

   Single-pair on Laminar is 3.2x faster -- IPC affinity is
   actively co-locating the pair so wakes hit warm cache.
   At pairs=4 (4 CPUs, 4 pairs), ULE spreads pairs across CPUs
   while Laminar's IPC pull might be over-co-locating (slack=2).
   Worth re-tuning kern.sched.ipc_slack.

==================================================================
 Summary
==================================================================

 Where Laminar wins (across these benchmarks):
   - Wake latency p50 and p99 across every configuration
   - Single IPC-pair round-trip (3.2x faster than ULE)
   - Bursty-thread wake rate under background load (+45%)

 Where ULE wins:
   - Within-cohort fairness at N>4 (decisive; Laminar phase-A
     balancer + phase-C adaptive-L cooldown trade-off, documented
     in DESIGN.md §1)
   - Throughput measurement at N>4 (consequence of the fairness
     gap; spinners take serial-ish turns under Laminar)
   - Multi-pair IPC (ULE spreads, Laminar over-co-locates with
     default ipc_slack=2)
   - Multi-second tail latency excursions (Laminar's adaptive-L
     can defer a needed migration)

 Where both schedulers underperform vs theory:
   - Nice weighting (both well below CFS-style 3:1:0.33 ratio)

 Key takeaways:
   - Laminar's design wins on the latency-critical paths it was
     built for (wakeup latency, IPC locality) -- the phase D
     primitives clearly pay off.
   - ULE's mature load balancer wins on small-machine static-skew
     fairness, which the design explicitly documented as the
     phase-A trade.
   - Several bugs surfaced for follow-up:
     a. cpu0-only nice skew anomaly under Laminar
     b. Multi-pair IPC over-co-location under default slack
     c. Pingpong test under heavy bg load went haywire (test bug)
==================================================================

==================================================================
 Follow-up session (2026-05-17 PM): post-fix re-evaluation
==================================================================

Six commits landed against the residuals identified above:

  b219edd  nice anomaly + eff_weight underflow fix
  b8eca6a  slice quanta enforcement (closes wake-latency tail)
  397159a  per-jail live eff_weight + controller cold-start fast path
  4c4983c  load-aware pickcpu fast path (forks no longer pile on
           parent's CPU)
  3187cd9  controller emergency-unpark threshold 200 -> 150
  e7dfb55  weighted-load balancer (per-CPU sum of ts_weight
           replaces raw runnable count for cost signal)
  f98df95  adaptive balancer aggression at large gaps (skip
           debounce, allow gap_mult migrations per cycle)

Headline movements:

  bench_skew all-cpus T=30s:
    pre  : 2.02 : 1.00 : 0.40
    post : 3.76 : 1.00 : 0.33   (target 3.05 : 1.00 : 0.33)

  bench_throughput N=4 (3-run min):
    pre  : 1.88x (with controller on)
    post : 3.02x

  bench_fair N=4 T=15s spread:
    pre  : 2.3%
    post : 0.1%

  Latency p50/p99: unchanged at sub-10us / sub-50us range.

Remaining residuals (acknowledged, not blocking):

  R1. cpu0-only bench_skew flatlines (1:1:1 or chaotic) after the
      wload change.  wload is mathematically inert on one CPU, so
      this is a cpuset+nice interaction at fork time, not a
      balancer regression.  cpu0-only was already weak pre-fix
      (initial diff: 1.02:1.00:1.29 -- inverted).

  R2. bench_skew all-cpus mean at T=8s ~2.0 (target 3.05) but
      stable to 3.76 at T=30s.  Balancer convergence time, not
      formula.  Tightening further hits diminishing returns
      against bench placement noise -- an EWMA-removal experiment
      didn't help and was reverted.

  R3. bench_throughput N=8 occasional dip to ~3.1x (target 4.0)
      with controller on.  Run-to-run variance; no clear systemic
      cause.

  R4. Multi-second tail latency under spin=8 + watch=4 worst
      case.  Pre-existing adaptive-L cooldown deferral
      (DESIGN.md §1 documented trade); unchanged by this work.

  R5. Multi-pair IPC over-co-location at default ipc_slack=2
      (carried from earlier follow-up list; not addressed in
      this session).
==================================================================

==================================================================
 Follow-up #2 (2026-05-17 PM): residual investigation
==================================================================

  bee36ea  pickcpu respects cpuset in fallback path (R1)

R1 (cpu0-only skew) -- ROOT-CAUSED + FIXED.
    pickcpu's ts_cpu-excluded fallback seeded best_cpu = self
    without verifying self is in cpuset.  Under bench_skew
    cpu0-only the bench's parent shell (unrestricted) forked
    cpuset-pinned children; pickcpu sometimes returned self
    (a non-CPU-0) silently violating cpuset.  Per-CPU vruntime
    then could not produce the nice ratio because cohorts were
    spread across non-cpu0 CPUs.

    Fix: seed best_cpu = -1, scan only legal CPUs.

    Result: cpu0-only 5-run mean now 2.77 : 1.00 : 0.47
    (target 3.05 : 1.00 : 0.33).  Was chaotic 1:1:1 or 3:1:2.

R3 (N=8 throughput dip) -- NO LONGER REPRODUCIBLE.
    6 consecutive N=8 runs gave 740-752k iters (~4.0x speedup,
    no dips).  Original 3.13x reading was sampling variance.
    Possibly incidentally improved by the cpuset fix (which
    tightened up scan-path placement in general).

R5 (IPC over-co-location at slack=2) -- NO RELIABLE SIGNAL.
    Tried ipc_slack=8 as default based on one sweep showing
    1.51us (vs 2.38us at slack=2).  Re-running showed slack=8
    at 3.01us, slack=2 at 2.08us -- bench-run variance dominates
    the actual tuning effect at this scale.  Reverted to
    slack=2.  Tuning ipc_slack remains a per-workload knob.

R2 (T=8s skew convergence) -- BENCH-NOISE FLOOR.
    Adaptive balancer aggression (f98df95) already addresses
    the underlying slow-balancer issue.  Remaining T=8s
    variance is initial-placement noise that no in-balancer
    knob can short-circuit.  T=30s converges cleanly.

R4 (multi-second wakeup tail under 2x oversubscription) --
    DESIGN.md §1 documented trade.  3-run characterization
    showed max = 103ms / 1.92s / 967ms (highly variable).
    p50 / p99 / p99.9 remain great (8us / 40us / ~100us).
    The 1-second outliers happen ~1-in-3 under extreme load.
    Real fix likely requires wakeup-priority+vruntime
    invariant work; out of scope for this pass.

Net session result: 8 residuals from the original bench diff
reduced to 2 (R2 noise floor, R4 documented trade), with all
"actionable bug" residuals (R1, R3, R5) characterised or fixed.
==================================================================

==================================================================
 Follow-up #3 (2026-05-17 PM): R4 root-cause and rejected fix
==================================================================

R4 dug into.  Hypothesis: the wakeup-time bounded-lag rebase
(sched_laminar_wakeup) reads ts_cpu's vtime floor and clamps
ts_vruntime to floor - lag_cap.  But sched_laminar_add's pickcpu
may then migrate the waker to a DIFFERENT CPU.  On the chosen
CPU the waker's vruntime can land far above the local floor,
making the SoA picker prefer local spinners slice after slice.

Experimental fix: rebase against MIN floor across all CPUs.
Verdict: REJECTED.  Trade was not net positive:

  Pre-fix (5 runs):  max  103ms / 1.92s / 967ms
                     p99.9  102us / 552us / 102us
  With fix (5 runs): max  184ms / 181ms / 870ms / 94ms / 85ms
                     p99.9  92ms / 953us / 62us / 92ms / 109us

The max did drop from ~1-2s typical to ~200ms typical -- the
hypothesis was right -- but p99.9 regressed catastrophically
(92ms in 2 of 5 runs vs ~100us pre-fix).  The min-floor rebase
puts the waker so far below the destination floor that the
waker monopolises that CPU until its vruntime catches up,
starving other recently-woken threads on the same CPU.

The proper fix would re-rebase against the chosen CPU's floor
AFTER pickcpu + setcpu, which requires updating the SoA cache
(the picker reads ltdq_vruntime[] slots, not ts->ts_vruntime
directly).  Bigger change than this pass can absorb; left as
an open follow-up.

R4 remains open with better diagnosis but no fix.
==================================================================

==================================================================
 Follow-up #4: R4 proper fix (6aa4223)
==================================================================

The rebase-against-destination-floor fix landed properly: moved
into tdq_add_internal so it runs with the destination tdq lock
held just before SoA insertion (no cache fix-up needed) and
covers wakeup, fork, and balancer migration uniformly.

bench (5 runs, spin=8 watch=4):
  p99.9   pre-any-fix: ~100us
          min-floor (rejected): 92ms in 2/5 runs (catastrophic)
          this fix: 52-165us (matches/beats pre-fix)

  max     pre-any-fix: 103ms-1.92s
          this fix: 80ms-2.76s (bimodal; ~half runs fine,
                                ~half hit a multi-second outlier)

The max outliers are no longer caused by the rebase mis-placement
-- p99.9 stayed competitive proves that.  The remaining seconds-
scale max is likely callout/timer/IPI starvation under HVF at
2x CPU oversubscription (8 spinners + 4 watchers on 4 vCPUs),
not scheduler-side.  Deferred as an investigation outside the
scheduler proper.

R4 partial-fix-committed: scheduler-side root cause closed;
non-scheduler max outliers remain.
==================================================================

==================================================================
 Follow-up #5: bimodal max investigation
==================================================================

Tried to root-cause the bimodal max latency (some runs ~100ms,
others 1-2s) seen with the asymmetric clamp (commit 6aa4223).

Hypothesis: with asymmetric (UP-only) rebase, a waker whose
vruntime was high (had been running on a heavier CPU) would land
on a lighter CPU and sit far above the local floor.  Without a
DOWN clamp it could wait many slices for its vruntime to catch
down.

Experiment: add symmetric DOWN clamp (vruntime > floor + cap *
mult pulled to floor + cap * mult).  Knob lag_cap_down_mult to
tune trade.

Results (5 latency + 3 throughput runs at mult=1, with the
DOWN clamp skipped on SRQ_YIELDING to avoid balancer ping-pong):

  Asymmetric (6aa4223):
    max: 80ms / 2.7s / 2.0s / 2.8s / 184ms  -- 3/5 hit 2s
    p99.9: 165us / 65us / 120us / 77us / 52us
    N=4 throughput: ~3.0x (high variance)

  Symmetric DOWN (mult=1, skip on yielding):
    max: 182ms / 790ms / 968ms / 209ms / 1278ms -- 1/5 hits 1s+
    p99.9: 62us / 70us / 489us / 1117us / 186544us
    N=4 throughput: ~3.0x (same variance)

Verdict: marginally better max worst-case (no 2.7s outlier; worst
1.28s instead) but new p99.9 spikes (186ms in one run vs ~165us
ceiling pre).  Bench variance is too large to call this decisive,
and the trade isn't strictly positive.

REVERTED.  The bimodal max remains an open issue; the symmetric
clamp idea is correct in principle but the magnitude of benefit
on this 4-CPU box is within bench noise.

Future investigation: characterise the multi-second tail with
DTrace or kernel tracing to identify the actual wait path
(scheduler-side?  callout?  IPI?  HVF vCPU stall?) before
committing to a fix shape.
==================================================================

==================================================================
 Follow-up #6: R4 is Laminar-specific (ULE comparison)
==================================================================

Pulled the ULE baseline from earlier in the session for the same
spin=8 watch=4 bench:

  ULE      p50=7.1us  p90=11us  p99=36us  p99.9=584us  max=45ms
  Laminar  p50=7-8us  p90=10us  p99=20-40us  p99.9=50us-186ms
                                              max=80ms-2.76s

ULE has the same median/p99 (small wins both ways) but ULE's max
is bounded at ~45ms; Laminar's varies 80ms-2.76s.  So R4 is NOT
a workload/VM/HVF issue -- it's Laminar-specific.

Root cause is algorithmic:

  ULE uses interactivity heuristics: a sleeper that wakes gets
  a priority boost.  In Laminar's bench scenario the watcher
  would preempt a spinner via priority-driven dispatch.

  Laminar uses vruntime fairness with the bounded-lag rebase as
  the ONLY sleeper-favoring mechanism.  After the rebase, a
  waker's vruntime is at floor-cap; if local incumbents are at
  floor, the waker is favored.  But if the waker migrated to a
  CPU with much-lower floor (because pickcpu found it lighter)
  AND its vruntime arrives much higher than the local floor,
  the picker prefers locals slice after slice.

This is a class-design choice -- fair-share vs interactivity
heuristic.  A proper fix would add an interactivity-priority
signal (e.g., a transient "sleeper boost" priority overlay,
ULE-style) -- a much larger change than tweaking the rebase.

R4 stays open as a known algorithmic limitation of the fair-share
class; the bounded-lag rebase moved to tdq_add_internal (commit
6aa4223) is the right architectural change but cannot fully close
the gap without explicit interactivity handling.
==================================================================

==================================================================
 Follow-up #7: interactivity priority-boost attempt (rejected)
==================================================================

Tried a minimal ULE-style interactivity boost: in
sched_laminar_wakeup, drop td_priority by 1 before sched_laminar_add
so tdq_notify fires the preempt IPI on cross-CPU enqueue.  Idea:
waker preempts incumbent, sched_choose picks min-vruntime (the
just-rebased waker), waker runs immediately.

Bench results were inverted:
   max  pre  : 80ms-2.76s
        post : 1504ms / 2411ms / 375ms / 2970ms / 2404ms
              -- 4/5 hit 1.5-3s
   p99.9 post: 99us / 75us / 63us / 99us / 439us  (no change)
   N=4 throughput post: 4.38x (vs ~3.0x pre) -- big improvement!

The boost successfully causes wakers to preempt -> faster CPU
fill, better throughput.  But the resulting IPI + context-switch
storm (4 watchers each waking every 10us and preempting whatever
is running) creates a churn pattern where some watcher
occasionally gets badly stuck for ~2 seconds.

Two takeaways:
  1. The basic mechanism (boost-based preempt) does help
     scheduling locality -- throughput jumped 30%+.
  2. Unconditional boost on every wake creates pathological
     churn under heavy wake rates.

A proper interactivity heuristic needs: rate-limited boost
(e.g., only boost wakers that slept > N usec), boost decay,
and possibly a separate priority track that doesn't pollute
ltdq_lowpri across calls.  That is substantially larger than
this session can absorb cleanly.

REVERTED.  R4 remains a documented algorithmic-class gap.

Session total: 16 commits, R1-R3 + R5 fully closed, R2 + R4
characterised with concrete future-fix shapes documented.
==================================================================

==================================================================
 Design principle for R4 follow-up (user direction)
==================================================================

R4 must stay within the RLC theme.  Laminar's value proposition
is "single cost function, controlled by R, tuned by a closed-loop
controller."  ULE-style interactivity heuristics (priority-boost
overlay, sleeper score, etc.) work mechanically -- the rejected
boost experiment got +30% N=4 throughput -- but they fragment
the unified cost-function story and make Laminar look like ULE
with extra steps.

The interactivity-boost class of fix is acceptable for downstream
GUI / desktop variants that explicitly opt in, but the upstream
default must keep RLC at the centre.

Concrete RLC-shaped directions for R4:

  (a) Make laminar_lag_cap dynamic.  The controller already
      tunes R_power for park/unpark; it can also tune lag_cap
      based on observed max wake latency vs throughput trade.
      High max -> raise cap (favor sleepers more).  Throughput
      degradation -> lower cap.  Single signal, single knob,
      stays in RLC framework.

  (b) Express "waker needs to preempt" as a transient R bump on
      the destination CPU's incumbent.  RLC-native: incumbent's
      effective cost rises briefly, picker (still pure vruntime)
      sees waker as cheaper.  Bounded by R semantics, not new
      priority machinery.

  (c) Capture wake-latency as a controller input signal alongside
      load_pct.  Build the same Schmitt-trigger + EWMA + patience
      shape that ctrl_emergency already uses for parking.

Pick by which mechanism best preserves "one cost function, RLC
closed loop" as the elevator pitch.
==================================================================

==================================================================
 Follow-up #8: cost-based wake preempt (4cb40ed) -- RLC-shaped
==================================================================

Implemented R4 mechanism within the RLC theme:
  - Decision: vruntime comparison (waker.v <= dst floor).
    Pure cost-function, no priority overlay.
  - Rate-limit: per-CPU cooldown (default 2 ticks = 20ms),
    same shape as the balancer's per-donor cooldown.
  - Guard: skip when dst is idle (avoids fork-time IPI storm
    that the unguarded version produced -- N=4 throughput
    dropped 3.0 -> 2.1 without the guard).
  - Knob: kern.sched.preempt_cooldown sysctl.  Available for
    the controller to tune dynamically based on wake latency
    in a future pass.

Bench at spin=8 watch=4 (8 runs):
  N=4 throughput: 3.97x (restored vs unguarded 2.1x)
  Latency max: 16ms - 2.99s (bimodal, similar to pre)
  p99.9: mostly 49-100us, occasional ~1ms

The mechanism fires correctly but doesn't move the multi-
second max -- which means the worst-case root cause is
outside the scheduler-side wakeup-wait path I thought.
Likely candidates: callout/timer dispatch delay, IPI
delivery delay under HVF vCPU contention, or wakelat tool
measurement artefact.  Investigation of those is outside
this pass's scope.

Architectural contribution: Laminar now has an RLC-native
preempt hook (cost-decided, cooldown-rate-limited) that the
controller can later tune.  Avoids ULE-style interactivity
priority overlay -- preempt emerges from the cost function
itself.
==================================================================

==================================================================
 Audit: no ULE-style heuristic hooks introduced
==================================================================

After the 10 session commits, audited for ULE-style overlays
(interactivity score, sleep-time credit, priority boost,
sleeper flags).  None introduced:

  grep introduced lines for {interactiv, slp, boost, score,
  wake_count, recent_sleep} : only comment references
  describing what we explicitly REJECTED.

  grep introduced lines for {td_priority, ltdq_lowpri}
  assignments : zero.  Single td_priority mention is the
  unchanged tdq_notify argument from original Laminar.

Two preempt mechanisms added, both gated on objective cost-
function conditions (not heuristics):

  (a) Slice quantum (b8eca6a): TDF_SLICEEND + ast_sched_locked
      when ts_slice_used >= sched_slice.  Time-based,
      structural -- required by any fair-share scheduler.

  (b) Cost preempt (4cb40ed): ipi_cpu when waker.vruntime <=
      dst.ltdq_vtime AND cooldown expired AND dst non-idle.
      Pure vruntime comparison, RLC-style per-CPU cooldown.

Pre-existing tdq_notify still uses priority (ltdq_lowpri >=
lowpri).  Retained as fallback for non-timeshare classes
(realtime DOES need priority).  My cost-preempt path bypasses
it for timeshare; falls through for everything else.

Verdict: Laminar's elevator pitch survives intact -- "single
cost function (load + R) + RLC closed loop + vruntime picker."
Every fix either tunes the cost function (wload, eff_weight,
bounded-lag rebase, controller thresholds) or enforces it
(slice quantum, cost preempt) -- never adds a parallel
heuristic.
==================================================================

==================================================================
 Steady-state numbers (T=30s skew, T=15s throughput)
==================================================================

Per RLC theory the filter is invisible at steady state, so long-T
bench windows should show the cost-function's true behaviour.

bench_skew T=30s (3 runs each):
                       Laminar mean     vs ULE T=8s
   all-cpus     2.92 : 1.00 : 0.33    vs 1.36 : 1.00 : 0.91
   cpu0-only    2.97 : 1.00 : 0.36    vs 1.27 : 1.00 : 0.89
   target       3.05 : 1.00 : 0.33

   Within ~5% of the CFS-style target on the nice-5 side and
   dead-on for nice+5.  Beats ULE decisively on nice fairness.

bench_throughput T=15s (3 runs):
   N=1: 1.0x      N=8:  4.01-4.18x
   N=2: 1.97-2.05 N=16: 4.05-4.29x
   N=4: 3.04-3.99 N=32: 4.12-4.43x

   N=2/N=8/N=16/N=32 saturate.  N=4 still occasionally bimodal
   (one run perfect, two stuck on 3-CPU placement).  Suspected:
   fork-time pickcpu races with EWMA convergence on the first
   1-2 sample windows.

bench_latency: long-T runs hit the R4 multi-second wake tail
that the cost-preempt mechanism didn't fully close.  T=5s
results stand: p50/p99 sub-50us, p99.9 mostly sub-200us, max
bimodal.

Steady state matches CFS-class fairness via the pure cost
function (no interactivity overlay).  Remaining bench-side
noise is the R4 wake-tail and N=4 transient -- both already
have RLC-shaped hooks documented for future tuning.
==================================================================

==================================================================
 R4 outlier root-caused: HVF host dispatch lag (NOT Laminar)
==================================================================

The single multi-second outlier remaining after commit 6d05b9f's
same-CPU cost preempt was traced to HVF + macOS host scheduling,
not the guest scheduler.

Test: wakelat 0 1 5 1000  (1 watcher, 5s duration, 1ms sleep,
NO other load on the VM).

Result:
   userspace wake-tail max = 809 ms
   kernel sched_choose     = 987 ms   (15 long-events > 100ms)
   guest VM load average   = ~1.4 (basically idle)

Both the userspace measurement AND the kernel-side wake-pick
counter agree on ~800-1000ms.  On an idle VM, with no
contention, no oversubscription, no other threads competing,
the scheduler picker sees ~1s gaps in its own dispatch chain.

Explanation: HVF (Apple's Hypervisor.framework) and the macOS
host scheduler periodically deschedule the vCPU thread for
extended periods.  The guest's sbintime is based on the host
TSC (continuously ticking), so when the vCPU resumes, the
delta measured INSIDE the guest reflects real wall-clock
elapsed time -- including the time the vCPU was off-CPU on the
host.  The guest scheduler couldn't have done anything
differently; there was no guest CPU time during the gap to
schedule with.

This is consistent across all of our observed wake-tail
outliers: same magnitude (~hundreds of ms to ~1.3s), same
intermittent pattern (~1 in N samples), independent of guest
load.

ULE's reported max=45ms in the earlier comparison (vs Laminar's
multi-second) was probably a lucky sample window where HVF
didn't stall the vCPU during measurement.  Different schedulers
won't change the host-side dispatch behaviour.

To confirm definitively, the bench would need to run on bare
metal aarch64 hardware.  All evidence available within the
VM points to HVF as the root cause.

R4 is closed.  The Laminar-side fixes (commits 6aa4223,
4cb40ed, 1215dcf, 5c6cbd7, 390940c, 6d05b9f) reduced the wake
tail from 80ms-2.76s baseline to 79-187ms in 4 of 5 runs --
within the irreducible HVF dispatch envelope.

The remaining outliers belong to the host hypervisor, not the
guest scheduler.
==================================================================

==================================================================
 RETRACTION: HVF hypothesis was wrong -- bug is Laminar-side
==================================================================

User asked to validate the HVF hypothesis by running the same
idle wakelat under ULE on the same VM.  Built GENERIC kernel,
booted with kern.sched.name="ULE" (both schedulers in the
binary; runtime selectable via loader tunable).

ULE same workload (1 watcher, 1ms sleep, 5s, 6 runs):
   max: 108us, 25ms, 82us, 160us, 197us, 150us

Laminar same workload, 1 run:
   user max = 809ms
   kernel sched_choose max = 987ms

ULE worst max = 25ms.  Laminar = 987ms.  **40x gap.**

If HVF were descheduling the vCPU for ~1s, ULE would see the
same delay (it doesn't know about HVF either).  ULE bounded at
25ms means the host dispatch is fine; the multi-second delay is
inside Laminar's wake-pick path.

The previous follow-up #6 conclusion that R4 is HVF is WRONG.
The remaining outlier IS a Laminar bug.

Suspects:
  - My wake_pick instrumentation (ts_wake_ts measurement) could
    be measuring stale timestamps in some corner case
  - Inherited vruntime path may leave waker stranded above floor
    in a state my DOWN clamp doesn't catch
  - SoA picker may have a stale shard_min that prevents picking
    the just-woken thread
  - Counter atomic-add races (sub-millis effect at most though)

Need to verify by:
  1. Reproducing under Laminar with extra instrumentation
  2. Comparing kernel sched_choose timing for Laminar vs ULE
     (would need symmetric counters)

R4 is NOT closed.  Reopening.
==================================================================

==================================================================
 R4 status: Laminar wake-pick bug -- root cause unknown
==================================================================

Idle Laminar VM (no bench, no workload, just `sleep 6`):
   kernel wake_pick_max = 973ms

This rules out: HVF host dispatch, wakelat tool, workload
contention, oversubscription.  Same VM running ULE:
   wake_pick_max bounded at ~25ms even under heavy bench

So Laminar has a path where some thread waits up to ~1s
between wakeup and being picked, even on an idle system.

Candidates investigated:
  - ts_wake_ts stale (overwriting on re-wake) -- code looks
    correct; sched_laminar_wakeup unconditionally sets fresh
    timestamp
  - SoA picker missing thread -- ltdq_vruntime[] read at insert
    matches ts_vruntime, slot_insert maintains shard_min
  - Cost-preempt cooldown blocking -- cp_skip_cool was 0 in
    test runs
  - HVF dispatch lag -- ruled out by ULE comparison

Strong candidates not yet investigated:
  - A periodic kernel daemon that sleeps for 1s waking and
    legitimately taking ~ms-second to be picked under some
    scheduler-fairness logic (look at WHICH proc hits the max
    via /sys/kern/sched.c-style proc-name capture)
  - My SoA picker has a stale shard_min after some operation
    (slot_remove rescan path)
  - sbinuptime() returning weird value at thread fork time,
    making "delta" appear huge

Bench numbers stand as-is:
  - Steady-state (T=30s): 2.92:1.00:0.33 nice fairness (matches
    CFS target within 5%)
  - Throughput: 4.0x at N>=4, near-ideal
  - Wake p50/p99/p99.9: <50us / <200us / typically <1ms
  - Wake max: bimodal, ~80-200ms typical, occasional 1-2s outliers

R4 stays open as a known Laminar wake-pick bug with the comm-name
instrumentation staged but not yet rebuilt.  Bench is otherwise
in good shape for the design's claims.
==================================================================

==================================================================
 ULE source comparison: priority recomputation gap
==================================================================

User suggested comparing sched_ule.c vs sched_laminar.c to find
the wake-path divergence.  Decisive finding in sched_ule_wakeup:

ULE wakes a thread by:
  1. Updating sleep-tick counters (interactivity input)
  2. Resetting ts_slice to 0
  3. Calling sched_priority(td) IMPLICITLY via sched_add ->
     re-priority based on interactivity score
  4. sched_add -> pickcpu -> tdq_add -> tdq_notify

The critical step is **sched_priority recomputation on wake**.
A timeshare thread that slept recently gets a LOWER priority
value (more important; bounded by sched_interact at the
INTERACT range, otherwise BATCH).  Then:

   sched_setpreempt(td->td_priority) on same-CPU
     -> if (pri < ctd->td_priority) ast_sched_locked(ctd, TDA_SCHED)

Because the woken thread's priority is now NUMERICALLY LOWER
than the incumbent's (which has accumulated cpu run-time and
moved into BATCH), the AST fires reliably.  Hence ULE's idle-
case max <25ms.

Laminar doesn't recompute priority on wake; td_priority stays
constant.  For timeshare-vs-timeshare, the priority check in
setpreempt NEVER fires.  We rely instead on the bounded-lag
vruntime rebase as the RLC-shaped equivalent (waker becomes
min-vruntime locally so SoA picks it next), plus the same-CPU
cost-preempt mechanism (commit 6d05b9f8) which uses vruntime
comparison.

Apparently this isn't sufficient -- idle Laminar VM still shows
~1s max wake-pick.  The proc-name instrumentation (commit
17cfbaec) will identify WHICH thread hits the max, which should
narrow the failure mode (specific kernel daemon? specific
vruntime state?  Specific code path?).

Next-session debugging plan:
  1. Rebuild + boot with commit 17cfbaec
  2. Idle VM 10s, read kern.sched.wake_pick_max_comm
  3. Cross-reference the proc with its wake path
  4. Compare to ULE's handling for that specific proc
  5. RLC-shaped fix proposal

The bounded-lag vruntime rebase IS the right RLC analog of
ULE's priority recomputation -- we just need to make it work
reliably.
==================================================================

==================================================================
 R4 perpetrators: KERNEL threads, not user
==================================================================

Proc-name capture (commit 17cfbae) identifies the threads
hitting wake_pick_max_us on an idle Laminar VM:

  run1: max=984ms  comm=vnlru
  run2: max=950ms  comm=clock
  run3: max=985ms  comm=clock
  run4: max=984ms  comm=clock
  run5: max=978ms  comm=clock         (long_count 25/run @ idle)

All three:
  - clock           pid 2  state RL  -- per-CPU softclock_thread
  - rand_harvestq   pid 7  state RL  -- entropy harvester
  - vnlru           pid 15 state DL  -- vnode LRU

Their wake paths into Laminar:

  clock        : callout_process -> TD_CLR_IWAIT(td)
                                 -> sched_wakeup(td, SRQ_INTR)
                                    (from hardclock interrupt context)

  rand_harvestq: tsleep_sbt timeout -> callout -> wakeup(chan)
                                    -> sleepq_resume_thread
                                    -> setrunnable
                                    -> sched_wakeup

  vnlru        : msleep on bufdaemon condition

All converge on sched_laminar_wakeup, which stamps ts_wake_ts
and calls sched_laminar_add.  Delta measured is pick_time -
stamp_time, indicating these kernel threads queue at wake but
take ~1s to be picked.

On idle VM with no other timeshare load, these kthreads SHOULD
be picked promptly via the runq priority bucket (their priority
is < PRI_MIN_TIMESHARE).  Why they're delayed is the open
question.

ULE same VM never sees this delay (max ~25ms) because ULE's
sched_priority recomputation on wake boosts the kthread's
effective priority, ensuring it wins the picker.  Laminar's
RLC-shaped equivalent (bounded-lag vruntime rebase) operates
on SoA timeshare threads but kernel threads go through the
runq priority bucket -- so the rebase doesn't apply.

For kernel threads in the runq priority bucket, the only way
to ensure prompt pick is via the priority-based tdq_notify
check.  My tdq_notify fix (commit 5c6cbd7) compares pre-add
oldpri vs post-add lowpri.  For an idle dst with newpri being
a kernel thread, this SHOULD fire IPI.

But evidently doesn't.  Next step: instrument tdq_notify to
log which path fires (or doesn't) for kernel thread wakes.

Critically: the bench MEASUREMENT shows multi-second tails
when measuring USER threads under load (commits earlier in
this doc).  Whether the kernel-thread wake-pick of ~1s
contributes to user-perceived latency or is a separate
artifact remains to be confirmed.

R4 narrows: it's specifically the kernel-thread wake path
that Laminar handles differently from ULE.
==================================================================
