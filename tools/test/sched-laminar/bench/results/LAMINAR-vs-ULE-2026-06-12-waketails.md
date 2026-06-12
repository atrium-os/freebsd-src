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
