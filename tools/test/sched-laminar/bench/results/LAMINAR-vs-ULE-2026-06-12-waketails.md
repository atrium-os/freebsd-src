# Wake-latency tails A/B — plan P0.2 baseline (+ P0.3 reproduction)

2026-06-12, kernel #141 (LAMINAR-DEV, both schedulers compiled in; switched via
`kern.sched.name` loader tunable + reboot). 4-CPU QEMU/HVF guest. `wakelat`
(p50/p90/p99/p99.9/max µs), 5 passes per config + 3 heavy-tail passes.
Raw: `laminar-2026-06-12.txt`, `ule-2026-06-12.txt`. This is the plan's D10
"undeclared-tier validation" baseline — run as *validation, not design driver*.

## Summary (medians across passes)

| Config                 | Metric  | Laminar    | ULE        | verdict |
|------------------------|---------|------------|------------|---------|
| spin=0 watch=1 (idle)  | p50     | **5.4**    | 6.0        | Laminar, and far more *consistent* |
|                        | p99     | **28**     | 128        | Laminar (ULE varies 31–934) |
|                        | p99.9   | 453        | 1450       | Laminar (both noisy) |
|                        | max     | 4.8 ms     | 4.6 ms     | parity (Laminar 2.5–9.3, ULE 0.3–15.8) |
| spin=8 watch=4 (mod.)  | p50     | 6.0        | **5.8–6.2**| parity |
|                        | p99     | **15384 (plateau)** | 39 | **ULE, decisively** |
|                        | p99.9   | 15.7 ms    | **0.11 ms**| **ULE** |
|                        | max     | 29 ms      | 41 ms      | parity-ish |
| spin=16 watch=4 (sat.) | p99     | 683        | **25**     | **ULE** |
|                        | p99.9   | 13.6 ms    | **2.7 ms** | **ULE** |
|                        | max     | **2.2–2.8 s (bimodal!)** | 52–70 ms | **ULE, decisively** |

## Findings

1. **The symmetric bounded-lag clamp paid off at low contention.** vs the
   2026-05-20 verdict ("wake max low-contention: ULE 5–10x"), Laminar is now the
   *more consistent* scheduler on an idle box (tight p50/p99 across every pass;
   ULE wanders run-to-run). That deficit is gone.

2. **NEW deficit isolated — the ~15.39 ms p99 plateau at spin=8.** Four of five
   passes show p99 pinned at 15371–15392 µs: a *quantum signature*. ~1% of wakes
   land while all CPUs run spinners and **wait out a slice** instead of
   preempting; ULE's wakeup-preemption keeps its p99 at 23–55 µs. Laminar's
   clamp makes the waker *competitive at the next pick* but nothing *forces* the
   pick. This is precisely the gap the plan's lane preemption / modulate-`L`
   work (P2/P5) addresses — and per D10, this measured gap is what licenses a
   wakeup-preempt (or slice-knob) fix in the WFQ tier. It should fall out of
   P2's lane preemption mechanism applied at wake.

3. **P0.3 REPRODUCED: the bimodal multi-second max persists at saturation.**
   spin=16: maxes 2.79 s / 37 ms / 2.22 s (2 of 3 passes multi-second; the
   clamp fixed this regime at spin=8 but not spin=16). ULE never exceeds 70 ms.
   Contradicts the 2026-05-20 "wake max saturation: Laminar 3–6x" verdict at
   this N — the regime flips somewhere between the configs.
   **Hypotheses to chase (in likelihood order):**
   (a) *stale shard-min masking*: a woken thread inserted into a shard whose
       cached min doesn't reflect it (or whose stale min hides it) sits
       invisible to the hierarchical picker until an unrelated invalidation —
       bimodality = "depends which slot/shard you land in";
   (b) *no forced preemption at wake* (finding 2) compounding with (a) — the
       15 ms plateau becomes seconds when the pick *also* misses;
   (c) *balancer blind spot*: the watchdog parked on an over-loaded CPU with no
       idle-steal pulling it while siblings go idle between spinner slices.
   Next step per plan P0.3: DTrace/KTR the multi-second wait path (timestamp
   enqueue → first pick) to discriminate (a)/(b)/(c).

