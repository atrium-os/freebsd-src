/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Atrium Project
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Atrium Laminar scheduler.
 *
 * A unified cost-composition scheduler for FreeBSD/Atrium.  Coexists with
 * ULE and 4BSD; selected at boot via "kern.sched.name=Laminar".  This file
 * implements the struct sched_instance vtable defined in <sys/sched.h>.
 *
 * Design and rationale: docs/spec/atrium-scheduler-impl.md and
 * scratch/sched-sim/WHITEPAPER.md in the atrium-os/atrium repository.
 *
 * This file is built only when "options SCHED_LAMINAR" is set.  Slots are
 * populated incrementally across the atrium/scheduler-phase-A commit
 * series; until each slot lands, calling it panics.  The kernel can be
 * built with the option enabled at any commit in the series, but selecting
 * Laminar as the active scheduler is only meaningful once A.3 lands.
 */

#include "opt_sched.h"

#include <sys/types.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/cpuset.h>
#include <sys/callout.h>
#include <sys/conf.h>
#include <sys/event.h>
#include <sys/eventhandler.h>
#include <sys/fcntl.h>
#include <sys/poll.h>
#include <sys/priv.h>
#include <sys/selinfo.h>
#include <sys/uio.h>
#include <sys/jail.h>
#include <sys/limits.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/sbuf.h>
#include <sys/pcpu.h>
#include <sys/proc.h>
#include <sys/runq.h>
#include <sys/sched.h>
#include <sys/smp.h>
#include <sys/sysctl.h>
#include <sys/turnstile.h>
#include <sys/bus.h>
#include <sys/cpu.h>
#include <sys/energy_budget.h>
#include <sys/taskqueue.h>
#include <machine/smp.h>

#include "cpufreq_if.h"

#ifdef SMP
static void laminar_dvfs_step(int load_lead, int load_bare);
/* Phase H state referenced by laminar_ctrl_cb before its full definition. */
static int laminar_phaseh_enable;
static int laminar_phaseh_present;
static int laminar_dvfs_n_levels;
static int laminar_dvfs_cur_idx;
#endif

/*
 * Per-thread scheduler state.  Appended to each struct thread; accessed
 * via td_get_sched(td).  All fields are protected by the owning per-CPU
 * runqueue lock unless otherwise noted.
 */

/* ts_flags. */
#define	TSF_BOUND	0x0001	/* sched_bind(): thread cannot migrate */
#define	TSF_XFERABLE	0x0002	/* On a runqueue and transferable. */
#define	TSF_IDLE_CLASS	0x0004	/* PRI_IDLE: bypass vruntime ordering. */
#define	TSF_RT_CLASS	0x0008	/* PRI_ITHD/REALTIME: real-time runq. */
#define	TSF_INSOA	0x0010	/* Currently in the timeshare SoA arrays. */

/*
 * Per-CPU capacity for the SoA timeshare arrays.  Sized to cover
 * the steady-state per-CPU runnable count comfortably; we never
 * grow under the tdq spin lock (malloc is forbidden in that
 * context), so callers panic if the cap is exceeded.  A future
 * commit can move grow to an out-of-band taskqueue.
 */
#define	LAMINAR_TS_CAP		2048

/*
 * Hierarchical SoA layer.  The flat vruntime[] array is partitioned
 * into fixed-size shards; each shard caches its current min.  The
 * picker scans the shard-min array (small) to find the winning
 * shard, then scans within just that shard.  Total pick work is
 *   K + n/K  ops where K = ceil(ts_n / SHARD_SIZE).
 * Reduces O(n) to O(sqrt(n)) at the high end without changing the
 * picker's behavior for small n (with SHARD_SIZE=64 the per-pick
 * overhead at n=8 is one extra shard-min compare).
 *
 * Inserts maintain shard_min in O(1).  Removes are O(1) in the
 * common case (the removed entry was not its shard's min); when the
 * min is invalidated, the affected shard is rescanned at cost
 * O(SHARD_SIZE).
 *
 * Choosing SHARD_SIZE: smaller = better headroom (less work per
 * shard scan) but more shards to scan.  Sqrt of the design's
 * targeted high-end n (~few hundred to low thousands) puts the
 * optimum near 32-64; 64 also matches an aarch64 cache line.
 */
#define	LAMINAR_SHARD_SIZE	64
#define	LAMINAR_NSHARDS		(LAMINAR_TS_CAP / LAMINAR_SHARD_SIZE)
_Static_assert((LAMINAR_TS_CAP % LAMINAR_SHARD_SIZE) == 0,
    "LAMINAR_TS_CAP must be a multiple of LAMINAR_SHARD_SIZE");

/*
 * Nice -> ts_weight table.  Each step is a factor of ~1.25 (every
 * one-unit nice change is ~10% CPU share), matching CFS's convention.
 * Index = nice + 20, so [0]=nice -20, [20]=nice 0, [39]=nice +19.
 * The picker keeps the same vruntime accumulator on every tick; with
 * ts_eff_weight = LAMINAR_NICE_0_WEIGHT^2 / ts_weight, higher-weight
 * (lower-nice) threads accumulate vruntime more slowly and therefore
 * get picked more often, in correct proportion.
 */
#define	LAMINAR_NICE_0_WEIGHT	1024U

/*
 * RLC balancer scale + smoother constants.
 *   CAP_BASE is the canonical capacity unit.  A CPU at full
 *   capacity has ltdq_capacity = CAP_BASE; a half-capacity E-core
 *   would be CAP_BASE/2.  Scaled cost = (load + R) * CAP_BASE /
 *   capacity, so a half-capacity CPU's effective cost doubles
 *   (placement attracts proportionally less load).
 *
 *   EWMA weighting: new = (EWMA_OLD * prev + EWMA_NEW * sample) /
 *   (EWMA_OLD + EWMA_NEW).  3:1 is the simulator's converged
 *   smoothing for the post-scale signal.
 */
#define	LAMINAR_CAP_BASE	100
/*
 * EWMA smoothing for the per-CPU placement signal (C in the RLC
 * filter analogy).  Higher OLD:NEW ratio = more capacitance = longer
 * transient settle but better noise rejection.  Originally 3:1
 * (~400ms time constant @ 100ms sample) was tuned for count-based
 * load which flickered at tick boundaries; with wload (sum of
 * ts_weight) the underlying signal is intrinsically smoother, so
 * lower the cap to 1:1 (~200ms) and halve the transient settle.
 * Bench N=4 throughput jumped from ~3.0x to ~4.6x because cross-
 * CPU placement now converges within 1-2 balancer cycles instead
 * of waiting for the EWMA to catch up.
 */
#define	LAMINAR_EWMA_OLD	1
#define	LAMINAR_EWMA_NEW	1
#define	LAMINAR_EWMA_DEN	(LAMINAR_EWMA_OLD + LAMINAR_EWMA_NEW)

/*
 * Lead-compensated EWMA (the "L" in RLC).  Every Laminar filter today
 * is first-order (single EWMA pole = RC low-pass); a single pole forces
 * a tradeoff between noise rejection and transient response.  The
 * residual gaps to ULE after 2026-05-19 (cold-start wake-max, balancer
 * lag, fork-time placement spill) are all the same fact: a first-order
 * filter takes ~5 time constants to absorb a step input.
 *
 * Adding a zero to the transfer function -- a discrete lead compensator
 * -- gives near-instant response to step changes without compromising
 * steady-state smoothing.  Continuous-time:
 *
 *           1 + s * tau_d
 *   G(s) = ---------------
 *           1 + s * tau_p
 *
 * Discrete form composes with whatever LP the caller already runs.
 * The helper keeps one new state variable (prev_input) and returns
 * just the lead term; callers add it to their own LP-filtered value:
 *
 *   delta      = x - state->prev_input
 *   lead_term  = (K_d * delta) / LEAD_DEN
 *   prev_input = x
 *   output     = caller_lp + lead_term
 *
 * Keeping the helper LP-agnostic lets different sites pick their own
 * pole (ctrl_load_ewma uses 3:1 for patient park/unpark; signal_ewma
 * uses 1:1 for responsive balance ranking) without forcing a single
 * shape on all of them.
 *
 * K_d / LEAD_DEN is the lead gain.  Default 4/8 = 0.5 ⇒ output is
 * pushed halfway toward x on each step in addition to the LP response.
 * With K_d=0 the lead term is 0 and the output equals the LP exactly
 * (back-compat).  K_d > LEAD_DEN causes overshoot (under-damped);
 * under-damping is fine for fast actuators (band) but undesirable for
 * slow ones (park/unpark hysteresis), so each site picks its own K_d.
 *
 * Settled-input behaviour is identical to the pure EWMA: delta = 0,
 * lead term = 0, output = ewma.  No new tuning is required to preserve
 * existing steady-state behaviour at any site.
 */
#define	LAMINAR_LEAD_DEN		8
#define	LAMINAR_LEAD_KD_DEFAULT		4	/* 4/8 = 0.5 (no overshoot) */

/*
 * The state is just the previous input; the caller keeps its own LP
 * (different sites use different pole values -- 1:1 for signal_ewma,
 * 3:1 for ctrl_load_ewma -- and we want this helper to compose with
 * any of them).  laminar_lead_term computes only the derivative
 * contribution; callers add it to their existing LP-filtered value to
 * get the full lead-compensated output.
 */
struct laminar_lead_state {
	int	prev_input;	/* x[n-1] */
};

static __inline int
laminar_lead_term(struct laminar_lead_state *s, int x, int kd)
{
	int delta = x - s->prev_input;
	int lead;

	s->prev_input = x;
	/*
	 * Round-to-nearest, signed-correct for negative delta.
	 */
	lead = kd * delta;
	if (lead >= 0)
		lead = (lead + LAMINAR_LEAD_DEN / 2) / LAMINAR_LEAD_DEN;
	else
		lead = (lead - LAMINAR_LEAD_DEN / 2) / LAMINAR_LEAD_DEN;
	return (lead);
}

/*
 * IPC affinity tunables (whitepaper §6).
 *   CONF_MIN: confidence threshold at which the dom_waker slot is
 *     trusted as the thread's IPC home.
 *   CONF_MAX: saturation cap so a long-running pairing doesn't
 *     overflow the 16-bit counter or take forever to forget.
 *   slack: how much extra placement cost we will pay on the home
 *     CPU vs the global minimum to honor IPC affinity.  Larger =
 *     more IPC locality, less load balance; 0 = strict balance.
 */
#define	LAMINAR_IPC_CONF_MIN	3
#define	LAMINAR_IPC_CONF_MAX	16
/* Default; SYSCTL_INT registration sits next to the other balancer
 * tunables further down where the sysctl_ctx is.  Definition is here
 * so pickcpu (earlier in the file) can reference it. */
static int laminar_ipc_slack = 2;

/*
 * Phase E: per-jail proportional share.  Side-table keyed by
 * prison id rather than fields on struct prison itself -- keeps
 * the cross-tree ABI surface to zero at the cost of a small fixed
 * table.  Entries are claimed on first sysctl write or first
 * thread fork into the jail; lookup is lock-free (atomic int
 * compare on lpr_id).  An empty slot has lpr_id = -1.
 *
 * eff_weight formula extension (whitepaper §6 / spec §11 phase E):
 *
 *   eff_weight = NICE_0_WEIGHT^2 * jail_nthreads
 *              / (ts_weight        * jail_weight)
 *
 * Default jail_weight = NICE_0_WEIGHT, jail_nthreads = 1 -- both
 * cancel out and we recover the phase A-D formula.  Configured
 * jails get proportional share at the jail granularity: two jails
 * weighted 50:50 split CPU 50:50 regardless of how many threads
 * each runs.
 */
/*
 * Phase F: NUMA placement bias.  R_numa(td, c) = laminar_numa_alpha
 * when CPU c is in a memory domain other than the thread's inferred
 * home domain; 0 when same-domain.  Inferred home = the domain of
 * the CPU on which the thread most recently ran (whitepaper §8's
 * "recent-CPU bias" fallback; the spec notes this is inadequate for
 * the full NUMA story and the proper signal is a VM-side page
 * residency tracker, which is a separate larger project).
 *
 * On homogeneous single-domain systems (our dev VM, most laptops,
 * single-socket boxes) every pc_domain is 0 and R_numa is
 * unconditionally 0 -- the code path is inert and zero-cost.
 */
static int laminar_numa_alpha = 1;	/* extra cost per cross-domain hop */

/*
 * Above-timeshare occupancy penalty (phase I prerequisite, 2026-06-12).
 *
 * The placement signal (wload = sum of timeshare weights) sees an RT or
 * interrupt-class thread as just its weight (~1 nice-0 unit), but a
 * CPU-bound RT thread consumes the WHOLE cpu regardless of weight: for a
 * timeshare candidate the CPU is effectively zero-capacity, not
 * capacity-minus-one.  Untreated, the RT-hogged CPU looks LEAST loaded,
 * pickcpu funnels every wake onto it, and the victims starve until the RT
 * thread yields (measured: piperlat `rt` reproducer, p90 6.2s / max 8.2s
 * timeshare wake latency with a single rtprio busy-looper on a 4-CPU box).
 *
 * Fix shape (cost, not exclusion, per the RLC theme): when the target
 * CPU's *running* thread is above timeshare class, add a large cost so the
 * CPU is avoided while any alternative exists — but remains placeable when
 * everything is RT-occupied.  Read of the remote curthread is the same
 * unlocked atomic pattern as laminar_v_fair(); staleness only mis-prices
 * one decision.  Brief ithreads cause a transient penalty at worst (some
 * other CPU is chosen — harmless).
 */
static int laminar_rt_occupied_cost = 64;	/* in nice-0 thread units */

/*
 * Phase I: the declared real-deadline lane (CBS-lite over EDF).
 * Design: docs/spec/atrium-scheduler-federation.md §1/§3 + the
 * implementation plan D5-D9/S1-S3; math proven in gpusim lane.rs.
 *
 * A sponsored thread declares (Q budget, T period).  Admission caps
 * per-CPU lane utilization (sum Q/T, per-mille) at
 * laminar_deadline_util_max; entities are bound to their CPU.  The
 * pick is EDF among released, in-budget entities, ahead of the
 * timeshare SoA and behind the POSIX RT class (the escape hatch
 * stays on top).  Budget is charged at statclock; exhaustion
 * throttles the entity to plain timeshare until the per-entity
 * replenishment callout (absolute, C_PRECISE, pinned to the
 * entity's CPU) resets it at the period boundary — overrun
 * isolation: a lying entity cannot eat its neighbours' deadlines.
 * The callout also measures its own lateness (the plan's R2 / the
 * wakelat bimodal artifact instrumentation).
 *
 * Gated by laminar_deadline_enable.  It shipped 0 (inert) through
 * phase-I bring-up; now ON BY DEFAULT (1) -- the lane is proven
 * (gpusim L6: 0 underruns at the 5.3ms hardware-minimum buffer under
 * load where plain timeshare needs 128ms; crash isolation + K-b
 * adoption verified in-VM) and safe to enable: admission caps lane
 * util at laminar_deadline_util_max (75%/CPU), POSIX RT stays above
 * it as the escape hatch, overrun isolation throttles a liar, and
 * /dev/laminar is root-only (0600) so sponsorship is privileged.
 * Set to 0 to restore pure-timeshare behavior everywhere.
 */
#define	LAMINAR_LANE_MAX	8	/* entities per CPU */

struct laminar_lane_entity {
	struct thread	*le_td;		/* sponsored thread; NULL = free */
	int		le_cpu;
	bool		le_throttled;	/* budget exhausted this period */
	bool		le_yielded;	/* done; sleeping until replenish */
	int64_t		le_budget_us;	/* remaining budget this period */
	uint64_t	le_q_us;	/* declared budget per period */
	uint64_t	le_t_us;	/* declared period */
	sbintime_t	le_t_sbt;	/* period in sbintime */
	sbintime_t	le_deadline;	/* absolute current-period deadline */
	sbintime_t	le_run_start;	/* sbinuptime at last on-cpu (0 = off) */
	struct callout	le_callout;	/* period replenishment */
	void		*le_priv;	/* owning /dev/laminar fd cookie */
	u_char		le_saved_upri;	/* pre-band td_base_user_pri */
	bool		le_banded;	/* K-a band applied */
	uint64_t	le_gen;		/* bumped at teardown (K-b adopters) */
	/* stats (read via LAMIOC_STATS) */
	uint64_t	le_periods;
	uint64_t	le_misses;	/* period ended, work not yielded */
	uint64_t	le_throttles;
	uint64_t	le_max_late_us;	/* worst replenish-callout lateness */
};

static int laminar_deadline_enable = 1;		/* RWTUN master gate (on by default) */
static int laminar_deadline_util_max = 750;	/* per-mille per CPU */
static u_long laminar_lane_sponsors = 0;
static u_long laminar_lane_preempts = 0;
static u_long laminar_lane_wakes = 0;	/* lane wakes seen at local sched_add */
static u_long laminar_lane_wake_blocked = 0; /* incumbent not timeshare */
static void laminar_lane_priv_dtor(void *data);
static void laminar_lane_teardown(struct laminar_lane_entity *le);
static void laminar_lane_band_apply(struct thread *td,
    struct laminar_lane_entity *le);
static void laminar_lane_band_restore(pid_t pid, lwpid_t tid, u_char saved);

#define	LAMINAR_MAX_PRISONS	32
struct laminar_prison {
	int		lpr_id;		/* prison.pr_id, -1 = free */
	uint32_t	lpr_weight;	/* user-set; LAMINAR_NICE_0_WEIGHT default */
	uint32_t	lpr_nthreads;	/* atomic-updated runnable count */
	uint32_t	lpr_broker;	/* D9: deadline_broker capability */
};
static struct laminar_prison laminar_prisons[LAMINAR_MAX_PRISONS];

/*
 * Initialize all slots to "empty" (lpr_id = -1) at boot.  Done via
 * a SYSINIT so the array is set up before any thread fork.
 */
static void
laminar_prisons_init(void *arg __unused)
{
	int i;

	for (i = 0; i < LAMINAR_MAX_PRISONS; i++) {
		laminar_prisons[i].lpr_id = -1;
		laminar_prisons[i].lpr_weight = LAMINAR_NICE_0_WEIGHT;
		laminar_prisons[i].lpr_nthreads = 0;
		laminar_prisons[i].lpr_broker = 0;
	}
}
SYSINIT(laminar_prisons, SI_SUB_INTRINSIC, SI_ORDER_ANY,
    laminar_prisons_init, NULL);

/*
 * Lock-free lookup by pr_id.  Returns NULL if no slot is claimed
 * for this jail.  Linear scan -- LAMINAR_MAX_PRISONS is small.
 */
static struct laminar_prison *
laminar_prison_lookup(int pr_id)
{
	int i, slot;

	for (i = 0; i < LAMINAR_MAX_PRISONS; i++) {
		slot = atomic_load_int(&laminar_prisons[i].lpr_id);
		if (slot == pr_id)
			return (&laminar_prisons[i]);
	}
	return (NULL);
}

/*
 * Lookup or claim a slot for pr_id.  Used on first thread fork or
 * first sysctl write.  Compare-and-swap on lpr_id from -1 -> pr_id
 * makes claim atomic.  Returns NULL only when the table is full.
 */
static struct laminar_prison *
laminar_prison_claim(int pr_id)
{
	struct laminar_prison *lpr;
	int i, expected;

	lpr = laminar_prison_lookup(pr_id);
	if (lpr != NULL)
		return (lpr);
	for (i = 0; i < LAMINAR_MAX_PRISONS; i++) {
		lpr = &laminar_prisons[i];
		expected = -1;
		if (atomic_cmpset_int(&lpr->lpr_id, expected, pr_id)) {
			lpr->lpr_weight = LAMINAR_NICE_0_WEIGHT;
			lpr->lpr_nthreads = 0;
			return (lpr);
		}
		/* Lost the race?  Maybe this is now our pr_id. */
		if (atomic_load_int(&lpr->lpr_id) == pr_id)
			return (lpr);
	}
	return (NULL);
}

/*
 * Prison helper for a thread; returns NULL safely (e.g. thread0
 * during early init, or proc with no cred yet).
 */
static __inline struct prison *
laminar_prison_of(struct thread *td)
{

	if (td == NULL || td->td_proc == NULL ||
	    td->td_proc->p_ucred == NULL)
		return (NULL);
	return (td->td_proc->p_ucred->cr_prison);
}
static const uint32_t laminar_nice_weight[40] = {
	/* -20 */ 88761, 71755, 56483, 46273, 36291,
	/* -15 */ 29154, 23254, 18705, 14949, 11916,
	/* -10 */  9548,  7620,  6100,  4904,  3906,
	/*  -5 */  3121,  2501,  1991,  1586,  1277,
	/*   0 */  1024,   820,   655,   526,   423,
	/*   5 */   335,   272,   215,   172,   137,
	/*  10 */   110,    87,    70,    56,    45,
	/*  15 */    36,    29,    23,    18,    15,
};

static __inline uint32_t
laminar_nice_to_weight(int nice)
{
	int idx = nice + 20;

	if (idx < 0)
		idx = 0;
	else if (idx >= 40)
		idx = 39;
	return (laminar_nice_weight[idx]);
}

/*
 * laminar_set_weight: defined after struct td_sched (further below)
 * so the pointer dereferences typecheck.
 */

static MALLOC_DEFINE(M_LAMINAR, "laminar", "Laminar scheduler data");

/* Per-scheduler use of generic td_flags bits (mirrors ULE / 4BSD). */
#define	TDF_SLICEEND	TDF_SCHED2	/* Thread time slice is over. */

/* Common scheduler predicates (mirror ULE / 4BSD definitions). */
#define	THREAD_CAN_SCHED(td, cpu)					\
    CPU_ISSET((cpu), &(td)->td_cpuset->cs_mask)
#define	THREAD_CAN_MIGRATE(td)	((td)->td_pinned == 0)

struct td_sched {
	/* Picker hot fields. */
	uint64_t	ts_vruntime;	/* Virtual runtime. */
	uint64_t	ts_eff_weight;	/* Precomputed jail_nthreads /
					 * (weight * jail_weight). */
	uint32_t	ts_weight;	/* Nice-derived per-thread weight. */
	uint32_t	ts_wload_contrib;	/* Weight currently summed into
						 * owning tdq's wload; used for
						 * symmetric add/rem so nice
						 * changes don't drift wload
						 * out of sync with ts_weight. */
	uint32_t	ts_slot;	/* Index in tdq SoA arrays. */
	/* Placement / migration. */
	int		ts_cpu;		/* Current or last CPU. */
	uint16_t	ts_flags;	/* TSF_*. */
	uint8_t		ts_class;	/* RT / TIMESHARE / IDLE. */
	uint8_t		ts_class_pri;	/* Priority within RT/IDLE class. */
	/*
	 * IPC affinity (whitepaper §6).  Boyer-Moore single-slot
	 * majority tracker over wakeup edges, but recorded as the
	 * waker's CPU rather than the waker's thread pointer -- the
	 * pointer is unsafe (waker can exit) and the home decision
	 * pickcpu actually needs is a CPU/domain, not an identity.
	 * Most communicating partners stay on one CPU between
	 * migrations, so the CPU is an adequate proxy for identity.
	 * ts_dom_waker_cpu = -1 when slot is empty.
	 */
	int16_t		ts_dom_waker_cpu;
	uint16_t	ts_dom_waker_conf;
	/*
	 * Slice quantum (sched_clock follow-up).  Ticks accumulated
	 * since this thread was last switched in.  When it hits
	 * sched_slice (in stathz ticks), sched_laminar_clock sets
	 * TDF_SLICEEND so the thread voluntarily yields at the next
	 * AST -- without this, two CPU-bound threads at the same
	 * priority share-class never preempt each other (no priority
	 * IPI fires for same-class), and a freshly-woken thread can
	 * be enqueued behind a long-running spinner indefinitely.
	 */
	uint32_t	ts_slice_used;
	sbintime_t	ts_wake_ts;	/* timestamp (sbintime) at last
					 * enqueue via the wakeup/add path;
					 * 0 if not pending pick.  Used to
					 * measure wake-to-on-cpu delay for
					 * R4 instrumentation. */
	struct laminar_lane_entity *ts_adopted; /* (t) K-b: entity whose
					 * budget this thread burns while
					 * working on the client's behalf */
	uint64_t	ts_adopted_gen;	/* le_gen snapshot at ADOPT */
	sbintime_t	ts_adopt_start;	/* on-cpu stamp while adopted */
	u_char		ts_adopt_upri;	/* pre-adoption base_user_pri */
	struct laminar_lane_entity *ts_lane; /* (t) deadline-lane entity, or
					 * NULL.  Consumers re-check
					 * le_td == td (entities are a
					 * static array, never freed). */
	sbintime_t	ts_queue_ts;	/* timestamp at tdq_runq_add.
					 * pick - queue = queue-to-cpu delay
					 * (excludes wake-to-queue time). */
	/* NUMA (whitepaper §8). */
	uint16_t	ts_home_node_conf;
	int16_t		ts_home_node;	/* -1 = unset. */
	bool		ts_mem_bw;	/* Bandwidth-bound class flag. */
};

_Static_assert(sizeof(struct thread) + sizeof(struct td_sched) <=
    sizeof(struct thread0_storage),
    "increase struct thread0_storage.t0st_sched size for Laminar");

/*
 * Compute eff_weight including the optional jail term (phase E).
 * Centralised so the formula has exactly one definition.
 *
 *   eff_weight = NICE_0_WEIGHT^3 * jail_nthreads
 *              / (ts_weight       * jail_weight)
 *
 * The K = NICE_0_WEIGHT^3 numerator was chosen so that:
 *
 *   (a) nice 0 thread + default jail (jail_w = NICE_0_WEIGHT,
 *       jail_n = 1) -> eff_weight = NICE_0_WEIGHT (= 1024),
 *       recovering the phase A/D nice-only formula.
 *
 *   (b) nice -5 thread (ts_weight = 3121) + default jail ->
 *       eff_weight = 1024^3 / (3121 * 1024) = 1024^2 / 3121 = 335,
 *       NOT zero.  The earlier K = NICE_0_WEIGHT^2 caused the
 *       jail_weight default of 1024 to cancel one factor of 1024
 *       in the numerator, leaving 1024 / 3121 = 0 in integer
 *       division.  That made nice -5 threads' vruntime stop
 *       accumulating, so they monopolised the picker (lowest
 *       vruntime wins, and theirs never grew) -- bench_skew
 *       caught the resulting CPU-share inversion.
 *
 *   (c) overflow-safe: numerator <= 2^30 * 2^14 (jail_n max ~10k)
 *       = 2^44, fits in u64.  result <= 2^27, vruntime over years
 *       of running fits.
 */
static __inline uint64_t
laminar_compute_eff_weight(uint32_t ts_weight, struct prison *pr)
{
	struct laminar_prison *lpr;
	uint64_t jail_w = LAMINAR_NICE_0_WEIGHT;
	uint64_t jail_n = 1;

	if (pr != NULL && (lpr = laminar_prison_lookup(pr->pr_id)) != NULL) {
		jail_w = atomic_load_int(&lpr->lpr_weight);
		jail_n = atomic_load_int(&lpr->lpr_nthreads);
		if (jail_w == 0)
			jail_w = LAMINAR_NICE_0_WEIGHT;
		if (jail_n == 0)
			jail_n = 1;
	}
	return (((uint64_t)LAMINAR_NICE_0_WEIGHT *
	    LAMINAR_NICE_0_WEIGHT * LAMINAR_NICE_0_WEIGHT * jail_n) /
	    ((uint64_t)ts_weight * jail_w));
}

static __inline void
laminar_set_weight(struct td_sched *ts, uint32_t weight)
{

	ts->ts_weight = weight;
	/* Default path: no prison context; recover phase A/D formula. */
	ts->ts_eff_weight = laminar_compute_eff_weight(weight, NULL);
}

/*
 * Set weight for a thread including its current jail context.  Used
 * after fork (when prison is known) and from sched_laminar_nice
 * (for per-thread nice updates).
 */
static __inline void
laminar_set_weight_for_thread(struct thread *td, uint32_t weight)
{
	struct td_sched *ts = td_get_sched(td);

	ts->ts_weight = weight;
	ts->ts_eff_weight = laminar_compute_eff_weight(weight,
	    laminar_prison_of(td));
}

/*
 * Per-CPU runqueue.  The timeshare class uses a packed structure-of-arrays
 * for the Laminar min-vruntime pick (ltdq_vruntime[] hot, ltdq_slot[]
 * cold).  The real-time and idle classes reuse the standard struct runq
 * priority-bucket layout from sys/runq.h.
 *
 * Locking annotations:
 * (c)  constant after init
 * (l)  CPU-local accesses only
 * (ls) stores by the local CPU; loads may be lockless
 * (t)  protected by ltdq_lock
 * (ts) stores under lock; loads may be lockless
 */
struct laminar_tdq {
	struct mtx_padalign ltdq_lock;	/* Run queue spin mutex. */
	struct cpu_group *ltdq_cg;	/* (c) Topology pointer. */

	/*
	 * Run queue.  A single struct runq covering all priority bands
	 * (RT, TIMESHARE, IDLE) -- the same shape ULE uses.  Threads at
	 * different priorities land in different priority buckets within
	 * this runq.  Phase A.4 introduces an SoA scan that replaces the
	 * timeshare bucket lookup with min-vruntime; the SoA arrays below
	 * are declared now and remain unused until then.
	 */
	struct runq	ltdq_runq;	/* (t) Active runq for all classes. */
	uint32_t	ltdq_ts_n;	/* (t) Timeshare SoA slot count. */
	uint32_t	ltdq_ts_cap;	/* (c) Timeshare SoA capacity. */
	/* Deadline lane (phase I).  Array malloc'd at setup (keeps the
	 * DPCPU footprint flat); all access under the tdq lock. */
	struct laminar_lane_entity *ltdq_lane;	/* (t) LAMINAR_LANE_MAX slots */
	int		ltdq_lane_n;	/* (t) live entities */
	int		ltdq_lane_util;	/* (t) sum Q/T, per-mille */
	uint64_t	*ltdq_vruntime;	/* (t) Hot, scanned within winning shard. */
	struct thread	**ltdq_slot;	/* (t) Cold, by winner index. */
	uint64_t	ltdq_vtime;	/* (t) Virtual time floor (A.5). */
	/* Hierarchical layer: per-shard cached min over ltdq_vruntime. */
	uint64_t	ltdq_shard_min[LAMINAR_NSHARDS]; /* (t) per-shard min. */

	/*
	 * Power-aware migration barrier (phase B).  Added to ltdq_load
	 * to form the effective placement cost; high values make a CPU
	 * unattractive for wakeup placement and load balancing.  Tuned
	 * via per-CPU sysctl kern.sched.laminar.cpu.<N>.resistance.
	 * Loaded locklessly via atomic_load_int from pickcpu / balance
	 * paths; staleness of a few hundred ms is harmless.
	 */
	int		ltdq_resistance;	/* (ts) user-set R via sysctl. */
	int		ltdq_resistance_power;	/* (ts) controller-set R_power. */
	int		ltdq_ctrl_preempt_band;	/* (ts) controller-set R band for
						 *      preempt gates only;
						 *      stacked on user R.
						 *      Does NOT affect placement
						 *      cost (raw_cost) or balancer
						 *      evac heuristic. */

