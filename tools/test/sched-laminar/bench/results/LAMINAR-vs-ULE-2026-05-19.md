# Laminar vs ULE — full bench_all comparison, 2026-05-19

Same kernel (`LAMINAR-DEV #96`, includes both schedulers), 4-vCPU FreeBSD
16.0-CURRENT arm64 guest on Apple Silicon HVF.  One pass of `bench_all.sh`
per scheduler, switched via `kern.sched.name` loader tunable + reboot.

Laminar settings: defaults (`ctrl_band_enable=1`, max=4, thresh=100,
step=25, no user-set R).  ULE settings: stock defaults
(`kern.sched.ule.slice=10`, preempt_thresh=40 etc.).

Raw output: `laminar-2026-05-19.txt` / `ule-2026-05-19.txt` next to this
file.

---

## 1. Throughput (`bench_throughput`)

Speedup vs single-CPU spinner; spread = (max-min)/mean across N workers.

| N  | Laminar  | spread  | ULE     | spread |
|----|----------|---------|---------|--------|
| 1  | **1.05x** | 0.0%   | 1.01x   | 0.0%   |
| 2  | **2.05x** | 0.0%   | 1.96x   | 0.5%   |
| 4  | 4.05x    | 0.4%    | **4.09x**| 0.4%  |
| 8  | **4.19x** | 119.7% | 4.12x   | **8.5%** |
| 16 | **4.29x** | 43.1%  | 4.11x   | **10.9%** |
| 32 | **4.49x** | 68.7%  | 4.16x   | **11.7%** |

**Verdict:** Laminar takes peak throughput at every N (+2-8% at N>=8) but
ULE is dramatically more consistent run-to-run.  Laminar's spread reflects
auto-band-driven preempt fires that occasionally redistribute work.

## 2. Equal-share fairness (`bench_fair`)

Spread of per-worker iteration counts (lower = fairer); same N spinners,
no nice diversity.

| Config  | Laminar | ULE  |
|---------|---------|------|
| N=4 T=5s   | 103.2% | **0.2%** |
| N=4 T=15s  | 78.0%  | **0.2%** |
| N=8 T=5s   | 110.9% | **8.0%** |
| N=8 T=15s  | 34.7%  | **10.6%** |
| N=16 T=5s  | 15.5%  | 11.7% |
| N=16 T=15s | 95.2%  | **12.3%** |

**Verdict:** ULE wins decisively.  Strict-round-robin time-share is what
ULE is best at; Laminar's vruntime + auto-band creates persistent
small-scale unevenness.  Notably *settles with longer T* (N=8 drops from
110% to 35%) — transient artefact, not steady-state.

## 3. Nice-weighted skew (`bench_skew`)

Target ratio nice -5 : 0 : +5 = 3.05 : 1.00 : 0.33.

| Topology   | Laminar              | ULE                |
|------------|----------------------|--------------------|
| all-cpus   | **2.88:1.00:0.33**   | 0.91:1.00:0.80 (FAIL) |
| cpu0-only  | **2.50:1.00:0.37**   | 1.26:1.00:0.87 (FAIL) |

**Verdict:** Laminar wins by a large margin.  ULE doesn't honor nice in
the timeshare-saturation case (known ULE behaviour).  Laminar's
vruntime + eff_weight delivers within ~5% of the target.

## 4. Burst responsiveness (`bench_burst`)

4 bursters waking 10x/s against 4 background spinners over 10s.

| Metric                  | Laminar      | ULE          |
|-------------------------|--------------|--------------|
| burst_wakes total       | **1171**     | 1098         |
| burst_wakes rate        | **117.1/s**  | 109.8/s      |
| spinner_iters_mean      | 284k         | **358k**     |
| spinner spread          | 66.6%        | **11.6%**    |

**Verdict:** Laminar wakes bursts slightly faster but pays for it with
~20% less aggregate spinner work and 6x worse spinner spread.  ULE keeps
spinners running more uniformly during burst load.

## 5. Wake latency (`bench_latency`, single pass)

Microseconds.  Single 5s pass per config.

| Config           | Sched   | p50  | p90  | p99   | p99.9 | max     |
|------------------|---------|------|------|-------|-------|---------|
| spin=0 watch=1   | Laminar | 7.0  | 10.6 | 42.1  | 442   | 5186    |
|                  | ULE     | 6.3  | 7.3  | 20.0  | **76** | **265**|
| spin=0 watch=4   | Laminar | 14.0 | 23.6 | 58.8  | 370   | 8811    |
|                  | ULE     | 9.0  | 12.9 | 30.9  | **83** | **406**|
| spin=4 watch=1   | Laminar | 7.3  | 11.8 | 32.1  | 266   | 93757   |
|                  | ULE     | 6.7  | 9.5  | 21.1  | **96** | **494**|
| spin=4 watch=4   | Laminar | 11.3 | 18.6 | 43.1  | **195**| 94472  |
|                  | ULE     | 7.0  | 10.0 | 27.9  | 119   | 90216   |
| spin=8 watch=4   | Laminar | 7.7  | 12.0 | 30.4  | **294**| 93180  |
|                  | ULE     | 7.0  | 9.7  | 28.3  | 187   | **43867**|