## Plan consequences

- D10's parity band: **met at low contention, NOT met at moderate/heavy.** The
  measured gaps (plateau + bimodal max) license the wakeup-preemption fix —
  which is already on the roadmap as lane preemption (P2) + modulate-`L` (P5),
  now with a number to beat: p99 ≤ ~100 µs at spin=8, max ≤ ~100 ms at spin=16.
- P0.3 is reproduced, characterized, and has discriminating hypotheses; the
  DTrace pass is the remaining work before any balancer claims.

## P0.3 round 2 — DTrace discrimination (same day)

Instrumented enqueue→on-cpu latency (`waketrace.d`, `waketrace2.d`).

**Result 1 (the big one): the multi-second delay is *pre-enqueue*.** A traced
run reproduced max = 5.22 s in wakelat, yet **zero** enqueue→on-cpu gaps over
200 ms fired for wakelat threads. The woken thread was never sitting on a
runqueue for seconds — the time elapses between the *intended* wake (nanosleep
expiry) and the *enqueue*. Hypothesis (a) (stale shard-min picker masking) is
**dead in its original form**; the suspect is the **timer/wakeup path**:
callout → sleepq wakeup → setrunnable. Since ULE on the same kernel caps at
70 ms, it is *scheduler-dependent* — pointing at how Laminar schedules/permits
the softclock ("clock"/"intr") threads, or at the cpu_tick/eventtimer **rearm
interaction** (one-shot ARM MPCore eventtimer, `periodic=0`: a missed rearm
costs until the next scheduled event — seconds-scale by nature).

**Result 2: the artifact is DTrace-shy.** 0-for-9 traced runs reproduced the
multi-second max (vs ~40% untraced). P(0/9 | p=0.4) ≈ 1% — the tracing's extra
interrupts/timing perturbation almost certainly mask it. Consistent with a
lost/late timer-rearm or IPI race, not a queue-state bug.

**Next discriminators (non-perturbing):**
1. `piperlat`: watchdogs block on a pipe written by an **rtprio spinning
   ticker** (no callout anywhere in the wake path). Multi-second persists →
   post-wakeup scheduler path after all; vanishes → callout/eventtimer path
   confirmed.
2. KTR (compiled-in, near-zero overhead) timestamps in `sleepq_timeout` vs
   callout-scheduled time — fold into the P2 kernel work (the lane needs
   callout precision instrumentation anyway, plan risk R2).
3. Audit Laminar's treatment of `PRI_ITHD` enqueue/preempt vs ULE's, and the
   `cpu_idle`/eventtimer rearm interaction (no idle at spin=16, so rearm happens
   from the tick path — does Laminar's tick handling differ?).

Status: P0.3 narrowed from "scheduler picker bug" to "timer-or-ithread wake
path, perturbation-masked, Laminar-correlated". The plan's R2 risk (callout
precision in the VM) and this artifact may be the same animal.

## P0.3 round 3 — piperlat (callout-free wake path) + an RT placement bug

`piperlat`: a busy-waiting ticker (no nanosleep → no callout → no eventtimer in
the wake path) writes send-timestamps into pipes; timeshare watchdogs block in
read(). Deltas cover wakeup→enqueue→pick→on-cpu only. (Percentiles are skewed
by queue-drain after a stall — **max** is the comparable stat. Also fixed a
fork-fd-inheritance deadlock in the harness itself; see source comment.)

**Result 1 — the pipe wake path never goes multi-second.** Non-RT, spin=16,
4 runs: max 31–47 ms (p50 2.6 µs; p99 ~8–15 ms = the same no-wake-preempt slice
plateau). Today's interleaved wakelat controls happened not to fire the
multi-second event (0/4; ~13% chance given the ~40% rate), so strictly this is
cumulative rather than same-session evidence — but across *all* runs ever, the
callout path has produced 2.2–5.2 s maxes repeatedly and the pipe path never
exceeded 47 ms. The artifact lives in the **callout/eventtimer wake path**.

