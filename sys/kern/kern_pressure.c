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
#include <sys/callout.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/pcpu.h>
#include <sys/proc.h>
#include <sys/smp.h>
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

/*
 * `full` (PSI full): wall-time during which a thread is blocked on memory AND no
 * CPU is doing productive work — nothing is progressing because the only things
 * that want the CPU are stalled. "Productive" excludes the idle thread and the
 * pagedaemon (reclaim is not progress). Sampled at the scheduler control cadence
 * via pressure_sample_cpus(); ended promptly when the last staller exits.
 */
static sbintime_t	pressure_full_last;	/* sbinuptime at the previous sample */
static uint64_t		pressure_full_ns;	/* banked wall-ns fully stalled */

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
 * Sample whether the system is in `full` stall: a thread is blocked on memory
 * (nstalled > 0) AND no CPU is running productive work. "Productive" = a USER
 * thread making progress; the idle thread and any KERNEL thread (P_KPROC: the
 * pagedaemon, laundry, swap-I/O workers, …) do not count — they are reclaim/IO
 * infrastructure, not workload progress. Counting them productive is exactly the
 * confound that made the total_load proxy useless (`full` stuck at 0). Called from
 * the scheduler control loop (~100 ms); no hot-path hook. Reading other CPUs'
 * pc_curthread is racy by design — a sampled signal tolerates the occasional miss.
 */
void
pressure_sample_cpus(void)
{
	struct thread *td;
	struct proc *p;
	sbintime_t now;
	int cpu, productive = 0;

	CPU_FOREACH(cpu) {
		td = pcpu_find(cpu)->pc_curthread;
		if (td == NULL || TD_IS_IDLETHREAD(td))
			continue;
		p = td->td_proc;
		if (p != NULL && (p->p_flag & P_KPROC) == 0)
			productive++;	/* a user thread is making progress */
	}

	/*
	 * Riemann-sum integration at the sample rate: attribute the elapsed period
	 * to `full` iff the full condition holds now. Each period is added at most
	 * once → `full_ns` can never exceed wall-clock (the interval-tracking version
	 * could double-count against the precise some-exit path).
	 */
	mtx_lock(&pressure_mtx);
	now = sbinuptime();
	if (pressure_full_last != 0 && now > pressure_full_last &&
	    pressure_nstalled > 0 && productive == 0)
		pressure_full_ns += (uint64_t)sbttons(now - pressure_full_last);
	pressure_full_last = now;
	mtx_unlock(&pressure_mtx);
}

/*
 * The banked `some` total plus any in-flight interval, so the counter is live
 * even while a stall is ongoing (otherwise a sustained stall reads flat until it
 * ends). Caller must hold pressure_mtx.
 */
static uint64_t
pressure_some_ns_locked(void)
{
	uint64_t v = pressure_some_ns;
	sbintime_t now;

	if (pressure_nstalled > 0) {
		now = sbinuptime();
		if (now > pressure_some_since)
			v += (uint64_t)sbttons(now - pressure_some_since);
	}
	return (v);
}

static int
pressure_some_ns_sysctl(SYSCTL_HANDLER_ARGS)
{
	uint64_t v;

	mtx_lock(&pressure_mtx);
	v = pressure_some_ns_locked();
	mtx_unlock(&pressure_mtx);
	return (sysctl_handle_64(oidp, &v, 0, req));
}

static int
pressure_full_ns_sysctl(SYSCTL_HANDLER_ARGS)
{
	uint64_t v;

	mtx_lock(&pressure_mtx);
	v = pressure_full_ns;
	mtx_unlock(&pressure_mtx);
	return (sysctl_handle_64(oidp, &v, 0, req));
}

/*
 * PSI-style decaying averages: the % of recent wall-time spent in `some` stall,
 * over 10/60/300 s, so the signal is a directly-consumable RATE (not a raw
 * counter). A 1 s callout samples the `some` delta, forms the instantaneous
 * fraction, and folds it into three fixed-point EWMAs. Fixed point (no kernel
 * FPU): FIXED_1 = 1<<16; decay_W = exp(-period/W) for a 1 s period, so a window
 * loses 1/W of its weight per second. Exposed as fraction x10000 (100% = 10000).
 */
#define	PRESSURE_FIXED_1	65536u
#define	PRESSURE_DECAY_10	59303u	/* exp(-1/10)  * 65536 */
#define	PRESSURE_DECAY_60	64453u	/* exp(-1/60)  * 65536 */
#define	PRESSURE_DECAY_300	65318u	/* exp(-1/300) * 65536 */

static uint32_t	pressure_avg10;		/* fixed-point `some` EWMAs, 0..FIXED_1 */
static uint32_t	pressure_avg60;
static uint32_t	pressure_avg300;
static uint32_t	pressure_full_avg10;	/* fixed-point `full` EWMAs */
static uint32_t	pressure_full_avg60;
static uint32_t	pressure_full_avg300;
static uint64_t	pressure_last_some_ns;	/* `some_ns` at the previous sample */
static uint64_t	pressure_last_full_ns;	/* `full_ns` at the previous sample */
static sbintime_t pressure_last_sbt;	/* sbinuptime at the previous sample */
static struct callout pressure_callout;