	/*
	 * RLC balancer filter state (phase C, DESIGN.md §1).  All terms
	 * are unconditional -- they are inert on homogeneous /
	 * single-leaf systems (capacity ratio = 1, leaf covers all CPUs,
	 * EWMA settles to raw cost), and active on heterogeneous / NUMA.
	 *
	 *   ltdq_capacity: per-CPU compute capacity, default
	 *     LAMINAR_CAP_BASE.  Half-capacity P/E core would be 50.
	 *     Settable via sysctl kern.sched.laminar.cpu.N.capacity.
	 *   ltdq_signal_ewma: smoothed (scaled) placement cost.  Used
	 *     for rank decisions; raw transferable count gates the
	 *     migrate (rank/gate decouple).
	 *   ltdq_streak: C2 debounce counter; donor must persist this
	 *     many cycles before action.  Evac (R > 0) skips.
	 *   ltdq_last_xfer: ticks @ last successful migration FROM this
	 *     CPU.  Adaptive-L cooldown gates subsequent migrations
	 *     off the same donor.
	 */
	int		ltdq_capacity;		/* (ts) per-CPU capacity. */
	int		ltdq_signal_ewma;	/* (t) smoothed scaled cost. */
	int		ltdq_streak;		/* (t) C2 debounce counter. */
	int		ltdq_last_xfer;		/* (t) ticks @ last migration. */
	int		ltdq_last_preempt;	/* (t) ticks @ last wake-preempt
						 *      IPI; cooldown for the
						 *      cost-based preempt path. */
	int		ltdq_last_local_preempt; /* (t) ticks @ last same-CPU
						 *      AST fire; cooldown for
						 *      local cost-preempt. */

	/* Aggregate state. */
	int		ltdq_load;	/* (ts) Total runnable. */
	uint64_t	ltdq_wload;	/* (ts) Sum of ts_weight of runnable
					 *      threads.  Replaces ltdq_load as
					 *      the balancer's load signal so
					 *      nice-weighted work spreads
					 *      proportionally to weight, not
					 *      thread count. */
	int		ltdq_sysload;	/* (ts) Non-ITHD load. */
	int		ltdq_transferable; /* (ts) Migration-eligible count. */
	int		ltdq_id;	/* (c) CPU id. */

	/* Per-CPU current state. */
	struct thread	*ltdq_curthread; /* (t) Running thread. */
	u_char		ltdq_lowpri;	/* (ts) Lowest priority on rq. */
	u_char		ltdq_owepreempt; /* (ts) Remote preempt pending. */
	short		ltdq_switchcnt;	/* (l) Switches this tick. */
	short		ltdq_oldswitchcnt; /* (l) Switches last tick. */
	char		ltdq_name[24];	/* (c) Per-instance lock name. */
};

#ifdef SMP
DPCPU_DEFINE_STATIC(struct laminar_tdq, ltdq);

#define	LAMINAR_TDQ_SELF()	((struct laminar_tdq *)PCPU_GET(sched))
#define	LAMINAR_TDQ_CPU(cpu)	(DPCPU_ID_PTR((cpu), ltdq))
#define	LAMINAR_TDQ_ID(tdq)	((tdq)->ltdq_id)
#else	/* !SMP */
static struct laminar_tdq laminar_tdq_cpu;

#define	LAMINAR_TDQ_SELF()	(&laminar_tdq_cpu)
#define	LAMINAR_TDQ_CPU(cpu)	(&laminar_tdq_cpu)
#define	LAMINAR_TDQ_ID(tdq)	(0)
#endif

#define	LAMINAR_TDQ_LOCKPTR(t)	((struct mtx *)(&(t)->ltdq_lock))
#define	LAMINAR_TDQ_LOCK(t)	mtx_lock_spin(LAMINAR_TDQ_LOCKPTR((t)))
#define	LAMINAR_TDQ_UNLOCK(t)	mtx_unlock_spin(LAMINAR_TDQ_LOCKPTR((t)))
#define	LAMINAR_TDQ_LOCK_ASSERT(t, type)				\
    mtx_assert(LAMINAR_TDQ_LOCKPTR((t)), (type))

/*
 * Tick-domain constants set in sched_laminar_initticks(), once stathz is
 * known.  sched_slice is the default time slice in stathz ticks before a
 * thread is reconsidered for preemption.
 *
 * sched_slice_min is the saturation-load slice (ULE's slice_min pattern).
 * Under heavy load (local runq has >= SCHED_SLICE_MIN_DIVISOR threads),
 * use the shorter slice so wakees rotate faster -- the same load-adaptive
 * mechanism ULE uses to get desktop-class wake-latency under saturation
 * without paying context-switch overhead at low load.
 *
 * Default: slice = ~94 ms (stathz/10), slice_min = ~16 ms (slice/6).
 * Both are sysctl-tunable for posture: shorter slice for desktop, longer
 * for batch / throughput.
 */
#define	SCHED_SLICE_DEFAULT_DIVISOR	10	/* ~94 ms at stathz=127 */
#define	SCHED_SLICE_MIN_DIVISOR		6	/* slice_min = slice/6 (~16 ms) */
static int __read_mostly realstathz = 127;
static int __read_mostly sched_slice = 10;
static int __read_mostly sched_slice_min = 1;
/*
 * Load threshold at/above which the slice-end check uses slice_min
 * instead of slice.  Default 3 = "true saturation per CPU"
 * (>= 3 runnable on this CPU).  ULE uses 6 (heavier saturation);
 * Laminar default is more aggressive for desktop wake-latency posture.
 * Lower = more wake-rotation (better latency, more context switches);
 * higher = closer to ULE.
 */
static int laminar_slice_min_load = 3;

/*
 * Restore a thread's td_lock after thread_lock_block().  ULE-style:
 * an atomic store of the saved mtx releases the BLOCKED_LOCK marker.
 * This is NOT a mutex unlock.
 */
static __inline void
thread_unblock_switch(struct thread *td, struct mtx *mtx)
{

	atomic_store_rel_ptr((volatile uintptr_t *)&td->td_lock,
	    (uintptr_t)mtx);
}

static void __dead2
sched_laminar_unimpl(const char *fn)
{

	panic("sched_laminar: %s not yet implemented", fn);
}

#define	UNIMPL()	sched_laminar_unimpl(__func__)

/* Forward declarations for the periodic balancer (defined below). */
static int tdq_add_internal(struct laminar_tdq *, struct thread *, int);
extern u_long laminar_lag_cap;
extern u_long laminar_preempt_cooldown;
extern u_long laminar_wake_pick_max_us;
extern u_long laminar_wake_pick_long_count;
extern u_long laminar_wake_pick_long_threshold;
extern u_long laminar_queue_pick_max_us;
extern char   laminar_wake_pick_max_comm[16];
extern char   laminar_queue_pick_max_comm[16];
#define LAMINAR_LONG_LOG	16
extern char   laminar_long_log[LAMINAR_LONG_LOG][16];
extern u_long laminar_long_log_count[LAMINAR_LONG_LOG];
extern u_long laminar_choose_calls;
extern u_long laminar_slice_ends;
extern u_long laminar_wake_picks;
extern u_long laminar_cp_ipi;
extern u_long laminar_cp_skip_v;
extern u_long laminar_cp_skip_cool;
extern u_long laminar_cp_skip_idle;
extern u_long laminar_cp_skip_owe;
extern u_long laminar_notify_ipi;
extern u_long laminar_notify_skip;
#define	LAMINAR_WAKE_LONG_US	100000ULL
static void sched_laminar_rem(struct thread *);
static int laminar_transferable(struct laminar_tdq *);

/*
 * Initialize a per-CPU runqueue.  Called once per CPU at boot.
 */
static void
tdq_setup(struct laminar_tdq *tdq, int id)
{

	if (bootverbose)
		printf("laminar: setup cpu %d\n", id);
	runq_init(&tdq->ltdq_runq);
	tdq->ltdq_id = id;
	tdq->ltdq_ts_n = 0;
	tdq->ltdq_ts_cap = LAMINAR_TS_CAP;
	tdq->ltdq_vruntime = malloc(tdq->ltdq_ts_cap *
	    sizeof(*tdq->ltdq_vruntime), M_LAMINAR, M_WAITOK | M_ZERO);
	tdq->ltdq_slot = malloc(tdq->ltdq_ts_cap *
	    sizeof(*tdq->ltdq_slot), M_LAMINAR, M_WAITOK | M_ZERO);
	tdq->ltdq_lane = malloc(LAMINAR_LANE_MAX *
	    sizeof(*tdq->ltdq_lane), M_LAMINAR, M_WAITOK | M_ZERO);
	for (int _i = 0; _i < LAMINAR_LANE_MAX; _i++)
		callout_init(&tdq->ltdq_lane[_i].le_callout, 1);
	tdq->ltdq_lane_n = 0;
	tdq->ltdq_lane_util = 0;
	tdq->ltdq_vtime = 0;
	tdq->ltdq_resistance = 0;
	tdq->ltdq_resistance_power = 0;
	tdq->ltdq_ctrl_preempt_band = 0;
	tdq->ltdq_capacity = LAMINAR_CAP_BASE;
	tdq->ltdq_signal_ewma = 0;
	tdq->ltdq_streak = 0;
	tdq->ltdq_last_xfer = 0;
	for (int s = 0; s < LAMINAR_NSHARDS; s++)
		tdq->ltdq_shard_min[s] = UINT64_MAX;
	snprintf(tdq->ltdq_name, sizeof(tdq->ltdq_name),
	    "sched lock %d", id);
	mtx_init(LAMINAR_TDQ_LOCKPTR(tdq), tdq->ltdq_name, "sched lock",
	    MTX_SPIN);
}

/*
 * Recompute the cached min for one shard from its current contents.
 * Cost: O(SHARD_SIZE).  Called when a remove invalidates the cached
 * min (the removed entry's value equalled it).
 */
static void
laminar_shard_recompute(struct laminar_tdq *tdq, uint32_t s)
{
	uint64_t best;
	uint32_t lo, hi, i;

	LAMINAR_TDQ_LOCK_ASSERT(tdq, MA_OWNED);
	lo = s * LAMINAR_SHARD_SIZE;
	hi = lo + LAMINAR_SHARD_SIZE;
	if (hi > tdq->ltdq_ts_n)
		hi = tdq->ltdq_ts_n;
	if (lo >= hi) {
		tdq->ltdq_shard_min[s] = UINT64_MAX;
		return;
	}
	best = tdq->ltdq_vruntime[lo];
	for (i = lo + 1; i < hi; i++) {
		if (tdq->ltdq_vruntime[i] < best)
			best = tdq->ltdq_vruntime[i];
	}
	tdq->ltdq_shard_min[s] = best;
}

/*
 * Hierarchical min-vruntime pick: scan per-shard cached mins to
 * find the winning shard, then linear-scan inside just that shard.
 * Total cost K + n/K where K = ceil(ts_n / SHARD_SIZE).
 */
static __inline uint32_t
laminar_min_index(const struct laminar_tdq *tdq)
{
	uint64_t shard_best, slot_best;
	uint32_t s, nshards, win_shard, win_slot, lo, hi, i;

	KASSERT(tdq->ltdq_ts_n != 0, ("laminar_min_index: empty"));

	nshards = (tdq->ltdq_ts_n + LAMINAR_SHARD_SIZE - 1) /
	    LAMINAR_SHARD_SIZE;
	shard_best = tdq->ltdq_shard_min[0];
	win_shard = 0;
	for (s = 1; s < nshards; s++) {
		if (tdq->ltdq_shard_min[s] < shard_best) {
			shard_best = tdq->ltdq_shard_min[s];
			win_shard = s;
		}
	}
	lo = win_shard * LAMINAR_SHARD_SIZE;
	hi = lo + LAMINAR_SHARD_SIZE;
	if (hi > tdq->ltdq_ts_n)
		hi = tdq->ltdq_ts_n;
	slot_best = tdq->ltdq_vruntime[lo];
	win_slot = lo;
	for (i = lo + 1; i < hi; i++) {
		if (tdq->ltdq_vruntime[i] < slot_best) {
			slot_best = tdq->ltdq_vruntime[i];
			win_slot = i;
		}
	}
	return (win_slot);
}

/*
 * Append td to the timeshare SoA arrays at index ts_n.  Maintains
 * the cached shard min in O(1).
 */
static void
laminar_slot_insert(struct laminar_tdq *tdq, struct thread *td)
{
	struct td_sched *ts;
	uint32_t i, s;

	LAMINAR_TDQ_LOCK_ASSERT(tdq, MA_OWNED);
	if (__predict_false(tdq->ltdq_ts_n == tdq->ltdq_ts_cap))
		panic("laminar_slot_insert: cpu %d SoA cap %u exhausted "
		    "(raise LAMINAR_TS_CAP)", tdq->ltdq_id, tdq->ltdq_ts_cap);
	i = tdq->ltdq_ts_n++;
	ts = td_get_sched(td);
	tdq->ltdq_vruntime[i] = ts->ts_vruntime;
	tdq->ltdq_slot[i] = td;
	ts->ts_slot = i;
	ts->ts_flags |= TSF_INSOA;

	s = i / LAMINAR_SHARD_SIZE;
	if ((i % LAMINAR_SHARD_SIZE) == 0)
		tdq->ltdq_shard_min[s] = ts->ts_vruntime;
	else if (ts->ts_vruntime < tdq->ltdq_shard_min[s])
		tdq->ltdq_shard_min[s] = ts->ts_vruntime;
}

/*
 * O(1) tail-swap remove from the timeshare SoA arrays.  Shard mins
 * stay correct without a rescan in the common case (removed entry
 * was not its shard's min); when invalidated, the affected shard
 * is rescanned (O(SHARD_SIZE)).
 */
static void
laminar_slot_remove(struct laminar_tdq *tdq, struct thread *td)
{
	struct td_sched *ts;
	uint64_t removed_v, moved_v;
	uint32_t i, last, si, slast;

	LAMINAR_TDQ_LOCK_ASSERT(tdq, MA_OWNED);
	ts = td_get_sched(td);
	MPASS((ts->ts_flags & TSF_INSOA) != 0);
	i = ts->ts_slot;
	MPASS(i < tdq->ltdq_ts_n);
	MPASS(tdq->ltdq_slot[i] == td);

	removed_v = tdq->ltdq_vruntime[i];
	last = --tdq->ltdq_ts_n;
	si = i / LAMINAR_SHARD_SIZE;
	slast = last / LAMINAR_SHARD_SIZE;
	moved_v = (i != last) ? tdq->ltdq_vruntime[last] : 0;

	if (i != last) {
		tdq->ltdq_vruntime[i] = moved_v;
		tdq->ltdq_slot[i] = tdq->ltdq_slot[last];
		td_get_sched(tdq->ltdq_slot[i])->ts_slot = i;
	}

	if (si == slast) {
		/*
		 * Same-shard remove: shard si lost the entry at i (which
		 * was removed_v).  If i != last, an entry that was in the
		 * same shard at `last' moved into slot i (still in shard
		 * si, but no net change to shard membership beyond losing
		 * removed_v).  Either way, we only have to rescan if the
		 * removed value was the cached min.
		 */
		if (removed_v == tdq->ltdq_shard_min[si])
			laminar_shard_recompute(tdq, si);
	} else {
		/*
		 * Cross-shard remove: shard si replaced removed_v with
		 * moved_v; shard slast lost the entry at `last' (moved_v).
		 */
		if (removed_v == tdq->ltdq_shard_min[si])
			laminar_shard_recompute(tdq, si);
		else if (moved_v < tdq->ltdq_shard_min[si])
			tdq->ltdq_shard_min[si] = moved_v;
		if (moved_v == tdq->ltdq_shard_min[slast])
			laminar_shard_recompute(tdq, slast);
	}
	ts->ts_flags &= ~TSF_INSOA;
}

/*
 * Class predicate.  Timeshare threads go through the SoA picker; RT
 * (ITHD/REALTIME/KERN) and IDLE threads use the standard runq.
 */
static __inline bool
laminar_is_timeshare(struct thread *td)
{

	/*
	 * Class AND numeric priority must both indicate timeshare.
	 * Kernel daemons (pagedaemon, bufdaemon, rand_harvestq, etc.)
	 * are created via kproc_create which sets td_pri_class to
	 * PRI_TIMESHARE (inherited from thread0) but td_priority to
	 * PVM (= 41, kernel-priority range).  Putting them in the
	 * SoA timeshare picker means vruntime fairness IGNORES their
	 * kernel priority -- spinners with lower vruntime got picked
	 * over them, stranding pagedaemon/rand_harvestq for seconds
	 * under heavy load.  This was the R4 root cause under load.
	 *
	 * Filter on priority too: only threads with priority in the
	 * timeshare numeric range belong in SoA; kernel-priority
	 * sleepers go to the priority-bucket runq where they are
	 * picked ahead of timeshare by tdq_choose's rt-path.
	 */
	return (PRI_BASE(td->td_pri_class) == PRI_TIMESHARE &&
	    td->td_priority >= PRI_MIN_TIMESHARE);
}

#ifdef SMP
/*
 * Walk the topology and initialize one runqueue per CPU.  Called from
 * sched_laminar_setup() on the boot CPU.
 */
static void
sched_setup_smp(void)
{
	struct laminar_tdq *tdq;
	int i;

	CPU_FOREACH(i) {
		tdq = LAMINAR_TDQ_CPU(i);
		tdq_setup(tdq, i);
		tdq->ltdq_cg = smp_topo_find(cpu_top, i);
		if (tdq->ltdq_cg == NULL)
			panic("laminar: no cpu group for cpu %d", i);
	}
	PCPU_SET(sched, DPCPU_PTR(ltdq));
}

/*
 * Register kern.sched.laminar.cpu.<N>.resistance for each online CPU.
 * Run from a SYSINIT after sysctl is up but before userspace lands.
 */
static void
laminar_sysctl_register(void *arg __unused)
{
	struct sysctl_oid *root, *cpu_node;
	struct laminar_tdq *tdq;
	char name[16];
	int i;

	root = SYSCTL_ADD_NODE(NULL,
	    SYSCTL_STATIC_CHILDREN(_kern_sched), OID_AUTO, "laminar",
	    CTLFLAG_RW | CTLFLAG_MPSAFE, NULL, "Laminar scheduler tunables");
	if (root == NULL)
		return;
	CPU_FOREACH(i) {
		tdq = LAMINAR_TDQ_CPU(i);
		snprintf(name, sizeof(name), "cpu%d", i);
		cpu_node = SYSCTL_ADD_NODE(NULL,
		    SYSCTL_CHILDREN(root), OID_AUTO, name,
		    CTLFLAG_RW | CTLFLAG_MPSAFE, NULL, "per-CPU tunables");
		if (cpu_node == NULL)
			continue;
		SYSCTL_ADD_INT(NULL, SYSCTL_CHILDREN(cpu_node), OID_AUTO,
		    "resistance", CTLFLAG_RW, &tdq->ltdq_resistance, 0,
		    "Power-aware placement R (added to load for pickcpu "
		    "and balance ranking)");
		SYSCTL_ADD_INT(NULL, SYSCTL_CHILDREN(cpu_node), OID_AUTO,
		    "capacity", CTLFLAG_RW, &tdq->ltdq_capacity, 0,
		    "Per-CPU compute capacity (LAMINAR_CAP_BASE = full; "
		    "lower for E-cores / capped cores).  Scales placement "
		    "cost: (load + R) * CAP_BASE / capacity.");
		SYSCTL_ADD_INT(NULL, SYSCTL_CHILDREN(cpu_node), OID_AUTO,
		    "signal_ewma", CTLFLAG_RD, &tdq->ltdq_signal_ewma, 0,
		    "Current smoothed scaled placement signal (read-only).");
		SYSCTL_ADD_INT(NULL, SYSCTL_CHILDREN(cpu_node), OID_AUTO,
		    "resistance_power", CTLFLAG_RD,
		    &tdq->ltdq_resistance_power, 0,
		    "Closed-loop controller's R_power for this CPU (RD; "
		    "nonzero = parked by controller).");
		SYSCTL_ADD_INT(NULL, SYSCTL_CHILDREN(cpu_node), OID_AUTO,
		    "ctrl_preempt_band", CTLFLAG_RD,
		    &tdq->ltdq_ctrl_preempt_band, 0,
		    "Controller-set R tolerance band for preempt gates (RD; "
		    "stacked on user R; affects preempt only, not placement).");
		SYSCTL_ADD_INT(NULL, SYSCTL_CHILDREN(cpu_node), OID_AUTO,
		    "load", CTLFLAG_RD, &tdq->ltdq_load, 0,
		    "Current runnable count on this CPU (RD; debug).");
		SYSCTL_ADD_U64(NULL, SYSCTL_CHILDREN(cpu_node), OID_AUTO,
		    "wload", CTLFLAG_RD, &tdq->ltdq_wload, 0,
		    "Sum of ts_weight of runnable threads (RD; debug).  "
		    "Divide by 1024 for nice-0-equivalent load.");
		SYSCTL_ADD_U64(NULL, SYSCTL_CHILDREN(cpu_node), OID_AUTO,
		    "vtime", CTLFLAG_RD, &tdq->ltdq_vtime, 0,
		    "Picker's vruntime floor on this CPU (= min V[i] for "
		    "i in runq).  Used by picker-placer coupling (vlag).");
	}
}
SYSINIT(laminar_sysctl, SI_SUB_KICK_SCHEDULER, SI_ORDER_FIRST,
    laminar_sysctl_register, NULL);
#endif

/*
 * Per-CPU runqueue primitives.  These mirror the ULE shape and serve
 * as the foundation for the slot implementations that follow.
 */

static __inline void
tdq_load_add(struct laminar_tdq *tdq, struct thread *td)
{

	struct td_sched *ts;

	LAMINAR_TDQ_LOCK_ASSERT(tdq, MA_OWNED);
	tdq->ltdq_load++;
	ts = td_get_sched(td);
	/*
	 * Symmetric wload accounting: contribute ts_weight, remember the
	 * exact value contributed so rem subtracts the same amount.  If
	 * ts_weight changes (nice()) while td is on a tdq, sched_laminar_nice
	 * fixes wload AND ts_wload_contrib under the tdq lock.  Without
	 * the contrib record, an add at weight X followed by nice + rem
	 * at weight Y would underflow wload to 0 and lose accounting.
	 */
	ts->ts_wload_contrib = ts->ts_weight;
	tdq->ltdq_wload += ts->ts_wload_contrib;
	if ((td->td_flags & TDF_NOLOAD) == 0)
		tdq->ltdq_sysload++;
}

static __inline void
tdq_load_rem(struct laminar_tdq *tdq, struct thread *td)
{
	struct td_sched *ts;
	uint32_t w;

	LAMINAR_TDQ_LOCK_ASSERT(tdq, MA_OWNED);
	KASSERT(tdq->ltdq_load > 0,
	    ("tdq_load_rem: load underflow on cpu %d", tdq->ltdq_id));
	tdq->ltdq_load--;
	ts = td_get_sched(td);
	w = ts->ts_wload_contrib;
	ts->ts_wload_contrib = 0;
	if (tdq->ltdq_wload >= w)
		tdq->ltdq_wload -= w;
	else
		tdq->ltdq_wload = 0;
	if ((td->td_flags & TDF_NOLOAD) == 0)
		tdq->ltdq_sysload--;
}

static __inline void
tdq_runq_add(struct laminar_tdq *tdq, struct thread *td, int flags)
{

	LAMINAR_TDQ_LOCK_ASSERT(tdq, MA_OWNED);
	THREAD_LOCK_BLOCKED_ASSERT(td, MA_OWNED);
	/*
	 * Mark the thread as on a runq.  Mirrors ULE's tdq_runq_add;
	 * sched_rem callers (including the balancer) assert TD_ON_RUNQ.
	 */
	TD_SET_RUNQ(td);
	td_get_sched(td)->ts_queue_ts = sbinuptime();
	if (laminar_is_timeshare(td))
		laminar_slot_insert(tdq, td);
	else
		runq_add(&tdq->ltdq_runq, td, flags);
}

static __inline void
tdq_runq_rem(struct laminar_tdq *tdq, struct thread *td)
{
	struct td_sched *ts;

	LAMINAR_TDQ_LOCK_ASSERT(tdq, MA_OWNED);
	ts = td_get_sched(td);
	if ((ts->ts_flags & TSF_INSOA) != 0)
		laminar_slot_remove(tdq, td);
	else
		(void)runq_remove(&tdq->ltdq_runq, td);
}

/*
 * Pick the next thread to run.  RT (priority < PRI_MIN_TIMESHARE)
 * always preempts timeshare; timeshare uses the SoA min-vruntime
 * pick; IDLE only runs when neither of the above is runnable.
 */
static struct thread *
tdq_choose(struct laminar_tdq *tdq)
{
	struct thread *rt;

	LAMINAR_TDQ_LOCK_ASSERT(tdq, MA_OWNED);
	rt = runq_choose(&tdq->ltdq_runq);
	if (rt != NULL && rt->td_priority < PRI_MIN_TIMESHARE) {
		struct laminar_lane_entity *rle = td_get_sched(rt)->ts_lane;

		/*
		 * A banded lane thread (K-a, priority LANE_BAND) lives in
		 * this runq but is DISPATCHED by the EDF scan below, so
		 * same-CPU lane entities order by real deadline rather
		 * than FIFO-within-band.  Anything else here (POSIX RT,
		 * kernel-priority sleepers, PI-BOOSTED HOLDERS at the
		 * band) is returned as before — the boosted holder
		 * winning selection IS the K-a inheritance mechanism.
		 */
		if (!(__predict_false(laminar_deadline_enable) &&
		    rle != NULL && rle->le_td == rt && !rle->le_throttled &&
		    !rle->le_yielded))
			return (rt);
		/* deferred: the EDF scan below dispatches lane threads */
	}
	/*
	 * Deadline lane: EDF among released, in-budget, un-yielded
	 * entities whose thread is runnable here.  Behind the POSIX RT
	 * class (the escape hatch stays on top), ahead of timeshare.
	 * Entities are few (<= LAMINAR_LANE_MAX); a linear scan is the
	 * S1 design.  The caller's tdq_runq_rem dequeues by ts_slot, so
	 * returning an SoA-resident thread out of vruntime order is
	 * fine; the vtime floor is deliberately not advanced (lane picks
	 * don't participate in WFQ fairness).
	 */
	if (__predict_false(laminar_deadline_enable) &&
	    tdq->ltdq_lane_n > 0) {
		struct laminar_lane_entity *le, *best = NULL;
		int i;

		for (i = 0; i < LAMINAR_LANE_MAX; i++) {
			le = &tdq->ltdq_lane[i];
			if (le->le_td == NULL || le->le_throttled ||
			    le->le_yielded || !TD_ON_RUNQ(le->le_td) ||
			    td_get_sched(le->le_td)->ts_cpu != tdq->ltdq_id)
				continue;	/* not queued here (migrated) */
			if (best == NULL || le->le_deadline < best->le_deadline)
				best = le;
		}
		if (best != NULL)
			return (best->le_td);
	}
	if (tdq->ltdq_ts_n != 0) {
		uint32_t i = laminar_min_index(tdq);
		/*
		 * Advance the per-CPU vruntime floor to the winner's
		 * vruntime.  Lockless readers (sched_laminar_wakeup's
		 * lag-cap rebase) tolerate slightly stale values.
		 */
		atomic_store_64(&tdq->ltdq_vtime, tdq->ltdq_vruntime[i]);
		return (tdq->ltdq_slot[i]);
	}
	return (rt);	/* IDLE or NULL */
}

#ifdef SMP
/*
 * Memory domain of CPU `cpu'.  Single accessor so we can swap it
 * cheaply (e.g. to honor cpuset memory-domain policy later).  On
 * single-domain systems this is always 0.
 */
static __inline int
laminar_cpu_domain(int cpu)
{

	return (pcpu_find(cpu)->pc_domain);
}

/*
 * R_numa: extra placement cost added when CPU `cpu' is in a memory
 * domain other than the thread's inferred home.  Inert when the
 * thread has no home recorded (ts_home_node < 0) or when alpha is
 * 0 (sysctl off).  No domain-distance matrix yet -- treats all
 * cross-domain hops as the same cost, which matches the spec's
 * recent-CPU-bias fallback.
 */
static __inline int
laminar_numa_cost(struct thread *td, int cpu)
{
	int home;

	if (laminar_numa_alpha == 0)
		return (0);
	home = td_get_sched(td)->ts_home_node;
	if (home < 0)
		return (0);
	if (home == laminar_cpu_domain(cpu))
		return (0);
	return (laminar_numa_alpha);
}

/*
 * V_fair: the system-wide fair-share vruntime, approximated as the
 * min ltdq_vtime across CPUs that have runnable threads.  Returned
 * value is in raw vruntime units (eff_weight per stathz tick of
 * accumulated weighted runtime).  Used by the picker-placer coupling
 * (vlag) -- see docs/spec/atrium-scheduler-picker-placer-coupling.md.
 *
 * Approximation rationale: avg is the "right" V_fair but requires a
 * div by count and an extra atomic per CPU.  On 4-CPU systems with
 * balanced load, min and avg agree to within a slice.  In imbalanced
 * cases min gives a slightly aggressive "you're behind" reading,
 * which is the conservative direction for placement decisions (lag
 * term clamps to [0,1] so over-estimating doesn't break anything).
 */
static __inline uint64_t
laminar_v_fair(void)
{
	uint64_t v_fair = UINT64_MAX;
	uint64_t v;
	int cpu;
	bool any = false;

	if (smp_started == 0)
		return (0);
	CPU_FOREACH(cpu) {
		struct laminar_tdq *tdq = LAMINAR_TDQ_CPU(cpu);
		struct thread *ct;

		if (atomic_load_int(&tdq->ltdq_resistance_power) > 0)
			continue;
		if (atomic_load_int(&tdq->ltdq_load) == 0)
			continue;
		/*
		 * Picker's "next to run" on this CPU is min(curthread.v,
		 * min(queued.v)).  ltdq_vtime is the queued min (set at
		 * choose time).  curthread.v is the running thread's live
		 * accumulated vruntime.  Take the min of both, falling back
		 * to whichever is available.
		 */
		ct = atomic_load_ptr(&tdq->ltdq_curthread);
		uint64_t v_ct = UINT64_MAX, v_q = UINT64_MAX;

		if (ct != NULL && !TD_IS_IDLETHREAD(ct) &&
		    laminar_is_timeshare(ct))
			v_ct = atomic_load_64(&td_get_sched(ct)->ts_vruntime);
		v_q = atomic_load_64(&tdq->ltdq_vtime);
		if (v_q == 0)
			v_q = UINT64_MAX;	/* unset, ignore */
		if (v_ct == UINT64_MAX && v_q == UINT64_MAX)
			continue;
		v = (v_ct < v_q) ? v_ct : v_q;
		if (v < v_fair)
			v_fair = v;
		any = true;
	}
	if (!any)
		return (0);
	return (v_fair);
}

