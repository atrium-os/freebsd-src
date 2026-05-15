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
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/pcpu.h>
#include <sys/proc.h>
#include <sys/runq.h>
#include <sys/sched.h>
#include <sys/smp.h>

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

	/* Timeshare class: SoA Laminar arrays. */
	uint32_t	ltdq_ts_n;	/* (t) Slot count. */
	uint32_t	ltdq_ts_cap;	/* (c) Slot array capacity. */
	uint64_t	*ltdq_vruntime;	/* (t) Hot, SIMD-scannable. */
	struct thread	**ltdq_slot;	/* (t) Cold, indexed by winner. */
	uint64_t	ltdq_vtime;	/* (t) Virtual time floor. */

	/* RT and IDLE classes: standard runq. */
	struct runq	ltdq_rt;	/* (t) ITHD + REALTIME. */
	struct runq	ltdq_idle;	/* (t) IDLE. */

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

static void __dead2
sched_laminar_unimpl(const char *fn)
{

	panic("sched_laminar: %s not yet implemented", fn);
}

#define	UNIMPL()	sched_laminar_unimpl(__func__)

/*
 * General scheduling info.
 */
static int
sched_laminar_load(void)
{

	UNIMPL();
}

static int
sched_laminar_rr_interval(void)
{

	UNIMPL();
}

static bool
sched_laminar_runnable(void)
{

	UNIMPL();
}

/*
 * Proc/thread lifecycle hooks.
 */
static void
sched_laminar_exit(struct proc *p, struct thread *childtd)
{

	UNIMPL();
}

static void
sched_laminar_fork(struct thread *td, struct thread *childtd)
{

	UNIMPL();
}

static void
sched_laminar_fork_exit(struct thread *td)
{

	UNIMPL();
}

static void
sched_laminar_class(struct thread *td, int class)
{

	UNIMPL();
}

static void
sched_laminar_nice(struct proc *p, int nice)
{

	UNIMPL();
}

static void
sched_laminar_ap_entry(void)
{

	UNIMPL();
}

static void
sched_laminar_exit_thread(struct thread *td, struct thread *child)
{

	UNIMPL();
}

static u_int
sched_laminar_estcpu(struct thread *td)
{

	UNIMPL();
}

static void
sched_laminar_fork_thread(struct thread *td, struct thread *child)
{

	UNIMPL();
}

/*
 * Priority manipulation.
 */
static void
sched_laminar_ithread_prio(struct thread *td, u_char prio)
{

	UNIMPL();
}

static void
sched_laminar_lend_prio(struct thread *td, u_char prio)
{

	UNIMPL();
}

static void
sched_laminar_lend_user_prio(struct thread *td, u_char pri)
{

	UNIMPL();
}

static void
sched_laminar_lend_user_prio_cond(struct thread *td, u_char pri)
{

	UNIMPL();
}

static fixpt_t
sched_laminar_pctcpu(struct thread *td)
{

	UNIMPL();
}

static void
sched_laminar_prio(struct thread *td, u_char prio)
{

	UNIMPL();
}

static void
sched_laminar_sleep(struct thread *td, int prio)
{

	UNIMPL();
}

static void
sched_laminar_sswitch(struct thread *td, int flags)
{

	UNIMPL();
}

static void
sched_laminar_throw(struct thread *td)
{

	UNIMPL();
}

static void
sched_laminar_unlend_prio(struct thread *td, u_char prio)
{

	UNIMPL();
}

static void
sched_laminar_user_prio(struct thread *td, u_char prio)
{

	UNIMPL();
}

static void
sched_laminar_userret_slowpath(struct thread *td)
{

	UNIMPL();
}

/*
 * Run queue manipulation.
 */
static void
sched_laminar_add(struct thread *td, int flags)
{

	UNIMPL();
}

static struct thread *
sched_laminar_choose(void)
{

	UNIMPL();
}

static void
sched_laminar_clock(struct thread *td, int cnt)
{

	UNIMPL();
}

static void
sched_laminar_idletd(void *dummy)
{

	UNIMPL();
}

static void
sched_laminar_preempt(struct thread *td)
{

	UNIMPL();
}

static void
sched_laminar_relinquish(struct thread *td)
{

	UNIMPL();
}

static void
sched_laminar_rem(struct thread *td)
{

	UNIMPL();
}

static void
sched_laminar_wakeup(struct thread *td, int srqflags)
{

	UNIMPL();
}

/*
 * Binding / affinity.
 */
static void
sched_laminar_bind(struct thread *td, int cpu)
{

	UNIMPL();
}

static void
sched_laminar_unbind(struct thread *td)
{

	UNIMPL();
}

static int
sched_laminar_is_bound(struct thread *td)
{

	UNIMPL();
}

static void
sched_laminar_affinity(struct thread *td)
{

	UNIMPL();
}

/*
 * Sizing.
 */
static int
sched_laminar_sizeof_proc(void)
{

	UNIMPL();
}

static int
sched_laminar_sizeof_thread(void)
{

	UNIMPL();
}

/*
 * KTR thread-name accessors.
 */
static char *
sched_laminar_tdname(struct thread *td)
{

	UNIMPL();
}

static void
sched_laminar_clear_tdname(struct thread *td)
{

	UNIMPL();
}

/*
 * Misc.
 */
static bool
sched_laminar_do_timer_accounting(void)
{

	UNIMPL();
}

static int
sched_laminar_find_l2_neighbor(int cpuid)
{

	UNIMPL();
}

/*
 * Initialization.
 */
static void
sched_laminar_init(void)
{

	UNIMPL();
}

static void
sched_laminar_init_ap(void)
{

	UNIMPL();
}

static void
sched_laminar_setup(void)
{

	UNIMPL();
}

static void
sched_laminar_initticks(void)
{

	UNIMPL();
}

static void
sched_laminar_schedcpu(void)
{

	UNIMPL();
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