static uint32_t
pressure_ewma(uint32_t avg, uint32_t frac, uint32_t decay)
{
	/* avg = avg*decay + frac*(1-decay), all in FIXED_1 units. */
	uint64_t a = (uint64_t)avg * decay +
	    (uint64_t)frac * (PRESSURE_FIXED_1 - decay);
	return ((uint32_t)(a / PRESSURE_FIXED_1));
}

static void
pressure_aggregate(void *arg __unused)
{
	uint64_t some, full, delta, period_ns;
	sbintime_t now;
	uint32_t frac;

	mtx_lock(&pressure_mtx);
	some = pressure_some_ns_locked();
	full = pressure_full_ns;
	now = sbinuptime();
	period_ns = (now > pressure_last_sbt) ?
	    (uint64_t)sbttons(now - pressure_last_sbt) : 1;

	/* `some` fraction this period, clamped to [0, 1]. */
	delta = some - pressure_last_some_ns;
	if (period_ns == 0)
		frac = 0;
	else if (delta >= period_ns)
		frac = PRESSURE_FIXED_1;
	else
		frac = (uint32_t)(delta * PRESSURE_FIXED_1 / period_ns);
	pressure_avg10 = pressure_ewma(pressure_avg10, frac, PRESSURE_DECAY_10);
	pressure_avg60 = pressure_ewma(pressure_avg60, frac, PRESSURE_DECAY_60);
	pressure_avg300 = pressure_ewma(pressure_avg300, frac, PRESSURE_DECAY_300);

	/* `full` fraction this period. */
	delta = full - pressure_last_full_ns;
	if (period_ns == 0)
		frac = 0;
	else if (delta >= period_ns)
		frac = PRESSURE_FIXED_1;
	else
		frac = (uint32_t)(delta * PRESSURE_FIXED_1 / period_ns);
	pressure_full_avg10 = pressure_ewma(pressure_full_avg10, frac, PRESSURE_DECAY_10);
	pressure_full_avg60 = pressure_ewma(pressure_full_avg60, frac, PRESSURE_DECAY_60);
	pressure_full_avg300 = pressure_ewma(pressure_full_avg300, frac, PRESSURE_DECAY_300);

	pressure_last_some_ns = some;
	pressure_last_full_ns = full;
	pressure_last_sbt = now;
	mtx_unlock(&pressure_mtx);

	callout_reset(&pressure_callout, hz, pressure_aggregate, NULL);
}

static void
pressure_init(void *arg __unused)
{

	pressure_last_sbt = sbinuptime();
	callout_init(&pressure_callout, 1);
	callout_reset(&pressure_callout, hz, pressure_aggregate, NULL);
}
SYSINIT(pressure, SI_SUB_KICK_SCHEDULER, SI_ORDER_ANY, pressure_init, NULL);

/* fraction x10000 (100.00% = 10000) of an EWMA, for the sysctl. */
static int
pressure_avg_sysctl(SYSCTL_HANDLER_ARGS)
{
	uint32_t *avgp = arg1;
	int pct;

	pct = (int)((uint64_t)*avgp * 10000 / PRESSURE_FIXED_1);
	return (sysctl_handle_int(oidp, &pct, 0, req));
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

SYSCTL_PROC(_kern_pressure_memory, OID_AUTO, full_ns,
    CTLTYPE_U64 | CTLFLAG_RD | CTLFLAG_MPSAFE, NULL, 0,
    pressure_full_ns_sysctl, "QU",
    "Cumulative wall-ns fully stalled — a thread blocked on memory and no CPU "
    "doing productive work, pagedaemon excluded (PSI 'full')");

SYSCTL_PROC(_kern_pressure_memory, OID_AUTO, avg10,
    CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_MPSAFE, &pressure_avg10, 0,
    pressure_avg_sysctl, "I",
    "Memory stall 'some' over 10s, fraction x10000 (PSI avg10)");
SYSCTL_PROC(_kern_pressure_memory, OID_AUTO, avg60,
    CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_MPSAFE, &pressure_avg60, 0,
    pressure_avg_sysctl, "I",
    "Memory stall 'some' over 60s, fraction x10000 (PSI avg60)");
SYSCTL_PROC(_kern_pressure_memory, OID_AUTO, avg300,
    CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_MPSAFE, &pressure_avg300, 0,
    pressure_avg_sysctl, "I",
    "Memory stall 'some' over 300s, fraction x10000 (PSI avg300)");

SYSCTL_PROC(_kern_pressure_memory, OID_AUTO, full_avg10,
    CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_MPSAFE, &pressure_full_avg10, 0,
    pressure_avg_sysctl, "I",
    "Memory 'full' stall over 10s, fraction x10000 (PSI full avg10)");
SYSCTL_PROC(_kern_pressure_memory, OID_AUTO, full_avg60,
    CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_MPSAFE, &pressure_full_avg60, 0,
    pressure_avg_sysctl, "I",
    "Memory 'full' stall over 60s, fraction x10000 (PSI full avg60)");
SYSCTL_PROC(_kern_pressure_memory, OID_AUTO, full_avg300,
    CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_MPSAFE, &pressure_full_avg300, 0,
    pressure_avg_sysctl, "I",
    "Memory 'full' stall over 300s, fraction x10000 (PSI full avg300)");