/*
 * Raw (unscaled) cost: load + R.  Used for absolute decisions like
 * "are we balanced?" and as the gate / decouple input -- raw count
 * of transferable work is the right unit for "is there anything
 * actually to move", independent of capacity ratios.
 */
static __inline int
laminar_raw_cost(const struct laminar_tdq *tdq)
{
	uint64_t wload;
	int weighted;

	/*
	 * Weighted cost: use the sum of ts_weight on this CPU (normalised
	 * to nice-0 = 1 unit) instead of a raw runnable count.  Two
	 * nice-0 threads on CPU A and one nice=-5 thread on CPU B are
	 * comparably loaded (~2 units each); the count-based view treated
	 * B as 50% lighter.  R and R_power stay in count units (caller
	 * semantics: "R=1 = one nice-0 thread of resistance"), no scaling
	 * needed.
	 */
	wload = atomic_load_64(__DECONST(uint64_t *, &tdq->ltdq_wload));
	weighted = (int)(wload / LAMINAR_NICE_0_WEIGHT);
	return (weighted +
	    atomic_load_int(&tdq->ltdq_resistance) +
	    atomic_load_int(&tdq->ltdq_resistance_power));
}

/*
 * Scaled placement cost: (load + R) * CAP_BASE / capacity.  Half-
 * capacity CPU's effective cost doubles, so placement attracts
 * proportionally less work.  Inert on homogeneous (capacity ratio
 * is 1 across all CPUs).
 */
static __inline int
laminar_scaled_cost(const struct laminar_tdq *tdq)
{
	int cap;

	cap = atomic_load_int(&tdq->ltdq_capacity);
	if (cap <= 0)
		cap = LAMINAR_CAP_BASE;
	return (laminar_raw_cost(tdq) * LAMINAR_CAP_BASE / cap);
}

/*
 * EWMA-smoothed scaled cost.  The simulator's converged smoothing
 * removes single-tick spikes after the capacity divide injects
 * granularity.  On homogeneous the scaled signal equals the raw
 * signal and the EWMA is a no-op latency.
 */
static __inline int
laminar_signal(struct laminar_tdq *tdq)
{
	int sample, prev, ema;

	sample = laminar_scaled_cost(tdq);
	prev = atomic_load_int(&tdq->ltdq_signal_ewma);
	/*
	 * Round-to-nearest, not truncate.  Plain integer truncation gets
	 * stuck on rising integer transitions: prev=0 sample=2 yields
	 * (0+2)/2=1, then (1+2)/2=1, ema never reaches 2.  A persistent
	 * load=2 vs load=0 imbalance ends up gap=1 < threshold=2 and
	 * the balancer never fires -- this was the bench_fair bimodal
	 * pattern (50% of N=4 runs at >60% spread, balancer stuck).
	 */
	ema = (LAMINAR_EWMA_OLD * prev + LAMINAR_EWMA_NEW * sample +
	    LAMINAR_EWMA_DEN / 2) / LAMINAR_EWMA_DEN;
	atomic_store_int(&tdq->ltdq_signal_ewma, ema);
	return (ema);
}

/*
 * pickcpu uses the raw cost (not the smoothed signal) because it is
 * called at wakeup latency-critical paths -- one wakeup doesn't see
 * enough samples for smoothing to matter, and the smoothing window
 * lives in the balancer's periodic scan instead.
 */
static __inline int
laminar_placement_cost(const struct laminar_tdq *tdq)
{

	return (laminar_scaled_cost(tdq));
}

/*
 * Thread-aware placement cost: per-CPU scaled cost + R_numa for
 * this thread on this CPU.  Used by pickcpu and by the balancer's
 * acceptor scoring when we are moving a known thread.  On
 * single-domain systems the NUMA term is 0 and this is identical
 * to laminar_placement_cost.
 */
static __inline int
laminar_thread_cost(struct thread *td, int cpu)
{
	struct laminar_tdq *tdq = LAMINAR_TDQ_CPU(cpu);
	struct thread *ct;
	int cost;

	cost = laminar_scaled_cost(tdq) + laminar_numa_cost(td, cpu);
	/*
	 * Above-timeshare occupancy: if the CPU is currently running an
	 * RT/interrupt-class thread that the candidate CANNOT preempt, the
	 * candidate may not run there at all until the incumbent yields —
	 * price the CPU accordingly (see laminar_rt_occupied_cost block
	 * comment).
	 *
	 * Do NOT gate on the candidate being timeshare: pipe/socket wakers
	 * run at *kernel sleep priority* (e.g. 43) until userret, so a
	 * timeshare-only gate exempts exactly the threads that get trapped
	 * (measured: 4 watchdogs runnable at pri 43 parked behind a pri-8 RT
	 * ticker on the lowest-wload CPU, starving for the whole run while
	 * the timeshare-only penalty looked on).  The preempt test
	 * (ct->td_priority <= td->td_priority) keeps the penalty correct for
	 * every class pairing — an RT candidate is also steered away from an
	 * equal-or-better RT incumbent, spreading RT load.
	 */
	if (laminar_rt_occupied_cost > 0) {
		ct = atomic_load_ptr(&tdq->ltdq_curthread);
		if (ct != NULL && !TD_IS_IDLETHREAD(ct) &&
		    ct->td_priority < PRI_MIN_TIMESHARE &&
		    ct->td_priority <= td->td_priority)
			cost += laminar_rt_occupied_cost;
	}
	return (cost);
}

/*
 * Placement-trace ring (2026-05-20).  Records each new-thread (fork)
 * placement decision: the chosen CPU, the inherited ts_cpu seed, and a
 * per-CPU snapshot of load/wload/R at the moment pickcpu ran.  Enabled
 * via kern.sched.place_trace_enable; dumped via kern.sched.place_trace_dump;
 * also bumps kern.sched.balance_migrations_total at every successful
 * balancer migration.  Diagnostic only; not load-bearing.
 */
#define	LAMINAR_PLACE_TRACE_RING	256
#define	LAMINAR_PLACE_TRACE_NCPU	16

struct laminar_place_rec {
	int		ticks;
	int		cpu;		/* chosen target */
	int		ts_cpu_seed;	/* inherited ts_cpu at entry */
	int		td_lastcpu;	/* td_lastcpu at entry */
	uint32_t	load[LAMINAR_PLACE_TRACE_NCPU];
	uint32_t	wload[LAMINAR_PLACE_TRACE_NCPU];
	uint32_t	r[LAMINAR_PLACE_TRACE_NCPU];
	pid_t		pid;
	lwpid_t		tid;
};

static struct laminar_place_rec laminar_place_ring[LAMINAR_PLACE_TRACE_RING];
static u_int laminar_place_head;
static int laminar_place_trace_enable;
static unsigned long laminar_balance_migrations_total;

SYSCTL_INT(_kern_sched, OID_AUTO, place_trace_enable, CTLFLAG_RW,
    &laminar_place_trace_enable, 0,
    "Laminar: enable fork-time placement trace ring");

SYSCTL_ULONG(_kern_sched, OID_AUTO, balance_migrations_total, CTLFLAG_RW,
    &laminar_balance_migrations_total, 0,
    "Laminar: total balancer migrations since boot/reset (write 0)");

static void
laminar_place_trace_record(struct thread *td, int cpu, int ts_cpu_seed)
{
	struct laminar_place_rec *r;
	struct laminar_tdq *tdq;
	u_int idx;
	int c;

	if (!laminar_place_trace_enable)
		return;
	idx = atomic_fetchadd_int(&laminar_place_head, 1) %
	    LAMINAR_PLACE_TRACE_RING;
	r = &laminar_place_ring[idx];
	r->ticks = ticks;
	r->cpu = cpu;
	r->ts_cpu_seed = ts_cpu_seed;
	r->td_lastcpu = td->td_lastcpu;
	r->pid = (td->td_proc != NULL) ? td->td_proc->p_pid : -1;
	r->tid = td->td_tid;
	memset(r->load, 0, sizeof(r->load));
	memset(r->wload, 0, sizeof(r->wload));
	memset(r->r, 0, sizeof(r->r));
	CPU_FOREACH(c) {
		if (c >= LAMINAR_PLACE_TRACE_NCPU)
			continue;
		tdq = LAMINAR_TDQ_CPU(c);
		r->load[c] = atomic_load_int(&tdq->ltdq_load);
		r->wload[c] = (uint32_t)atomic_load_64(
		    __DECONST(uint64_t *, &tdq->ltdq_wload));
		r->r[c] = atomic_load_int(&tdq->ltdq_resistance);
	}
}

static int
laminar_place_trace_dump_sysctl(SYSCTL_HANDLER_ARGS)
{
	struct sbuf sb;
	struct laminar_place_rec *r;
	u_int head, n, i, idx;
	int error, c, ncpu;

	sbuf_new_for_sysctl(&sb, NULL, 4096, req);
	head = atomic_load_int(&laminar_place_head);
	n = (head < LAMINAR_PLACE_TRACE_RING) ? head :
	    LAMINAR_PLACE_TRACE_RING;
	ncpu = mp_ncpus;
	if (ncpu > LAMINAR_PLACE_TRACE_NCPU)
		ncpu = LAMINAR_PLACE_TRACE_NCPU;
	sbuf_printf(&sb, "head=%u entries=%u ncpu=%d\n", head, n, ncpu);
	for (i = 0; i < n; i++) {
		idx = (head - n + i) % LAMINAR_PLACE_TRACE_RING;
		r = &laminar_place_ring[idx];
		sbuf_printf(&sb,
		    "t=%d pid=%d tid=%d lastcpu=%d ts_seed=%d -> cpu=%d ; load=",
		    r->ticks, r->pid, r->tid, r->td_lastcpu,
		    r->ts_cpu_seed, r->cpu);
		for (c = 0; c < ncpu; c++)
			sbuf_printf(&sb, "%u%s", r->load[c],
			    c == ncpu - 1 ? "" : ",");
		sbuf_printf(&sb, " wload=");
		for (c = 0; c < ncpu; c++)
			sbuf_printf(&sb, "%u%s", r->wload[c],
			    c == ncpu - 1 ? "" : ",");
		sbuf_printf(&sb, " r=");
		for (c = 0; c < ncpu; c++)
			sbuf_printf(&sb, "%u%s", r->r[c],
			    c == ncpu - 1 ? "" : ",");
		sbuf_printf(&sb, "\n");
	}
	error = sbuf_finish(&sb);
	sbuf_delete(&sb);
	return (error);
}

SYSCTL_PROC(_kern_sched, OID_AUTO, place_trace_dump,
    CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_MPSAFE, NULL, 0,
    laminar_place_trace_dump_sysctl, "A",
    "Laminar: dump fork-time placement trace ring");

/*
 * Pick a target CPU for the thread.  Phase B: honor ts_cpu by
 * default (preserves the phase A behavior + lets the periodic
 * balancer handle long-term spread), and only shop around when
 * ts_cpu has nonzero resistance -- that is the explicit
 * "drain this CPU" signal.  This keeps wake-time pickcpu O(1)
 * in the common case and avoids regressing wake latency.
 *
 * If td is pinned (td_pinned > 0), it has to stay on ts_cpu.
 */
static int
sched_laminar_pickcpu(struct thread *td, int flags)
{
	struct td_sched *ts;
	int self, ts_cpu, best_cpu, best_cost, cpu, cost;
	int min_cost = INT_MAX, min_cpu = -1;
	int home_cpu, home_cost;

	(void)flags;

	self = PCPU_GET(cpuid);
	if (smp_started == 0)
		return (self);
	ts = td_get_sched(td);
	ts_cpu = ts->ts_cpu;
	if (!THREAD_CAN_MIGRATE(td))
		return (ts_cpu);
	/* Active lane entities are placed on their admitted CPU, always. */
	if (__predict_false(laminar_deadline_enable) && ts->ts_lane != NULL &&
	    ts->ts_lane->le_td == td)
		return (ts->ts_lane->le_cpu);

	/*
	 * IPC affinity (whitepaper §6).  If we have a confident IPC
	 * home and its cost is within ipc_slack of the global min,
	 * honor it.  Computing global min here is one CPU_FOREACH
	 * pass; cheap, and skipped entirely when conf < CONF_MIN.
	 */
	if (ts->ts_dom_waker_conf >= LAMINAR_IPC_CONF_MIN) {
		home_cpu = ts->ts_dom_waker_cpu;
		if (home_cpu >= 0 && home_cpu <= mp_maxid &&
		    THREAD_CAN_SCHED(td, home_cpu)) {
			home_cost = laminar_thread_cost(td, home_cpu);
			CPU_FOREACH(cpu) {
				cost = laminar_thread_cost(td, cpu);
				if (cost < min_cost) {
					min_cost = cost;
					min_cpu = cpu;
				}
			}
			if (home_cost <= min_cost + laminar_ipc_slack)
				return (home_cpu);
			/* Home too expensive; fall through.  Reuse the
			 * already-computed min as the scan seed. */
			best_cpu = min_cpu;
			best_cost = min_cost;
			goto done_scan;
		}
	}

	if (THREAD_CAN_SCHED(td, ts_cpu)) {
		struct laminar_tdq *_ts_tdq = LAMINAR_TDQ_CPU(ts_cpu);
		/*
		 * Fast path: ts_cpu is idle (load == 0), unparked, and
		 * NUMA-local.  Skipping the global scan here matters most
		 * for wakeups onto an idle parent.
		 *
		 * IMPORTANT: skip this for newly-created threads
		 * (td_lastcpu == NOCPU).  In a tight fork() loop, child N+1's
		 * pickcpu reads load(ts_cpu) before child N's tdq_add has
		 * incremented it, so siblings race onto the same CPU even
		 * though one of them has already been placed there.  The
		 * balancer then has multiple ticks of work to undo it, and
		 * bench_fair's 5s window can run out before placement
		 * converges -- this was the bimodal pattern (50% of N=4
		 * runs perfectly fair, 50% at 60%+ spread with one starved
		 * thread).  Forks always do the full scan.
		 */
		if (td->td_lastcpu != NOCPU &&
		    atomic_load_int(&_ts_tdq->ltdq_load) == 0 &&
		    atomic_load_int(&_ts_tdq->ltdq_resistance) == 0 &&
		    atomic_load_int(&_ts_tdq->ltdq_resistance_power) == 0 &&
		    laminar_numa_cost(td, ts_cpu) == 0)
			return (ts_cpu);	/* fast path: idle, no R, no NUMA */
		best_cpu = ts_cpu;
		best_cost = laminar_thread_cost(td, ts_cpu);
	} else {
		/*
		 * ts_cpu is excluded by cpuset.  Don't seed with self --
		 * self may also be excluded, and the strict-less-than
		 * scan below would then keep self and violate cpuset.
		 * Force the scan to choose only among legal CPUs.
		 */
		best_cpu = -1;
		best_cost = INT_MAX;
	}
	/*
	 * Strictly less-than means equal-cost CPUs lose to the seeded
	 * tiebreaker (ts_cpu or self), preserving soft affinity.
	 */
	CPU_FOREACH(cpu) {
		if (cpu == best_cpu)
			continue;
		if (!THREAD_CAN_SCHED(td, cpu))
			continue;
		cost = laminar_thread_cost(td, cpu);
		if (cost < best_cost) {
			best_cost = cost;
			best_cpu = cpu;
		}
	}
done_scan:
	/*
	 * Defensive: scan ran with best_cpu=-1 seed (ts_cpu excluded
	 * by cpuset) and found nothing legal.  Shouldn't happen for a
	 * runnable thread but fall back to self to avoid returning -1.
	 */
	if (best_cpu < 0)
		best_cpu = self;
	if (td->td_lastcpu == NOCPU)
		laminar_place_trace_record(td, best_cpu, ts_cpu);
	return (best_cpu);
}

/*
 * Move a thread to a target CPU's runqueue.  Ported from ULE's
 * sched_setcpu.  Drops the caller's thread lock and acquires the
 * target tdq's lock, switching td_lock atomically across the change.
 */
static struct laminar_tdq *
sched_laminar_setcpu(struct thread *td, int cpu, int flags)
{
	struct laminar_tdq *tdq;
	struct mtx *mtx;

	THREAD_LOCK_ASSERT(td, MA_OWNED);
	tdq = LAMINAR_TDQ_CPU(cpu);
	td_get_sched(td)->ts_cpu = cpu;
	if (td->td_lock == LAMINAR_TDQ_LOCKPTR(tdq)) {
		KASSERT((flags & SRQ_HOLD) == 0,
		    ("sched_laminar_setcpu: SRQ_HOLD with same lock"));
		return (tdq);
	}
	spinlock_enter();
	mtx = thread_lock_block(td);
	if ((flags & SRQ_HOLD) == 0)
		mtx_unlock_spin(mtx);
	/*
	 * Diagnostic: before we attempt to acquire the target tdq's
	 * spin mutex, detect the recursion case (curthread already owns
	 * it) and panic with caller-side context.  Without this the bare
	 * mtx_lock_spin assertion only tells us which lock recursed, not
	 * which outer context already held it.
	 */
	{
		struct mtx *tdq_mtx = LAMINAR_TDQ_LOCKPTR(tdq);
		uintptr_t owner = atomic_load_acq_ptr(&tdq_mtx->mtx_lock);
		uintptr_t self = (uintptr_t)curthread;

		if ((owner & ~(uintptr_t)MTX_FLAGMASK) == self) {
			panic("laminar setcpu cross-CPU recursion: "
			    "target cpu=%d tdq=%p mtx=%p owner=%#lx self=%#lx "
			    "td=%p td_lock=%p td_tid=%d "
			    "curtdq=%p curtdq_lock=%p flags=%#x",
			    cpu, tdq, tdq_mtx, (u_long)owner, (u_long)self,
			    td, td->td_lock, td->td_tid,
			    LAMINAR_TDQ_SELF(),
			    LAMINAR_TDQ_LOCKPTR(LAMINAR_TDQ_SELF()),
			    flags);
		}
	}
	LAMINAR_TDQ_LOCK(tdq);
	thread_lock_unblock(td, LAMINAR_TDQ_LOCKPTR(tdq));
	spinlock_exit();
	return (tdq);
}

/*
 * Send a preempt IPI to the CPU if the newly-enqueued thread's
 * priority warrants displacing the currently-running one.  Ported
 * from ULE's tdq_notify (without the idle-aware optimization).
 */
static void
tdq_notify(struct laminar_tdq *tdq, int oldpri)
{
	int cpu, newpri;

	LAMINAR_TDQ_LOCK_ASSERT(tdq, MA_OWNED);
	if (tdq->ltdq_owepreempt)
		return;
	/*
	 * Mirror ULE's sched_shouldpreempt: oldpri is the pre-add
	 * lowest priority on tdq (i.e., the running thread's pri);
	 * newpri is the post-add lowest (typically the newly-added
	 * thread's, if it lowered the bar).  Preempt when newpri is
	 * strictly more important than oldpri, or when oldpri is
	 * idle.
	 *
	 * Previously this used td->td_priority for `oldpri` (passed
	 * by sched_laminar_add), which always equalled newpri after
	 * the add and so returned early in every timeshare-vs-
	 * timeshare case AND in the idle-CPU case (oldpri=224,
	 * newpri=76, 76 >= 76 -> no IPI).  That blocked wakers from
	 * preempting incumbents or even waking idle CPUs, observed
	 * as R4 multi-second wake-tail with cp_skip_idle=5523 hits.
	 */
	newpri = tdq->ltdq_lowpri;
	if (newpri >= oldpri && oldpri < PRI_MIN_IDLE) {
		laminar_notify_skip++;
		return;
	}
	atomic_thread_fence_seq_cst();
	cpu = LAMINAR_TDQ_ID(tdq);
	if (cpu == PCPU_GET(cpuid)) {
		laminar_notify_skip++;
		return;			/* same CPU; no IPI needed */
	}
	tdq->ltdq_owepreempt = 1;
	ipi_cpu(cpu, IPI_PREEMPT);
	laminar_notify_ipi++;
}

/*
 * Periodic cross-CPU load balancer.  Phase A version is intentionally
 * minimal -- a single global callout that scans all CPUs every
 * laminar_balance_interval ms, finds the most-loaded and least-loaded,
 * and moves one transferable timeshare thread when the gap is large
 * enough.  Phase C replaces this with the §3 RLC filter from the
 * whitepaper.  Without this, all fork()s land on the parent's CPU and
 * APs sit idle.
 */
static struct callout laminar_balance_callout;
static int laminar_balance_interval = 100;	/* ms */
static int laminar_balance_threshold = 2;	/* migrate when high - low >= this */
/*
 * RLC filter tunables (DESIGN.md §1).  Defaults match the simulator's
 * converged operating point.
 *   debounce: consecutive imbalanced cycles required before action.
 *   cooldown_base: ticks between successive migrations off the same
 *     donor at minimum-actionable imbalance; shrinks as gap grows.
 *   drain_max: safety cap on per-callout migrations (prevents a
 *     runaway from monopolizing the callout thread).
 */
static int laminar_debounce = 2;
static int laminar_cooldown_base = 16;
static int laminar_drain_max = 32;

SYSCTL_INT(_kern_sched, OID_AUTO, balance_interval, CTLFLAG_RW,
    &laminar_balance_interval, 0,
    "Laminar: cross-CPU balance period in ms");
SYSCTL_INT(_kern_sched, OID_AUTO, balance_threshold, CTLFLAG_RW,
    &laminar_balance_threshold, 0,
    "Laminar: load difference required to trigger a migration");
SYSCTL_INT(_kern_sched, OID_AUTO, debounce, CTLFLAG_RW,
    &laminar_debounce, 0,
    "Laminar: C2 debounce cycles (donor must be hi for this many)");
SYSCTL_INT(_kern_sched, OID_AUTO, cooldown_base, CTLFLAG_RW,
    &laminar_cooldown_base, 0,
    "Laminar: per-donor cooldown base ticks; shrinks as imbalance grows");
SYSCTL_INT(_kern_sched, OID_AUTO, drain_max, CTLFLAG_RW,
    &laminar_drain_max, 0,
    "Laminar: max migrations per balance pass (safety cap)");

/*
 * IPC affinity (whitepaper §6).  Variable definition is up top so
 * pickcpu (defined earlier) can reference it; only the sysctl knob
 * registration lives here, next to the other tunables.
 */
SYSCTL_INT(_kern_sched, OID_AUTO, ipc_slack, CTLFLAG_RW,
    &laminar_ipc_slack, 0,
    "Laminar: IPC home stickiness vs balance (extra cost units "
    "tolerated to honor dom_waker home CPU; 0 disables IPC pull)");
SYSCTL_INT(_kern_sched, OID_AUTO, numa_alpha, CTLFLAG_RW,
    &laminar_numa_alpha, 0,
    "Laminar: NUMA cross-domain placement penalty in cost units; "
    "0 disables NUMA-aware placement; inert on single-domain "
    "systems regardless of value.");

/*
 * Phase E: jail-table sysctl.  A single dump/set sysctl rather than
 * dynamic per-jail tree because jails come and go and we don't want
 * to register / unregister oids in the hot path.  Read prints
 * "pr_id weight nthreads" lines; write parses "pr_id weight" and
 * claims a slot (failing only if the table is full).
 */
static int
laminar_jails_sysctl(SYSCTL_HANDLER_ARGS)
{
	struct sbuf sb;
	char ibuf[64];
	int error, i, id, w;

	if (req->newptr != NULL) {
		if (req->newlen >= sizeof(ibuf))
			return (EINVAL);
		error = SYSCTL_IN(req, ibuf, req->newlen);
		if (error != 0)
			return (error);
		ibuf[req->newlen] = '\0';
		if (sscanf(ibuf, "%d %d", &id, &w) != 2 || id < 0 || w <= 0)
			return (EINVAL);
		struct laminar_prison *lpr = laminar_prison_claim(id);
		if (lpr == NULL)
			return (ENOMEM);
		atomic_store_int(&lpr->lpr_weight, w);
		return (0);
	}
	sbuf_new_for_sysctl(&sb, NULL, 128, req);
	for (i = 0; i < LAMINAR_MAX_PRISONS; i++) {
		int slot = atomic_load_int(&laminar_prisons[i].lpr_id);
		if (slot < 0)
			continue;
		sbuf_printf(&sb, "%d %u %u\n", slot,
		    laminar_prisons[i].lpr_weight,
		    laminar_prisons[i].lpr_nthreads);
	}
	error = sbuf_finish(&sb);
	sbuf_delete(&sb);
	return (error);
}
/*
 * D9: the deadline_broker capability's kernel half.  portcullisd (host
 * root) writes "pr_id 0|1" when materializing a jail whose manifest
 * grants deadline_broker; reads list flagged jails.  Writes from
 * INSIDE a jail are rejected — a jail cannot self-grant.  SPONSOR_FOR
 * consults this for jailed callers (host root keeps priv_check).
 */
static int
laminar_brokers_sysctl(SYSCTL_HANDLER_ARGS)
{
	struct sbuf sb;
	char ibuf[32];
	int error, i, id, on;

	if (req->newptr != NULL) {
		if (req->td->td_ucred->cr_prison != &prison0)
			return (EPERM);
		if (req->newlen >= sizeof(ibuf))
			return (EINVAL);
		error = SYSCTL_IN(req, ibuf, req->newlen);
		if (error != 0)
			return (error);
		ibuf[req->newlen] = '\0';
		if (sscanf(ibuf, "%d %d", &id, &on) != 2 || id < 0)
			return (EINVAL);
		struct laminar_prison *lpr = laminar_prison_claim(id);
		if (lpr == NULL)
			return (ENOMEM);
		atomic_store_int(&lpr->lpr_broker, on != 0);
		return (0);
	}
	sbuf_new_for_sysctl(&sb, NULL, 64, req);
	for (i = 0; i < LAMINAR_MAX_PRISONS; i++) {
		int slot = atomic_load_int(&laminar_prisons[i].lpr_id);
		if (slot < 0 ||
		    atomic_load_int(&laminar_prisons[i].lpr_broker) == 0)
			continue;
		sbuf_printf(&sb, "%d\n", slot);
	}
	error = sbuf_finish(&sb);
	sbuf_delete(&sb);
	return (error);
}
SYSCTL_PROC(_kern_sched, OID_AUTO, deadline_brokers,
    CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_MPSAFE, NULL, 0,
    laminar_brokers_sysctl, "A",
    "Laminar D9: jails holding the deadline_broker capability "
    "(write 'pr_id 0|1' from the host; read lists granted pr_ids)");

SYSCTL_PROC(_kern_sched, OID_AUTO, jails,
    CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_MPSAFE, NULL, 0,
    laminar_jails_sysctl, "A",
    "Laminar per-jail proportional share: read = "
    "'pr_id weight nthreads\\n'... ; write = 'pr_id weight' to set");

/*
 * Acquire two tdq locks in address order to avoid deadlock with any
 * other pair-locking site.  Mirrors ULE's tdq_lock_pair.
 */
static void
laminar_tdq_lock_pair(struct laminar_tdq *a, struct laminar_tdq *b)
{

	if (a < b) {
		LAMINAR_TDQ_LOCK(a);
		mtx_lock_spin_flags(LAMINAR_TDQ_LOCKPTR(b), MTX_DUPOK);
	} else {
		LAMINAR_TDQ_LOCK(b);
		mtx_lock_spin_flags(LAMINAR_TDQ_LOCKPTR(a), MTX_DUPOK);
	}
}

/*
 * Scan the timeshare SoA for a migratable thread to send to dst_cpu.
 * Cost-driven choice: among migratable candidates, return the one with
 * the largest ts_weight -- moving the heaviest thread maximises the
 * wload-gap reduction per migration, so the balancer's transient
 * settle time scales with the number of cohorts, not threads.
 *
 * (Pre-cost-driven version returned the first migratable slot, which
 * is arbitrary and made big-skew transients take seconds instead of
 * the RLC settle time the filter parameters imply.)
 *
 * Caller holds the source tdq lock.
 */
static struct thread *
laminar_steal_timeshare(struct laminar_tdq *from, int dst_cpu)
{
	struct thread *td, *best = NULL;
	uint32_t best_w = 0, w;
	uint32_t i;

	LAMINAR_TDQ_LOCK_ASSERT(from, MA_OWNED);
	for (i = 0; i < from->ltdq_ts_n; i++) {
		td = from->ltdq_slot[i];
		if (!THREAD_CAN_MIGRATE(td))
			continue;
		if (!THREAD_CAN_SCHED(td, dst_cpu))
			continue;
		w = td_get_sched(td)->ts_weight;
		if (best == NULL || w > best_w) {
			best = td;
			best_w = w;
		}
	}
	return (best);
}

/*
 * If high has at least laminar_balance_threshold more load than low,
 * move one transferable timeshare thread from high to low.  Both tdq
 * locks acquired internally in address order.  Returns true if a
 * migration occurred.
 */
static bool
laminar_balance_pair(struct laminar_tdq *high, struct laminar_tdq *low)
{
	struct thread *td;
	int dst_cpu, high_cost, low_cost;
	bool moved = false;

	laminar_tdq_lock_pair(high, low);
	/*
	 * Rank with smoothed scaled signal; gate with raw transferable
	 * count (rank/gate decouple, DESIGN.md §1).  Without the
	 * decouple, low-capacity / smoothed-up CPUs can rank as donors
	 * even when they have no migratable work, and we waste cycles.
	 */
	high_cost = laminar_scaled_cost(high);
	low_cost = laminar_scaled_cost(low);
	if (high_cost < low_cost + laminar_balance_threshold)
		goto out;
	if (laminar_transferable(high) < 1)
		goto out;
	dst_cpu = LAMINAR_TDQ_ID(low);
	td = laminar_steal_timeshare(high, dst_cpu);
	if (td == NULL)
		goto out;
	/*
	 * We hold both tdq locks; thread's td_lock points at the source
	 * tdq, so we own the thread lock too.  Wait for any concurrent
	 * thread_lock_block in flight, then move: remove from source,
	 * reassign td_lock + ts_cpu, add to dest, IPI dest if cross-CPU.
	 */
	thread_lock_block_wait(td);
	sched_laminar_rem(td);
	td->td_lock = LAMINAR_TDQ_LOCKPTR(low);
	td_get_sched(td)->ts_cpu = dst_cpu;
	{
		int old_lowpri = tdq_add_internal(low, td, SRQ_YIELDING);

		if (dst_cpu != PCPU_GET(cpuid))
			tdq_notify(low, old_lowpri);
	}
	moved = true;
	atomic_add_long(&laminar_balance_migrations_total, 1);
out:
	LAMINAR_TDQ_UNLOCK(high);
	LAMINAR_TDQ_UNLOCK(low);
	return (moved);
}

