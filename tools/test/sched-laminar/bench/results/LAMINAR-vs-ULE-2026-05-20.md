# Laminar vs ULE — full bench_all comparison, 2026-05-20

Second head-to-head after the picker-placer coupling investigation.
Same kernel binary contains both schedulers; selected via
`kern.sched.name` loader tunable.  4-vCPU FreeBSD 16.0-CURRENT arm64
guest on Apple Silicon HVF.

Laminar settings: defaults (`ctrl_band_enable=1`, `ctrl_band_max=4`,
`slice_min_load=3`, `placer_alpha=0`, etc.).  ULE: stock defaults
(`kern.sched.ule.slice=10`).

Raw output: `laminar-2026-05-20.txt` / `ule-2026-05-20.txt` next to
this file.

---

## 1. Throughput (`bench_throughput`)

Speedup vs single-CPU spinner; spread = (max-min)/mean across N
workers.

| N  | Laminar   | spread | ULE     | spread |
|----|-----------|--------|---------|--------|
| 1  | 0.99x     | 0.0%   | **1.03x** | 0.0%  |
| 2  | 1.98x     | 0.3%   | **2.05x** | 0.2%  |
| 4  | 3.32x     | 39.1%  | **4.00x** | 0.3%  |
| 8  | **4.16x** | 55.3%  | 4.03x   | 10.0%  |
| 16 | **4.16x** | 57.5%  | 4.06x   | 12.3%  |
| 32 | **4.21x** | 30.7%  | 4.09x   | 10.6%  |

**Verdict:** Laminar holds a slight peak edge at N≥8 (within bench
noise), but its spread is much wider at every N≥4.  N=4 ULE wins
20% — that's the placement variance the picker-placer investigation
confirmed is structural in Laminar's wload-only placer.

## 2. Equal-share fairness (`bench_fair`)

Spread of per-worker iteration counts.

| Config       | Laminar    | ULE     |
|--------------|------------|---------|
| N=4 T=5s     | **0.1%**   | 0.1%    |
| N=4 T=15s    | 48.7%      | **0.1%** |
| N=8 T=5s     | **7.7%**   | 13.7%   |
| N=8 T=15s    | 145.9%     | **2.6%**|
| N=16 T=5s    | **9.9%**   | 20.2%   |
| N=16 T=15s   | 107.8%     | **6.9%**|

**Verdict:** ULE more consistent at long T.  Laminar can hit perfect
(N=4 T=5s at 0.1%) but bimodality kicks in.  T=5s results are mixed;
T=15s ULE clearly tighter.

## 3. Nice-weighted skew (`bench_skew`)

Target ratio nice -5 : 0 : +5 = 3.05 : 1.00 : 0.33.

| Topology   | Laminar              | ULE                  |
|------------|----------------------|----------------------|
| all-cpus   | **3.61:1.00:0.33**   | 1.06:1.00:0.72 (fail) |
| cpu0-only  | **2.89:1.00:0.38**   | 1.25:1.00:0.87 (fail) |

**Verdict:** Laminar wins decisively, as before.  ULE has not gotten
better at nice handling.  Laminar all-cpus actually came out slightly
*over* target this time (3.61 vs 3.05 — placement variance once
again, but in the favorable direction).  See `skew_pinned` (commit
`92f0fc7f`) for a placement-isolated picker validation: Laminar
delivers 2.83–3.00 across 15 controlled runs.

## 4. Burst responsiveness (`bench_burst`)

4 bursters waking 10x/s against 4 background spinners over 10s.

| Metric                  | Laminar      | ULE          |
|-------------------------|--------------|--------------|
| burst_wakes total       | **1556**     | 1065         |
| burst_wakes rate        | **155.6/s**  | 106.5/s      |
| spinner_iters_mean      | 287k         | **359k**     |
| spinner spread          | 59.6%        | **12.9%**    |

**Verdict:** Laminar wakes ~46% more bursts/s; ULE keeps spinners
more uniform.  Same tradeoff as 2026-05-19.

## 5. Wake latency (`bench_latency`)

Microseconds.  Single 5s pass per config.

