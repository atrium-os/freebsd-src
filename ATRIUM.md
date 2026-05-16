# Atrium-OS downstream changes to FreeBSD

This file indexes Atrium-OS-specific additions to this fork.  Upstream
FreeBSD content is untouched except where noted in each section.

## Schedulers

### SCHED_LAMINAR (phase A)

New per-CPU runqueue scheduler that replaces ULE's priority-bucket
timeshare pick with a packed-SoA min-vruntime pick over per-shard
cached minima.  Lives at:

- `sys/kern/sched_laminar.c` — the implementation
- `sys/conf/files`, `sys/conf/options` — registration
- `sys/amd64/conf/GENERIC`, `sys/arm64/conf/std.arm64` —
  compiled-in (default scheduler remains ULE)
- `tools/test/sched-laminar/` — fair / skew / bursty proxy tests
  with a small `spin` helper

Selected at boot time via `kern.sched.name="Laminar"` in
`/boot/loader.conf`.

Design and implementation rationale (the "why"): the implementation
spec lives in the parent Atrium-OS repository at
`docs/spec/atrium-scheduler-impl.md` (kept out of this tree because
it references parts of the wider Atrium platform).  See also
`scratch/sched-sim/` in the same parent repo for the simulator that
drove the load-balancer and DVFS design.

Phase A scope:
1. Skeleton + per-thread / per-CPU data structures
2. `sched_instance` slot fill-in (priority, lifecycle, runqueue ops,
   switch, affinity, etc.)
3. SMP placement (cross-CPU pickcpu / setcpu / tdq_notify)
4. SoA min-vruntime picker (A.4) + hierarchical shard layer
5. Wake re-base with bounded lag (A.5) -- `kern.sched.lag_cap`
6. Compile into GENERIC (A.6, A.7)
7. Stress tests (A.8) -- `tools/test/sched-laminar/`
8. This index (A.9)

Known gaps in phase A (deferred):
- Real cross-CPU load balancer (the picker honors `ts_cpu` but
  there is no periodic `sched_balance` equivalent yet; APs sit
  idle when work is unbalanced)
- Nice -> `ts_eff_weight` derivation (every thread weighs 1
  today; `skew.sh` will surface this until landed)
- IPC affinity (whitepaper §6), NUMA (§8), DVFS coupling (§4)
