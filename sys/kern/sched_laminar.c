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
#include <machine/smp.h>

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
#define	LAMINAR_EWMA_OLD	3
#define	LAMINAR_EWMA_NEW	1
#define	LAMINAR_EWMA_DEN	(LAMINAR_EWMA_OLD + LAMINAR_EWMA_NEW)

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
#define	LAMINAR_MAX_PRISONS	32
struct laminar_prison {
	int		lpr_id;		/* prison.pr_id, -1 = free */
	uint32_t	lpr_weight;	/* user-set; LAMINAR_NICE_0_WEIGHT default */
	uint32_t	lpr_nthreads;	/* atomic-updated runnable count */
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
 *   eff_weight = NICE_0_WEIGHT^2 * jail_nthreads
 *              / (ts_weight       * jail_weight)
 *
 * When the thread's prison has no laminar slot (or the slot is
 * the defaults), jail_weight = NICE_0_WEIGHT and jail_nthreads
 * is treated as 1 -- both cancel and we recover the nice-only
 * formula from phase A/D.
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
	    LAMINAR_NICE_0_WEIGHT * jail_n) /
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
	int		ltdq_resistance;	/* (ts) additive placement R. */

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

	/* Aggregate state. */
	int		ltdq_load;	/* (ts) Total runnable. */
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
 */
#define	SCHED_SLICE_DEFAULT_DIVISOR	10	/* ~94 ms at stathz=127 */
static int __read_mostly realstathz = 127;
static int __read_mostly sched_slice = 10;

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
	tdq->ltdq_vtime = 0;
	tdq->ltdq_resistance = 0;
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

	return (PRI_BASE(td->td_pri_class) == PRI_TIMESHARE);
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

	LAMINAR_TDQ_LOCK_ASSERT(tdq, MA_OWNED);
	tdq->ltdq_load++;
	if ((td->td_flags & TDF_NOLOAD) == 0)
		tdq->ltdq_sysload++;
}

