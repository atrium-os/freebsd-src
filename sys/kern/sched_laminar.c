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
#include <sys/proc.h>
#include <sys/sched.h>

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