/*
 * Walk all CPUs to find the most- and least-loaded tdq.  If the gap
 * is wide enough, move one thread.  Reschedule the callout afterwards.
 */
/*
 * Count transferable (migratable, on-runq, timeshare) threads on a
 * tdq.  The "gate" half of the rank/gate decouple: a CPU may rank
 * as "hi" by smoothed scaled cost but have nothing actually
 * stealable -- in that case the migrate is a no-op and we should
 * not waste a cycle on it.
 */
static int
laminar_transferable(struct laminar_tdq *tdq)
{
	int n = 0;
	uint32_t i;

	LAMINAR_TDQ_LOCK_ASSERT(tdq, MA_OWNED);
	for (i = 0; i < tdq->ltdq_ts_n; i++) {
		if (THREAD_CAN_MIGRATE(tdq->ltdq_slot[i]))
			n++;
	}
	return (n);
}

/*
 * Find current hi/lo using the SMOOTHED scaled signal for rank
 * decisions (rank/gate decouple).  Updates each CPU's EWMA as a
 * side effect; the simulator's "smoothed" path requires that the
 * signal be sampled on every pass even when no migration happens.
 *
 * Topology-R: on homogeneous single-leaf systems this is a no-op;
 * on NUMA, prefer a low-cost CPU in the same cpu_group leaf as
 * the donor.  The leaf preference goes into lo selection only --
 * hi is whichever CPU has the highest cost globally.
 */
static void
laminar_find_hi_lo(struct laminar_tdq **hip, int *hi_costp,
    struct laminar_tdq **lop, int *lo_costp)
{
	struct laminar_tdq *hi = NULL, *lo = NULL, *tdq;
	int hi_cost = -1, lo_cost = INT_MAX;
	int cost, cpu;

	CPU_FOREACH(cpu) {
		tdq = LAMINAR_TDQ_CPU(cpu);
		cost = laminar_signal(tdq);	/* updates EWMA */
		if (cost > hi_cost) { hi_cost = cost; hi = tdq; }
	}
	if (hi != NULL && hi->ltdq_cg != NULL) {
		/*
		 * Topology-R pass: prefer same-leaf-group acceptor.
		 * The first pass restricts the lo search to hi's cpu
		 * group leaf; if nothing useful is found there, fall
		 * through to a global pass.
		 */
		CPU_FOREACH(cpu) {
			tdq = LAMINAR_TDQ_CPU(cpu);
			if (tdq == hi || tdq->ltdq_cg != hi->ltdq_cg)
				continue;
			cost = atomic_load_int(&tdq->ltdq_signal_ewma);
			if (cost < lo_cost) { lo_cost = cost; lo = tdq; }
		}
	}
	if (lo == NULL) {
		CPU_FOREACH(cpu) {
			tdq = LAMINAR_TDQ_CPU(cpu);
			if (tdq == hi)
				continue;
			cost = atomic_load_int(&tdq->ltdq_signal_ewma);
			if (cost < lo_cost) { lo_cost = cost; lo = tdq; }
		}
	}
	*hip = hi; *hi_costp = hi_cost;
	*lop = lo; *lo_costp = lo_cost;
}

/*
 * Adaptive-L cooldown: at minimum-actionable imbalance, wait
 * cooldown_base ticks before another migration off the same donor.
 * Bigger gap -> shorter cooldown.  At gap = threshold we get full
 * base; at gap = 4 * threshold we get ~base/4; never below 1.
 */
static __inline int
laminar_cooldown_ticks(int gap)
{
	int divisor;

	divisor = imax(1, gap / imax(1, laminar_balance_threshold));
	return (imax(1, laminar_cooldown_base / divisor));
}

/*
 * RLC balancer.  Drain loop: keep re-picking hi/lo and migrating
 * one thread until the system is balanced, the donor cools down,
 * or we hit drain_max.  C2 debounce gates entry; evac (R > 0 on
 * the donor) skips debounce and drains the donor toward empty in
 * a single pass.  Mirrors DESIGN.md §1 plus §2's evac shape.
 */
static void
laminar_balance_cb(void *arg __unused)
{
	struct laminar_tdq *hi, *lo;
	int hi_cost, lo_cost, gap;
	int migrations = 0;
	int now = ticks;
	bool evac;

	for (;;) {
		laminar_find_hi_lo(&hi, &hi_cost, &lo, &lo_cost);
		if (hi == NULL || lo == NULL || hi == lo)
			break;
		gap = hi_cost - lo_cost;
		if (gap < laminar_balance_threshold) {
			/* Balanced -- reset hi's streak. */
			hi->ltdq_streak = 0;
			break;
		}
		evac = atomic_load_int(&hi->ltdq_resistance) > 0 ||
		    atomic_load_int(&hi->ltdq_resistance_power) > 0;
		/*
		 * Adaptive aggression: when gap is small (near threshold)
		 * keep the original debounce + 1-per-cycle protection,
		 * which guards against ping-pong on noisy edges.  When
		 * gap >= 2 * threshold the signal is unambiguous, so:
		 *   - skip the debounce streak
		 *   - allow up to (gap / threshold) migrations this cycle
		 * Cooldown is already gap-adaptive (cooldown_ticks).
		 * Evac path is unchanged.
		 */
		int gap_mult = gap / imax(1, laminar_balance_threshold);
		bool big_gap = gap_mult >= 2;
		if (!evac) {
			if (!big_gap) {
				/* C2 debounce: imbalance must persist. */
				hi->ltdq_streak++;
				if (hi->ltdq_streak < laminar_debounce)
					break;
			}
			/* Adaptive-L cooldown: rate-limit per donor. */
			if (now - hi->ltdq_last_xfer <
			    laminar_cooldown_ticks(gap))
				break;
		}
		if (!laminar_balance_pair(hi, lo))
			break;
		hi->ltdq_streak = 0;
		hi->ltdq_last_xfer = now;
		if (++migrations >= laminar_drain_max)
			break;
		/*
		 * Non-evac: one migration per cycle by default; up to
		 * gap_mult per cycle when the gap is unambiguous.
		 */
		if (!evac && migrations >= gap_mult)
			break;
	}

	callout_reset(&laminar_balance_callout,
	    imax(1, hz * laminar_balance_interval / 1000),
	    laminar_balance_cb, NULL);
}

static void
laminar_balance_start(void)
{

	callout_init(&laminar_balance_callout, 1);
	callout_reset(&laminar_balance_callout,
	    imax(1, hz * laminar_balance_interval / 1000),
	    laminar_balance_cb, NULL);
}

/*
 * Phase G: closed-loop balanced controller (whitepaper §5).
 *
 * Measures system-wide load EWMA per control interval, compares to
 * a setpoint (CTRL_HEADROOM) with a Schmitt trigger (CTRL_DEADBAND)
 * and asymmetric patience (park = many over-provisioned cycles,
 * unpark = one), and actuates by writing R_power on individual
 * CPUs.  Asymmetry: park lazy (wrong-park is a latency hit),
 * unpark eager (wrong-awake just wastes a little power).
 *
 * Per-domain park is the proper implementation; this phase-G ships
 * the single-domain shape (parks one CPU at a time, treating the
 * whole system as one power domain).  Per-cpu_group expansion is a
 * follow-up but uses the same primitives, just keyed by
 * cpu_group instead of "any CPU".
 */
static struct callout laminar_ctrl_callout;
static int laminar_ctrl_interval = 100;	/* ms between samples */
/*
 * Emergency-unpark threshold: a load_pct sample above this
 * unparks a CPU immediately, bypassing the EWMA + patience.
 * Catches the cold-start case where the system was idle (most
 * CPUs parked) and a burst arrives -- with the normal 1Hz
 * sample + EWMA smoothing, parked-CPU unpark could lag the
 * burst by seconds and starve interactive work (sshd banner
 * timeouts during heavy bench were the symptom that surfaced
 * this).  Default 200 = "average unparked CPU has 2+ runnable
 * threads queued" -- that is overload by any measure.
 */
static int laminar_ctrl_emergency = 150;
static int laminar_ctrl_headroom = 75;		/* % of capacity setpoint */
static int laminar_ctrl_deadband = 15;		/* % Schmitt deadband */
static int laminar_ctrl_park_pat = 30;		/* over-provisioned cycles (3s at 100ms interval) */
static int laminar_ctrl_unpark_pat = 1;		/* under-provisioned cycles */
static int laminar_ctrl_evac_r = 99;		/* R_power for parked CPUs */
static int laminar_ctrl_enable = 1;		/* master switch */

/*
 * Controller-driven preempt tolerance band.  Raised when sustained
 * load_pct exceeds the threshold to widen the cost-preempt gate
 * (RLC: controller observes load, actuates R for preempt only).
 * Result: desktop-responsive wake latency under saturation without
 * a static slice_min special case or sleeper-credit overlay.
 */
static int laminar_ctrl_band_enable = 1;	/* auto-band on by default */
static int laminar_ctrl_band_max = 4;		/* clamp band per CPU */
static int laminar_ctrl_band_thresh = 100;	/* load_pct floor */
static int laminar_ctrl_band_step = 25;		/* load_pct per band unit */

/*
 * Lead-compensation state for the band signal (RLC L term).  The
 * existing 3:1 LP on laminar_ctrl_load_ewma is the C; this adds the L.
 * Park/unpark continue to read the bare ewma (patient signal) -- only
 * the band reads (ewma + lead_term), so cold-start band-up is instant
 * without changing park/unpark dynamics.
 */
static struct laminar_lead_state laminar_ctrl_load_lead;
static int laminar_ctrl_load_lead_kd = LAMINAR_LEAD_KD_DEFAULT;

/* Observability + state (all single-controller for now). */
static int laminar_ctrl_load_ewma;
static int laminar_ctrl_park_streak;
static int laminar_ctrl_unpark_streak;

SYSCTL_INT(_kern_sched, OID_AUTO, ctrl_enable, CTLFLAG_RW,
    &laminar_ctrl_enable, 0,
    "Laminar: closed-loop balanced controller enable (1=on, 0=off)");
SYSCTL_INT(_kern_sched, OID_AUTO, ctrl_interval, CTLFLAG_RW,
    &laminar_ctrl_interval, 0,
    "Laminar: controller sample interval in ms");
SYSCTL_INT(_kern_sched, OID_AUTO, ctrl_headroom, CTLFLAG_RW,
    &laminar_ctrl_headroom, 0,
    "Laminar: controller setpoint as percent of total capacity");
SYSCTL_INT(_kern_sched, OID_AUTO, ctrl_deadband, CTLFLAG_RW,
    &laminar_ctrl_deadband, 0,
    "Laminar: controller Schmitt deadband (percent)");
SYSCTL_INT(_kern_sched, OID_AUTO, ctrl_park_pat, CTLFLAG_RW,
    &laminar_ctrl_park_pat, 0,
    "Laminar: cycles below low threshold before parking a CPU");
SYSCTL_INT(_kern_sched, OID_AUTO, ctrl_unpark_pat, CTLFLAG_RW,
    &laminar_ctrl_unpark_pat, 0,
    "Laminar: cycles above high threshold before unparking a CPU");
SYSCTL_INT(_kern_sched, OID_AUTO, ctrl_evac_r, CTLFLAG_RW,
    &laminar_ctrl_evac_r, 0,
    "Laminar: R_power value applied to a parked CPU (evac strength)");
SYSCTL_INT(_kern_sched, OID_AUTO, ctrl_band_enable, CTLFLAG_RWTUN,
    &laminar_ctrl_band_enable, 0,
    "Laminar: controller-driven preempt R-band on (1) / off (0)");
SYSCTL_INT(_kern_sched, OID_AUTO, ctrl_band_max, CTLFLAG_RWTUN,
    &laminar_ctrl_band_max, 0,
    "Laminar: max controller-set preempt R band per CPU");
SYSCTL_INT(_kern_sched, OID_AUTO, ctrl_band_thresh, CTLFLAG_RWTUN,
    &laminar_ctrl_band_thresh, 0,
    "Laminar: load_pct at/below which controller R band is 0");
SYSCTL_INT(_kern_sched, OID_AUTO, ctrl_band_step, CTLFLAG_RWTUN,
    &laminar_ctrl_band_step, 0,
    "Laminar: load_pct per +1 unit of controller preempt R band");
SYSCTL_INT(_kern_sched, OID_AUTO, rt_occupied_cost, CTLFLAG_RWTUN,
    &laminar_rt_occupied_cost, 0,
    "Laminar: placement cost added for a CPU currently running an "
    "above-timeshare (RT/interrupt) thread (0 = disabled)");
SYSCTL_INT(_kern_sched, OID_AUTO, deadline_enable, CTLFLAG_RWTUN,
    &laminar_deadline_enable, 0,
    "Laminar: enable the declared real-deadline lane (EDF+CBS); on by default");
SYSCTL_INT(_kern_sched, OID_AUTO, deadline_util_max, CTLFLAG_RWTUN,
    &laminar_deadline_util_max, 0,
    "Laminar: per-CPU deadline-lane utilization admission cap (per-mille)");
SYSCTL_ULONG(_kern_sched, OID_AUTO, lane_sponsors, CTLFLAG_RD,
    &laminar_lane_sponsors, 0, "Laminar: total lane sponsorships");
SYSCTL_ULONG(_kern_sched, OID_AUTO, lane_preempts, CTLFLAG_RD,
    &laminar_lane_preempts, 0, "Laminar: lane wake-preempts sent");
SYSCTL_ULONG(_kern_sched, OID_AUTO, lane_wakes, CTLFLAG_RD,
    &laminar_lane_wakes, 0, "Laminar: lane wakes seen at local sched_add");
SYSCTL_ULONG(_kern_sched, OID_AUTO, lane_wake_blocked, CTLFLAG_RD,
    &laminar_lane_wake_blocked, 0,
    "Laminar: lane wakes with non-timeshare incumbent");
SYSCTL_INT(_kern_sched, OID_AUTO, slice, CTLFLAG_RW,
    &sched_slice, 0,
    "Laminar: base time slice in stathz ticks (low-load posture)");
SYSCTL_INT(_kern_sched, OID_AUTO, slice_min, CTLFLAG_RW,
    &sched_slice_min, 0,
    "Laminar: saturation slice in stathz ticks (used when local load "
    ">= slice_min_load)");
SYSCTL_INT(_kern_sched, OID_AUTO, slice_min_load, CTLFLAG_RWTUN,
    &laminar_slice_min_load, 0,
    "Laminar: local runq load at/above which the shorter slice_min "
    "applies (default 2 = any contention)");
SYSCTL_INT(_kern_sched, OID_AUTO, ctrl_load_lead_kd, CTLFLAG_RWTUN,
    &laminar_ctrl_load_lead_kd, 0,
    "Laminar: lead-compensation K_d (0..LAMINAR_LEAD_DEN, default 4) "
    "applied to load_pct for the band path.  0 = pure LP (back-compat), "
    "LEAD_DEN = fully proportional (no smoothing), >LEAD_DEN = overshoot");
SYSCTL_INT(_kern_sched, OID_AUTO, ctrl_load_ewma, CTLFLAG_RD,
    &laminar_ctrl_load_ewma, 0,
    "Laminar: smoothed system load as percent of total capacity");
SYSCTL_INT(_kern_sched, OID_AUTO, ctrl_emergency, CTLFLAG_RW,
    &laminar_ctrl_emergency, 0,
    "Laminar: raw load_pct that triggers immediate unpark "
    "(bypasses EWMA + patience for cold-start bursts)");

static void
laminar_ctrl_cb_federation(void);

/*
 * The system power posture, 0..10 (0 powersave .. 5 balanced .. 10 perf).
 * Defined below (with its sysctl); forward-declared here as a tentative
 * definition so the federation loop can thread it to every member's
 * demand()/actuate() as the soft preference (atrium-power-posture.md §3).
 */
static int laminar_power_policy;
static void laminar_apply_power_policy(int level);

/*
 * Energy-budget federation (P6 across-member half; federation doc §4).
 * Members register (demand probe, actuator, weight); every ctrl tick with
 * a cap set, water_fill allocates the cap max-min fair by weight and pushes
 * each grant + the system posture to the member's actuator.  Laminar itself
 * registers the CPU member (demand = load x current level power; actuator =
 * posture knobs + a DVFS level ceiling); the GPU/display kmods register their
 * devices (demand/actuate regs).  Posture is threaded so "performance" drives
 * all members coherently without a second control path (invariant #2).
 */
#define	ENERGY_MAX_MEMBERS	4
static struct energy_member {
	char		em_name[16];
	energy_demand_fn *em_demand;
	energy_actuate_fn *em_actuate;
	void		*em_arg;
	uint64_t	em_weight;
	uint64_t	em_demand_last;	/* observability */
	uint64_t	em_budget_last;	/* observability */
	bool		em_used;
} energy_members[ENERGY_MAX_MEMBERS];
static struct mtx energy_lock;
MTX_SYSINIT(energy_lock, &energy_lock, "energy_budget", MTX_DEF);
static uint64_t energy_cap_mw = 0;	/* 0 = federation off */
static int energy_posture_last = -1;	/* last posture broadcast (-1 = none) */

SYSCTL_U64(_kern_sched, OID_AUTO, energy_cap_mw, CTLFLAG_RWTUN,
    &energy_cap_mw, 0,
    "Energy federation: shared power cap (mW) split max-min fair "
    "across members; 0 disables (members uncapped)");

int
energy_member_register(const char *name, energy_demand_fn *demand,
    energy_actuate_fn *actuate, void *arg, uint64_t weight)
{
	int i;

	mtx_lock(&energy_lock);
	for (i = 0; i < ENERGY_MAX_MEMBERS; i++) {
		if (!energy_members[i].em_used) {
			strlcpy(energy_members[i].em_name, name,
			    sizeof(energy_members[i].em_name));
			energy_members[i].em_demand = demand;
			energy_members[i].em_actuate = actuate;
			energy_members[i].em_arg = arg;
			energy_members[i].em_weight = weight == 0 ? 1 : weight;
			energy_members[i].em_demand_last = 0;
			energy_members[i].em_budget_last = 0;
			energy_members[i].em_used = true;
			mtx_unlock(&energy_lock);
			return (i);
		}
	}
	mtx_unlock(&energy_lock);
	return (-1);
}

void
energy_member_unregister(int id)
{

	if (id < 0 || id >= ENERGY_MAX_MEMBERS)
		return;
	mtx_lock(&energy_lock);
	if (energy_members[id].em_used && energy_members[id].em_actuate != NULL)
		energy_members[id].em_actuate(energy_members[id].em_arg, 0,
		    laminar_power_policy);
	energy_members[id].em_used = false;
	mtx_unlock(&energy_lock);
}

/*
 * Max-min fair split of cap_mw across demands by weight (the same
 * water_fill as gpusim federation.rs / MST link-BW): grant each active
 * member min(demand, weighted share of the remainder); members whose
 * demand fits drop out and their slack redistributes; work-conserving.
 */
static void
energy_water_fill(uint64_t cap_mw, const uint64_t *demand, uint64_t *grant,
    const uint64_t *weight, bool *active, int n)
{
	uint64_t remaining = cap_mw, wsum;
	bool progress = true;
	int i;

	for (i = 0; i < n; i++)
		grant[i] = 0;
	while (progress) {
		progress = false;
		wsum = 0;
		for (i = 0; i < n; i++)
			if (active[i])
				wsum += weight[i];
		if (wsum == 0)
			break;
		for (i = 0; i < n; i++) {
			uint64_t share;

			if (!active[i])
				continue;
			share = remaining * weight[i] / wsum;
			if (demand[i] <= share) {
				/* fits: grant fully, redistribute slack */
				grant[i] = demand[i];
				remaining -= demand[i];
				active[i] = false;
				progress = true;
				break;	/* recompute shares */
			}
		}
		if (!progress) {
			/* all remaining members are saturated: weighted */
			for (i = 0; i < n; i++) {
				if (active[i]) {
					grant[i] = remaining * weight[i] /
					    wsum;
					active[i] = false;
				}
			}
		}
	}
}

static void
laminar_ctrl_cb_federation(void)
{
	uint64_t demand[ENERGY_MAX_MEMBERS], grant[ENERGY_MAX_MEMBERS];
	uint64_t weight[ENERGY_MAX_MEMBERS];
	bool active[ENERGY_MAX_MEMBERS];
	uint64_t cap = energy_cap_mw;
	int posture = laminar_power_policy;
	int i;

	mtx_lock(&energy_lock);
	if (cap == 0) {
		/*
		 * Federation off: grant is unbounded (mw = 0 = uncapped), but
		 * the posture is STILL live (invariant #4: actuate(inf, P)).  So
		 * re-actuate a member when it was capped on a previous tick OR
		 * the posture changed — its actuator seeks the pure posture
		 * target.  Idempotent: actuators tolerate repeated (0, P).
		 */
		for (i = 0; i < ENERGY_MAX_MEMBERS; i++) {
			if (energy_members[i].em_used &&
			    energy_members[i].em_actuate != NULL &&
			    (energy_members[i].em_budget_last != 0 ||
			    posture != energy_posture_last)) {
				energy_members[i].em_actuate(
				    energy_members[i].em_arg, 0, posture);
				energy_members[i].em_budget_last = 0;
			}
		}
		energy_posture_last = posture;
		mtx_unlock(&energy_lock);
		return;
	}
	for (i = 0; i < ENERGY_MAX_MEMBERS; i++) {
		active[i] = energy_members[i].em_used &&
		    energy_members[i].em_demand != NULL;
		demand[i] = active[i] ?
		    energy_members[i].em_demand(energy_members[i].em_arg,
		    posture) : 0;
		weight[i] = energy_members[i].em_weight;
		if (active[i])
			energy_members[i].em_demand_last = demand[i];
	}
	energy_water_fill(cap, demand, grant, weight, active,
	    ENERGY_MAX_MEMBERS);
	for (i = 0; i < ENERGY_MAX_MEMBERS; i++) {
		if (energy_members[i].em_used &&
		    energy_members[i].em_actuate != NULL) {
			energy_members[i].em_actuate(energy_members[i].em_arg,
			    grant[i], posture);
			energy_members[i].em_budget_last = grant[i];
		}
	}
	energy_posture_last = posture;
	mtx_unlock(&energy_lock);
}

static int
energy_members_sysctl(SYSCTL_HANDLER_ARGS)
{
	struct sbuf sb;
	int error, i;

	sbuf_new_for_sysctl(&sb, NULL, 256, req);
	mtx_lock(&energy_lock);
	for (i = 0; i < ENERGY_MAX_MEMBERS; i++) {
		if (!energy_members[i].em_used)
			continue;
		sbuf_printf(&sb, "%s demand=%ju budget=%ju weight=%ju\n",
		    energy_members[i].em_name,
		    (uintmax_t)energy_members[i].em_demand_last,
		    (uintmax_t)energy_members[i].em_budget_last,
		    (uintmax_t)energy_members[i].em_weight);
	}
	mtx_unlock(&energy_lock);
	error = sbuf_finish(&sb);
	sbuf_delete(&sb);
	return (error);
}
SYSCTL_PROC(_kern_sched, OID_AUTO, energy_members,
    CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_MPSAFE, NULL, 0,
    energy_members_sysctl, "A",
    "Energy federation members: name demand(mW) budget(mW) weight");

static void
laminar_ctrl_cb(void *arg __unused)
{
	struct laminar_tdq *tdq;
	int cpu, total_load = 0;
	int load_pct, upper, lower;
	int n_parked = 0, n_total = 0;
	int min_load = INT_MAX, min_cpu = -1;
	int first_parked = -1;

	if (!laminar_ctrl_enable)
		goto reschedule;

	CPU_FOREACH(cpu) {
		tdq = LAMINAR_TDQ_CPU(cpu);
		total_load += atomic_load_int(&tdq->ltdq_load);
		n_total++;
		if (atomic_load_int(&tdq->ltdq_resistance_power) > 0) {
			n_parked++;
			if (first_parked < 0)
				first_parked = cpu;
		} else if (atomic_load_int(&tdq->ltdq_load) < min_load) {
			min_load = atomic_load_int(&tdq->ltdq_load);
			min_cpu = cpu;
		}
	}
	int n_unparked = n_total - n_parked;
	if (n_unparked <= 0)
		goto reschedule;

	/*
	 * Utilization metric: runnable threads per unparked CPU,
	 * scaled to "100% = 1 thread/CPU".  ltdq_load is a thread
	 * count, not a CPU-time fraction, so load_pct above 100 is
	 * normal under heavy load.  Headroom = 75% means "the average
	 * unparked CPU has <0.75 threads queued"; deadband 15 sets
	 * unpark above 90% and park below 60%.
	 */
	load_pct = total_load * 100 / n_unparked;
	/* EWMA smoothing (3:1 like the balancer's signal). */
	laminar_ctrl_load_ewma = (3 * laminar_ctrl_load_ewma + load_pct) / 4;
	/*
	 * Lead term (the L in RLC) on the same load_pct input.  Park/unpark
	 * keep reading the bare ewma (patient); the band reads
	 * load_lead_compensated for fast transient response.
	 */
	int load_lead_offset = laminar_lead_term(&laminar_ctrl_load_lead,
	    load_pct, laminar_ctrl_load_lead_kd);
	int load_lead_compensated = laminar_ctrl_load_ewma + load_lead_offset;

	/*
	 * Emergency unpark: a raw load_pct way above setpoint means
	 * a burst arrived against the parked set.  Unpark ALL parked
	 * CPUs immediately, bypassing EWMA + patience.  Without this
	 * a cold-start burst (system was idle, controller had parked
	 * most CPUs, then a load arrives) starves interactive work
	 * for whole multiples of ctrl_interval until the smoother
	 * caught up -- sshd banner timeouts during heavy benchmarks
	 * surfaced this.
	 */
	if (laminar_ctrl_emergency > 0 &&
	    load_pct > laminar_ctrl_emergency && n_parked > 0) {
		/*
		 * Emergency unpark: a burst arrived against a parked set.
		 * Unpark all parked CPUs in one tick, bypassing patience.
		 * The band's own cold-start used to need a separate fast
		 * path here -- the L term on load_lead_compensated now
		 * handles it without a special case (RLC residuals plan).
		 */
		CPU_FOREACH(cpu) {
			tdq = LAMINAR_TDQ_CPU(cpu);
			if (atomic_load_int(&tdq->ltdq_resistance_power) > 0)
				atomic_store_int(&tdq->ltdq_resistance_power, 0);
		}
		laminar_ctrl_unpark_streak = 0;
		laminar_ctrl_park_streak = 0;
		/*
		 * Fall through to the band-update block below so the just-
		 * unparked CPUs get a non-zero ctrl_preempt_band on this
		 * same tick (driven by load_lead_compensated, which already
		 * reflects the burst).  Without this fallthrough, the band
		 * stayed at 0 until the next controller tick.
		 */
	}

	upper = laminar_ctrl_headroom + laminar_ctrl_deadband;
	lower = laminar_ctrl_headroom - laminar_ctrl_deadband;

	if (laminar_ctrl_load_ewma > upper) {
		/* Over-loaded -- unpark a CPU. */
		laminar_ctrl_unpark_streak++;
		laminar_ctrl_park_streak = 0;
		if (laminar_ctrl_unpark_streak >= laminar_ctrl_unpark_pat &&
		    first_parked >= 0) {
			tdq = LAMINAR_TDQ_CPU(first_parked);
			atomic_store_int(&tdq->ltdq_resistance_power, 0);
			laminar_ctrl_unpark_streak = 0;
		}
	} else if (laminar_ctrl_load_ewma < lower) {
		/* Under-loaded -- park a CPU (but keep at least 1 alive). */
		laminar_ctrl_park_streak++;
		laminar_ctrl_unpark_streak = 0;
		/*
		 * Phase H.3: gate parking on having reached the minimum
		 * cpufreq level first.  Frequency-first, park-second: the
		 * gentle ramp through P-states recovers more power per unit
		 * disruption than parking does, and a parked CPU at top
		 * freq still leaks energy through its power rail.  Only
		 * park once we are already at min frequency AND the load
		 * has stayed below the lower threshold for park_pat ticks.
		 *
		 * Inert if phase H is disabled or no cpufreq backend, so
		 * the existing phase-G park behavior is preserved on
		 * platforms without DVFS.
		 */
		bool freq_at_floor = true;
		if (laminar_phaseh_enable && laminar_phaseh_present &&
		    laminar_dvfs_n_levels > 0)
			freq_at_floor = (laminar_dvfs_cur_idx >=
			    laminar_dvfs_n_levels - 1);
		if (laminar_ctrl_park_streak >= laminar_ctrl_park_pat &&
		    min_cpu >= 0 && n_parked < n_total - 1 &&
		    freq_at_floor) {
			tdq = LAMINAR_TDQ_CPU(min_cpu);
			atomic_store_int(&tdq->ltdq_resistance_power,
			    laminar_ctrl_evac_r);
			laminar_ctrl_park_streak = 0;
		}
	} else {
		/* In deadband -- decay streaks. */
		laminar_ctrl_park_streak = 0;
		laminar_ctrl_unpark_streak = 0;
	}

	/*
	 * Controller-driven preempt R band.  load_pct is raw runnable
	 * count per unparked CPU * 100.  Above the threshold, widen the
	 * preempt cost-tolerance band proportionally.  Applied uniformly
	 * to all unparked CPUs (parked CPUs keep band=0 -- they aren't
	 * picking anything).  Step / threshold / max are tunable; default
	 * thresh=100 (one runnable per CPU), step=25, max=4: band reaches
	 * +1 at load_pct=125, +4 at load_pct>=200.
	 */
	if (laminar_ctrl_band_enable) {
		int band_target = 0;
		int load_for_band = load_lead_compensated;
		if (load_for_band > laminar_ctrl_band_thresh &&
		    laminar_ctrl_band_step > 0) {
			band_target = (load_for_band - laminar_ctrl_band_thresh) /
			    laminar_ctrl_band_step;
			if (band_target > laminar_ctrl_band_max)
				band_target = laminar_ctrl_band_max;
			if (band_target < 0)
				band_target = 0;
		}
		CPU_FOREACH(cpu) {
			tdq = LAMINAR_TDQ_CPU(cpu);
			if (atomic_load_int(&tdq->ltdq_resistance_power) > 0)
				atomic_store_int(&tdq->ltdq_ctrl_preempt_band, 0);
			else
				atomic_store_int(&tdq->ltdq_ctrl_preempt_band,
				    band_target);
		}
	}

	/*
	 * Phase H.2: DVFS actuation.  Inert unless phaseh_enable=1 AND a
	 * cpufreq backend was found at SI_SUB_LAST.  Uses the same load
	 * signals already computed:
	 *   load_lead_compensated -- eager (upshift decisions)
	 *   laminar_ctrl_load_ewma -- patient (downshift decisions)
	 */
	laminar_dvfs_step(load_lead_compensated, laminar_ctrl_load_ewma);
	laminar_ctrl_cb_federation();

reschedule:
	callout_reset(&laminar_ctrl_callout,
	    imax(1, hz * laminar_ctrl_interval / 1000),
	    laminar_ctrl_cb, NULL);
}