static __inline void
tdq_load_rem(struct laminar_tdq *tdq, struct thread *td)
{

	LAMINAR_TDQ_LOCK_ASSERT(tdq, MA_OWNED);
	KASSERT(tdq->ltdq_load > 0,
	    ("tdq_load_rem: load underflow on cpu %d", tdq->ltdq_id));
	tdq->ltdq_load--;
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
	if (rt != NULL && rt->td_priority < PRI_MIN_TIMESHARE)
		return (rt);
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
 * Raw (unscaled) cost: load + R.  Used for absolute decisions like
 * "are we balanced?" and as the gate / decouple input -- raw count
 * of transferable work is the right unit for "is there anything
 * actually to move", independent of capacity ratios.
 */
static __inline int
laminar_raw_cost(const struct laminar_tdq *tdq)
{

	return (atomic_load_int(&tdq->ltdq_load) +
	    atomic_load_int(&tdq->ltdq_resistance));
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
	ema = (LAMINAR_EWMA_OLD * prev + LAMINAR_EWMA_NEW * sample) /
	    LAMINAR_EWMA_DEN;
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
			home_cost = laminar_placement_cost(
			    LAMINAR_TDQ_CPU(home_cpu));
			CPU_FOREACH(cpu) {
				cost = laminar_placement_cost(
				    LAMINAR_TDQ_CPU(cpu));
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
		if (atomic_load_int(&LAMINAR_TDQ_CPU(ts_cpu)->ltdq_resistance)
		    == 0)
			return (ts_cpu);	/* fast path, no R pressure */
		best_cpu = ts_cpu;
		best_cost = laminar_placement_cost(LAMINAR_TDQ_CPU(ts_cpu));
	} else {
		best_cpu = self;
		best_cost = laminar_placement_cost(LAMINAR_TDQ_CPU(self));
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
		cost = laminar_placement_cost(LAMINAR_TDQ_CPU(cpu));
		if (cost < best_cost) {
			best_cost = cost;
			best_cpu = cpu;
		}
	}
done_scan:
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
tdq_notify(struct laminar_tdq *tdq, int lowpri)
{
	int cpu;

	LAMINAR_TDQ_LOCK_ASSERT(tdq, MA_OWNED);
	if (tdq->ltdq_owepreempt)
		return;
	if (tdq->ltdq_lowpri >= lowpri)	/* new one not higher priority */
		return;
	atomic_thread_fence_seq_cst();
	cpu = LAMINAR_TDQ_ID(tdq);
	if (cpu == PCPU_GET(cpuid))
		return;			/* same CPU; no IPI needed */
	tdq->ltdq_owepreempt = 1;
	ipi_cpu(cpu, IPI_PREEMPT);
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
 * Scan the timeshare SoA for a thread that can migrate to dst_cpu.
 * Returns NULL if nothing transferable; caller holds the source tdq
 * lock.
 */
static struct thread *
laminar_steal_timeshare(struct laminar_tdq *from, int dst_cpu)
{
	struct thread *td;
	uint32_t i;

	LAMINAR_TDQ_LOCK_ASSERT(from, MA_OWNED);
	for (i = 0; i < from->ltdq_ts_n; i++) {
		td = from->ltdq_slot[i];
		if (!THREAD_CAN_MIGRATE(td))
			continue;
		if (!THREAD_CAN_SCHED(td, dst_cpu))
			continue;
		return (td);
	}
	return (NULL);
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
	(void)tdq_add_internal(low, td, SRQ_YIELDING);
	if (dst_cpu != PCPU_GET(cpuid))
		tdq_notify(low, td->td_priority);
	moved = true;
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
		evac = atomic_load_int(&hi->ltdq_resistance) > 0;
		if (!evac) {
			/* C2 debounce: imbalance must persist. */
			hi->ltdq_streak++;
			if (hi->ltdq_streak < laminar_debounce)
				break;
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
		/* If non-evac, single migration per cycle (per DESIGN.md). */
		if (!evac)
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
		thread_lock(td);
		laminar_set_weight(td_get_sched(td), w);
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
			(void)tdq_add_internal(tdn, td, srqflag);
			tdq_notify(tdn, td->td_priority);
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
static void
sched_laminar_setpreempt(int pri)
{
	struct thread *ctd;

	ctd = curthread;
	THREAD_LOCK_ASSERT(ctd, MA_OWNED);
	if (pri < ctd->td_priority)
		ast_sched_locked(ctd, TDA_SCHED);
}

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
	if (cpu != PCPU_GET(cpuid))
		tdq_notify(tdq, td->td_priority);
	else if ((flags & SRQ_YIELDING) == 0)
		sched_laminar_setpreempt(td->td_priority);
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
	td = tdq_choose(tdq);
	if (td != NULL) {
		/*
		 * Pull off the runq; do NOT decrement load.  Load tracks
		 * threads associated with this CPU (running + on runq).
		 * choose moves the thread from runq to running, so the
		 * load count is unchanged.  Mirrors ULE's tdq_choose.
		 */
		tdq_runq_rem(tdq, td);
		tdq->ltdq_lowpri = td->td_priority;
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
	 * Account work to the thread's vruntime.  Phase A.4: ts_eff_weight
	 * is 1 for everyone today, so vruntime accumulates at the unit
	 * rate.  Nice-derived re-weighting lands later, at which point
	 * this expression keeps its shape (one multiply, no divide).
	 */
	ts->ts_vruntime += (uint64_t)cnt * ts->ts_eff_weight;
	tdq->ltdq_switchcnt = tdq->ltdq_switchcnt + 1;
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
			cpu_idle(0);
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
static u_long laminar_lag_cap = 1000000;
SYSCTL_DECL(_kern_sched);
SYSCTL_ULONG(_kern_sched, OID_AUTO, lag_cap, CTLFLAG_RW,
    &laminar_lag_cap, 0,
    "Laminar: max vruntime units a waker may sit below the per-CPU floor");

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
	struct td_sched *ts;
	struct laminar_tdq *tdq;
	uint64_t floor, cap;

	THREAD_LOCK_ASSERT(td, MA_OWNED);
	laminar_ipc_record_edge(td);
	/*
	 * Bounded-lag rebase (whitepaper §7).  Pull the waker's
	 * vruntime up to within laminar_lag_cap of the target CPU's
	 * current vruntime floor.  The read of ltdq_vtime is
	 * intentionally lockless -- it is a (ts) field, updated under
	 * the tdq lock and stored as a single 64-bit atomic, and the
	 * cap absorbs any staleness.
	 */
	ts = td_get_sched(td);
	if (laminar_is_timeshare(td)) {
		tdq = LAMINAR_TDQ_CPU(ts->ts_cpu);
		floor = atomic_load_64(&tdq->ltdq_vtime);
		cap = laminar_lag_cap;
		if (floor > cap && ts->ts_vruntime < floor - cap)
			ts->ts_vruntime = floor - cap;
	}
	/*
	 * Let sched_laminar_add's pickcpu pick the target CPU.  ts_cpu is
	 * preserved from the thread's last run, which gives natural soft
	 * affinity until a real cost-minimizing pickcpu lands.
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

	THREAD_LOCK_ASSERT(td, MA_OWNED);
	KASSERT(td == curthread,
	    ("sched_laminar_unbind: not curthread"));
	td_get_sched(td)->ts_flags &= ~TSF_BOUND;
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
