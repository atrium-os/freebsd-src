/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Memory-pressure stall accounting — the PSI-equivalent `some` signal
 * (atrium-memory-pressure.md, modelled deterministically in gpusim
 * engine/src/pressure.rs). Bracketed at the memory-reclaim sleep points in
 * vm_page.c, this measures the wall-clock time during which at least one thread
 * is blocked waiting on memory — the productivity-loss signal a federated memory
 * controller / OOM policy consumes, in place of the misleading free-page count.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/pressure.h>

/*
 * `some` accounting: nstalled is the count of threads currently blocked on
 * memory; some_ns accumulates wall-clock nanoseconds during which nstalled > 0
 * (the PSI `some` definition — wall time with at least one stall, NOT a sum of
 * per-thread stall times). The 0->1 transition starts the clock, 1->0 stops it
 * and banks the interval. A leaf mutex serializes the transition with the count.
 */
static struct mtx pressure_mtx;
MTX_SYSINIT(pressure_mtx, &pressure_mtx, "mem pressure", MTX_DEF);

static int		pressure_nstalled;	/* threads currently stalled */
static sbintime_t	pressure_some_since;	/* sbinuptime at the 0->1 edge */
static uint64_t		pressure_some_ns;	/* banked wall-ns with >=1 stalled */

void
pressure_mem_enter(void)
{

	mtx_lock(&pressure_mtx);
	if (pressure_nstalled++ == 0)
		pressure_some_since = sbinuptime();
	mtx_unlock(&pressure_mtx);
}

void
pressure_mem_exit(void)
{
	sbintime_t now;

	mtx_lock(&pressure_mtx);
	if (--pressure_nstalled == 0) {
		now = sbinuptime();
		if (now > pressure_some_since)
			pressure_some_ns += (uint64_t)
			    sbttons(now - pressure_some_since);
	}
	KASSERT(pressure_nstalled >= 0, ("pressure_nstalled underflow"));
	mtx_unlock(&pressure_mtx);
}

/*
 * Read the banked `some` total, plus any in-flight interval so the counter is
 * live even while a stall is ongoing (otherwise a sustained stall would read as
 * flat until it ends).
 */
static int
pressure_some_ns_sysctl(SYSCTL_HANDLER_ARGS)
{
	uint64_t v;
	sbintime_t now;

	mtx_lock(&pressure_mtx);
	v = pressure_some_ns;
	if (pressure_nstalled > 0) {
		now = sbinuptime();
		if (now > pressure_some_since)
			v += (uint64_t)sbttons(now - pressure_some_since);
	}
	mtx_unlock(&pressure_mtx);
	return (sysctl_handle_64(oidp, &v, 0, req));
}

static SYSCTL_NODE(_kern, OID_AUTO, pressure, CTLFLAG_RD | CTLFLAG_MPSAFE, NULL,
    "Resource pressure stall information");
static SYSCTL_NODE(_kern_pressure, OID_AUTO, memory,
    CTLFLAG_RD | CTLFLAG_MPSAFE, NULL, "Memory pressure (PSI-equivalent)");

SYSCTL_PROC(_kern_pressure_memory, OID_AUTO, some_ns,
    CTLTYPE_U64 | CTLFLAG_RD | CTLFLAG_MPSAFE, NULL, 0,
    pressure_some_ns_sysctl, "QU",
    "Cumulative wall-ns with >=1 thread stalled on memory (PSI 'some')");

SYSCTL_INT(_kern_pressure_memory, OID_AUTO, nstalled, CTLFLAG_RD,
    &pressure_nstalled, 0,
    "Threads currently blocked on memory");