**Result 2 — NEW, deterministic, severe: placement is RT-blind.** One RT
busy-looper (piperlat `rt`): p50 14 ms, **p90 6.19 s, p99.9 8.17 s, max 8.19 s**
— reproducible, not bimodal. Timeshare watchdogs keep being placed on the
RT-occupied CPU and starve for the rest of the run: the placement signal
(wload = Σ timeshare weights) does not see non-timeshare occupancy, and nothing
steals them back. **Plan impact (P2-prerequisite):** the deadline lane runs
threads above timeshare — without counting lane/RT occupancy in the placement
signal (or marking such CPUs unavailable to timeshare placement), the lane
would reproduce this starvation systemically. Fix belongs in `sched_laminar.c`
placement (count PRI_ITHD/RT/lane occupancy into the effective load, or exclude
occupied CPUs), gated + benched like every phase.

**P0.3 disposition:** (i) multi-second nanosleep max → callout/eventtimer path,
perturbation-sensitive; close via KTR in `sleepq_timeout`/callout rearm during
P2 (same instrumentation as plan risk R2). (ii) RT-blind placement → confirmed
bug with a deterministic reproducer (`piperlat N M T 1000 rt`); fix scheduled as
a P2 prerequisite. (iii) The 15.39 ms p99 plateau (finding 2 above) → wake
preemption, addressed by P2's lane-preempt mechanism.

## P0.3 round 4 — RT-occupancy placement fix: landed, verified, ~500x

Live observation (procstat during starvation) found the precise mechanism and
broke the first fix attempt open: the trapped threads are **pipe wakers at
kernel sleep priority (43)** — not timeshare — so a timeshare-only penalty
exempted exactly the victims. They funnel to the RT CPU because it has the
lowest wload (the RT hog counts as ~1 unit), the IPC-affinity home points at
their waker (the ticker, on that CPU), and once queued there the
above-timeshare picker path correctly prefers the pri-8 incumbent forever.

Fix (`kern.sched.rt_occupied_cost`, default 64, RWTUN): in
`laminar_thread_cost()`, add the penalty when the target CPU's running thread
is above timeshare class AND the candidate cannot preempt it
(`ct->td_priority <= td->td_priority`) — no candidate-class gate. Same-boot
A/B (piperlat 16 4 10 1000 rt):

| | p50 | p90 | max |
|---|---|---|---|
| rt_occupied_cost=64 | 5.4–6.4 ms | 13 ms | 31–157 ms |
| rt_occupied_cost=0  | 2.8–3.1 s  | 6.8–7.1 s | 8.19 s |

~500x at p50/p90. No-RT regression: piperlat p50 2.6 µs unchanged; wakelat
spin=8 clean. Residual ms-scale p50 under RT load is queue-drain skew + the
known no-wake-preempt quantum — P2 territory, not starvation.

Note for P2: the same penalty term is exactly where **deadline-lane occupancy**
will plug in (lane threads run above timeshare), so the lane inherits this fix
by construction.

Remaining P0.3 thread: the bimodal multi-second *nanosleep* max (callout path)
— unchanged by this fix, still closed via KTR in P2. Full bench_all regression
sweep before P2 starts.

## Regression sweep after the rt_occupied_cost fix (same day)

Full suite on the fixed kernel (`laminar-2026-06-12-postfix.txt`), vs the
2026-05-20 Laminar baseline:

- **burst**: 181.6 wakes/s vs 155.6 — **+17%**.
- **skew**: all-cpus 2.74:1.00:0.30 (target 3.05); cpu0-only **exactly
  3.05:1.00:0.33** — nice weighting intact.
- **latency**: all configs clean; notably spin=8 p99 = 17.3 µs in the sweep
  pass (the 15.39 ms plateau did not appear — promising but needs repeated
  passes before claiming; possibly the penalty now scatters simultaneous
  kernel-priority wakers that previously piled onto one CPU).
- **ipc**: 1-pair RTT 1.34 µs, 4-pair 2.12 µs — in line with May.
- **throughput/fair**: noisy as documented (fork-storm variance). Knob on/off
  A/B (2 runs each side + 2 confirm): N=8 cost=64 → 4.20/3.31/4.10/4.44x,
  cost=0 → 4.18/4.18x — overlapping distributions, no attributable
  regression; the knob-on set contains the single best result of the day
  (4.44x at 6.1% spread).

