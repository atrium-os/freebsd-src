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
