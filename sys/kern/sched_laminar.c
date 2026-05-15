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
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/pcpu.h>
#include <sys/proc.h>
#include <sys/runq.h>
#include <sys/sched.h>
#include <sys/smp.h>
#include <sys/turnstile.h>

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

/* Per-scheduler use of generic td_flags bits (mirrors ULE / 4BSD). */
#define	TDF_SLICEEND	TDF_SCHED2	/* Thread time slice is over. */

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
	/* IPC affinity (whitepaper §6). */
	struct thread	*ts_dom_waker;
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
	uint64_t	*ltdq_vruntime;	/* (t) Hot, SIMD-scannable (A.4). */
	struct thread	**ltdq_slot;	/* (t) Cold, by winner index (A.4). */
	uint64_t	ltdq_vtime;	/* (t) Virtual time floor (A.4). */

	/* Aggregate state. */
	int		ltdq_load;	/* (ts) Total runnable. */
	int		ltdq_sysload;	/* (ts) Non-ITHD load. */
	int		ltdq_transferable; /* (ts) Migration-eligible count. */
	int		ltdq_id;	/* (c) CPU id. */

	/* Per-CPU current state. */
	struct thread	*ltdq_curthread; /* (t) Running thread. */
	u_char		ltdq_lowpri;	/* (ts) Lowest priority on rq. */
	short		ltdq_switchcnt;	/* (l) Switches this tick. */
	short		ltdq_oldswitchcnt; /* (l) Switches last tick. */
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
	tdq->ltdq_ts_cap = 0;		/* Allocated in commit A.4. */
	tdq->ltdq_vruntime = NULL;
	tdq->ltdq_slot = NULL;
	tdq->ltdq_vtime = 0;
	mtx_init(LAMINAR_TDQ_LOCKPTR(tdq), "laminar sched lock", "sched lock",
	    MTX_SPIN);
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
	runq_add(&tdq->ltdq_runq, td, flags);
}

static __inline void
tdq_runq_rem(struct laminar_tdq *tdq, struct thread *td)
{

	LAMINAR_TDQ_LOCK_ASSERT(tdq, MA_OWNED);
	(void)runq_remove(&tdq->ltdq_runq, td);
}

/*
 * Pick the highest-priority thread on this runqueue.  Phase A.4
 * replaces the timeshare scan with min-vruntime; until then this is
 * pure priority order across all three priority bands.
 */
static struct thread *
tdq_choose(struct laminar_tdq *tdq)
{

	LAMINAR_TDQ_LOCK_ASSERT(tdq, MA_OWNED);
	return (runq_choose(&tdq->ltdq_runq));
}

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

	PROC_LOCK_ASSERT(p, MA_OWNED);
	p->p_nice = nice;
	/*
	 * Laminar derives ts_weight (and consequently ts_eff_weight) from
	 * the proc's nice value.  Weight re-derivation on per-thread state
	 * lands with the runqueue ops commit (A.3d) where the precompute
	 * site exists; for now we just record the nice value.
	 */
	FOREACH_THREAD_IN_PROC(p, td) {
		thread_lock(td);
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

	thread_lock(child);
	/*
	 * Load decrement on the exiting thread happens during the
	 * scheduler-rem path which is wired up in A.3d.  Until then this
	 * is a no-op; thread state outside the scheduler is torn down by
	 * the caller.
	 */
	thread_unlock(child);
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
	ts->ts_eff_weight = tsc->ts_eff_weight;
	ts->ts_weight = tsc->ts_weight;
	ts->ts_cpu = tsc->ts_cpu;
	ts->ts_class = tsc->ts_class;
	ts->ts_home_node = -1;
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

	THREAD_LOCK_ASSERT(td, MA_OWNED);

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
	 * Account work to the thread's vruntime.  Phase A.4 wires this
	 * to ts_eff_weight; for now ts_eff_weight is 1 for everyone, so
	 * vruntime accumulates at the unit rate -- enough to maintain
	 * monotonicity but not yet enforcing proportional share.
	 */
	ts->ts_vruntime += (uint64_t)cnt;
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
	} else {
		flags = SW_INVOL | SW_PREEMPT;
		flags |= TD_IS_IDLETHREAD(td) ? SWT_REMOTEWAKEIDLE :
		    SWT_REMOTEPREEMPT;
		mi_switch(flags);
	}
}

static void
sched_laminar_relinquish(struct thread *td)
{

	thread_lock(td);
	mi_switch(SW_VOL | SWT_RELINQUISH);
}

static void
sched_laminar_wakeup(struct thread *td, int srqflags)
{

	THREAD_LOCK_ASSERT(td, MA_OWNED);
	td_get_sched(td)->ts_cpu = PCPU_GET(cpuid);
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
	ts->ts_flags |= TSF_BOUND;
	ts->ts_cpu = cpu;
#ifdef SMP
	if (PCPU_GET(cpuid) == cpu)
		return;
	/*
	 * Cross-CPU bind would require migration here (which depends on
	 * the cross-CPU enqueue path landing in A.3e/A.4).  Until then
	 * the bind only takes effect at the next sched_add, and we do
	 * not synchronously switch the thread off.
	 */
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
	ts0->ts_eff_weight = 1;
	ts0->ts_weight = 1;
	ts0->ts_slot = 0;
	ts0->ts_cpu = curcpu;
	ts0->ts_flags = 0;
	ts0->ts_class = 0;		/* refined in a later phase */
	ts0->ts_class_pri = 0;
	ts0->ts_dom_waker = NULL;
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