**Verdict: the placement fix is regression-clean within the suite's noise
envelope, with burst improved and the RT-starvation pathology eliminated
(~500x).** One bench-harness note: scripts lose +x over 9p (run from a guest
copy) and spin-based benches need `../spin` next to the bench dir — the first
sweep silently produced garbage iters without it.

## Phase I first landing — the deadline lane core (same day)

The lane is in (`kern.sched.deadline_enable`, default 0): per-CPU entity array,
EDF-before-WFQ pick (behind POSIX RT), CBS budgets with **precise switch-boundary
charging** (the statclock-quantum version mis-throttled 15% on an idle box —
7.9ms ticks vs ms budgets), absolute per-entity replenishment callouts pinned to
the entity's CPU with **in-kernel lateness instrumentation**, lane wake-preempt
(vruntime-gate + cooldown bypass), forced pickcpu placement (NOT sched_bind —
"userret: Returning with pinned thread", learned by panic), /dev/laminar
ioctls (SPONSOR/WITHDRAW/YIELD/STATS), `metronome` gate test.

Gate status (metronome, audio shape 1.2/2.7ms + frame shape 4.8/16.2ms):
- **idle: PERFECT** — 0 misses, 0 throttles, max replenish lateness 23 µs.
- under 16 spinners: misses correlate 1:1 with replenish-callout lateness
  (227ms late → 274 misses; 8.8ms → 7; 11.9ms → 1). Throttles 0 — the lane
  scheduling itself is correct; **the sole remaining blocker is callout
  latency under load, i.e. the P0.3 artifact, now measured in-kernel**
  (a C_ABSOLUTE callout firing 8–227ms late while spinners run).

Next: instrument WHERE the callout latency lives (hardclock→swi-enqueue vs
swi-enqueue→handler) — the swi wake path through setpreempt looks correct on
inspection, pointing at eventtimer reprogramming / callout-wheel processing
under load on ARM/HVF. Lane disabled (default): bench numbers unchanged.

## Phase-I gate CLOSED (2026-06-12, second pass)

Three kernel fixes landed after the first phase-I pass:

1. **Replenish callout → C_DIRECT_EXEC.** The 8–227 ms replenish lateness
   under 16 spinners was the softclock-swi *scheduling* latency (the swi
   waits behind spinners), not eventtimer drift: running the replenish in
   the timer interrupt itself collapsed worst lateness to 11–35 µs loaded.
   This also localizes the P0.3 wakelat artifact: nanosleep-class wakes
   ride the same swi path.
2. **Wake-preempt gate dropped `!le_yielded`.** The YIELD sleep's backstop
   timeout and the replenish callout expire at the same instant; when the
   timeout path made the thread runnable first, `le_yielded` was still
   true and the lane wake-preempt was skipped (lane_preempts stuck at 1,
   alternating misses, 2 periods/cycle). The backstop now also sleeps T/8
   *past* the deadline so the replenish wakeup() is the normal waker.
3. **cdevpriv dtor + pick guard.** A metronome killed mid-run (ssh drop)
   left a stale entity whose le_td pointed at a freed thread → UAF panic
   in the lane pick ("ltdq_slot[i] == td"). Entity teardown is now the
   cdevpriv destructor (fd close == withdraw, idempotent), and the EDF
   pick skips entities whose thread is not queued on this CPU
   (ts_cpu != ltdq_id, e.g. balancer-migrated).

### Gate results (lane kernel, deadline_enable=1)

    idle  audio:  periods=100  misses=0 throttles=0 max_replenish_late_us=1189 (boot noise)
    16sp  audio:  periods=1000 misses=0 throttles=0 max_replenish_late_us=35
    16sp  audio:  periods=1000 misses=0 throttles=1 max_replenish_late_us=31
    16sp  frame:  periods=200  misses=0 throttles=0 max_replenish_late_us=11
    counters: lane_wakes=2300 lane_preempts=2200 lane_wake_blocked=0
    metronome wake-vs-grid (16sp): max 18 us, mean ~0