static void
laminar_ctrl_start(void)
{

	callout_init(&laminar_ctrl_callout, 1);
	callout_reset(&laminar_ctrl_callout,
	    imax(1, hz * laminar_ctrl_interval / 1000),
	    laminar_ctrl_cb, NULL);
}

/*
 * Phase H: DVFS as a second RLC actuation channel.
 *
 * Scope (H.2): closed-loop frequency actuation through cpufreq(4),
 * driven by the same controller (laminar_ctrl_cb) that today writes
 * R_power for parking.  Same cost equation: lowering frequency
 * lowers ltdq_capacity, which raises (load+R)*CAP_BASE/capacity, so
 * placer naturally sends less work there.  Inert without a cpufreq
 * backend (laminar_phaseh_present=0) or when phaseh_enable=0.
 *
 * Design: docs/spec/atrium-scheduler-phase-H-dvfs.md.
 *
 * Damping (asymmetric, matches phase-G's lazy-park / eager-unpark):
 *   up:   1-tick patience, jump straight to target (lead-compensated
 *         load drives this; bursts must respond quickly).
 *   down: 5-tick patience, one level per commit (gentle ramp; avoids
 *         high->low->high oscillation that would waste cache).
 *
 * cf_set_method takes an sx lock (kern_cpu.c:91), so actuation MUST
 * happen outside the callout context.  The controller computes a
 * target index in the callout and enqueues a taskqueue task; the
 * task does CPUFREQ_SET + CPUFREQ_GET + per-CPU ltdq_capacity write.
 */
static device_t laminar_cf_dev[MAXCPU];	/* cpufreq child per CPU, NULL = absent */
/* laminar_phaseh_present, laminar_phaseh_enable, laminar_dvfs_n_levels,
 * laminar_dvfs_cur_idx forward-declared near top of file. */
static int laminar_power_policy = 5;	/* 0..10, 5 = balanced (today's default) */

/* H.2 damping parameters (sysctl-tunable). */
static int laminar_dvfs_up_patience = 1;	/* ticks before commit on upshift */
static int laminar_dvfs_down_patience = 5;	/* ticks before commit on downshift */
static int laminar_dvfs_target_headroom = 25;	/* % capacity above load_pct */

/* H.2 state. */
#define	LAMINAR_DVFS_MAX_LEVELS	16
static struct cf_level laminar_dvfs_levels[LAMINAR_DVFS_MAX_LEVELS];
static int laminar_dvfs_max_freq;	/* levels[0].total_set.freq, MHz */
static int laminar_dvfs_committed_idx;	/* atomic, read by taskqueue */
static int laminar_dvfs_up_streak;
static int laminar_dvfs_down_streak;
static int laminar_dvfs_changes_total;	/* observability */
/*
 * P6: the energy-optimal frequency floor f* (federation plan; proven in
 * gpusim dvfs.rs).  E(f) = k*W*f^2 + P_static*W/f is convex with its
 * minimum at f* = (P_static/2k)^(1/3): BELOW f*, running slower WASTES
 * energy — leakage over the longer runtime beats the dynamic saving,
 * so race-to-idle at f* wins.  In-kernel f* needs no constants: energy
 * per unit work at level i is (P_i - P_idle)/f_i straight from the
 * cpufreq level table; f* = argmin.  dvfs_idle_mw is the platform idle
 * power the table's active numbers exclude (raises f*; default 0 =
 * conservative).  Levels without power data leave f* unconstrained.
 */
static int laminar_dvfs_idle_mw = 0;
static int laminar_dvfs_fstar_idx = -1;	/* observability; -1 = none */
/*
 * P6 federation: the CPU member's budget arrives as a DVFS level
 * CEILING (slowest-compatible index; -1 = uncapped).  Demand = load%% x
 * the current level's power.
 */
static int laminar_dvfs_budget_idx = -1;

static uint64_t
laminar_cpu_demand_mw(void *arg __unused, int posture __unused)
{
	int idx = laminar_dvfs_cur_idx;
	int load = laminar_ctrl_load_ewma;

	if (laminar_dvfs_n_levels == 0 || idx < 0 ||
	    idx >= laminar_dvfs_n_levels)
		return (0);
	if (load < 0)
		load = 0;
	if (load > 100)
		load = 100;
	/*
	 * posture could scale the ask (performance -> demand more so the cap
	 * binds sooner); for now the CPU demand is the measured load x level
	 * power, and posture shapes only the actuator (headroom/ceiling).
	 */
	return ((uint64_t)laminar_dvfs_levels[idx].total_set.power *
	    load / 100);
}

/*
 * The CPU energy actuator (invariant #2: ONE controller).  It seeks the
 * posture target (parking + DVFS knobs, via laminar_apply_power_policy) AND
 * clamps to the granted budget (a DVFS level ceiling).  Threading posture
 * here folds the old standalone power-policy application into the same path,
 * so the cap-clamp and the posture set-point cannot diverge.
 */
static void
laminar_cpu_actuate(void *arg __unused, uint64_t mw, int posture)
{
	int i;

	/* Posture set-point: re-assert the policy knobs for this posture. */
	laminar_apply_power_policy(posture);

	if (mw == 0 || laminar_dvfs_n_levels == 0) {
		laminar_dvfs_budget_idx = -1;	/* uncapped */
		return;
	}
	/* fastest level whose full power fits the budget; levels are
	 * sorted fastest-first, so scan forward to the first that fits */
	for (i = 0; i < laminar_dvfs_n_levels; i++) {
		if ((uint64_t)laminar_dvfs_levels[i].total_set.power <= mw)
			break;
	}
	if (i >= laminar_dvfs_n_levels)
		i = laminar_dvfs_n_levels - 1;	/* even the floor exceeds */
	laminar_dvfs_budget_idx = i;
}

static struct taskqueue *laminar_dvfs_tq;
static struct task laminar_dvfs_task;
static int laminar_dvfs_task_inflight;	/* coalesce enqueues */

SYSCTL_INT(_kern_sched, OID_AUTO, phaseh_enable, CTLFLAG_RW,
    &laminar_phaseh_enable, 0,
    "Laminar phase H (DVFS) master enable; controller writes P-state via "
    "cpufreq(4) when 1 and a backend is present");

/*
 * Phase H.4: power policy table.  power_policy (0..10) maps to a set
 * of underlying knobs that shape both the parking controller (phase G)
 * and the DVFS controller (phase H.2-3) without introducing a separate
 * decision path.  Level 5 = balanced = today's defaults.  Level 10 =
 * performance = pins freq at max (huge headroom + never-downshift) AND
 * blocks parking (freq_at_floor gate from H.3 stays false).  Level 0 =
 * powersave = aggressive on both axes.
 */
static const struct laminar_policy_knobs {
	int headroom;		/* ctrl_headroom %                       */
	int park_pat;		/* ctrl_park_pat ticks                   */
	int dvfs_headroom;	/* dvfs_target_headroom %                */
	int dvfs_down_pat;	/* dvfs_down_patience ticks              */
} laminar_policy_table[11] = {
	/* 0: powersave */ {  30,        10,    5,            3 },
	/* 1            */ {  39,        14,    9,            3 },
	/* 2            */ {  48,        18,   13,            4 },
	/* 3            */ {  57,        22,   17,            4 },
	/* 4            */ {  66,        26,   21,            5 },
	/* 5: balanced  */ {  75,        30,   25,            5 },
	/* 6            */ {  79,        50,   35,            6 },
	/* 7            */ {  83,       100,   45,            8 },
	/* 8            */ {  87,      1000,   60,           12 },
	/* 9            */ {  91,     10000,   80,           20 },
	/* 10: perf     */ {  95, INT_MAX/2,  100,    INT_MAX/2 },
};

static void
laminar_apply_power_policy(int level)
{
	const struct laminar_policy_knobs *k;

	if (level < 0)
		level = 0;
	if (level > 10)
		level = 10;
	laminar_power_policy = level;
	k = &laminar_policy_table[level];
	laminar_ctrl_headroom = k->headroom;
	laminar_ctrl_park_pat = k->park_pat;
	laminar_dvfs_target_headroom = k->dvfs_headroom;
	laminar_dvfs_down_patience = k->dvfs_down_pat;
}

