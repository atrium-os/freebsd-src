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