An admitted (Q,T) entity on a fully saturated 4-vCPU timeshare kernel
wakes within tens of microseconds of its period grid and misses zero
deadlines — the P1 lane.rs proof now holds on the real kernel.
Regression sanity with deadline_enable=0: piperlat p50 2.6 us, known
equal-priority p99 plateau unchanged.

## ULE A/B + long tails (2026-06-12, pre-broker checkpoint)

Same kernel binary both sides (LAMINAR-DEV carries both schedulers;
`kern.sched.name` tunable selects at boot). Workload: audio shape
(T=2.7 ms, work=1 ms), 10000 periods (~27 s), 16 spinners on 4 vCPUs.
metronome-posix = clock_nanosleep(TIMER_ABSTIME) grid + busy work;
"miss" = work not finished by period end.

| config                  | misses        | wake p50 | p99    | p99.9  | max     |
|-------------------------|---------------|----------|--------|--------|---------|
| Laminar LANE            | 56–223 (runs) | (a)      | (a)    | (a)    | (a)     |
| Laminar timeshare       | 9859 (98.6%)  | 14.8 s   | —      | —      | 28 s    |
| Laminar rtprio          | 23            | 6.7 µs   | 25 µs  | 6.2 ms | 13.9 ms |
| ULE timeshare           | 332 (3.3%)    | 5.9 µs   | 49 ms  | 89 ms  | 100 ms  |
| ULE rtprio              | 0             | 5.9 µs   | 15 µs  | 34 µs  | 473 µs  |
| RT idle ctrl (Laminar)  | 0–4           | 5.2 µs   | 21 µs  | 0.1–1.5 ms | 1.1–8 ms |
| RT idle ctrl (ULE)      | 0             | 4.7 µs   | 17 µs  | 49 µs  | 1.1 ms  |

(a) The lane metronome's user-side percentiles proved unreliable at long
horizons: residuals spread uniformly over [0, T/2] while the kernel saw
only ~2% misses — if phase truly wandered, ~37% of replenishes would
catch the thread mid-work. Suspected userspace-timecounter vs kernel-sbt
phase artifact under HVF; un-diagnosed, kernel counters are the valid
measure. Short-window raw lateness (500 periods, 16 spinners): max 18 µs.

pipe-wake tails (piperlat 16 spinners, 4 watchdogs, 10 s × 3 passes):

    Laminar: p50 2.6 µs  p99 2.8–15.4 ms  p99.9 24–47 ms  max 47–78 ms
    ULE:     p50 2.4 µs  p99 24–28 µs     p99.9 126–142 ms max 315–402 ms

### Reading

1. **The lane is the only mechanism giving unprivileged, admitted work
   near-RT periodicity.** Laminar timeshare fails the workload outright
   (no equal-priority wake preempt — by design); the lane brings it to
   ~0.5–2% misses, all correlated with rare platform/kernel stalls (see
   3), with admission control and overrun isolation rtprio cannot give
   (an admitted runaway gets throttled; an RT runaway starves the box,
   and rtprio needs root).
2. **Undeclared tails, honestly:** ULE's interactivity heuristic wins
   p50–p99 on pipe wakes (25 µs vs Laminar's slice plateau, 2.8–15.4 ms);
   Laminar's bounded-lag clamp wins the extreme tail by ~5× (max 78 ms
   vs 402 ms). The federation doc's "competitive on undeclared tails"
   holds at p99.9/max, not at p99 — the declared lane (not a heuristic)
   is Atrium's answer for the wakes that matter.
3. **OPEN before phase J:** Laminar boots show rare 7–97 ms stall events
   that ULE boots do not (same horizons, adjacent runs): replenish
   direct-exec callouts 31–97 ms late, RT wake max 13.9 ms vs ULE
   473 µs, idle-control max 8 ms vs 1.1 ms. A late DIRECT callout means
   a delayed timer interrupt — pointing at interrupts-disabled sections
   (spinlock hold times: balancer? sharded reduction?) or a Laminar-
   correlated HVF artifact (IPI rate), NOT scheduling policy. Must be
   bracketed before the frescod broker builds on lane latency.

## Stall investigation: the "Laminar-only stalls" verdict (2026-06-12)