static int
laminar_power_policy_sysctl(SYSCTL_HANDLER_ARGS)
{
	int val, error;

	val = laminar_power_policy;
	error = sysctl_handle_int(oidp, &val, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	laminar_apply_power_policy(val);
	return (0);
}

static int
laminar_power_policy_name_sysctl(SYSCTL_HANDLER_ARGS)
{
	char buf[16];
	int new_level, error;
	const char *name;

	switch (laminar_power_policy) {
	case 0:  name = "powersave"; break;
	case 5:  name = "balanced";  break;
	case 10: name = "performance"; break;
	default:
		if (laminar_power_policy < 5)
			name = "below-balanced";
		else
			name = "above-balanced";
		break;
	}
	strlcpy(buf, name, sizeof(buf));
	error = sysctl_handle_string(oidp, buf, sizeof(buf), req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	if (strcmp(buf, "powersave") == 0)
		new_level = 0;
	else if (strcmp(buf, "balanced") == 0)
		new_level = 5;
	else if (strcmp(buf, "performance") == 0)
		new_level = 10;
	else
		return (EINVAL);
	laminar_apply_power_policy(new_level);
	return (0);
}

SYSCTL_PROC(_kern_sched, OID_AUTO, power_policy,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_MPSAFE, NULL, 0,
    laminar_power_policy_sysctl, "I",
    "Laminar power policy (0=powersave .. 5=balanced .. 10=performance); "
    "writing maps to ctrl_headroom, ctrl_park_pat, dvfs_target_headroom, "
    "and dvfs_down_patience via a single curated lookup table");
SYSCTL_PROC(_kern_sched, OID_AUTO, power_policy_name,
    CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_MPSAFE, NULL, 0,
    laminar_power_policy_name_sysctl, "A",
    "Laminar power policy by name: 'powersave' (level 0), 'balanced' "
    "(5, default), or 'performance' (10)");
SYSCTL_INT(_kern_sched, OID_AUTO, phaseh_present, CTLFLAG_RD,
    &laminar_phaseh_present, 0,
    "1 if a cpufreq(4) backend was found for any CPU, 0 otherwise");
SYSCTL_INT(_kern_sched, OID_AUTO, dvfs_up_patience, CTLFLAG_RW,
    &laminar_dvfs_up_patience, 0,
    "Laminar DVFS: ticks of consistent upshift target before committing "
    "(default 1 = ~100 ms; lead-compensator amplifies bursts so 1 tick is "
    "responsive)");
SYSCTL_INT(_kern_sched, OID_AUTO, dvfs_down_patience, CTLFLAG_RW,
    &laminar_dvfs_down_patience, 0,
    "Laminar DVFS: ticks of consistent downshift target before committing "
    "(default 5 = ~500 ms; gentle ramp avoids cache-wasting oscillation)");
SYSCTL_INT(_kern_sched, OID_AUTO, dvfs_target_headroom, CTLFLAG_RW,
    &laminar_dvfs_target_headroom, 0,
    "Laminar DVFS: percent capacity above current load_pct used as the "
    "frequency target (default 25)");
SYSCTL_INT(_kern_sched, OID_AUTO, dvfs_idle_mw, CTLFLAG_RWTUN,
    &laminar_dvfs_idle_mw, 0,
    "Laminar P6: platform idle power (mW) excluded from cpufreq level "
    "power; raises the energy-optimal floor f*");
SYSCTL_INT(_kern_sched, OID_AUTO, dvfs_fstar_idx, CTLFLAG_RD,
    &laminar_dvfs_fstar_idx, 0,
    "Laminar P6: computed energy-optimal frequency floor (level index; "
    "-1 = no power data, unconstrained)");
SYSCTL_INT(_kern_sched, OID_AUTO, dvfs_cur_idx, CTLFLAG_RD,
    &laminar_dvfs_cur_idx, 0,
    "Laminar DVFS: current P-state index (0 = fastest)");
SYSCTL_INT(_kern_sched, OID_AUTO, dvfs_n_levels, CTLFLAG_RD,
    &laminar_dvfs_n_levels, 0,
    "Laminar DVFS: number of cached P-state levels (0 until first apply)");
SYSCTL_INT(_kern_sched, OID_AUTO, dvfs_max_freq, CTLFLAG_RD,
    &laminar_dvfs_max_freq, 0,
    "Laminar DVFS: max frequency from cached levels[0], MHz");
SYSCTL_INT(_kern_sched, OID_AUTO, dvfs_changes_total, CTLFLAG_RW,
    &laminar_dvfs_changes_total, 0,
    "Laminar DVFS: total CPUFREQ_SET applications since boot (write 0 reset)");

/*
 * Update per-CPU ltdq_capacity to reflect a newly-applied frequency.
 * Called from the taskqueue after CPUFREQ_GET reports the achieved freq.
 * Per kern_cpu.c, cpufreq is system-wide uniform, so all CPUs get the
 * same capacity ratio.
 */
static void
laminar_dvfs_write_capacity(int achieved_freq_mhz)
{
	struct laminar_tdq *tdq;
	int cpu, new_cap;

	if (laminar_dvfs_max_freq <= 0)
		return;
	new_cap = achieved_freq_mhz * LAMINAR_CAP_BASE / laminar_dvfs_max_freq;
	if (new_cap < 1)
		new_cap = 1;	/* divide-by-zero guard in laminar_scaled_cost */
	if (new_cap > LAMINAR_CAP_BASE)
		new_cap = LAMINAR_CAP_BASE;
	CPU_FOREACH(cpu) {
		tdq = LAMINAR_TDQ_CPU(cpu);
		atomic_store_int(&tdq->ltdq_capacity, new_cap);
	}
}

/*
 * Taskqueue handler: applies the committed P-state.  Lazy-discovers
 * the level array on first call (CPUFREQ_LEVELS takes the cpufreq sx
 * lock, which sleeps -- safe here, not in the callout).
 */
static void
laminar_dvfs_apply(void *ctx __unused, int pending __unused)
{
	device_t cf;
	int target_idx, count, error;
	struct cf_level cur_level;

	cf = laminar_cf_dev[0];	/* uniform system-wide; one cf_dev suffices */
	if (cf == NULL) {
		atomic_store_int(&laminar_dvfs_task_inflight, 0);
		return;
	}

	/* Lazy level discovery on first invocation. */
	if (laminar_dvfs_n_levels == 0) {
		count = LAMINAR_DVFS_MAX_LEVELS;
		error = CPUFREQ_LEVELS(cf, laminar_dvfs_levels, &count);
		if (error != 0 || count <= 0) {
			atomic_store_int(&laminar_dvfs_task_inflight, 0);
			return;	/* will retry on next enqueue */
		}
		laminar_dvfs_n_levels = count;
		{
			static bool cpu_member_registered = false;

			if (!cpu_member_registered && count > 0) {
				cpu_member_registered = true;
				(void)energy_member_register("cpu",
				    laminar_cpu_demand_mw,
				    laminar_cpu_actuate, NULL, 2);
			}
		}
		laminar_dvfs_max_freq = laminar_dvfs_levels[0].total_set.freq;
		/* Seed cur to whatever cpufreq is at now. */
		if (CPUFREQ_GET(cf, &cur_level) == 0) {
			int i;
			for (i = 0; i < count; i++) {
				if (laminar_dvfs_levels[i].total_set.freq ==
				    cur_level.total_set.freq) {
					laminar_dvfs_cur_idx = i;
					break;
				}
			}
			laminar_dvfs_write_capacity(cur_level.total_set.freq);
		}
	}

	target_idx = atomic_load_int(&laminar_dvfs_committed_idx);
	if (target_idx < 0 || target_idx >= laminar_dvfs_n_levels) {
		atomic_store_int(&laminar_dvfs_task_inflight, 0);
		return;
	}
	if (target_idx == laminar_dvfs_cur_idx) {
		atomic_store_int(&laminar_dvfs_task_inflight, 0);
		return;
	}

	error = CPUFREQ_SET(cf, &laminar_dvfs_levels[target_idx],
	    CPUFREQ_PRIO_KERN);
	if (error == 0) {
		if (CPUFREQ_GET(cf, &cur_level) == 0) {
			laminar_dvfs_write_capacity(cur_level.total_set.freq);
			laminar_dvfs_cur_idx = target_idx;
			laminar_dvfs_changes_total++;
		}
	}
	atomic_store_int(&laminar_dvfs_task_inflight, 0);
}

/*
 * Compute the controller's target P-state index from a load_pct
 * sample, apply asymmetric patience, and enqueue the taskqueue if
 * the commit threshold is reached.  Called once per controller tick
 * from laminar_ctrl_cb.
 *
 * load_lead is the lead-compensated load_pct (eager up-response);
 * load_bare is the bare EWMA (patient down-response).
 *
 * Returns immediately if phase H is disabled, no backend, or levels
 * not yet discovered (first taskqueue invocation populates them).
 */
static void
laminar_dvfs_step(int load_lead, int load_bare)
{
	int target_freq, target_idx, i;
	int up_freq, mid;
	int down_freq;

	if (!laminar_phaseh_enable || !laminar_phaseh_present)
		return;
	if (laminar_dvfs_n_levels == 0) {
		/* Kick the taskqueue to discover levels; harmless if pending. */
		if (atomic_cmpset_int(&laminar_dvfs_task_inflight, 0, 1))
			taskqueue_enqueue(laminar_dvfs_tq, &laminar_dvfs_task);
		return;
	}

	/*
	 * Up-decision uses lead-compensated load (eager).  Map load + headroom
	 * to a target frequency, then to the nearest level (levels[] sorted
	 * descending by freq).  Snap by midpoint between adjacent levels.
	 */
	{
		int load_for_up = load_lead + laminar_dvfs_target_headroom;
		if (load_for_up > 100)
			load_for_up = 100;
		if (load_for_up < 1)
			load_for_up = 1;
		up_freq = laminar_dvfs_max_freq * load_for_up / 100;
	}
	{
		int load_for_dn = load_bare + laminar_dvfs_target_headroom;
		if (load_for_dn > 100)
			load_for_dn = 100;
		if (load_for_dn < 1)
			load_for_dn = 1;
		down_freq = laminar_dvfs_max_freq * load_for_dn / 100;
	}

	/*
	 * P6: compute the energy-optimal floor f* from the level table
	 * (argmin (P_i - idle)/f_i, fixed-point x1024).  Cheap (<= a few
	 * levels), recomputed every step so dvfs_idle_mw edits apply
	 * immediately.  Indexes are sorted fastest-first, so "never run
	 * slower than f*" is a clamp to idx <= fstar.
	 */
	{
		int fstar = -1;
		int64_t best = INT64_MAX;

		for (i = 0; i < laminar_dvfs_n_levels; i++) {
			int p = laminar_dvfs_levels[i].total_set.power;
			int f = laminar_dvfs_levels[i].total_set.freq;
			int64_t sc;

			if (p <= 0 || f <= 0) {
				fstar = -1;	/* incomplete data: no floor */
				break;
			}
			p -= laminar_dvfs_idle_mw;
			if (p < 1)
				p = 1;
			sc = ((int64_t)p << 10) / f;
			if (sc < best) {
				best = sc;
				fstar = i;
			}
		}
		laminar_dvfs_fstar_idx = fstar;
	}

	/* For commit purposes, derive ONE target_idx.  Use the up freq (eager). */
	target_freq = up_freq;
	target_idx = laminar_dvfs_n_levels - 1;
	for (i = 0; i < laminar_dvfs_n_levels - 1; i++) {
		mid = (laminar_dvfs_levels[i].total_set.freq +
		    laminar_dvfs_levels[i + 1].total_set.freq) / 2;
		if (target_freq >= mid) {
			target_idx = i;
			break;
		}
	}

	/*
	 * P6 federation: the budget ceiling is a HARD power constraint,
	 * not a load preference.  If we are running faster than the cap
	 * allows (cur_idx < budget_idx), jump straight down to it NOW —
	 * bypassing the down-patience and the bare-EWMA "load wants
	 * speed" hold below, which would otherwise let a busy CPU sit
	 * over its power budget indefinitely (the cap exists precisely
	 * to override what the load wants).
	 */
	if (laminar_dvfs_budget_idx >= 0 &&
	    laminar_dvfs_cur_idx < laminar_dvfs_budget_idx) {
		laminar_dvfs_up_streak = 0;
		laminar_dvfs_down_streak = 0;
		atomic_store_int(&laminar_dvfs_committed_idx,
		    laminar_dvfs_budget_idx);
		if (atomic_cmpset_int(&laminar_dvfs_task_inflight, 0, 1))
			taskqueue_enqueue(laminar_dvfs_tq,
			    &laminar_dvfs_task);
		return;
	}
	/* Below the cap: clamp the load-driven target up to the ceiling. */
	if (laminar_dvfs_budget_idx >= 0 &&
	    target_idx < laminar_dvfs_budget_idx)
		target_idx = laminar_dvfs_budget_idx;
	/* P6: never slower than the energy-optimal floor (the ceiling
	 * wins when they conflict — the cap is a hard constraint). */
	if (laminar_dvfs_fstar_idx >= 0 &&
	    target_idx > laminar_dvfs_fstar_idx &&
	    (laminar_dvfs_budget_idx < 0 ||
	    laminar_dvfs_fstar_idx >= laminar_dvfs_budget_idx))
		target_idx = laminar_dvfs_fstar_idx;

	if (target_idx == laminar_dvfs_cur_idx) {
		laminar_dvfs_up_streak = 0;
		laminar_dvfs_down_streak = 0;
		return;
	}

	if (target_idx < laminar_dvfs_cur_idx) {
		/* upshift (faster) */
		laminar_dvfs_down_streak = 0;
		laminar_dvfs_up_streak++;
		if (laminar_dvfs_up_streak >= laminar_dvfs_up_patience) {
			/* Jump straight to target. */
			laminar_dvfs_up_streak = 0;
			atomic_store_int(&laminar_dvfs_committed_idx,
			    target_idx);
			if (atomic_cmpset_int(&laminar_dvfs_task_inflight, 0, 1))
				taskqueue_enqueue(laminar_dvfs_tq,
				    &laminar_dvfs_task);
		}
	} else {
		/* downshift (slower).  Re-evaluate using bare-EWMA target. */
		int down_idx = laminar_dvfs_n_levels - 1;
		int j;
		for (j = 0; j < laminar_dvfs_n_levels - 1; j++) {
			int dmid = (laminar_dvfs_levels[j].total_set.freq +
			    laminar_dvfs_levels[j + 1].total_set.freq) / 2;
			if (down_freq >= dmid) {
				down_idx = j;
				break;
			}
		}
		if (down_idx <= laminar_dvfs_cur_idx) {
			/* Bare EWMA disagrees with lead-compensated -- hold. */
			laminar_dvfs_up_streak = 0;
			laminar_dvfs_down_streak = 0;
			return;
		}
		laminar_dvfs_up_streak = 0;
		laminar_dvfs_down_streak++;
		if (laminar_dvfs_down_streak >= laminar_dvfs_down_patience) {
			/* One level per commit. */
			laminar_dvfs_down_streak = 0;
			atomic_store_int(&laminar_dvfs_committed_idx,
			    laminar_dvfs_cur_idx + 1);
			if (atomic_cmpset_int(&laminar_dvfs_task_inflight, 0, 1))
				taskqueue_enqueue(laminar_dvfs_tq,
				    &laminar_dvfs_task);
		}
	}
}

/*
 * Walk all CPUs, look up the cpufreq child of each cpu device, cache
 * the device_t pointer.  Runs once at SI_SUB_LAST so device attach is
 * complete.  Also creates the DVFS taskqueue used by H.2 actuation.
 *
 * Per kern_cpu.c:1096-1098, FreeBSD's cpufreq core enforces uniform
 * system-wide P-state today, so finding any one CPU's cf_dev is
 * sufficient for actuation.  We still cache per CPU so future
 * per-domain support is a drop-in.
 */
static void
laminar_phaseh_init(void *arg __unused)
{
	device_t cpu_dev, cf_dev;
	int cpu, found = 0;

	CPU_FOREACH(cpu) {
		cpu_dev = pcpu_find(cpu)->pc_device;
		if (cpu_dev == NULL)
			continue;
		cf_dev = device_find_child(cpu_dev, "cpufreq",
		    DEVICE_UNIT_ANY);
		laminar_cf_dev[cpu] = cf_dev;
		if (cf_dev != NULL)
			found++;
	}
	laminar_phaseh_present = (found > 0) ? 1 : 0;
	if (laminar_phaseh_present) {
		TASK_INIT(&laminar_dvfs_task, 0, laminar_dvfs_apply, NULL);
		laminar_dvfs_tq = taskqueue_create("laminar_dvfs", M_WAITOK,
		    taskqueue_thread_enqueue, &laminar_dvfs_tq);
		taskqueue_start_threads(&laminar_dvfs_tq, 1, PWAIT,
		    "laminar_dvfs");
		printf("laminar: phase H (DVFS) -- cpufreq backend found on "
		    "%d CPU(s); set kern.sched.phaseh_enable=1 to actuate.\n",
		    found);
	} else {
		printf("laminar: phase H (DVFS) -- no cpufreq backend; "
		    "park-only power control.\n");
	}
}
SYSINIT(laminar_phaseh, SI_SUB_LAST, SI_ORDER_ANY, laminar_phaseh_init, NULL);
#endif /* SMP */

/*
 * Enqueue a thread on the tdq.  Returns the previous lowpri so callers
 * can detect whether they should request preemption.  Assumes the
 * caller already holds the tdq lock and the thread lock.
 */
static int
tdq_add_internal(struct laminar_tdq *tdq, struct thread *td, int flags)
{
	int lowpri;

	LAMINAR_TDQ_LOCK_ASSERT(tdq, MA_OWNED);
	THREAD_LOCK_BLOCKED_ASSERT(td, MA_OWNED);
	KASSERT(td->td_inhibitors == 0,
	    ("tdq_add_internal: inhibited thread"));
	KASSERT(TD_CAN_RUN(td) || TD_IS_RUNNING(td),
	    ("tdq_add_internal: bad thread state"));
	KASSERT((td->td_flags & TDF_INMEM) != 0,
	    ("tdq_add_internal: thread swapped out"));

	/*
	 * Bounded-lag rebase relative to THIS tdq's vruntime floor
	 * (whitepaper §7).  We are about to insert td into this CPU's
	 * SoA picker, so its vruntime must be competitive locally.
	 *
	 * Previously rebase lived in sched_laminar_wakeup using the
	 * waker's ts_cpu floor, but sched_laminar_add's pickcpu can
	 * migrate the waker to a different CPU with a much higher
	 * floor -- leaving the waker stranded above the new floor and
	 * starved slice after slice (multi-second wake-latency tails
	 * under 2x oversubscription, R4).
	 *
	 * Running here covers fork, wakeup, and balancer migration
	 * uniformly: every cross-CPU enqueue lands with vruntime
	 * within lag_cap of the destination floor, so the SoA picker
	 * treats the arriver fairly against local incumbents.
	 *
	 * Only applies to timeshare threads -- realtime/idle classes
	 * don't use vruntime.
	 */
	if (laminar_is_timeshare(td)) {
		struct td_sched *ts = td_get_sched(td);
		uint64_t floor = atomic_load_64(&tdq->ltdq_vtime);
		uint64_t cap = laminar_lag_cap;
		uint64_t lo = (floor > cap) ? (floor - cap) : 0;

		/*
		 * Symmetric bounded-lag clamp.  The original was UP-only
		 * gated on (floor > cap), which never fired in benches
		 * shorter than ~10s @ hz=100 (floor < cap = 1M).  That
		 * left wakers with inherited-from-parent vruntime FAR
		 * above the local floor; the SoA picker then preferred
		 * incumbents at floor for seconds, observed as R4
		 * multi-second wake-pick max (1.3-3s).
		 *
		 * For balancer migrations (SRQ_YIELDING) we keep the
		 * old UP-only behaviour -- the migrant's vruntime
		 * represents accumulated work on the source CPU and
		 * should be preserved, not clamped down to local floor.
		 * Otherwise balancer ping-pong: migrate, clamp down,
		 * earn "credit" locally, balancer migrates again on
		 * next cycle, clamp again, etc.
		 *
		 * For wakeups / forks (non-YIELDING) clamp DOWN as well:
		 * the arriver is fresh and should be competitive locally.
		 */
		if (ts->ts_vruntime < lo) {
			ts->ts_vruntime = lo;
		} else if ((flags & SRQ_YIELDING) == 0 &&
		    ts->ts_vruntime > floor) {
			/*
			 * Clamp DOWN to floor (not floor+cap).  Waker
			 * is competitive locally and the picker (pure
			 * min-vruntime) will pick it on the next
			 * sched_choose.  floor+cap = floor+1M was way
			 * too lenient -- waker stayed above local floor
			 * by up to cap/eff_weight = 10s of incumbent
			 * vruntime advancement before picker switched.
			 * SRQ_YIELDING (balancer migration) skipped to
			 * preserve cross-CPU fairness accounting.
			 */
			ts->ts_vruntime = floor;
		}
	}

	lowpri = tdq->ltdq_lowpri;
	if (td->td_priority < lowpri)
		tdq->ltdq_lowpri = td->td_priority;
	tdq_runq_add(tdq, td, flags);
	tdq_load_add(tdq, td);
	return (lowpri);
}

/*
 * General scheduling info.
 */
static int
sched_laminar_load(void)
{
#ifdef SMP
	int total, i;

	total = 0;
	CPU_FOREACH(i)
		total += atomic_load_int(&LAMINAR_TDQ_CPU(i)->ltdq_sysload);
	return (total);
#else
	return (atomic_load_int(&LAMINAR_TDQ_SELF()->ltdq_sysload));
#endif
}

static int
sched_laminar_rr_interval(void)
{

	/* Convert sched_slice (stathz ticks) to hz ticks. */
	return (imax(1, (sched_slice * hz + realstathz / 2) / realstathz));
}

static bool
sched_laminar_runnable(void)
{
	struct laminar_tdq *tdq;
	int load;

	tdq = LAMINAR_TDQ_SELF();
	load = atomic_load_int(&tdq->ltdq_load);
	return (load > (TD_IS_IDLETHREAD(curthread) ? 0 : 1));
}

/*
 * Proc/thread lifecycle hooks.
 *
 * The subset that does not need sched_choose / mi_switch lands in this
 * commit (A.3c).  fork_exit, throw, idletd, and ap_entry are tightly
 * coupled with sched_choose and land with the runqueue ops (A.3d).
 */
static void
sched_laminar_exit(struct proc *p, struct thread *childtd)
{

	PROC_LOCK_ASSERT(p, MA_OWNED);
	sched_exit_thread(FIRST_THREAD_IN_PROC(p), childtd);
}

static void
sched_laminar_fork(struct thread *td, struct thread *childtd)
{

	sched_fork_thread(td, childtd);
}

static void
sched_laminar_fork_exit(struct thread *td)
{
	struct laminar_tdq *tdq;
	int cpuid;

	KASSERT(curthread->td_md.md_spinlock_count == 1,
	    ("sched_laminar_fork_exit: invalid spinlock count"));
	cpuid = PCPU_GET(cpuid);
	tdq = LAMINAR_TDQ_SELF();
	LAMINAR_TDQ_LOCK(tdq);
	spinlock_exit();
	MPASS(td->td_lock == LAMINAR_TDQ_LOCKPTR(tdq));
	td->td_oncpu = cpuid;
}

static void
sched_laminar_class(struct thread *td, int class)
{

	THREAD_LOCK_ASSERT(td, MA_OWNED);
	td->td_pri_class = class;
	/*
	 * Cross-class moves (e.g., timeshare -> realtime) need to migrate
	 * the thread between SoA timeshare arrays and the std runq buckets.
	 * That logic lands with the runqueue ops commit (A.3d); for now we
	 * just update the class label.  Any class change while the thread
	 * is on a runqueue is corrected at the next sched_add().
	 */
}

static void
sched_laminar_nice(struct proc *p, int nice)
{
	struct thread *td;
	uint32_t w;

	PROC_LOCK_ASSERT(p, MA_OWNED);
	p->p_nice = nice;
	w = laminar_nice_to_weight(nice);
	/*
	 * Apply the new weight to every thread in this proc.  Threads
	 * share their proc's nice, so they all get the same ts_weight.
	 * Holding thread_lock around laminar_set_weight pairs with the
	 * locked readers in the picker; only the per-thread eff_weight
	 * store is published, the vruntime accumulator is untouched.
	 */
	FOREACH_THREAD_IN_PROC(p, td) {
		uint32_t old;

		thread_lock(td);
		old = td_get_sched(td)->ts_weight;
		laminar_set_weight_for_thread(td, w);
		/*
		 * If this thread is on a runqueue or running, fix the
		 * owning tdq's weighted load by the delta.  thread_lock
		 * is the tdq lock for runnable threads -- same critical
		 * section as tdq_load_add/rem.
		 */
		if (old != w && (TD_ON_RUNQ(td) || TD_IS_RUNNING(td))) {
			struct laminar_tdq *tdq =
			    LAMINAR_TDQ_CPU(td_get_sched(td)->ts_cpu);
			struct td_sched *ts = td_get_sched(td);
			uint32_t contrib = ts->ts_wload_contrib;

			/*
			 * Adjust by (new_weight - contrib), not (w - old),
			 * because ts_wload_contrib is what's actually summed
			 * into tdq->ltdq_wload (may differ from ts_weight
			 * if the thread wasn't on a tdq when contrib last
			 * synced).  Keep ts_wload_contrib in lockstep with
			 * the new weight so subsequent rem subtracts the
			 * right amount.
			 */
			if (w > contrib) {
				tdq->ltdq_wload += (w - contrib);
			} else if (w < contrib) {
				uint32_t diff = contrib - w;
				if (tdq->ltdq_wload >= diff)
					tdq->ltdq_wload -= diff;
				else
					tdq->ltdq_wload = 0;
			}
			ts->ts_wload_contrib = w;
		}
		thread_unlock(td);
	}
}

/*
 * Common: choose the next thread to run while in a spinlock section.
 * Returns with the tdq lock dropped.
 */
static struct thread *
sched_laminar_throw_grab(struct laminar_tdq *tdq)
{
	struct thread *newtd;

	newtd = choosethread();
	spinlock_enter();
	LAMINAR_TDQ_UNLOCK(tdq);
	return (newtd);
}

static void
sched_laminar_ap_entry(void)
{
	struct laminar_tdq *tdq;
	struct thread *newtd;

	tdq = LAMINAR_TDQ_SELF();
	THREAD_LOCKPTR_ASSERT(curthread, LAMINAR_TDQ_LOCKPTR(tdq));
	LAMINAR_TDQ_LOCK(tdq);
	spinlock_exit();
	PCPU_SET(switchtime, cpu_ticks());
	PCPU_SET(switchticks, ticks);
	newtd = sched_laminar_throw_grab(tdq);
	cpu_throw(NULL, newtd);		/* does not return */
}

static void
sched_laminar_exit_thread(struct thread *td, struct thread *child)
{
	struct prison *pr;
	struct laminar_prison *lpr;

	thread_lock(child);
	/*
	 * Load decrement on the exiting thread happens during the
	 * scheduler-rem path which is wired up in A.3d.  Until then this
	 * is a no-op; thread state outside the scheduler is torn down by
	 * the caller.
	 */
	thread_unlock(child);

	/*
	 * Phase E: decrement the jail's thread counter.  Done outside
	 * thread_lock since the atomic decrement is independent and we
	 * don't want to lengthen the critical section.  The slot itself
	 * is never freed -- leaving it claimed avoids races with
	 * concurrent claim/decrement.  Costs ~24 bytes per ever-used
	 * jail; bounded by LAMINAR_MAX_PRISONS.
	 */
	pr = laminar_prison_of(child);
	if (pr != NULL && (lpr = laminar_prison_lookup(pr->pr_id)) != NULL) {
		uint32_t n = atomic_load_int(&lpr->lpr_nthreads);
		if (n > 0)
			atomic_subtract_int(&lpr->lpr_nthreads, 1);
	}
}

static u_int
sched_laminar_estcpu(struct thread *td)
{

	/*
	 * Laminar accounts CPU use via vruntime, not estcpu.  Return 0;
	 * consumers (mostly userland %CPU display) will see no
	 * accumulation through this API until pctcpu/estcpu are derived
	 * from vruntime in a later commit.
	 */
	return (0);
}

static void
sched_laminar_fork_thread(struct thread *td, struct thread *child)
{
	struct td_sched *ts, *tsc;

	THREAD_LOCK_ASSERT(td, MA_OWNED);
	child->td_oncpu = NOCPU;
	child->td_lastcpu = NOCPU;
	child->td_lock = LAMINAR_TDQ_LOCKPTR(LAMINAR_TDQ_SELF());
	child->td_cpuset = cpuset_ref(td->td_cpuset);
	child->td_domain.dr_policy = td->td_cpuset->cs_domain;
	child->td_priority = child->td_base_pri;

	ts = td_get_sched(child);
	tsc = td_get_sched(td);

	bzero(ts, sizeof(*ts));
	ts->ts_vruntime = tsc->ts_vruntime;	/* inherit; halved at A.3d */
	ts->ts_weight = tsc->ts_weight;
	ts->ts_cpu = tsc->ts_cpu;
	ts->ts_class = tsc->ts_class;
	ts->ts_home_node = -1;
	ts->ts_dom_waker_cpu = -1;	/* no IPC home yet */
	/*
	 * Phase E: bump the child's jail nthreads counter (lazy claim
	 * a slot if the jail has no configured laminar entry yet, but
	 * skip the claim if the table is full -- defaults still apply).
	 * Then recompute eff_weight with current jail context.  The
	 * counter must be incremented BEFORE the recompute so the new
	 * thread's own membership is reflected.
	 */
	{
		struct prison *pr;

		pr = laminar_prison_of(child);
		if (pr != NULL) {
			struct laminar_prison *lpr =
			    laminar_prison_claim(pr->pr_id);
			if (lpr != NULL)
				atomic_add_int(&lpr->lpr_nthreads, 1);
		}
	}
	laminar_set_weight_for_thread(child, ts->ts_weight);
}

/*
 * Priority manipulation.
 *
 * Laminar's timeshare-class pick is vruntime-ordered, not priority-
 * ordered, so changing a timeshare thread's priority does not affect its
 * relative ordering on the local runqueue.  Real-time (PRI_ITHD,
 * PRI_REALTIME) and idle (PRI_IDLE) classes use the standard struct runq
 * with per-priority buckets and DO require a re-insert on priority
 * change; that re-insert lands together with the runqueue ops commit
 * (A.3d).  Until then, sched_priority() updates td_priority but does not
 * touch a thread already on a runqueue.
 */
static void
sched_priority(struct thread *td, u_char prio)
{

	THREAD_LOCK_ASSERT(td, MA_OWNED);
	if (td->td_priority == prio)
		return;
	td->td_priority = prio;
	/*
	 * If the thread is on a runqueue, leave it where it is for now.
	 * Re-insert on priority-bucket change is added in commit A.3d.
	 */
}

static void
sched_laminar_prio(struct thread *td, u_char prio)
{
	u_char oldprio;

	td->td_base_pri = prio;

	/*
	 * If the thread is borrowing another thread's priority, never
	 * lower it.
	 */
	if ((td->td_flags & TDF_BORROWING) != 0 && td->td_priority < prio)
		return;

	oldprio = td->td_priority;
	sched_priority(td, prio);

	if (TD_ON_LOCK(td) && oldprio != prio)
		turnstile_adjust(td, oldprio);
}

static void
sched_laminar_lend_prio(struct thread *td, u_char prio)
{

	td->td_flags |= TDF_BORROWING;
	sched_priority(td, prio);
}

static void
sched_laminar_lend_user_prio(struct thread *td, u_char prio)
{

	THREAD_LOCK_ASSERT(td, MA_OWNED);
	td->td_lend_user_pri = prio;
	td->td_user_pri = min(prio, td->td_base_user_pri);
	if (td->td_priority > td->td_user_pri)
		sched_laminar_prio(td, td->td_user_pri);
	else if (td->td_priority != td->td_user_pri)
		ast_sched_locked(td, TDA_SCHED);
}

static void
sched_laminar_lend_user_prio_cond(struct thread *td, u_char prio)
{

	if (td->td_lend_user_pri == prio)
		return;
	thread_lock(td);
	sched_laminar_lend_user_prio(td, prio);
	thread_unlock(td);
}

/*
 * Idle-thread %CPU accounting is vruntime-derived in Laminar; the legacy
 * estcpu / pctcpu values exposed through this API are zero until we wire
 * the conversion (planned alongside the runqueue ops commit).
 */
static fixpt_t
sched_laminar_pctcpu(struct thread *td)
{

	return (0);
}

static void
sched_laminar_ithread_prio(struct thread *td, u_char prio)
{

	THREAD_LOCK_ASSERT(td, MA_OWNED);
	MPASS(td->td_pri_class == PRI_ITHD);
	td->td_base_ithread_pri = prio;
	sched_laminar_prio(td, prio);
}

static void
sched_laminar_sleep(struct thread *td, int prio)
{

	THREAD_LOCK_ASSERT(td, MA_OWNED);
	td->td_slptick = ticks;
	if (prio != 0 && PRI_BASE(td->td_pri_class) == PRI_TIMESHARE)
		sched_laminar_prio(td, prio);
}

static void
sched_laminar_sswitch(struct thread *td, int flags)
{
	struct laminar_tdq *tdq;
	struct thread *newtd;
	struct mtx *mtx;
	int srqflag, preempted;

	THREAD_LOCK_ASSERT(td, MA_OWNED);

	tdq = LAMINAR_TDQ_SELF();
	td->td_lastcpu = td->td_oncpu;
	preempted = (td->td_flags & TDF_SLICEEND) == 0 &&
	    (flags & SW_PREEMPT) != 0;
	td->td_flags &= ~TDF_SLICEEND;
	ast_unsched_locked(td, TDA_SCHED);
	td->td_owepreempt = 0;
	if (!TD_IS_IDLETHREAD(td))
		tdq->ltdq_switchcnt++;

	mtx = thread_lock_block(td);
	spinlock_enter();
	if (TD_IS_IDLETHREAD(td)) {
		MPASS(mtx == LAMINAR_TDQ_LOCKPTR(tdq));
		TD_SET_CAN_RUN(td);
	} else if (TD_IS_RUNNING(td)) {
		MPASS(mtx == LAMINAR_TDQ_LOCKPTR(tdq));
		srqflag = SRQ_OURSELF | SRQ_YIELDING |
		    (preempted ? SRQ_PREEMPTED : 0);
#ifdef SMP
		if (td_get_sched(td)->ts_cpu != PCPU_GET(cpuid)) {
			/*
			 * sched_bind() set ts_cpu to a different CPU.  Move
			 * ourselves to the target's tdq so the next time we
			 * are picked we run there.  Mirrors ULE's
			 * sched_switch_migrate.  Caller's mtx pointer is
			 * updated so cpu_switch publishes td_lock = remote.
			 */
			struct laminar_tdq *tdn =
			    LAMINAR_TDQ_CPU(td_get_sched(td)->ts_cpu);
			tdq_load_rem(tdq, td);
			LAMINAR_TDQ_UNLOCK(tdq);
			LAMINAR_TDQ_LOCK(tdn);
			{
				int old_lowpri = tdq_add_internal(tdn,
				    td, srqflag);

				tdq_notify(tdn, old_lowpri);
			}
			LAMINAR_TDQ_UNLOCK(tdn);
			LAMINAR_TDQ_LOCK(tdq);
			mtx = LAMINAR_TDQ_LOCKPTR(tdn);
		} else
#endif
			tdq_runq_add(tdq, td, srqflag);
	} else {
		/* Thread is going to sleep. */
		if (mtx != LAMINAR_TDQ_LOCKPTR(tdq)) {
			mtx_unlock_spin(mtx);
			LAMINAR_TDQ_LOCK(tdq);
		}
		tdq_load_rem(tdq, td);
	}

	LAMINAR_TDQ_LOCK_ASSERT(tdq, MA_OWNED | MA_NOTRECURSED);
	MPASS(td == tdq->ltdq_curthread);
	newtd = choosethread();
	LAMINAR_TDQ_UNLOCK(tdq);

	if (td != newtd) {
		td->td_oncpu = NOCPU;
		cpu_switch(td, newtd, mtx);
		td->td_oncpu = PCPU_GET(cpuid);
	} else {
		/* No context switch: just unblock the thread lock. */
		thread_unblock_switch(td, mtx);
	}
	KASSERT(curthread->td_md.md_spinlock_count == 1,
	    ("sched_laminar_sswitch: invalid spinlock count"));
}

static void
sched_laminar_throw(struct thread *td)
{
	struct laminar_tdq *tdq;
	struct thread *newtd;

	tdq = LAMINAR_TDQ_SELF();
	MPASS(td != NULL);
	THREAD_LOCK_ASSERT(td, MA_OWNED);
	THREAD_LOCKPTR_ASSERT(td, LAMINAR_TDQ_LOCKPTR(tdq));

	tdq_load_rem(tdq, td);
	td->td_lastcpu = td->td_oncpu;
	td->td_oncpu = NOCPU;
	thread_lock_block(td);
	newtd = sched_laminar_throw_grab(tdq);
	cpu_switch(td, newtd, LAMINAR_TDQ_LOCKPTR(tdq));   /* no return */
}

static void
sched_laminar_unlend_prio(struct thread *td, u_char prio)
{
	u_char base_pri;

	if (td->td_base_pri >= PRI_MIN_TIMESHARE &&
	    td->td_base_pri <= PRI_MAX_TIMESHARE)
		base_pri = td->td_user_pri;
	else
		base_pri = td->td_base_pri;
	if (prio >= base_pri) {
		td->td_flags &= ~TDF_BORROWING;
		sched_laminar_prio(td, base_pri);
	} else
		sched_laminar_lend_prio(td, prio);
}

static void
sched_laminar_user_prio(struct thread *td, u_char prio)
{

	THREAD_LOCK_ASSERT(td, MA_OWNED);
	td->td_base_user_pri = prio;
	if (td->td_lend_user_pri <= prio)
		return;
	td->td_user_pri = prio;
}

static void
sched_laminar_userret_slowpath(struct thread *td)
{

	thread_lock(td);
	td->td_priority = td->td_user_pri;
	td->td_base_pri = td->td_user_pri;
	thread_unlock(td);
}

/*
 * Run queue manipulation.
 *
 * Placement on enqueue is intentionally simple in this commit: every
 * sched_add lands on the current CPU.  Real cross-CPU placement
 * (sched_pickcpu) is added in A.3e; until then the box runs but the
 * APs only execute work that is spawned on them.  This is enough to
 * reach multiuser.
 */
/*
 * Same-CPU cost-preempt cooldown (R4 fix).  Caps the rate at which
 * a local wake can force the running thread to yield via AST.  Too
 * low = picker thrash + IPI/switch storm (an earlier unguarded
 * attempt cut p50 throughput hard); too high = wake-tail latency
 * approaches a full sched_slice.  Default 1 tick @ hz=100 = 10ms,
 * roughly bounding tail at slice/8.
 */
u_long laminar_local_preempt_cooldown = 1;
u_long laminar_local_preempt_fires = 0;
u_long laminar_clock_calls = 0;
u_long laminar_clock_idle_calls = 0;
u_long laminar_clock_ts_sum = 0;
u_long laminar_idle_exits = 0;	/* idle while loop saw load > 0 and exited */
u_long laminar_idle_loops = 0;	/* total idle loop iterations */
u_long laminar_local_preempt_skip_cool = 0;
u_long laminar_local_preempt_skip_v = 0;

static void
sched_laminar_setpreempt(int pri)
{
	struct thread *ctd;
	struct laminar_tdq *tdq;

	ctd = curthread;
	THREAD_LOCK_ASSERT(ctd, MA_OWNED);
	if (pri < ctd->td_priority) {
		ast_sched_locked(ctd, TDA_SCHED);
		return;
	}
	/*
	 * Cost-based local preempt (R4 follow-up, RLC theme).  The
	 * priority check above doesn't fire for timeshare-vs-timeshare
	 * (equal priority) wakes -- the waker waits up to a full slice
	 * (~94ms) for the incumbent to give up the CPU.  Mirror the
	 * cross-CPU cost-preempt logic locally: if the local runq has
	 * a thread (the just-added waker, typically) with lower
	 * vruntime than ctd, the picker would prefer it; fire the AST
	 * so ctd yields.  Rate-limited by ltdq_last_local_preempt to
	 * cap the switch storm.
	 *
	 * Identified via instrumentation showing 99.99% of wakes take
	 * this same-CPU path (cp_ipi + notify_ipi = 4 out of 2.6M
	 * wakes).  All previous cross-CPU R4 fixes were addressing
	 * the wrong path.
	 */
	if (!laminar_is_timeshare(ctd))
		return;
	tdq = LAMINAR_TDQ_SELF();
	{
		int now = ticks;

		if (now - tdq->ltdq_last_local_preempt <
		    (int)laminar_local_preempt_cooldown) {
			laminar_local_preempt_skip_cool++;
			return;
		}
	}
	{
		uint64_t cv = td_get_sched(ctd)->ts_vruntime;
		uint32_t s, nshards;
		bool found_lower = false;
		/*
		 * R-as-tolerance-band mirrors the cross-CPU gate: a shard
		 * min within R slices below current's vruntime is "lower
		 * enough" to preempt.  R=0 keeps strict fairness.
		 */
		uint64_t r_band = (uint64_t)(atomic_load_int(
		    &tdq->ltdq_resistance) + atomic_load_int(
		    &tdq->ltdq_ctrl_preempt_band)) *
		    (uint64_t)sched_slice * LAMINAR_NICE_0_WEIGHT;
		uint64_t cv_thresh = cv + r_band;
		if (cv_thresh < cv)	/* overflow guard */
			cv_thresh = UINT64_MAX;

		nshards = (tdq->ltdq_ts_n + LAMINAR_SHARD_SIZE - 1) /
		    LAMINAR_SHARD_SIZE;
		for (s = 0; s < nshards; s++) {
			if (tdq->ltdq_shard_min[s] < cv_thresh) {
				found_lower = true;
				break;
			}
		}
		if (!found_lower) {
			laminar_local_preempt_skip_v++;
			return;
		}
	}
	tdq->ltdq_last_local_preempt = ticks;
	laminar_local_preempt_fires++;
	ast_sched_locked(ctd, TDA_SCHED);
}

/*
 * Deadline-lane management (phase I).  Control surface: /dev/laminar
 * ioctls (cdev + ioctl is the Atrium house style; graduates to the
 * broker syscall family with EVFILT_DEADLINE in phase J).  Phase I
 * sponsors the CALLING thread only — the metronome self-sponsors;
 * broker sponsorship-by-tid arrives with frescod integration.
 */
struct lam_lane_req {
	uint64_t q_us;		/* budget per period */
	uint64_t t_us;		/* period */
};
struct lam_lane_stats {
	uint64_t periods;
	uint64_t misses;
	uint64_t throttles;
	uint64_t max_late_us;	/* worst replenish-callout lateness */
	int32_t cpu;
	int32_t pad;
};
/*
 * Broker sponsorship (phase J): a trusted daemon (frescod, lyrad)
 * sponsors a CLIENT thread's deadlines.  anchor_ns (CLOCK_MONOTONIC)
 * phase-aligns the period grid to the hardware fact the deadline
 * derives from (a vblank timestamp, an audio DMA edge); 0 = no anchor.
 */
struct lam_lane_req_for {
	int32_t		pid;
	int32_t		tid;
	uint64_t	q_us, t_us;
	uint64_t	anchor_ns;
};

#define	LAMIOC_SPONSOR	_IOW('L', 1, struct lam_lane_req)
#define	LAMIOC_WITHDRAW	_IO('L', 2)
#define	LAMIOC_YIELD	_IO('L', 3)
#define	LAMIOC_STATS	_IOR('L', 4, struct lam_lane_stats)
#define	LAMIOC_SPONSOR_FOR	_IOW('L', 5, struct lam_lane_req_for)
#define	LAMIOC_WITHDRAW_FOR	_IOW('L', 6, struct lam_lane_req_for)
#define	LAMIOC_ADOPT		_IOW('L', 7, struct lam_lane_req_for)
#define	LAMIOC_DROP		_IO('L', 8)

/*
 * Miss event record, delivered to the owning fd via read(2); EVFILT_READ
 * on the fd fires when the ring is non-empty.  This is the broker's miss
 * feed (plan D6: "miss -> event to the broker"): frescod/lyrad kevent on
 * their broker fd and react by policy (skip a frame, resize a buffer,
 * withdraw a hopeless client).
 */
struct lam_miss_event {
	int32_t		pid, tid;
	uint64_t	periods;	/* entity period count at the miss */
	uint64_t	misses;		/* entity total misses so far */
};

#define	LAM_MISS_RING	64

/*
 * The fd-owner cookie: one per /dev/laminar open that has sponsored
 * anything.  Entities tag their owner via le_priv; the cdevpriv dtor
 * sweeps every lane slot owned by the cookie, so a broker (frescod,
 * lyrad) crashing or closing its fd reclaims ALL its clients' entities.
 *
 * Miss-event path: the replenish callout runs C_DIRECT_EXEC (interrupt
 * context), so it only enqueues into lp_ring under the SPIN lock and
 * fires lp_task; the task, in thread context, does the KNOTE + wakeup.
 * The deadline machinery stays direct; only the notification rides a
 * thread.
 */
struct laminar_lane_priv {
	struct mtx	lp_ringlock;	/* SPIN: ring, shared w/ replenish */
	struct mtx	lp_knlock;	/* DEF: knlist + read sleep */
	struct lam_miss_event lp_ring[LAM_MISS_RING];
	u_int		lp_head, lp_tail;	/* head = next write */
	u_long		lp_dropped;
	struct task	lp_task;
	struct selinfo	lp_sel;
};

static void
laminar_lane_miss_task(void *arg, int pending __unused)
{
	struct laminar_lane_priv *priv = arg;

	mtx_lock(&priv->lp_knlock);
	KNOTE_LOCKED(&priv->lp_sel.si_note, 0);
	wakeup(priv);
	mtx_unlock(&priv->lp_knlock);
}

/*
 * Period replenishment.  Runs as a callout pinned to the entity's CPU;
 * takes the tdq spin lock by hand (callout_init_mtx cannot carry a spin
 * mutex).  Scores the closing period (miss = the thread neither yielded
 * nor was throttled — it is still chewing past its deadline), resets the
 * budget, releases a yielded sleeper, and measures its own lateness —
 * the in-kernel callout-latency instrumentation the wakelat bimodal
 * artifact needs (plan risk R2).
 */
static void
laminar_lane_replenish(void *arg)
{
	struct laminar_lane_entity *le = arg;
	struct laminar_tdq *tdq = LAMINAR_TDQ_CPU(le->le_cpu);
	struct laminar_lane_priv *notify = NULL;
	sbintime_t now, late;
	bool was_yielded;

	LAMINAR_TDQ_LOCK(tdq);
	if (le->le_td == NULL) {	/* withdrawn while in flight */
		LAMINAR_TDQ_UNLOCK(tdq);
		return;
	}
	now = sbinuptime();
	late = now - le->le_deadline;
	if (late > 0) {
		uint64_t late_us = ((uint64_t)late * 1000000ULL) >> 32;

		if (late_us > le->le_max_late_us)
			le->le_max_late_us = late_us;
	}
	le->le_periods++;
	if (!le->le_yielded && !le->le_throttled) {
		le->le_misses++;
		/*
		 * Record the miss for the owning fd's feed.  Only the ring
		 * write happens here — the taskqueue enqueue is deferred
		 * past the tdq unlock below, because enqueue wakes the
		 * taskqueue swi and that wake re-enters sched_add (panic:
		 * "setcpu cross-CPU recursion" — learned the hard way).
		 */
		notify = le->le_priv;
		if (notify != NULL) {
			mtx_lock_spin(&notify->lp_ringlock);
			if (notify->lp_head - notify->lp_tail <
			    LAM_MISS_RING) {
				struct lam_miss_event *ev =
				    &notify->lp_ring[notify->lp_head %
				    LAM_MISS_RING];
				ev->pid = le->le_td->td_proc->p_pid;
				ev->tid = le->le_td->td_tid;
				ev->periods = le->le_periods;
				ev->misses = le->le_misses;
				notify->lp_head++;
			} else
				notify->lp_dropped++;
			mtx_unlock_spin(&notify->lp_ringlock);
		}
	}
	was_yielded = le->le_yielded;
	le->le_yielded = false;
	le->le_throttled = false;
	le->le_budget_us = le->le_q_us;
	le->le_deadline += le->le_t_sbt;
	callout_reset_sbt_on(&le->le_callout, le->le_deadline, 0,
	    laminar_lane_replenish, le, le->le_cpu, C_ABSOLUTE | C_DIRECT_EXEC);
	LAMINAR_TDQ_UNLOCK(tdq);
	if (was_yielded)
		wakeup(le);
	if (notify != NULL)
		taskqueue_enqueue(taskqueue_fast, &notify->lp_task);
}


/*
 * Sponsor `target` as a lane entity owned by `priv`; picks the least
 * lane-utilized admissible CPU.  anchor_ns != 0 aligns the period grid
 * to that CLOCK_MONOTONIC instant (a vblank timestamp): the first
 * deadline is the next grid point of anchor + k*T, so replenishment
 * ticks in phase with the hardware the deadline derives from.
 */
static int
laminar_lane_sponsor_td(struct thread *target, uint64_t q_us, uint64_t t_us,
    uint64_t anchor_ns, void *priv)
{
	struct laminar_tdq *tdq;
	struct laminar_lane_entity *le = NULL;
	sbintime_t now, first, t_sbt;
	int cpu, best_cpu = -1, best_util = INT_MAX, util, i;

	if (!laminar_deadline_enable)
		return (ENXIO);
	if (q_us == 0 || t_us < 1000 || t_us > 1000000 || q_us >= t_us)
		return (EINVAL);
	if (td_get_sched(target)->ts_lane != NULL)
		return (EBUSY);
	util = (int)((q_us * 1000) / t_us);	/* per-mille */

	CPU_FOREACH(cpu) {
		int u = LAMINAR_TDQ_CPU(cpu)->ltdq_lane_util;

		if (u + util <= laminar_deadline_util_max && u < best_util) {
			best_util = u;
			best_cpu = cpu;
		}
	}
	if (best_cpu < 0)
		return (ENOSPC);	/* admission: no CPU has room */

	/*
	 * No sched_bind: it pins (td_pinned) and a user thread cannot
	 * return to userspace pinned ("userret: Returning with pinned
	 * thread" — learned by panic).  Placement is forced instead in
	 * sched_laminar_pickcpu (active lane entity → le_cpu), which
	 * covers the dominant wake path; a rare balancer migration
	 * self-corrects at the next wake.
	 */
	t_sbt = (sbintime_t)t_us * SBT_1US;
	now = sbinuptime();
	if (anchor_ns != 0) {
		sbintime_t asbt = nstosbt((int64_t)anchor_ns);

		if (asbt <= now)
			first = asbt + ((now - asbt) / t_sbt + 1) * t_sbt;
		else
			first = asbt;
	} else
		first = now + t_sbt;

	tdq = LAMINAR_TDQ_CPU(best_cpu);
	LAMINAR_TDQ_LOCK(tdq);
	for (i = 0; i < LAMINAR_LANE_MAX; i++) {
		if (tdq->ltdq_lane[i].le_td == NULL) {
			le = &tdq->ltdq_lane[i];
			break;
		}
	}
	if (le == NULL) {
		LAMINAR_TDQ_UNLOCK(tdq);
		return (ENOSPC);
	}
	le->le_cpu = best_cpu;
	le->le_q_us = q_us;
	le->le_t_us = t_us;
	le->le_t_sbt = t_sbt;
	le->le_budget_us = q_us;
	le->le_throttled = false;
	le->le_yielded = false;
	le->le_periods = le->le_misses = le->le_throttles = 0;
	le->le_max_late_us = 0;
	le->le_deadline = first;
	le->le_priv = priv;
	le->le_saved_upri = 0;
	le->le_banded = false;
	le->le_td = target;
	td_get_sched(target)->ts_lane = le;
	tdq->ltdq_lane_n++;
	tdq->ltdq_lane_util += util;
	callout_reset_sbt_on(&le->le_callout, le->le_deadline, 0,
	    laminar_lane_replenish, le, best_cpu, C_ABSOLUTE | C_DIRECT_EXEC);
	LAMINAR_TDQ_UNLOCK(tdq);
	laminar_lane_band_apply(target, le);	/* K-a inheritance band */
	laminar_lane_sponsors++;
	return (0);
}

/*
 * K-a deadline inheritance — the turnstile BAND (plan D11 stage K-a).
 *
 * A sponsored, in-budget lane entity surfaces at LANE_BAND priority
 * (top of timeshare).  Inheritance then needs NO new propagation
 * machinery: when a lane thread blocks on a kernel lock or a
 * PTHREAD_PRIO_INHERIT umtx, the EXISTING turnstile / umtx PI lends
 * the band to the holder, who then outranks every undeclared thread,
 * runs promptly, and releases.  EDF dispatch is untouched: the band is
 * still timeshare-class, so the RT path does not pick lane threads —
 * the lane EDF scan still orders them by real deadline.
 *
 * Known K-a gap (documented, fixed by K-b accounting): a THROTTLED
 * entity keeps the band until its next replenish (<= T away) — the
 * statclock detector cannot take the remote thread lock to demote.
 */
/*
 * The band must be BELOW PRI_MIN_TIMESHARE: laminar_is_timeshare routes
 * priority >= PRI_MIN_TIMESHARE into the vruntime-ordered SoA, where
 * lent priority is INVISIBLE (WFQ picks by vruntime); < PRI_MIN_TIMESHARE
 * routes to the priority-bucket runq that tdq_choose's rt-path picks
 * first.  Band = top of the kernel range, so a PI-boosted holder wins
 * SELECTION, not just preemption checks.  (First attempt used
 * PRI_MIN_TIMESHARE itself: the boosted holder stayed in WFQ and
 * inheritance did nothing — measured as PI performing WORSE than the
 * plain-mutex control.)
 */
#define	LAMINAR_LANE_BAND	(PRI_MIN_TIMESHARE - 1)

static void
laminar_lane_band_apply(struct thread *td, struct laminar_lane_entity *le)
{

	thread_lock(td);
	le->le_saved_upri = td->td_base_user_pri;
	le->le_banded = true;
	if (td->td_base_user_pri > LAMINAR_LANE_BAND) {
		td->td_base_user_pri = LAMINAR_LANE_BAND;
		if (td->td_user_pri > LAMINAR_LANE_BAND)
			td->td_user_pri = LAMINAR_LANE_BAND;
	}
	thread_unlock(td);
}

/*
 * Restore the pre-band user priority after teardown.  The thread may
 * be gone (client died — nothing to restore) or re-sponsored (a newer
 * entity owns its priority now; leave it).
 */
static void
laminar_lane_band_restore(pid_t pid, lwpid_t tid, u_char saved)
{
	struct thread *td;

	td = tdfind(tid, pid);
	if (td == NULL)
		return;
	if (td_get_sched(td)->ts_lane == NULL) {
		thread_lock(td);
		if (td->td_base_user_pri == LAMINAR_LANE_BAND)
			td->td_base_user_pri = saved;
		if (td->td_user_pri == LAMINAR_LANE_BAND &&
		    td->td_lend_user_pri > saved)
			td->td_user_pri = saved;
		thread_unlock(td);
	}
	PROC_UNLOCK(td->td_proc);
}

/*
 * Get (or create) the calling fd's owner cookie.  Returns NULL only on
 * cdevpriv failure.
 */
static struct laminar_lane_priv *
laminar_lane_get_priv(void)
{
	struct laminar_lane_priv *priv;

	if (devfs_get_cdevpriv((void **)&priv) == 0)
		return (priv);
	priv = malloc(sizeof(*priv), M_LAMINAR, M_WAITOK | M_ZERO);
	mtx_init(&priv->lp_ringlock, "lam_ring", NULL, MTX_SPIN);
	mtx_init(&priv->lp_knlock, "lam_knl", NULL, MTX_DEF);
	TASK_INIT(&priv->lp_task, 0, laminar_lane_miss_task, priv);
	knlist_init_mtx(&priv->lp_sel.si_note, &priv->lp_knlock);
	if (devfs_set_cdevpriv(priv, laminar_lane_priv_dtor) != 0) {
		taskqueue_drain(taskqueue_fast, &priv->lp_task);
		knlist_destroy(&priv->lp_sel.si_note);
		mtx_destroy(&priv->lp_ringlock);
		mtx_destroy(&priv->lp_knlock);
		free(priv, M_LAMINAR);
		return (NULL);
	}
	return (priv);
}

/*
 * Entity teardown.  Idempotent; safe against the direct-exec replenish
 * (le_td cleared under the tdq lock, then the callout drained).
 */
static void
laminar_lane_teardown_common(struct laminar_lane_entity *le,
    bool restore_prio)
{
	struct laminar_tdq *tdq = LAMINAR_TDQ_CPU(le->le_cpu);
	struct thread *otd;
	pid_t opid = 0;
	lwpid_t otid = 0;
	u_char saved = 0;
	bool banded = false;
	int util;

	LAMINAR_TDQ_LOCK(tdq);
	otd = le->le_td;
	if (otd == NULL) {		/* already torn down */
		LAMINAR_TDQ_UNLOCK(tdq);
		return;
	}
	util = (int)((le->le_q_us * 1000) / le->le_t_us);
	if (restore_prio && le->le_banded) {
		/* capture identity while otd is still pinned by le_td */
		opid = otd->td_proc->p_pid;
		otid = otd->td_tid;
		saved = le->le_saved_upri;
		banded = true;
	}
	le->le_td = NULL;	/* pick + replenish stop touching it */
	le->le_priv = NULL;
	le->le_banded = false;
	le->le_gen++;		/* invalidates K-b adopters */
	if (td_get_sched(otd)->ts_lane == le)
		td_get_sched(otd)->ts_lane = NULL;
	tdq->ltdq_lane_n--;
	tdq->ltdq_lane_util -= util;
	LAMINAR_TDQ_UNLOCK(tdq);
	callout_drain(&le->le_callout);
	if (banded)
		laminar_lane_band_restore(opid, otid, saved);
}

static void
laminar_lane_teardown(struct laminar_lane_entity *le)
{

	laminar_lane_teardown_common(le, true);
}

/*
 * cdevpriv destructor: sweep every lane slot owned by this fd's cookie.
 * A process that dies (or closes the fd) without WITHDRAW cannot leave
 * a stale entity whose le_td points at a freed thread (learned by a
 * use-after-free panic in the lane pick: "ltdq_slot[i] == td failed");
 * a BROKER dying reclaims all of its clients' entities at once.
 */
static void
laminar_lane_priv_dtor(void *data)
{
	struct laminar_lane_priv *priv = data;
	struct laminar_tdq *tdq;
	int cpu, i;

	CPU_FOREACH(cpu) {
		tdq = LAMINAR_TDQ_CPU(cpu);
		for (i = 0; i < LAMINAR_LANE_MAX; i++) {
			if (tdq->ltdq_lane[i].le_priv == priv)
				laminar_lane_teardown(&tdq->ltdq_lane[i]);
		}
	}
	/* all owned callouts drained above; no new enqueues possible */
	taskqueue_drain(taskqueue_fast, &priv->lp_task);
	knlist_clear(&priv->lp_sel.si_note, 0);
	knlist_destroy(&priv->lp_sel.si_note);
	mtx_destroy(&priv->lp_ringlock);
	mtx_destroy(&priv->lp_knlock);
	free(priv, M_LAMINAR);
}

/*
 * Thread-exit invalidation: a BROKER-sponsored client thread can die
 * with no fd close tied to its lifetime, so the thread teardown path
 * must reclaim its entity.  thread_dtor runs in sleepable context with
 * the (type-stable) thread struct still intact.
 */
static void
laminar_lane_thread_dtor(void *arg __unused, struct thread *td)
{
	struct laminar_lane_entity *le = td_get_sched(td)->ts_lane;

	if (__predict_false(le != NULL) && le->le_td == td)
		laminar_lane_teardown_common(le, false);
	td_get_sched(td)->ts_adopted = NULL;	/* K-b: dying adopter */
}

static int
laminar_lane_withdraw(struct thread *td)
{
	struct laminar_lane_entity *le = td_get_sched(td)->ts_lane;

	if (le == NULL)
		return (ENOENT);
	laminar_lane_teardown(le);
	return (0);
}

/*
 * Done for this period: sleep until the replenishment callout.  The
 * absolute-deadline tsleep_sbt makes the wakeup race harmless — if the
 * callout's wakeup() slips between unlock and sleep, the sleep still
 * expires at the same instant the deadline passes.
 */
static int
laminar_lane_yield(struct thread *td)
{
	struct laminar_lane_entity *le = td_get_sched(td)->ts_lane;
	struct laminar_tdq *tdq;
	sbintime_t deadline;

	if (le == NULL || le->le_td != td)
		return (ENOENT);
	tdq = LAMINAR_TDQ_CPU(le->le_cpu);
	LAMINAR_TDQ_LOCK(tdq);
	le->le_yielded = true;
	deadline = le->le_deadline;
	LAMINAR_TDQ_UNLOCK(tdq);
	/*
	 * Backstop strictly PAST the deadline (T/8 late) so the replenish
	 * callout's wakeup() — which has already cleared le_yielded and
	 * reset the budget — is the normal waker; the timeout only saves
	 * a lost wakeup.
	 */
	(void)tsleep_sbt(le, 0, "lane", deadline + (le->le_t_sbt >> 3), 0,
	    C_ABSOLUTE);
	return (0);
}

/*
 * K-b deadline lending: the caller (an Aqueduct server thread working
 * on a lane client's request) ADOPTs the client's entity — it gains
 * the K-a band for SELECTION, and its runtime is CHARGED to the
 * client's CBS budget (see the charge path in sched_laminar_choose and
 * the overrun detector in sched_laminar_clock).  Band priority is
 * never free: it always burns an admitted budget, so adoption cannot
 * out-schedule the admission cap.  Gated like SPONSOR_FOR (D9 broker
 * capability + p_cansee).  le_gen guards entity-slot reuse.
 */
static int
laminar_lane_adopt(struct thread *td, pid_t pid, lwpid_t tid)
{
	struct laminar_lane_entity *le;
	struct thread *target;

	if (td_get_sched(td)->ts_lane != NULL ||
	    td_get_sched(td)->ts_adopted != NULL)
		return (EBUSY);
	target = tdfind(tid, pid);
	if (target == NULL)
		return (ESRCH);
	if (p_cansee(td, target->td_proc) != 0) {
		PROC_UNLOCK(target->td_proc);
		return (ESRCH);
	}
	le = td_get_sched(target)->ts_lane;
	if (le == NULL || le->le_td != target) {
		PROC_UNLOCK(target->td_proc);
		return (ENOENT);
	}
	td_get_sched(td)->ts_adopted = le;
	td_get_sched(td)->ts_adopted_gen = le->le_gen;
	/* the adopter is on-cpu NOW — charging starts here, not at the
	 * next pick (a band-priority burn may never switch under load) */
	td_get_sched(td)->ts_adopt_start = sbinuptime();
	PROC_UNLOCK(target->td_proc);

	thread_lock(td);
	td_get_sched(td)->ts_adopt_upri = td->td_base_user_pri;
	if (td->td_base_user_pri > LAMINAR_LANE_BAND) {
		td->td_base_user_pri = LAMINAR_LANE_BAND;
		if (td->td_user_pri > LAMINAR_LANE_BAND)
			td->td_user_pri = LAMINAR_LANE_BAND;
	}
	thread_unlock(td);
	return (0);
}

static int
laminar_lane_drop(struct thread *td)
{
	struct td_sched *ts = td_get_sched(td);

	if (ts->ts_adopted == NULL)
		return (ENOENT);
	ts->ts_adopted = NULL;
	thread_lock(td);
	if (td->td_base_user_pri == LAMINAR_LANE_BAND)
		td->td_base_user_pri = ts->ts_adopt_upri;
	if (td->td_user_pri == LAMINAR_LANE_BAND &&
	    td->td_lend_user_pri > ts->ts_adopt_upri)
		td->td_user_pri = ts->ts_adopt_upri;
	td->td_priority = td->td_user_pri;
	thread_unlock(td);
	return (0);
}

static int
laminar_lane_ioctl(struct cdev *dev, u_long cmd, caddr_t data, int fflag,
    struct thread *td)
{
	switch (cmd) {
	case LAMIOC_SPONSOR: {
		struct lam_lane_req *r = (struct lam_lane_req *)data;
		struct laminar_lane_priv *priv = laminar_lane_get_priv();

		if (priv == NULL)
			return (EBUSY);
		return (laminar_lane_sponsor_td(td, r->q_us, r->t_us, 0,
		    priv));
	}
	case LAMIOC_SPONSOR_FOR: {
		struct lam_lane_req_for *r = (struct lam_lane_req_for *)data;
		struct laminar_lane_priv *priv;
		struct thread *target;
		int error;

		/*
		 * Broker permission (D9): on the host, root (priv_check);
		 * inside a jail, the jail must hold the deadline_broker
		 * capability — granted by portcullisd from the manifest
		 * via kern.sched.deadline_brokers, never self-grantable.
		 */
		if (td->td_ucred->cr_prison == &prison0) {
			error = priv_check(td, PRIV_SCHED_RTPRIO);
			if (error != 0)
				return (error);
		} else {
			struct laminar_prison *lpr = laminar_prison_lookup(
			    td->td_ucred->cr_prison->pr_id);

			if (lpr == NULL ||
			    atomic_load_int(&lpr->lpr_broker) == 0)
				return (EPERM);
		}
		priv = laminar_lane_get_priv();
		if (priv == NULL)
			return (EBUSY);
		target = tdfind(r->tid, r->pid);
		if (target == NULL)
			return (ESRCH);
		/* tdfind returns with the target's proc locked, which
		 * also holds off thread_exit until we are published.
		 * Jail visibility: a broker may only sponsor threads it
		 * can see (its own jail's clients, or anyone for host). */
		error = p_cansee(td, target->td_proc);
		if (error != 0) {
			PROC_UNLOCK(target->td_proc);
			return (error);
		}
		error = laminar_lane_sponsor_td(target, r->q_us, r->t_us,
		    r->anchor_ns, priv);
		PROC_UNLOCK(target->td_proc);
		return (error);
	}
	case LAMIOC_WITHDRAW_FOR: {
		struct lam_lane_req_for *r = (struct lam_lane_req_for *)data;
		struct laminar_lane_priv *priv;
		struct laminar_lane_entity *le;
		struct thread *target;

		if (devfs_get_cdevpriv((void **)&priv) != 0)
			return (ENOENT);
		target = tdfind(r->tid, r->pid);
		if (target == NULL)
			return (ESRCH);
		le = td_get_sched(target)->ts_lane;
		if (le == NULL || le->le_priv != priv) {
			PROC_UNLOCK(target->td_proc);
			return (le == NULL ? ENOENT : EPERM);
		}
		PROC_UNLOCK(target->td_proc);
		laminar_lane_teardown(le);
		return (0);
	}
	case LAMIOC_ADOPT: {
		struct lam_lane_req_for *r = (struct lam_lane_req_for *)data;
		int error;

		/* same permission shape as SPONSOR_FOR (D9) */
		if (td->td_ucred->cr_prison == &prison0) {
			error = priv_check(td, PRIV_SCHED_RTPRIO);
			if (error != 0)
				return (error);
		} else {
			struct laminar_prison *lpr = laminar_prison_lookup(
			    td->td_ucred->cr_prison->pr_id);

			if (lpr == NULL ||
			    atomic_load_int(&lpr->lpr_broker) == 0)
				return (EPERM);
		}
		return (laminar_lane_adopt(td, r->pid, r->tid));
	}
	case LAMIOC_DROP:
		return (laminar_lane_drop(td));
	case LAMIOC_WITHDRAW:
		return (laminar_lane_withdraw(td));
	case LAMIOC_YIELD:
		return (laminar_lane_yield(td));
	case LAMIOC_STATS: {
		struct lam_lane_stats *s = (struct lam_lane_stats *)data;
		struct laminar_lane_entity *le = td_get_sched(td)->ts_lane;
		struct laminar_tdq *tdq;

		if (le == NULL)
			return (ENOENT);
		tdq = LAMINAR_TDQ_CPU(le->le_cpu);
		LAMINAR_TDQ_LOCK(tdq);
		s->periods = le->le_periods;
		s->misses = le->le_misses;
		s->throttles = le->le_throttles;
		s->max_late_us = le->le_max_late_us;
		s->cpu = le->le_cpu;
		s->pad = 0;
		LAMINAR_TDQ_UNLOCK(tdq);
		return (0);
	}
	default:
		return (ENOTTY);
	}
}

/*
 * read(2) on the fd drains pending miss events (struct lam_miss_event
 * records).  Blocks unless O_NONBLOCK; EVFILT_READ readiness matches.
 */
static int
laminar_lane_read(struct cdev *dev, struct uio *uio, int ioflag)
{
	struct laminar_lane_priv *priv;
	struct lam_miss_event ev;
	int error = 0, copied = 0;

	if (devfs_get_cdevpriv((void **)&priv) != 0)
		return (ENXIO);
	if (uio->uio_resid < (ssize_t)sizeof(ev))
		return (EINVAL);
	for (;;) {
		bool have = false;

		mtx_lock_spin(&priv->lp_ringlock);
		if (priv->lp_tail != priv->lp_head) {
			ev = priv->lp_ring[priv->lp_tail % LAM_MISS_RING];
			priv->lp_tail++;
			have = true;
		}
		mtx_unlock_spin(&priv->lp_ringlock);
		if (have) {
			error = uiomove(&ev, sizeof(ev), uio);
			if (error != 0)
				return (error);
			copied++;
			if (uio->uio_resid < (ssize_t)sizeof(ev))
				return (0);
			continue;
		}
		if (copied > 0)
			return (0);
		if ((ioflag & O_NONBLOCK) != 0)
			return (EWOULDBLOCK);
		mtx_lock(&priv->lp_knlock);
		/* re-check under the sleep lock to not miss a wakeup */
		mtx_lock_spin(&priv->lp_ringlock);
		have = (priv->lp_tail != priv->lp_head);
		mtx_unlock_spin(&priv->lp_ringlock);
		if (have) {
			mtx_unlock(&priv->lp_knlock);
			continue;
		}
		error = msleep(priv, &priv->lp_knlock, PCATCH, "lammiss", 0);
		mtx_unlock(&priv->lp_knlock);
		if (error != 0)
			return (error);
	}
}

static void
filt_laminar_detach(struct knote *kn)
{
	struct laminar_lane_priv *priv = kn->kn_hook;

	knlist_remove(&priv->lp_sel.si_note, kn, 0);
}

static int
filt_laminar_read(struct knote *kn, long hint __unused)
{
	struct laminar_lane_priv *priv = kn->kn_hook;
	u_int n;

	mtx_lock_spin(&priv->lp_ringlock);
	n = priv->lp_head - priv->lp_tail;
	mtx_unlock_spin(&priv->lp_ringlock);
	kn->kn_data = (int64_t)n * sizeof(struct lam_miss_event);
	return (n > 0);
}

static const struct filterops laminar_read_filtops = {
	.f_isfd = 1,
	.f_detach = filt_laminar_detach,
	.f_event = filt_laminar_read,
};

static int
laminar_lane_kqfilter(struct cdev *dev, struct knote *kn)
{
	struct laminar_lane_priv *priv;

	if (kn->kn_filter != EVFILT_READ)
		return (EINVAL);
	priv = laminar_lane_get_priv();
	if (priv == NULL)
		return (EBUSY);
	kn->kn_fop = &laminar_read_filtops;
	kn->kn_hook = priv;
	knlist_add(&priv->lp_sel.si_note, kn, 0);
	return (0);
}

static struct cdevsw laminar_lane_cdevsw = {
	.d_version =	D_VERSION,
	.d_read =	laminar_lane_read,
	.d_ioctl =	laminar_lane_ioctl,
	.d_kqfilter =	laminar_lane_kqfilter,
	.d_name =	"laminar",
};

static void
laminar_lane_dev_init(void *arg __unused)
{
	struct make_dev_args mda;
	struct cdev *dev;

	make_dev_args_init(&mda);
	mda.mda_devsw = &laminar_lane_cdevsw;
	mda.mda_uid = UID_ROOT;
	mda.mda_gid = GID_WHEEL;
	mda.mda_mode = 0600;
	(void)make_dev_s(&mda, &dev, "laminar");
	EVENTHANDLER_REGISTER(thread_dtor, laminar_lane_thread_dtor, NULL,
	    EVENTHANDLER_PRI_ANY);
}
SYSINIT(laminar_lane_dev, SI_SUB_DRIVERS, SI_ORDER_ANY,
    laminar_lane_dev_init, NULL);

static void
sched_laminar_add(struct thread *td, int flags)
{
	struct laminar_tdq *tdq;
	int lowpri;
#ifdef SMP
	int cpu;
#endif

	THREAD_LOCK_ASSERT(td, MA_OWNED);

#ifdef SMP
	/*
	 * Pick a target CPU and acquire its tdq lock, migrating the
	 * thread lock if needed.  sched_laminar_setcpu returns with that
	 * tdq's lock held and td_lock pointed at it.
	 */
	cpu = sched_laminar_pickcpu(td, flags);
	tdq = sched_laminar_setcpu(td, cpu, flags);
	lowpri = tdq_add_internal(tdq, td, flags);
	if (cpu != PCPU_GET(cpuid)) {
		/*
		 * Cost-based preempt (R4 follow-up, RLC-shaped).  The
		 * priority-based tdq_notify check never fires for
		 * timeshare-vs-timeshare wakeups (same priority), so a
		 * waker on a busy CPU waits up to a slice (often longer
		 * under heavy load) before being picked.  Decide via
		 * vruntime (the picker's actual cost metric): if the
		 * just-enqueued timeshare waker's vruntime is at or below
		 * the dst CPU's floor, it would be the picker's next
		 * choice -- send the preempt IPI directly so the
		 * incumbent yields and sched_choose picks us.
		 *
		 * Rate-limited per dst CPU by laminar_preempt_cooldown
		 * (ticks) to cap the IPI/switch storm that an uncapped
		 * preempt produced in an earlier ULE-style boost
		 * experiment (~3s max latency under 4 watchers each
		 * waking 40k/s).  Cooldown is the RLC knob.
		 *
		 * Falls through to tdq_notify (priority-based path) for
		 * non-timeshare and for the cooldown-blocked case.
		 */
		bool sent_cost_ipi = false;
		/*
		 * Deadline-lane wake: an in-budget lane entity's wake must
		 * preempt a timeshare incumbent NOW — its deadline is real;
		 * the vruntime gate and the cooldown do not apply (this is
		 * the wake-preempt the 15.39ms p99 plateau measured the
		 * absence of, scoped to the lane).
		 */
		/*
		 * NOTE: deliberately NOT gated on !le_yielded — the YIELD
		 * sleep's backstop timeout and the replenish callout expire
		 * at the same instant, and when the timeout path makes the
		 * thread runnable first le_yielded is still true.  A waking
		 * lane thread at period start must preempt either way
		 * (found as "lane_preempts stuck at 1" + alternating
		 * metronome misses).
		 */
		bool lane_wake = __predict_false(laminar_deadline_enable) &&
		    td_get_sched(td)->ts_lane != NULL &&
		    td_get_sched(td)->ts_lane->le_td == td &&
		    !td_get_sched(td)->ts_lane->le_throttled;
		if (laminar_is_timeshare(td) &&
		    (flags & SRQ_YIELDING) == 0) {
			if (tdq->ltdq_curthread == NULL ||
			    TD_IS_IDLETHREAD(tdq->ltdq_curthread)) {
				laminar_cp_skip_idle++;
			} else {
				uint64_t floor =
				    atomic_load_64(&tdq->ltdq_vtime);
				uint64_t v = td_get_sched(td)->ts_vruntime;
				/*
				 * R-as-tolerance-band: extend the strict
				 * fairness gate by R slices' worth of
				 * vruntime.  R=0 (default) keeps current
				 * strict semantics; R>0 lets near-equal-cost
				 * wakees preempt, shrinking the effective
				 * slice quantum.  Controller can drive R on
				 * load_pct -- RLC-shaped, no slice_min special
				 * case.  Unit: 1 stathz tick of nice-0
				 * runtime = LAMINAR_NICE_0_WEIGHT^2.
				 */
				uint64_t r_band = (uint64_t)(atomic_load_int(
				    &tdq->ltdq_resistance) +
				    atomic_load_int(
				    &tdq->ltdq_ctrl_preempt_band)) *
				    (uint64_t)sched_slice *
				    LAMINAR_NICE_0_WEIGHT;
				int now = ticks;

				if (!lane_wake && v > floor + r_band) {
					laminar_cp_skip_v++;
				} else if (!lane_wake &&
				    now - tdq->ltdq_last_preempt <
				    (int)laminar_preempt_cooldown) {
					laminar_cp_skip_cool++;
				} else if (tdq->ltdq_owepreempt) {
					laminar_cp_skip_owe++;
				} else {
					tdq->ltdq_last_preempt = now;
					tdq->ltdq_owepreempt = 1;
					atomic_thread_fence_seq_cst();
					ipi_cpu(cpu, IPI_PREEMPT);
					laminar_cp_ipi++;
					if (lane_wake)
						laminar_lane_preempts++;
					sent_cost_ipi = true;
				}
			}
		}
		if (!sent_cost_ipi)
			tdq_notify(tdq, lowpri);
	} else if ((flags & SRQ_YIELDING) == 0) {
		/*
		 * Local lane wake (the common case: the replenishment
		 * callout is pinned to the entity's CPU): force the AST on
		 * a timeshare incumbent so the lane pick happens at the
		 * next switch, not the next slice end.
		 */
		if (__predict_false(laminar_deadline_enable) &&
		    td_get_sched(td)->ts_lane != NULL &&
		    td_get_sched(td)->ts_lane->le_td == td &&
		    !td_get_sched(td)->ts_lane->le_throttled) {
			struct laminar_lane_entity *wle =
			    td_get_sched(td)->ts_lane;
			struct thread *ctd = tdq->ltdq_curthread;

			/*
			 * K-a: restore the inheritance band after a
			 * throttle demotion (clean period, waking on the
			 * grid; we hold the thread lock here).
			 */
			if (wle->le_banded &&
			    td->td_base_user_pri > LAMINAR_LANE_BAND) {
				td->td_base_user_pri = LAMINAR_LANE_BAND;
				td->td_user_pri = LAMINAR_LANE_BAND;
			}
			laminar_lane_wakes++;
			if (ctd != NULL && !TD_IS_IDLETHREAD(ctd) &&
			    (laminar_is_timeshare(ctd) ||
			    ctd->td_priority == LAMINAR_LANE_BAND)) {
				/*
				 * Band-priority incumbents (adopted
				 * servers, PI-boosted holders) are
				 * preemptible by a lane wake too —
				 * re-choose orders by deadline.
				 */
				ast_sched_locked(ctd, TDA_SCHED);
				laminar_lane_preempts++;
			} else if (ctd != NULL && !TD_IS_IDLETHREAD(ctd))
				laminar_lane_wake_blocked++;
		}
		sched_laminar_setpreempt(td->td_priority);
	}
#else
	tdq = LAMINAR_TDQ_SELF();
	if (td->td_lock != LAMINAR_TDQ_LOCKPTR(tdq)) {
		LAMINAR_TDQ_LOCK(tdq);
		if ((flags & SRQ_HOLD) != 0)
			td->td_lock = LAMINAR_TDQ_LOCKPTR(tdq);
		else
			thread_lock_set(td, LAMINAR_TDQ_LOCKPTR(tdq));
	}
	lowpri = tdq_add_internal(tdq, td, flags);
	if ((flags & SRQ_YIELDING) == 0)
		sched_laminar_setpreempt(td->td_priority);
#endif
	if ((flags & SRQ_HOLDTD) == 0)
		thread_unlock(td);
	(void)lowpri;
}

static void
sched_laminar_rem(struct thread *td)
{
	struct laminar_tdq *tdq;

	tdq = LAMINAR_TDQ_CPU(td_get_sched(td)->ts_cpu);
	LAMINAR_TDQ_LOCK_ASSERT(tdq, MA_OWNED);
	MPASS(td->td_lock == LAMINAR_TDQ_LOCKPTR(tdq));
	KASSERT(TD_ON_RUNQ(td),
	    ("sched_laminar_rem: thread not on run queue"));
	tdq_runq_rem(tdq, td);
	tdq_load_rem(tdq, td);
	TD_SET_CAN_RUN(td);
	if (td->td_priority == tdq->ltdq_lowpri)
		tdq->ltdq_lowpri = PRI_MAX_IDLE;
}

static struct thread *
sched_laminar_choose(void)
{
	struct laminar_tdq *tdq;
	struct thread *td;

	tdq = LAMINAR_TDQ_SELF();
	LAMINAR_TDQ_LOCK_ASSERT(tdq, MA_OWNED);
	laminar_choose_calls++;
	/* Lane budget: charge the outgoing thread its actual on-cpu time. */
	if (__predict_false(laminar_deadline_enable) &&
	    tdq->ltdq_curthread != NULL) {
		struct td_sched *ots = td_get_sched(tdq->ltdq_curthread);
		struct laminar_lane_entity *ole = ots->ts_lane;

		if (ole != NULL && ole->le_td == tdq->ltdq_curthread &&
		    ole->le_run_start != 0) {
			sbintime_t ran = sbinuptime() - ole->le_run_start;

			ole->le_budget_us -= (int64_t)
			    (((uint64_t)ran * 1000000ULL) >> 32);
			ole->le_run_start = 0;
		}
		/*
		 * K-b charge-back: an ADOPTED server thread burns the
		 * client entity's budget for its on-cpu time.  le_gen
		 * guards slot reuse; cross-CPU budget write races with
		 * the entity's own CPU are tolerable (the budget is a
		 * throttling signal, not an invariant).
		 */
		ole = ots->ts_adopted;
		if (ole != NULL && ots->ts_adopt_start != 0) {
			sbintime_t ran = sbinuptime() - ots->ts_adopt_start;

			if (ole->le_gen == ots->ts_adopted_gen &&
			    ole->le_td != NULL)
				ole->le_budget_us -= (int64_t)
				    (((uint64_t)ran * 1000000ULL) >> 32);
			ots->ts_adopt_start = 0;
		}
	}
	td = tdq_choose(tdq);
	if (td != NULL) {
		struct td_sched *ts = td_get_sched(td);

		/* K-b: adopted server going on-cpu burns the client's
		 * budget from here (charged at the next switch). */
		if (__predict_false(ts->ts_adopted != NULL))
			ts->ts_adopt_start = sbinuptime();

		/* R4 instrumentation: wake_ts -> on_cpu delay. */
		if (ts->ts_wake_ts != 0) {
			sbintime_t now = sbinuptime();
			sbintime_t delta = now - ts->ts_wake_ts;
			/* sbintime: seconds<<32 | fraction.  *1e6>>32 = us. */
			uint64_t delay_us = delta < 0 ? 0 : (uint64_t)
			    (((uint64_t)delta * 1000000ULL) >> 32);

			laminar_wake_picks++;
			if (delay_us > laminar_wake_pick_max_us) {
				laminar_wake_pick_max_us = delay_us;
				strlcpy(laminar_wake_pick_max_comm,
				    td->td_proc->p_comm,
				    sizeof(laminar_wake_pick_max_comm));
			}
			/* Also measure queue-to-pick (excludes wake->queue). */
			if (ts->ts_queue_ts != 0) {
				sbintime_t qdelta = now - ts->ts_queue_ts;
				uint64_t qus = qdelta < 0 ? 0 : (uint64_t)
				    (((uint64_t)qdelta * 1000000ULL) >> 32);
				if (qus > laminar_queue_pick_max_us) {
					laminar_queue_pick_max_us = qus;
					strlcpy(laminar_queue_pick_max_comm,
					    td->td_proc->p_comm,
					    sizeof(laminar_queue_pick_max_comm));
				}
			}
			ts->ts_queue_ts = 0;
			if (delay_us > laminar_wake_pick_long_threshold) {
				int idx;
				const char *comm = td->td_proc->p_comm;
				bool found = false;

				laminar_wake_pick_long_count++;
				/* Aggregate count by comm. */
				for (idx = 0; idx < LAMINAR_LONG_LOG; idx++) {
					if (laminar_long_log[idx][0] == '\0')
						break;
					if (strcmp(laminar_long_log[idx],
					    comm) == 0) {
						laminar_long_log_count[idx]++;
						found = true;
						break;
					}
				}
				if (!found && idx < LAMINAR_LONG_LOG) {
					strlcpy(laminar_long_log[idx], comm,
					    sizeof(laminar_long_log[idx]));
					laminar_long_log_count[idx] = 1;
				}
			}
			ts->ts_wake_ts = 0;
		}
		/*
		 * Pull off the runq; do NOT decrement load.  Load tracks
		 * threads associated with this CPU (running + on runq).
		 * choose moves the thread from runq to running, so the
		 * load count is unchanged.  Mirrors ULE's tdq_choose.
		 */
		tdq_runq_rem(tdq, td);
		tdq->ltdq_lowpri = td->td_priority;
		if (__predict_false(laminar_deadline_enable)) {
			struct laminar_lane_entity *nle =
			    td_get_sched(td)->ts_lane;

			if (nle != NULL && nle->le_td == td)
				nle->le_run_start = sbinuptime();
		}
	} else {
		tdq->ltdq_lowpri = PRI_MAX_IDLE;
		td = PCPU_GET(idlethread);
	}
	tdq->ltdq_curthread = td;
	return (td);
}

static void
sched_laminar_clock(struct thread *td, int cnt)
{
	struct laminar_tdq *tdq;
	struct td_sched *ts;

	THREAD_LOCK_ASSERT(td, MA_OWNED);
	tdq = LAMINAR_TDQ_SELF();
	ts = td_get_sched(td);

	/*
	 * Deadline-lane CBS budget charge.  Statclock granularity is the
	 * phase-I budget clock; exhaustion throttles the entity to plain
	 * timeshare until its replenishment callout (overrun isolation —
	 * the D5 guarantee).  TDF_SLICEEND forces the AST so the overrunner
	 * yields the CPU promptly rather than at its natural switch.
	 */
	if (__predict_false(laminar_deadline_enable) &&
	    ts->ts_adopted != NULL) {
		struct laminar_lane_entity *le = ts->ts_adopted;

		/*
		 * K-b: the adopted server exhausted the client's budget —
		 * throttle the ENTITY (its budget is gone; CBS denies
		 * everyone until replenish) and demote this thread's band
		 * (we hold its thread lock).  le_throttled is a cross-CPU
		 * bool write here: a tolerable racy signal.
		 */
		if (le->le_gen == ts->ts_adopted_gen && le->le_td != NULL &&
		    !le->le_throttled && ts->ts_adopt_start != 0) {
			sbintime_t ran = sbinuptime() - ts->ts_adopt_start;
			int64_t ran_us = (int64_t)
			    (((uint64_t)ran * 1000000ULL) >> 32);

			if (le->le_budget_us - ran_us <= 0) {
				le->le_throttled = true;
				le->le_throttles++;
				td->td_flags |= TDF_SLICEEND;
				td->td_base_user_pri = ts->ts_adopt_upri;
				td->td_user_pri = ts->ts_adopt_upri;
				td->td_priority = ts->ts_adopt_upri;
			}
		}
	}
	if (__predict_false(laminar_deadline_enable) && ts->ts_lane != NULL) {
		struct laminar_lane_entity *le = ts->ts_lane;

		/*
		 * Precise overrun DETECTION only — the actual charge happens
		 * at switch-out in sched_laminar_choose with real elapsed
		 * time.  A statclock-quantum charge here (7.9ms at
		 * stathz=127) instantly mis-throttled ms-scale budgets
		 * (measured: 15%% spurious throttles on an IDLE box).
		 */
		if (le->le_td == td && !le->le_throttled && !le->le_yielded &&
		    le->le_run_start != 0) {
			sbintime_t ran = sbinuptime() - le->le_run_start;
			int64_t ran_us = (int64_t)
			    (((uint64_t)ran * 1000000ULL) >> 32);

			if (le->le_budget_us - ran_us <= 0) {
				le->le_throttled = true;
				le->le_throttles++;
				td->td_flags |= TDF_SLICEEND;
				/*
				 * K-a: drop the inheritance band for the
				 * rest of the period — an overrunner at
				 * kernel-range priority would beat all of
				 * WFQ via the rt-path and the throttle
				 * would not isolate.  Re-banded by the
				 * lane wake path after a clean replenish.
				 */
				if (le->le_banded) {
					td->td_base_user_pri =
					    le->le_saved_upri;
					td->td_user_pri = le->le_saved_upri;
					td->td_priority = le->le_saved_upri;
				}
			}
		}
	}

	/*
	 * Account work to the thread's vruntime.  Phase A.4: ts_eff_weight
	 * is 1 for everyone today, so vruntime accumulates at the unit
	 * rate.  Nice-derived re-weighting lands later, at which point
	 * this expression keeps its shape (one multiply, no divide).
	 */
	/*
	 * Recompute eff_weight live rather than using the cached
	 * ts->ts_eff_weight from fork/renice time.  The cache went
	 * stale whenever jail_nthreads changed (other threads joined
	 * or left the same prison), which broke phase E's per-jail
	 * proportional share: nice -5 group that forked first got
	 * weighted against an older jail_n than nice +5 group that
	 * forked later, flattening the cross-group ratio in
	 * bench_skew.  The live recomputation is one extra read +
	 * multiply + divide per clock tick per running thread (~5ns),
	 * negligible at stathz frequencies.  ts_eff_weight is kept
	 * around as an observability hint, not load-bearing.
	 */
	ts->ts_vruntime += (uint64_t)cnt * laminar_compute_eff_weight(
	    ts->ts_weight, laminar_prison_of(td));
	tdq->ltdq_switchcnt = tdq->ltdq_switchcnt + 1;
	/*
	 * Phase F: refresh inferred NUMA home to the current CPU's
	 * domain.  Recent-CPU bias -- the thread has just consumed
	 * cache and memory traffic here so this is its current
	 * locality.  Inadequate vs a real page-residency signal but
	 * captures common case (long-running threads stay home).
	 */
	ts->ts_home_node = (int16_t)laminar_cpu_domain(PCPU_GET(cpuid));
	/*
	 * Slice quantum: if curthread has used its slice, set
	 * TDF_SLICEEND so it voluntarily yields at the next AST.
	 * Without this two same-class CPU-bound threads never
	 * preempt each other (no priority-IPI fires for same class)
	 * and a freshly-woken thread enqueued on this CPU starves
	 * until the running thread blocks or exits.  Idle threads
	 * are exempt -- they yield naturally.
	 */
	laminar_clock_calls++;
	if (TD_IS_IDLETHREAD(td)) {
		laminar_clock_idle_calls++;
	} else {
		ts->ts_slice_used += cnt;
		laminar_clock_ts_sum += cnt;
		/*
		 * Load-adaptive slice: under heavy contention (>= MIN_DIVISOR
		 * runnable threads per CPU) use the shorter slice_min so
		 * wakees rotate every ~16 ms instead of the ~94 ms base.
		 * ULE pattern; matches its desktop-under-load behaviour
		 * without changing the low-load throughput posture.
		 */
		struct laminar_tdq *_tdq = LAMINAR_TDQ_SELF();
		int _slice = (atomic_load_int(&_tdq->ltdq_load) >=
		    laminar_slice_min_load) ? sched_slice_min : sched_slice;
		if (ts->ts_slice_used >= (uint32_t)_slice) {
			ts->ts_slice_used = 0;
			td->td_flags |= TDF_SLICEEND;
			ast_sched_locked(td, TDA_SCHED);
			laminar_slice_ends++;
		}
	}
}

static void
sched_laminar_idletd(void *dummy)
{
	struct laminar_tdq *tdq;
	struct thread *td;

	td = curthread;
	tdq = LAMINAR_TDQ_SELF();
	THREAD_NO_SLEEPING();
	for (;;) {
		while (atomic_load_int(&tdq->ltdq_load) == 0) {
			/*
			 * busy=1 keeps the hardclock at active rate.
			 * cpu_idle(0) would call cpu_idleclock() which drops
			 * the hardclock to "next callout deadline" granularity,
			 * causing waker-callout chains to stall up to ~1s --
			 * the R4 multi-second wake-tail root cause.  ULE
			 * avoids this with a switchcnt-based busy hint
			 * (tried, regressed Laminar throughput further on
			 * this VM); simple always-busy is the best tradeoff
			 * here.  WFI is still issued inside cpu_idle when
			 * sched_runnable() is false, so true-idle still
			 * yields the vCPU to the HVF host.
			 */
			cpu_idle(1);
		}
		thread_lock(td);
		mi_switch(SW_VOL | SWT_IDLE);
	}
}

static void
sched_laminar_preempt(struct thread *td)
{
	int flags;

	thread_lock(td);
	if (td->td_critnest > 1) {
		td->td_owepreempt = 1;
		thread_unlock(td);
		return;
	}
	flags = SW_INVOL | SW_PREEMPT;
	flags |= TD_IS_IDLETHREAD(td) ? SWT_REMOTEWAKEIDLE :
	    SWT_REMOTEPREEMPT;
	mi_switch(flags);	/* drops thread_lock */
}

static void
sched_laminar_relinquish(struct thread *td)
{

	thread_lock(td);
	mi_switch(SW_VOL | SWT_RELINQUISH);
}

/*
 * Maximum lag (in vruntime units) by which a just-woken thread may
 * sit below the per-CPU vruntime floor.  Without this cap a thread
 * that slept for seconds would arrive with a stale (low) vruntime
 * and starve every other timeshare thread on the CPU until its
 * accumulator caught up.  Tunable for measurement.
 */
u_long laminar_lag_cap = 1000000;
/*
 * R4 instrumentation: max observed wake-to-on-cpu delay (us) and a
 * counter of "long wake" events (delay > 100ms).  Reset via the
 * sysctl with a write of 0.  Lets us tell "scheduler-side wait" from
 * "wake path / HVF stall" -- if scheduler max stays small but
 * userspace wakelat reports seconds, the wait is outside the picker.
 */
u_long laminar_wake_pick_max_us = 0;
u_long laminar_wake_pick_long_count = 0;
u_long laminar_queue_pick_max_us = 0;
char   laminar_wake_pick_max_comm[16] = "";
char   laminar_queue_pick_max_comm[16] = "";
/* Histogram of long-wake-pick events by comm.  Simple linear array. */
char   laminar_long_log[LAMINAR_LONG_LOG][16];
u_long laminar_long_log_count[LAMINAR_LONG_LOG];
/* Picker chain instrumentation for R4 root-cause. */
u_long laminar_choose_calls = 0;	/* sched_choose invocations */
u_long laminar_slice_ends = 0;		/* TDF_SLICEEND fired */
u_long laminar_wake_picks = 0;		/* wakes that became on-cpu */
u_long laminar_cp_ipi = 0;		/* cost-preempt IPIs sent */
u_long laminar_cp_skip_v = 0;		/* skipped: v > floor */
u_long laminar_cp_skip_cool = 0;	/* skipped: cooldown not expired */
u_long laminar_cp_skip_idle = 0;	/* skipped: dst idle */
u_long laminar_cp_skip_owe = 0;		/* skipped: owepreempt already set */
u_long laminar_notify_ipi = 0;		/* tdq_notify IPIs sent */
u_long laminar_notify_skip = 0;		/* tdq_notify decided no IPI */
u_long laminar_wake_pick_long_threshold = LAMINAR_WAKE_LONG_US;
/*
 * Per-CPU cooldown (in ticks) between cost-based preempt IPIs.
 * Caps the IPI rate so wakers don't storm a busy CPU.  The trade
 * is wake-tail-latency vs preempt-IPI churn -- lower = tighter
 * tail but more switches; higher = less churn but longer waits.
 * 1 tick @ hz=100 = 10ms.  Default 2 = 20ms.
 */
u_long laminar_preempt_cooldown = 2;
SYSCTL_DECL(_kern_sched);
SYSCTL_ULONG(_kern_sched, OID_AUTO, lag_cap, CTLFLAG_RW,
    &laminar_lag_cap, 0,
    "Laminar: max vruntime units a waker may sit below the per-CPU floor");
SYSCTL_ULONG(_kern_sched, OID_AUTO, wake_pick_max_us, CTLFLAG_RW,
    &laminar_wake_pick_max_us, 0,
    "Laminar: max wake-to-on-cpu delay (us) since reset.  Write 0 to reset.");
SYSCTL_ULONG(_kern_sched, OID_AUTO, wake_pick_long_count, CTLFLAG_RW,
    &laminar_wake_pick_long_count, 0,
    "Laminar: count of wake-to-on-cpu delays exceeding "
    "wake_pick_long_threshold.  Write 0 to reset.");
SYSCTL_ULONG(_kern_sched, OID_AUTO, wake_pick_long_threshold, CTLFLAG_RW,
    &laminar_wake_pick_long_threshold, 0,
    "Laminar: us threshold for wake_pick_long_count + long_log "
    "comm histogram.  Default 100000 (100ms); lower for finer "
    "tail-event capture.");
SYSCTL_STRING(_kern_sched, OID_AUTO, wake_pick_max_comm, CTLFLAG_RD,
    laminar_wake_pick_max_comm, sizeof(laminar_wake_pick_max_comm),
    "Laminar: proc name of the thread that hit wake_pick_max_us.");
SYSCTL_ULONG(_kern_sched, OID_AUTO, queue_pick_max_us, CTLFLAG_RW,
    &laminar_queue_pick_max_us, 0,
    "Laminar: max queue-to-on-cpu delay (us).  pick - tdq_runq_add_ts.");
SYSCTL_STRING(_kern_sched, OID_AUTO, queue_pick_max_comm, CTLFLAG_RD,
    laminar_queue_pick_max_comm, sizeof(laminar_queue_pick_max_comm),
    "Laminar: proc name that hit queue_pick_max_us.");

static int
laminar_long_log_sysctl(SYSCTL_HANDLER_ARGS)
{
	struct sbuf sb;
	int i, err;

	sbuf_new_for_sysctl(&sb, NULL, 256, req);
	for (i = 0; i < LAMINAR_LONG_LOG; i++) {
		if (laminar_long_log[i][0] == '\0')
			break;
		sbuf_printf(&sb, "%s=%lu\n", laminar_long_log[i],
		    laminar_long_log_count[i]);
	}
	err = sbuf_finish(&sb);
	sbuf_delete(&sb);
	return (err);
}
SYSCTL_PROC(_kern_sched, OID_AUTO, long_log,
    CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_MPSAFE, NULL, 0,
    laminar_long_log_sysctl, "A",
    "Laminar: long-wake-pick event histogram by proc name.");

static int
laminar_v_fair_sysctl(SYSCTL_HANDLER_ARGS)
{
	uint64_t v_fair = laminar_v_fair();

	return (sysctl_handle_64(oidp, &v_fair, 0, req));
}
SYSCTL_PROC(_kern_sched, OID_AUTO, v_fair,
    CTLTYPE_U64 | CTLFLAG_RD | CTLFLAG_MPSAFE, NULL, 0,
    laminar_v_fair_sysctl, "QU",
    "Laminar: system-wide fair vruntime (min of ltdq_vtime over CPUs "
    "with runnable threads).  Picker-placer coupling reference (vlag = "
    "V[T] - V_fair).");
SYSCTL_ULONG(_kern_sched, OID_AUTO, choose_calls, CTLFLAG_RW,
    &laminar_choose_calls, 0,
    "Laminar: sched_choose invocations since reset.  Write 0 to reset.");
SYSCTL_ULONG(_kern_sched, OID_AUTO, slice_ends, CTLFLAG_RW,
    &laminar_slice_ends, 0,
    "Laminar: TDF_SLICEEND firings since reset.  Write 0 to reset.");
SYSCTL_ULONG(_kern_sched, OID_AUTO, wake_picks, CTLFLAG_RW,
    &laminar_wake_picks, 0,
    "Laminar: wakes that became on-cpu since reset.  Write 0 to reset.");
SYSCTL_ULONG(_kern_sched, OID_AUTO, cp_ipi, CTLFLAG_RW,
    &laminar_cp_ipi, 0, "Laminar: cost-preempt IPIs sent.");
SYSCTL_ULONG(_kern_sched, OID_AUTO, cp_skip_v, CTLFLAG_RW,
    &laminar_cp_skip_v, 0, "Laminar: cost-preempt skipped (v > floor).");
SYSCTL_ULONG(_kern_sched, OID_AUTO, cp_skip_cool, CTLFLAG_RW,
    &laminar_cp_skip_cool, 0, "Laminar: cost-preempt skipped (cooldown).");
SYSCTL_ULONG(_kern_sched, OID_AUTO, cp_skip_idle, CTLFLAG_RW,
    &laminar_cp_skip_idle, 0, "Laminar: cost-preempt skipped (dst idle).");
SYSCTL_ULONG(_kern_sched, OID_AUTO, cp_skip_owe, CTLFLAG_RW,
    &laminar_cp_skip_owe, 0, "Laminar: cost-preempt skipped (owe set).");
SYSCTL_ULONG(_kern_sched, OID_AUTO, notify_ipi, CTLFLAG_RW,
    &laminar_notify_ipi, 0, "Laminar: tdq_notify IPIs sent.");
SYSCTL_ULONG(_kern_sched, OID_AUTO, notify_skip, CTLFLAG_RW,
    &laminar_notify_skip, 0, "Laminar: tdq_notify decisions no-IPI.");
SYSCTL_ULONG(_kern_sched, OID_AUTO, local_preempt_cooldown, CTLFLAG_RW,
    &laminar_local_preempt_cooldown, 0,
    "Laminar: per-CPU ticks between same-CPU cost-based AST fires "
    "(R4 fix; rate-limit to avoid local picker thrash).");
SYSCTL_ULONG(_kern_sched, OID_AUTO, local_preempt_fires, CTLFLAG_RW,
    &laminar_local_preempt_fires, 0,
    "Laminar: same-CPU cost-preempt AST fires.");
SYSCTL_ULONG(_kern_sched, OID_AUTO, local_preempt_skip_cool, CTLFLAG_RW,
    &laminar_local_preempt_skip_cool, 0,
    "Laminar: same-CPU cost-preempt skipped (cooldown).");
SYSCTL_ULONG(_kern_sched, OID_AUTO, local_preempt_skip_v, CTLFLAG_RW,
    &laminar_local_preempt_skip_v, 0,
    "Laminar: same-CPU cost-preempt skipped (no lower-vruntime found).");
SYSCTL_ULONG(_kern_sched, OID_AUTO, clock_calls, CTLFLAG_RW,
    &laminar_clock_calls, 0, "Laminar: sched_clock entries.");
SYSCTL_ULONG(_kern_sched, OID_AUTO, clock_idle_calls, CTLFLAG_RW,
    &laminar_clock_idle_calls, 0, "Laminar: sched_clock entries for idle.");
SYSCTL_ULONG(_kern_sched, OID_AUTO, clock_ts_sum, CTLFLAG_RW,
    &laminar_clock_ts_sum, 0, "Laminar: sum of cnt across non-idle clock.");
SYSCTL_ULONG(_kern_sched, OID_AUTO, idle_exits, CTLFLAG_RW,
    &laminar_idle_exits, 0, "Laminar: idle loop exits (load > 0 detected).");
SYSCTL_ULONG(_kern_sched, OID_AUTO, idle_loops, CTLFLAG_RW,
    &laminar_idle_loops, 0, "Laminar: idle while-loop iterations.");
SYSCTL_ULONG(_kern_sched, OID_AUTO, preempt_cooldown, CTLFLAG_RW,
    &laminar_preempt_cooldown, 0,
    "Laminar: per-CPU ticks between cost-based wake-preempt IPIs; "
    "rate-limit to avoid IPI/switch storm under high wake rate");

/*
 * Record one wakeup edge for IPC affinity inference (whitepaper §6).
 * Boyer-Moore single-slot majority over the waker's CPU.  Skipped
 * for self-wakes, idle-thread wakes, and when the waker has no
 * sensible CPU snapshot.
 */
static __inline void
laminar_ipc_record_edge(struct thread *td)
{
	struct td_sched *ts, *wts;
	struct thread *waker;
	int waker_cpu;

	waker = curthread;
	if (waker == td || waker == NULL || TD_IS_IDLETHREAD(waker))
		return;
	wts = td_get_sched(waker);
	waker_cpu = wts->ts_cpu;
	if (waker_cpu < 0 || waker_cpu > mp_maxid)
		return;
	ts = td_get_sched(td);
	if (ts->ts_dom_waker_cpu == (int16_t)waker_cpu) {
		if (ts->ts_dom_waker_conf < LAMINAR_IPC_CONF_MAX)
			ts->ts_dom_waker_conf++;
	} else if (ts->ts_dom_waker_conf > 0) {
		ts->ts_dom_waker_conf--;
	} else {
		ts->ts_dom_waker_cpu = (int16_t)waker_cpu;
		ts->ts_dom_waker_conf = 1;
	}
}

static void
sched_laminar_wakeup(struct thread *td, int srqflags)
{

	THREAD_LOCK_ASSERT(td, MA_OWNED);
	laminar_ipc_record_edge(td);
	/* R4 instrumentation: stamp the wake moment. */
	td_get_sched(td)->ts_wake_ts = sbinuptime();
	/*
	 * Let sched_laminar_add's pickcpu pick the target CPU.  ts_cpu is
	 * preserved from the thread's last run, which gives natural soft
	 * affinity until a real cost-minimizing pickcpu lands.  The
	 * bounded-lag vruntime rebase (whitepaper §7) happens inside
	 * tdq_add_internal against the chosen destination CPU's floor --
	 * doing it here against ts_cpu's floor would leave the waker
	 * stranded above the destination floor if pickcpu migrated it
	 * (cause of R4 multi-second wake-latency tails).
	 */
	sched_laminar_add(td, srqflags);
}

/*
 * Binding / affinity.
 *
 * sched_bind pins the current thread to a specific CPU; sched_unbind
 * releases the pin.  sched_affinity is called when a thread's
 * cpuset_t changes and recomputes whether its current CPU placement
 * is still valid.  In A.3d every thread runs on the CPU that
 * enqueued it, so the affinity recompute is a no-op until cross-CPU
 * placement lands.
 */
static void
sched_laminar_bind(struct thread *td, int cpu)
{
	struct td_sched *ts;

	THREAD_LOCK_ASSERT(td, MA_OWNED | MA_NOTRECURSED);
	KASSERT(td == curthread,
	    ("sched_laminar_bind: not curthread"));

	ts = td_get_sched(td);
	if (ts->ts_flags & TSF_BOUND)
		sched_unbind(td);
	KASSERT(THREAD_CAN_MIGRATE(td), ("sched_laminar_bind: %p not migratable", td));
	ts->ts_flags |= TSF_BOUND;
	sched_pin();
#ifdef SMP
	if (PCPU_GET(cpuid) == cpu)
		return;
	ts->ts_cpu = cpu;
	/*
	 * mi_switch routes through sched_laminar_sswitch's TD_IS_RUNNING
	 * cross-CPU branch (now wired) which moves us to ts_cpu's tdq.
	 * Returning from mi_switch we are running on the bound CPU.
	 */
	mi_switch(SW_VOL | SWT_BIND);
	thread_lock(td);
#else
	ts->ts_cpu = cpu;
#endif
}

static void
sched_laminar_unbind(struct thread *td)
{
	struct td_sched *ts;

	THREAD_LOCK_ASSERT(td, MA_OWNED);
	KASSERT(td == curthread,
	    ("sched_laminar_unbind: not curthread"));
	ts = td_get_sched(td);
	if ((ts->ts_flags & TSF_BOUND) == 0)
		return;
	ts->ts_flags &= ~TSF_BOUND;
	/*
	 * Pair the sched_pin() taken in sched_laminar_bind.  Without
	 * this, td_pinned accumulates and THREAD_CAN_MIGRATE eventually
	 * returns false; the next sched_bind() then hits mi_switch
	 * trying to migrate an unmovable thread and deadlocks.  Exposed
	 * by cpufreq's bind-do_set-unbind pattern when a cpufreq driver
	 * (e.g. cpufreq_mock) is present.
	 */
	sched_unpin();
}

static int
sched_laminar_is_bound(struct thread *td)
{

	THREAD_LOCK_ASSERT(td, MA_OWNED);
	return ((td_get_sched(td)->ts_flags & TSF_BOUND) != 0);
}

static void
sched_laminar_affinity(struct thread *td)
{

	/*
	 * cpuset change notification.  With single-CPU enqueue (A.3d)
	 * we have nothing to recompute: the thread already runs only on
	 * the CPU that enqueued it, and re-spawn after cpuset change
	 * will pick the right CPU via the new mask.  When cross-CPU
	 * placement lands, this slot becomes a real recompute and
	 * possibly an IPI to force migration.
	 */
	THREAD_LOCK_ASSERT(td, MA_OWNED);
}

/*
 * Sizing.
 */
static int
sched_laminar_sizeof_proc(void)
{

	return (sizeof(struct proc));
}

static int
sched_laminar_sizeof_thread(void)
{

	return (sizeof(struct thread) + sizeof(struct td_sched));
}

/*
 * KTR thread-name accessors.  The KTR-cached "%s tid %d" form is added
 * in a later commit (alongside the SDT probes); for now we expose the
 * thread's bare name.
 */
static char *
sched_laminar_tdname(struct thread *td)
{

	return (td->td_name);
}

static void
sched_laminar_clear_tdname(struct thread *td)
{

}

/*
 * Misc.
 */
static bool
sched_laminar_do_timer_accounting(void)
{

	return (true);
}

static int
sched_laminar_find_l2_neighbor(int cpuid)
{

	/*
	 * Deferred to a later phase.  Returning -1 indicates "no L2
	 * neighbor known"; callers fall back to non-L2-aware paths.
	 */
	return (-1);
}

/*
 * Initialization.  Called once on the boot CPU before any thread runs.
 */
static void
sched_laminar_init(void)
{
	struct td_sched *ts0;

	ts0 = td_get_sched(&thread0);
	ts0->ts_vruntime = 0;
	laminar_set_weight(ts0, laminar_nice_to_weight(0));
	ts0->ts_slot = 0;
	ts0->ts_cpu = curcpu;
	ts0->ts_flags = 0;
	ts0->ts_class = 0;		/* refined in a later phase */
	ts0->ts_class_pri = 0;
	ts0->ts_dom_waker_cpu = -1;
	ts0->ts_dom_waker_conf = 0;
	ts0->ts_home_node_conf = 0;
	ts0->ts_home_node = -1;
	ts0->ts_mem_bw = false;
}

/*
 * Per-AP initialization.  Called from schedinit_ap() on each application
 * processor before it enters the scheduler.
 */
static void
sched_laminar_init_ap(void)
{

#ifdef SMP
	PCPU_SET(sched, DPCPU_PTR(ltdq));
#endif
	PCPU_GET(idlethread)->td_lock =
	    LAMINAR_TDQ_LOCKPTR(LAMINAR_TDQ_SELF());
}

/*
 * Run-queue setup.  Called once at SI_SUB_RUN_QUEUE.  Initializes
 * per-CPU runqueues and attaches thread0 to the boot CPU's queue.
 */
static void
sched_laminar_setup(void)
{
	struct laminar_tdq *tdq;

#ifdef SMP
	sched_setup_smp();
#else
	tdq_setup(LAMINAR_TDQ_SELF(), 0);
#endif
	tdq = LAMINAR_TDQ_SELF();

	/*
	 * thread0 is already running; attach it to the boot tdq's lock
	 * and bump the runnable counts so subsequent accounting balances
	 * out.  Full enqueue logic lands with the runqueue ops commit.
	 */
	LAMINAR_TDQ_LOCK(tdq);
	thread0.td_lock = LAMINAR_TDQ_LOCKPTR(tdq);
	tdq->ltdq_load++;
	if ((thread0.td_flags & TDF_NOLOAD) == 0)
		tdq->ltdq_sysload++;
	tdq->ltdq_curthread = &thread0;
	tdq->ltdq_lowpri = thread0.td_priority;
	LAMINAR_TDQ_UNLOCK(tdq);
#ifdef SMP
	laminar_balance_start();
	laminar_ctrl_start();
#endif
}

/*
 * Late tick-domain initialization, after stathz is known.
 */
static void
sched_laminar_initticks(void)
{

	realstathz = stathz ? stathz : hz;
	sched_slice = realstathz / SCHED_SLICE_DEFAULT_DIVISOR;
	if (sched_slice < 1)
		sched_slice = 1;
	sched_slice_min = sched_slice / SCHED_SLICE_MIN_DIVISOR;
	if (sched_slice_min < 1)
		sched_slice_min = 1;
}

/*
 * Periodic scheduler maintenance kproc entry.  No-op; Laminar's vruntime
 * needs no %CPU window decay.
 */
static void
sched_laminar_schedcpu(void)
{

}

struct sched_instance sched_laminar_instance = {
#define	SLOT(name)	.name = sched_laminar_##name
	SLOT(load),
	SLOT(rr_interval),
	SLOT(runnable),
	SLOT(exit),
	SLOT(fork),
	SLOT(fork_exit),
	SLOT(class),
	SLOT(nice),
	SLOT(ap_entry),
	SLOT(exit_thread),
	SLOT(estcpu),
	SLOT(fork_thread),
	SLOT(ithread_prio),
	SLOT(lend_prio),
	SLOT(lend_user_prio),
	SLOT(lend_user_prio_cond),
	SLOT(pctcpu),
	SLOT(prio),
	SLOT(sleep),
	SLOT(sswitch),
	SLOT(throw),
	SLOT(unlend_prio),
	SLOT(user_prio),
	SLOT(userret_slowpath),
	SLOT(add),
	SLOT(choose),
	SLOT(clock),
	SLOT(idletd),
	SLOT(preempt),
	SLOT(relinquish),
	SLOT(rem),
	SLOT(wakeup),
	SLOT(bind),
	SLOT(unbind),
	SLOT(is_bound),
	SLOT(affinity),
	SLOT(sizeof_proc),
	SLOT(sizeof_thread),
	SLOT(tdname),
	SLOT(clear_tdname),
	SLOT(do_timer_accounting),
	SLOT(find_l2_neighbor),
	SLOT(init),
	SLOT(init_ap),
	SLOT(setup),
	SLOT(initticks),
	SLOT(schedcpu),
#undef	SLOT
};
DECLARE_SCHEDULER(laminar_sched_selector, "Laminar", &sched_laminar_instance);