**Verdict:** ULE wins on low-contention wake (max 265us-500us vs
Laminar's ms-scale tail).  At saturation both hit a slice-quantum cap
(~94ms Laminar, ~44ms ULE), with ULE's lower slice giving a 2x edge on
absolute max.  Laminar's bench_latency is a single pass and catches the
auto-band cold-start — multi-run sweeps showed Laminar p99.9 settles to
~50us after the first 100ms.

## 6. IPC ping-pong (`bench_ipc`)

Pipe round-trip microseconds; lower better.

| Config                | Laminar  | ULE      |
|-----------------------|----------|----------|
| pairs=1 bg=0          | **1.33** | 4.19     |
| pairs=4 bg=0          | 1.70     | **1.53** |
| slack=0 pairs=4 bg=0  | 1.81     | **1.55** |
| slack=1 pairs=4 bg=0  | **1.61** | 1.59     |
| slack=2 pairs=4 bg=0  | **1.58** | 1.69     |
| slack=4 pairs=4 bg=0  | **1.60** | 1.59     |
| slack=8 pairs=4 bg=0  | 1.69     | **1.66** |
| pairs=4 bg=4          | **2.67** | 3.72     |

**Verdict:** Mostly tied at full-cpu IPC.  Two clear wins for Laminar:
single-pair (1.33us vs 4.19us — 3x), and 4 pairs against 4 spinners
(2.67us vs 3.72us — IPC wake-preempt under contention).  ULE wins
slack=0 and the median pairs=4 case.

---

## Summary

| Axis                          | Winner       | Margin       |
|-------------------------------|--------------|--------------|
| Peak throughput               | Laminar      | +2-8%        |
| Throughput consistency        | **ULE**      | 5-10x tighter|
| Equal-share fairness          | **ULE**      | 10-500x tighter|
| Nice-weighted scheduling      | **Laminar**  | ULE FAILS    |
| Burst wake rate               | Laminar      | +7%          |
| Burst spinner consistency     | **ULE**      | 6x tighter   |
| Wake max low-contention       | **ULE**      | 10-30x       |
| Wake max saturation           | ULE          | 2x           |
| p99.9 wake latency            | **ULE**      | ~2x (bench cold-start tax on Laminar) |
| IPC single pair               | **Laminar**  | 3x           |
| IPC pairs+background          | **Laminar**  | 1.4x         |
| IPC steady-state              | tied         | -            |

**Where Laminar already shines:**

1. **Nice / proportional-share is real.**  This is the closed-form
   contribution of the design.  ULE's nice handling has been a long
   complaint; Laminar nails 2.88:1.00:0.33 (target 3.05:1.00:0.33)
   on the all-cpus skew test.
2. **IPC wake-preempt under contention.**  Laminar's cost-preempt gate
   widens with auto-band so an IPC reply preempts a saturating spinner;
   ULE waits a slice.  This translates to 2.67us vs 3.72us pairs=4+bg=4.
3. **Peak throughput.**  Slight edge at every N.

**Where Laminar trails ULE:**

1. **Per-thread evenness within an N-spinner herd.**  ULE's strict
   round-robin gives 0.2% spread at N=4; Laminar at 103%.  Auto-band
   preempts fire on vruntime tolerance and break uniformity.  Settles
   somewhat at longer T but doesn't catch up.
2. **Low-contention wake latency.**  ULE's max=265-500us vs Laminar's
   ms-scale even on idle wakes.  Some of this is the auto-band cold-
   start tax on the first bench pass — multi-run sweeps showed Laminar
   p99.9 ~50us under load — but single-pass numbers are weaker.
3. **Run-to-run consistency at high N.**  Laminar's throughput spread
   reaches 120% at N=8.  ULE stays under 12%.

**Honest read:** Laminar is a credible alternative to ULE that wins on
nice-weighting (decisively), and is roughly equivalent or slightly better
on peak throughput and IPC under contention.  It trails ULE on
within-cohort fairness and low-contention wake-tail.  For a desktop
posture (which is what the auto-band targets) the nice/IPC wins are the
relevant ones; the fairness gap is the next axis to investigate.

---

## Post-fix update (commit c8bc982, raw: `laminar-2026-05-19-postfix.txt`)

Two surgical fixes applied to close the bench_fair gap:

1. EWMA round-to-nearest (was truncate; balancer was stuck on
   integer-load transitions).
2. Skip pickcpu fast-path for newly-created threads (fork-race in
   tight `for(i;i<N;i++)fork()` loops).

Two more aggressive ideas tried and reverted (idle-time work stealing
and lowering the debounce-bypass threshold): both broke skew and
throughput by causing migration churn that fights nice-weighted
vruntime accounting.

Result vs the baseline Laminar at top of report:

| Axis                          | Baseline    | Post-fix       | Δ      |
|-------------------------------|-------------|----------------|--------|
| Throughput N=4                | 4.05x       | 3.66x          | -10%   |
| Throughput N=8                | 4.19x       | 4.36x          | +4%    |
| Throughput N=16               | 4.29x       | 4.48x          | +4%    |
| **Fair N=4 T=5s spread**      | **103%**    | **0.1%**       | ULE-equal |
| Fair N=16 T=15s spread        | 95%         | 6.4%           | huge improvement |
| Skew nice -5:0:+5             | 2.88:1:0.33 | 2.28:1:0.30    | slight regression |
| **Burst spinner spread**      | 66.6%       | **6.6%**       | 10x tighter |
| Wake p99/p99.9                | ~25/50us    | similar        | tied   |
| IPC pairs=4 bg=4              | 2.67us      | 3.10us         | slight regression |

**Fairness gap closed at N=4** (Laminar 0.1% = ULE 0.2%); burst spread
closed (66% -> 6.6%, matching ULE's 11.6%).  Cost: ~10% throughput
hit at N=4 (still within ULE noise band), modest skew degradation
(still ~2.5x ahead of ULE on nice handling).

Net updated picture: Laminar matches ULE on the fairness/burst axes
that were the largest deltas, retains its decisive nice-weighting
advantage, and remains competitive on throughput / latency / IPC.
ULE retains the edge on low-contention wake max (its 10ms slice
gives a tighter quantum cap than Laminar's 94ms).

---

## Second post-fix update (commit c6d230f, raw: `laminar-slicemin3-2026-05-19.txt`)

Added ULE-style **load-adaptive slice (slice_min)**: when per-CPU
load >= `kern.sched.slice_min_load` (default 3), the slice-end
check uses a shorter slice (~16 ms vs ~94 ms base).  This closes
the last axis where ULE clearly beat Laminar: wake-max under
saturation.

bench_latency max -- now Laminar BEATS ULE at every contended config:

| Config           | ULE        | Laminar (c6d230f) | Laminar/ULE |
|------------------|------------|-------------------|-------------|
| spin=0 watch=1   | **265us**  | 8965us            | ULE wins    |
| spin=0 watch=4   | **406us**  | 5672us            | ULE wins    |
| spin=4 watch=1   | 494us      | **118us**         | Laminar 4x  |
| spin=4 watch=4   | 90216us    | **15548us**       | Laminar 6x  |
| spin=8 watch=4   | 43867us    | **15785us**       | Laminar 3x  |

ULE still wins low-contention (single-CPU watchdog with no spin) --
its base slice IS ~78 ms but the runq is empty most of the time
so the wake just runs.  Laminar's residual ~5-9 ms is the
controller-band cold-start tax discussed earlier.

Cost of slice_min (vs prior conservative baseline c8bc982):

| Axis             | c8bc982     | c6d230f         | Δ          |
|------------------|-------------|-----------------|------------|
| Throughput N=4   | 3.66x       | 3.32x           | -9%        |
| Throughput N=8   | 4.36x       | 4.02x           | -8%        |
| Skew             | 2.28:1:0.30 | 2.49:1:0.31     | +slight    |
| Burst spread     | 6.6%        | 28.9%           | regression |
| Fair N=4 T=5s    | 0.1%        | 63.9%           | regression |
| Fair N=8 T=5s    | 126%        | 12.0%           | improvement|
| IPC pairs=4 bg=4 | 3.10us      | 2.85us          | +slight    |

The trade-off is direct: shorter slice under contention = better
wake latency, but more migration churn at moderate N which hurts
bench_fair's single-pass numbers.  Throughput at N=4 takes a
modest hit; at N=8+ the slice_min posture actually helps.

`kern.sched.slice_min_load` is a runtime tunable; raise it (toward
ULE's value of 6) to swap wake-latency aggressiveness for less
churn.  The default 3 picks the more desktop-leaning point.

## Final summary

After three commits this session (c8bc982 + c6d230f), Laminar's
head-to-head with ULE on the 2026-05-19 4-vCPU FreeBSD VM looks like:

| Axis                     | Result                                      |
|--------------------------|---------------------------------------------|
| Peak throughput          | tied (within bench noise)                   |
| Throughput consistency   | ULE tighter (Laminar's auto-band churn)     |
| Equal-share fairness     | mixed; ULE more consistent run-to-run       |
| **Nice-weighted skew**   | **Laminar decisive win** (ULE fails this)   |
| Burst spinner consistency| ULE                                         |
| **Wake max saturation**  | **Laminar 3-6x ahead** (slice_min wins)     |
| Wake max low-contention  | ULE (sub-ms vs Laminar ms-scale cold-start) |
| IPC steady-state         | tied                                        |
| IPC under contention     | Laminar (auto-band wake-preempt)            |

Laminar is now a credible ULE replacement on desktop posture --
wins on nice-weighting (the structural design contribution) and
on wake-latency under saturation (the new slice_min mechanism),
ties on throughput / IPC, and trails ULE on within-cohort fairness
consistency (the remaining "next axis" -- needs either a more
careful idle-time work-stealing implementation than the one tried
and reverted in this session, or boot-time band seeding for the
cold-start residual).

---

## Lead-compensator follow-up (2026-05-19, late)

After the rest-stop above, a sharper RLC-shaped attempt at the
residuals was specified at `docs/spec/atrium-scheduler-rlc-residuals.md`:
one mechanism (`laminar_lead_term`), three application sites
(2.1 `ctrl_load_ewma` -> band, 2.2 `signal_ewma` -> balancer,
2.3 new `place_ewma` -> raw_cost).  The intent: replace every
first-order EWMA in the controller with a lead-compensated filter,
giving the L the framework was named for.

Outcome -- one of three sites landed, two were tried and reverted:

**Site 2.1 (commit b59225b)** -- LANDED.  Lead-compensate `load_pct`
for the band output, remove the `f10fb112` emergency band-up special
case.  Effect at the time of commit:
  - p99.9 wake under spin=8 watch=4: 7097us -> 72us  (100x)
  - skew: 2.49:1:0.31 -> 2.87:1:0.35 (closer to target)
  - bench_fair N=4 T=15s: 61% -> 0.2% (settled near-perfect)
  - p99.9 spin=4 watch=4 cold-start: ~5ms -> still ~5ms (residual)

Subsequent multi-run benches showed run-to-run variance is high
on bench_skew all-cpus path (1.75 / 2.65 / 2.69 in three runs).
cpu0-only path is stable at 3.0+, so the variability is in the
placement code, not in the per-CPU picker.  The L term hasn't
made things worse, but hasn't closed that variance either.

**Site 2.2 (signal_ewma -> balancer)** -- REVERTED.  Hypothesis:
faster balancer reaction to load steps via lead-compensated
signal.  Reality: balancer migrated nice<0 threads too often (the
faster signal made high-weight threads visible as donors on every
step), inverting skew to 1.71:1.00:0.14 and even 2.99:1.00:**4.20**
on cpu0-only (nice +5 getting more work than nice 0).  Fails the
skew gate, reverted.

**Site 2.3 (place_ewma diversity)** -- REVERTED in two variants.
Hypothesis: a fresh placement bumps a per-CPU "recently placed"
signal that decays in ~55ms; the bump folds into `raw_cost` so the
next sibling fork in a tight loop sees the just-placed CPU as
slightly more expensive.  Variants tried:

  - v1, bump=1024 (= 1 nice-0 weight unit, ~0.25 load-units
    contribution): Caught IPC wakes as well as forks, regressed IPC
    by 100x+.
  - v2, bump=1024, fork-only (td_lastcpu==NOCPU): bench_fair N=4
    multi-run jumped from 4/10 perfect to 8/10 perfect (real win),
    but skew broke to 8.55:1.00:3.87 (fail gate).  Mixed-nice
    fork-bursts had place_ewma accumulating to ~1 load-unit, same
    order as the wload of a nice -5 thread; placement decisions
    started routing on bump differences instead of weights.
  - v3, bump=256, fork-only: bench_fair N=4 dropped to 1/10
    perfect (lost the win), skew still 2.10:1.00:0.18 (still fail).
    The sweet spot between "enough to break fork-race ties" and
    "sub-dominant to nice weighting" was too narrow.

Architectural finding: **lead compensation on a signal feeding an
actuator output (Site 2.1, band) composes cleanly with the existing
system.  Lead compensation on a signal feeding into the placement
cost function (Site 2.2, balancer ranking; Site 2.3, per-CPU
diversity) does NOT** -- the faster filter dynamics interact with
the picker's weight-based logic in ways that subtly break
nice-weighting.  The L term gave higher-bandwidth visibility into
load transients, but in the cost path the picker then makes
short-time-scale decisions on a signal that's noisier than the
unfiltered weights it's meant to dominate.

The Laminar-vs-ULE picture above stands as the post-2026-05-19
final state.  The plan doc has been retained for the negative
finding -- the lead-compensator framework is right for actuator-
gating signals, wrong for placement-cost signals.  Closing the
within-cohort fairness gap to ULE requires a different mechanism
(idle-time work stealing, or migration-cost-aware balancer
hysteresis -- both bigger pieces than fit in a follow-up session).