gapdet.c (new): rtprio tight loop pinned to a CPU recording execution
gaps — from userspace a multi-ms gap is host/HVF descheduling, a long
interrupts-off section, or higher-priority kernel-thread monopoly.

Dirty window (the boot that produced the A/B numbers above):
Laminar cpu0 idle: 20 gaps, worst 11.3 ms; 16 spinners: 267 gaps,
worst 27.2 ms. ULE (measured LATER): 1 gap ~1 ms in 30 s.

Fresh Laminar boot, same load matrix: 0–5 gaps, worst 1.4 ms —
ULE-grade — with the balancer at its default 100 ms period, parked at
10 s, and re-enabled (balancer NOT implicated). deadline_enable on or
off: no effect. And the lane gate at the same 10k-period horizon that
previously showed 56–223 misses: **0 misses, 0 throttles, worst
replenish lateness 139 µs–1.3 ms** — twice, including with gapdet
running concurrently on another CPU.

### Corrected verdict

The stalls are EPISODIC and window-correlated, not systematically
Laminar: the dirty window's ULE measurements were taken after the
window had passed, so "Laminar-only" was a time-confounded comparison.
A 96 ms-late C_DIRECT_EXEC callout is a delayed timer interrupt — no
guest scheduler path plausibly holds interrupts off that long; host
(HVF vCPU descheduling) episodes remain the prime suspect. What IS
established: on a quiet system the lane delivers 0/10000 misses with
sub-1.5 ms worst replenish lateness, and Laminar's gap profile equals
ULE's.

Protocol going forward: gapdet is the standing sentinel; if a dirty
window recurs, run gapdet on BOTH kernels INSIDE the window before
attributing. The phase-J broker is unblocked.

## Phase J.1 — broker sponsorship landed (2026-06-12)

Kernel: LAMIOC_SPONSOR_FOR {pid, tid, q_us, t_us, anchor_ns} — a broker
fd sponsors CLIENT threads; anchor_ns phase-aligns the period grid to a
hardware timestamp (vblank). One fd owns many entities (cdevpriv cookie
+ le_priv tag; fd close sweeps all — broker crash reclaims its clients).
LAMIOC_WITHDRAW_FOR (owner-checked). thread_dtor eventhandler reclaims a
client's entity when the client dies with no fd-close tied to it.
Privilege: root (PRIV_SCHED_RTPRIO) until the manifest deadline_broker
capability (plan D9).

vbroker (mock frescod): sponsors N forked clients on a shared synthetic
vblank grid; clients work+YIELD per frame; SIGKILL robustness mode.

Gates (frame shape 4800/16667, work 4 ms, 600 periods, 16 spinners):
- 2 clients: 0 misses each (1 pre-sync startup period, baselined).
- 4 clients: admission spreads one per CPU; 0 misses each, replenish
  lateness 71–79 µs.
- kill test: SIGKILL client 0 mid-run — no panic, entity reclaimed via
  thread-dtor, broker WITHDRAW_FOR returns ESRCH, survivor 0 misses.
- regression: self-sponsored metronome audio shape 1000 periods 0 miss.

Next (J.2): frescod sponsors real Fresco client frame threads anchored
to the display kmod's vblank; EVFILT_DEADLINE miss delivery to the
broker.

## Phase J.2a — the broker miss feed (2026-06-12)

A missed deadline now reaches the broker as an event, not just a
counter: the replenish callout records {pid, tid, periods, misses} into
the owning fd's ring; EVFILT_READ on the broker fd fires; read(2)
drains the records. Notification defers through taskqueue_fast — the
deadline machinery stays in the timer interrupt, only the policy
notification rides a thread.