| Config           | Sched   | p50  | p90  | p99   | p99.9 | max     |
|------------------|---------|------|------|-------|-------|---------|
| spin=0 watch=1   | Laminar | 7.0  | 11.3 | 35.3  | 354   | 6900    |
|                  | ULE     | 6.4  | 8.0  | 19.5  | **188**| **809**|
| spin=0 watch=4   | Laminar | 13.8 | 20.8 | 35.0  | 179   | 6578    |
|                  | ULE     | 9.0  | 13.6 | 35.0  | 177   | **1313**|
| spin=4 watch=1   | Laminar | 7.2  | 11.6 | 42.1  | 316   | **6730**|
|                  | ULE     | 6.8  | 10.0 | 29.7  | **166**| 26905  |
| spin=4 watch=4   | Laminar | 9.4  | 15.4 | 40.7  | 7264  | **15758**|
|                  | ULE     | 7.1  | 11.0 | 39.2  | **333**| 86445  |
| spin=8 watch=4   | Laminar | 7.8  | 12.3 | 38.0  | 5810  | **14736**|
|                  | ULE     | 7.0  | 10.5 | 37.4  | **360**| 45235  |

**Verdict (saturation):** Laminar's `slice_min=16ms` cap holds at
saturation (max 14.7–15.8 ms) while ULE's 78 ms slice gives 45–86 ms
max — Laminar wins by 3-5× on contended-config max.

**Verdict (low-contention):** ULE wins.  Single-watchdog max 809 µs
vs Laminar 6.9 ms.  Laminar's p99.9 also spikes (5–7 ms) under
contention while ULE stays at 333–360 µs.

## 6. IPC ping-pong (`bench_ipc`)

Pipe round-trip microseconds; lower better.

| Config                | Laminar      | ULE          |
|-----------------------|--------------|--------------|
| pairs=1 bg=0          | **1.35**     | 3.96         |
| pairs=4 bg=0          | 1.83         | **1.61**     |
| slack=0 pairs=4 bg=0  | 1.93         | **1.59**     |
| slack=1 pairs=4 bg=0  | 2.03         | **1.62**     |
| slack=8 pairs=4 bg=0  | 1.66         | **1.61**     |
| pairs=4 bg=4          | **2.69**     | 4.31         |

**Verdict:** Laminar 3× faster on single pair, 1.6× faster under
background contention.  ULE wins steady-state at full CPU count.

---

## Summary

| Axis                       | Winner       | Margin    |
|----------------------------|--------------|-----------|
| Peak throughput            | Laminar      | +2-3%     |
| Throughput consistency     | **ULE**      | 3-6x tighter |
| Equal-share fairness short T | Laminar    | +/- tied  |
| Equal-share fairness long T  | **ULE**    | bimodality |
| **Nice-weighted skew**     | **Laminar**  | ULE fails |
| Burst wake rate            | **Laminar**  | +46%      |
| Burst spinner consistency  | **ULE**      | 4.6x tighter |
| Wake max low-contention    | **ULE**      | 5-10x     |
| Wake p99.9 low-contention  | **ULE**      | ~2x       |
| **Wake max saturation**    | **Laminar**  | **3-6x**  |
| IPC single pair            | **Laminar**  | 3x        |
| IPC under contention       | **Laminar**  | 1.6x      |
| IPC steady-state           | ULE          | small     |

**Net picture (essentially unchanged from 2026-05-19):**

Laminar's **wins** are structural (the result of the design): nice
weighting (decisive — ULE just doesn't implement it well), wake
latency under saturation (slice_min), IPC under contention
(auto-band cost-preempt).

Laminar's **deficits** are also structural: fork-storm placement
variance (manifests as throughput/fairness spread under load), and
controller-band cold-start (manifests as low-contention wake-max
tax and p99.9 spikes on first-pass benches).

Both deficits are knowns:

1. **Placement variance** -- 8 attempts to "fix" via various
   mechanisms in this session, all reverted.  The picker-placer
   coupling investigation (docs/spec/atrium-scheduler-picker-placer-
   coupling.md) closes with empirical proof (via `skew_pinned`) that
   the picker is correct; the unpinned bench measurements include
   incidental placement variance.

2. **Cold-start band-up** -- partially addressed by the lead-
   compensator on the band path (commit b59225b), but a small tax
   remains in the first 100 ms of any bench.

For a desktop posture (the auto-band default), Laminar's wins on
nice-weighting, saturated wake latency, and IPC are the relevant
axes.  For a strict-throughput-consistency posture, ULE is more
predictable.