Two lessons paid for in panics:
1. taskqueue_enqueue CANNOT run under the tdq spin lock — enqueue wakes
   the taskqueue swi, and that wake re-enters sched_add ("laminar
   setcpu cross-CPU recursion"). Deferred past the unlock, next to the
   already-deferred wakeup().
2. taskqueue_thread cannot be enqueued from a direct-exec callout at
   all (sleep mutex in interrupt context); taskqueue_fast exists for
   exactly this.

Gate (vbroker miss mode, client 0 stalls 100 ms mid-run, 16 spinners):
broker receives MISS events pid/tid-correct for exactly the stall
window (periods 302-304, misses 1-3); client 0 final misses = 6 = the
stall's frame count; client 1 untouched (0 misses). Clean/kill/
metronome regressions all hold.

frescod's event loop shape is now complete on the kernel side: kevent
on the broker fd → read miss records → react by policy (skip a frame,
resize a buffer, withdraw a hopeless client). Next: J.2b — frescod
sponsoring real Fresco client frame threads anchored to the display
kmod's vblank.

## Phase J.2b — frescod IS the broker (2026-06-12, bsd repo af1912d)

The mock broker is retired: real frescod (frescod-aqueduct, venus
profile, real 60 Hz vblank from /dev/atrium-display0) now sponsors
client frame threads. Clients send OP_LANE_REQUEST {pid, tid, q_us}
over the Fresco socket; frescod verifies pid against LOCAL_PEERCRED,
sponsors with T = the connector refresh interval, grid anchored to the
latest wait_vblank return; disconnect withdraws; the J.2a miss feed
drains into the compositor loop each vblank.

Gate (in-VM, 16 spinners): lane-client sponsored via the protocol; 606
frames 0 misses; deliberate 100 ms stall = exactly 6 misses, each
arriving in frescod's log with correct pid/tid/period in real time.

Phase J complete: brokered, vblank-anchored, miss-fed deadline lane,
kernel-to-compositor. Next per plan: P4 inheritance (K-a turnstile
deadline bands), the lyrad audio broker when lyrad exists, and the
manifest deadline_broker capability to replace the root gate (D9).

## Phase K-a — deadline inheritance via the turnstile band (2026-06-12)

A sponsored, in-budget lane entity now carries its user priority at the
BAND (PRI_MIN_TIMESHARE - 1, top of the kernel range). Inheritance then
needs no new propagation machinery: when a lane thread blocks on a
kernel lock or a PTHREAD_PRIO_INHERIT umtx, the EXISTING turnstile /
umtx PI lends the band to the holder.

The Laminar-specific design lesson (paid for in a wrong first cut):
**lent priority is invisible inside the WFQ tier** — Laminar picks
timeshare by vruntime, so a holder boosted to PRI_MIN_TIMESHARE stayed
in the SoA and inherited nothing (measured: PI WORSE than the plain
control). The band must sit BELOW PRI_MIN_TIMESHARE so a boosted holder
routes to the priority-bucket runq that tdq_choose's rt-path picks
ahead of everything timeshare — the holder wins SELECTION, not just
preemption checks. Three corollaries landed with it: the rt-path defers
active lane threads to the EDF scan (same-CPU lane entities keep real-
deadline order instead of FIFO-within-band); the statclock throttle
demotes the band for the rest of the period (an overrunner at kernel-
range priority would defeat its own isolation); the lane wake path
re-bands after a clean replenish.

Gate (lane-pi: lane thread takes a shared mutex each 5 ms period; a
nice-20 holder process grabs it in 300 us bursts; 16 spinners; clean
window verified by gapdet, worst gap 936 us):

    PLAIN (PRIO_NONE) control:  misses = 213 / 2001 (10.65%)
    PI (PRIO_INHERIT) + band:   misses =   0 / 2001 (0.00%)

Audio-chain-shaped inversion, fully closed by K-a. Regressions hold
(metronome 0/1000, vbroker kill-test clean). Remaining for K-b:
deadline LENDING with charge-back (the holder runs on the blocked
entity's budget) and Aqueduct deadline-context propagation.

## D9 — the deadline_broker capability (2026-06-12)

SPONSOR_FOR's root gate is replaced by the manifest capability's kernel
half: kern.sched.deadline_brokers (write "pr_id 0|1", host-root only —
a jail cannot self-grant; read lists granted jails). portcullisd writes
it at jail materialization from the manifest. Host root keeps
priv_check; jailed callers need their prison flagged. tdfind targets
are additionally filtered by p_cansee, so a jailed broker can only
sponsor threads inside its own visibility.

Gate (in-VM): host broker unchanged (clean); jailed broker without the
grant cannot sponsor (EPERM, clients never admitted); grant -> jailed
broker runs clean; revoke -> denied again; self-grant from inside the
jail -> EPERM. Deadlines are now Portcullis capabilities end to end,
per the federation doc's admission rule.

## Phase K-b — deadline lending with charge-back (2026-06-12)

Explicit adoption is the lending primitive: a broker-privileged server
thread ADOPTs a lane client's entity (LAMIOC_ADOPT {pid,tid} / DROP) —
it gains the K-a band for SELECTION while its on-cpu time is CHARGED to
the client's CBS budget. Band priority is never free: it always burns
an admitted budget, so adoption cannot out-schedule the admission cap.
Gated like SPONSOR_FOR (D9 broker capability + p_cansee); le_gen guards
entity-slot reuse; turnstile-path attribution stays K-a-approximate
(the PI lend API does not carry the lender).

Two bugs found by the gate, both now fixed:
1. Charging stamped only at pick — a band-priority burn that never
   switches was never charged. Now stamped at ADOPT.
2. A lane wake could not preempt a BAND-priority incumbent
   (laminar_is_timeshare(ctd) excluded it) — an adopted server burning
   on the client's CPU blocked the client's own wakes (measured as a
   53% miss storm). Lane wakes now AST band incumbents too; re-choose
   orders by deadline.

Gate (16 spinners): client 1501 periods 0 misses; the server's 50 ms
adopted burn produced exactly burn/T = 6-7 entity throttles (the
charge-back arithmetic, period-exact); metronome and lane-pi
regressions hold. sys/sys/proc.h: thread0_storage reserve 10 -> 14
u64s for the grown td_sched.

The kernel primitive for Aqueduct deadline-context propagation is now
complete: a server handling a deadline client's request ADOPTs for the
request's duration — the userspace protocol work rides this.

## K-b in frescod — the first real deadline context (bsd 925c8e9)

No wire change needed: frescod's broker ledger already knows which
clients are sponsored, so on a successful lane request the client's
READER thread adopts the client's entity (request dispatch at band
priority on the client's budget) and its WRITER adopts lazily on first
delivery (frame callbacks / input ride the client's reservation).
Self-regulating: a heavy client throttles itself, never frescod.

In-VM (venus, 16 spinners): sponsored client 603 frames 0 misses with
both threads adopted. Explicit wire-level deadline context for
cross-service chains (app -> lyrad -> driver) rides the same primitive.

## P6 (CPU half) — the energy-optimal DVFS floor f* (2026-06-12)

The load-proportional DVFS target is now clamped to the energy-optimal
floor: E(f) = k·W·f² + P_static·W/f is convex with minimum at
f* = (P_static/2k)^(1/3) — BELOW f*, running slower wastes energy
(leakage over the longer runtime beats the dynamic saving; race-to-idle
at f* wins). In-kernel f* needs no model constants: energy per unit
work at level i is (P_i − P_idle)/f_i straight from the cpufreq level
table; f* = argmin, recomputed each step. kern.sched.dvfs_idle_mw
(platform idle power the table excludes) raises f*;
kern.sched.dvfs_fstar_idx is the observable. Levels without power data
leave f* unconstrained (no behavior change).

Gate (cpufreq_mock 5-level table): f* = idx 3 (800 MHz — the table's
P/f argmin; the 400 MHz level is voltage-floored so P/f rises again,
realistic); dvfs_idle_mw=300 moves f* to idx 4 as the math says; under
power_policy=0 (powersave) the controller settled at cur_idx == 3 ==
f* — the policy that previously dove to 400 MHz now stops at the
energy-optimal floor. Clamp-only change, inert at phaseh_enable=0.

Caveats recorded honestly: lane regressions could not be evaluated
tonight — both VM profiles entered a dirty window (gapdet sentinel: 564
gaps/41 ms worst headless; 240 ms-late direct callouts on venus),
metronome dirty with phaseh OFF too, so the noise is the episodic
host-stall class, not P6 (which is inert when disabled). Re-gate lanes
in a clean window. The across-member half of P6 (watt-budget
water_fill) awaits the GPU member integration.
