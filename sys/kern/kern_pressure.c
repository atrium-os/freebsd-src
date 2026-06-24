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
#include <sys/conf.h>
#include <sys/event.h>
#include <sys/jail.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/pcpu.h>
#include <sys/proc.h>
#include <sys/sbuf.h>
#include <sys/smp.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/ucred.h>
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

/*
 * Per-jail `some` attribution — the federation-member granularity (jails are the
 * Atrium app/member unit). A fixed table (the energy_members pattern: no pr_osd
 * lifecycle, no malloc in the stall path), claimed on a jail's first stall and
 * updated under pressure_mtx. Bounded; a full table silently drops attribution
 * (global `some` is unaffected). Slots are not reclaimed on jail destroy — fine for
 * the active-set; a production version would sweep dead jails.
 */
#define	PRESSURE_MAX_JAILS	16
static struct pressure_jail {
	int		jid;
	bool		used;
	int		nstalled;
	sbintime_t	some_since;
	uint64_t	some_ns;
	uint64_t	full_ns;	/* wall-ns this jail was fully stalled */
	/* decaying `full` averages (fixed-point, like the global ones) so a per-jail
	 * consumer gets a directly-usable RATE, not a raw counter — the Linux PSI
	 * per-cgroup avg10/60/300 analog. Folded by the 1 s aggregation callout. */
	uint32_t	full_avg10, full_avg60, full_avg300;
	uint64_t	last_full_ns;	/* clamped full_ns at the previous sample */
} pressure_jails[PRESSURE_MAX_JAILS];

/* The current thread's jail id (0 = host/prison0). */
static int
pressure_curjid(void)
{
	struct prison *pr = curthread->td_ucred->cr_prison;

	return (pr != NULL ? pr->pr_id : 0);
}

/* Find or claim the slot for `jid`; caller holds pressure_mtx. NULL if full. */
static struct pressure_jail *
pressure_jail_slot(int jid)
{
	int i, free = -1;

	for (i = 0; i < PRESSURE_MAX_JAILS; i++) {
		if (pressure_jails[i].used && pressure_jails[i].jid == jid)
			return (&pressure_jails[i]);
		if (!pressure_jails[i].used && free < 0)
			free = i;
	}
	if (free < 0)
		return (NULL);
	pressure_jails[free].used = true;
	pressure_jails[free].jid = jid;
	pressure_jails[free].nstalled = 0;
	pressure_jails[free].some_ns = 0;
	pressure_jails[free].full_ns = 0;
	pressure_jails[free].full_avg10 = 0;
	pressure_jails[free].full_avg60 = 0;
	pressure_jails[free].full_avg300 = 0;
	pressure_jails[free].last_full_ns = 0;
	return (&pressure_jails[free]);
}

void
pressure_mem_enter(void)
{
	struct pressure_jail *pj;

	mtx_lock(&pressure_mtx);
	if (pressure_nstalled++ == 0)
		pressure_some_since = sbinuptime();
	pj = pressure_jail_slot(pressure_curjid());
	if (pj != NULL && pj->nstalled++ == 0)
		pj->some_since = sbinuptime();
	mtx_unlock(&pressure_mtx);
}

void
pressure_mem_exit(void)
{
	struct pressure_jail *pj;
	sbintime_t now;

	mtx_lock(&pressure_mtx);
	if (--pressure_nstalled == 0) {
		now = sbinuptime();
		if (now > pressure_some_since)
			pressure_some_ns += (uint64_t)
			    sbttons(now - pressure_some_since);
	}
	KASSERT(pressure_nstalled >= 0, ("pressure_nstalled underflow"));
	pj = pressure_jail_slot(pressure_curjid());
	if (pj != NULL && pj->nstalled > 0 && --pj->nstalled == 0) {
		now = sbinuptime();
		if (now > pj->some_since)
			pj->some_ns += (uint64_t)sbttons(now - pj->some_since);
	}
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
	struct ucred *cr;
	sbintime_t now;
	uint64_t dt;
	bool jail_prod[PRESSURE_MAX_JAILS];
	int cpu, i, jid, productive = 0;

	/*
	 * Walk the CPUs under the lock so the same pass feeds both the global
	 * `full` (any productive thread anywhere) and per-jail `full` (a productive
	 * thread OF THAT JAIL). For each running user thread, mark its jail's slot
	 * productive. Reading other CPUs' pc_curthread (and its cred) is racy by
	 * design — a sampled signal tolerates the occasional miss.
	 */
	for (i = 0; i < PRESSURE_MAX_JAILS; i++)
		jail_prod[i] = false;

	mtx_lock(&pressure_mtx);
	CPU_FOREACH(cpu) {
		td = pcpu_find(cpu)->pc_curthread;
		if (td == NULL || TD_IS_IDLETHREAD(td))
			continue;
		p = td->td_proc;
		if (p == NULL || (p->p_flag & P_KPROC) != 0)
			continue;
		productive++;		/* a user thread is making progress */
		cr = td->td_ucred;
		jid = (cr != NULL && cr->cr_prison != NULL) ?
		    cr->cr_prison->pr_id : 0;
		for (i = 0; i < PRESSURE_MAX_JAILS; i++) {
			if (pressure_jails[i].used &&
			    pressure_jails[i].jid == jid) {
				jail_prod[i] = true;
				break;
			}
		}
	}

	/*
	 * Riemann-sum integration at the sample rate: attribute the elapsed period
	 * to `full` iff the full condition holds now. Each period is added at most
	 * once → `full_ns` can never exceed wall-clock (the interval-tracking version
	 * could double-count against the precise some-exit path).
	 */
	now = sbinuptime();
	if (pressure_full_last != 0 && now > pressure_full_last) {
		dt = (uint64_t)sbttons(now - pressure_full_last);
		if (pressure_nstalled > 0 && productive == 0)
			pressure_full_ns += dt;
		/*
		 * Per-jail `full`: the jail has a thread blocked on memory and
		 * none of its own threads ran this sample — it is locally
		 * thrashing, possibly while the system overall progresses via a
		 * different jail. (global full ⊆ every jail's full, never the
		 * reverse.)
		 */
		for (i = 0; i < PRESSURE_MAX_JAILS; i++) {
			if (pressure_jails[i].used &&
			    pressure_jails[i].nstalled > 0 && !jail_prod[i])
				pressure_jails[i].full_ns += dt;
		}
	}
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

/*
 * kqueue edge-trigger — the BSD-native realization of PSI's poll/trigger (Linux
 * exposes this via poll() on /proc/pressure/memory with a written threshold). A
 * userspace controller (memoryd) opens /dev/pressure and registers an EVFILT_READ
 * knote whose `data` field carries a `full` threshold in basis points (fraction
 * x10000, the same unit as the sysctls — 40% = 4000). The 1 s aggregation callout
 * KNOTEs the list; a knote is active while full_avg10 >= its threshold. So the
 * controller sleeps in kevent() with ZERO wakeups until the kernel pushes a
 * pressure edge — no 1 Hz poll, no idle CPU ([[feedback_kqueue_native]]).
 */
static struct mtx	pressure_knl_mtx;
static struct knlist	pressure_knl;
static struct cdev	*pressure_cdev;

/* full_avg10 in basis points (fraction x10000), matching the sysctl unit. */
static int
pressure_full_avg10_bp(void)
{
	return ((int)((uint64_t)pressure_full_avg10 * 10000 / PRESSURE_FIXED_1));
}

static int
pressure_kqf_event(struct knote *kn, long hint __unused)
{
	int bp = pressure_full_avg10_bp();

	kn->kn_data = bp;
	return (bp >= (int)kn->kn_sdata);
}

static void
pressure_kqf_detach(struct knote *kn)
{
	knlist_remove(&pressure_knl, kn, 0);
}

static const struct filterops pressure_filterops = {
	.f_isfd = 1,
	.f_detach = pressure_kqf_detach,
	.f_event = pressure_kqf_event,
};

static int
pressure_dev_kqfilter(struct cdev *dev __unused, struct knote *kn)
{
	if (kn->kn_filter != EVFILT_READ)
		return (EINVAL);
	kn->kn_fop = &pressure_filterops;
	knlist_add(&pressure_knl, kn, 0);
	return (0);
}

static int
pressure_dev_open(struct cdev *dev __unused, int oflags __unused,
    int devtype __unused, struct thread *td __unused)
{
	return (0);
}

static struct cdevsw pressure_cdevsw = {
	.d_version = D_VERSION,
	.d_open = pressure_dev_open,
	.d_kqfilter = pressure_dev_kqfilter,
	.d_name = "pressure",
};

static void
pressure_aggregate(void *arg __unused)
{
	uint64_t some, full, delta, period_ns;
	sbintime_t now;
	uint32_t frac;
	int i;

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

	/*
	 * Per-jail `full` EWMAs: same fold, per member. Clamp each jail's full to its
	 * live `some` (full ⊆ some) before differencing — both are monotonic so the
	 * clamped delta stays non-negative, and the per-jail rate never exceeds the
	 * jail's degraded rate.
	 */
	for (i = 0; i < PRESSURE_MAX_JAILS; i++) {
		struct pressure_jail *pj = &pressure_jails[i];
		uint64_t js, jf;

		if (!pj->used)
			continue;
		js = pj->some_ns;
		if (pj->nstalled > 0 && now > pj->some_since)
			js += (uint64_t)sbttons(now - pj->some_since);
		jf = pj->full_ns;
		if (jf > js)
			jf = js;
		delta = jf - pj->last_full_ns;
		if (period_ns == 0)
			frac = 0;
		else if (delta >= period_ns)
			frac = PRESSURE_FIXED_1;
		else
			frac = (uint32_t)(delta * PRESSURE_FIXED_1 / period_ns);
		pj->full_avg10 = pressure_ewma(pj->full_avg10, frac, PRESSURE_DECAY_10);
		pj->full_avg60 = pressure_ewma(pj->full_avg60, frac, PRESSURE_DECAY_60);
		pj->full_avg300 = pressure_ewma(pj->full_avg300, frac, PRESSURE_DECAY_300);
		pj->last_full_ns = jf;
	}

	pressure_last_some_ns = some;
	pressure_last_full_ns = full;
	pressure_last_sbt = now;
	mtx_unlock(&pressure_mtx);

	/*
	 * Push the pressure edge to any kevent() waiter. Each knote re-evaluates
	 * full_avg10 against its own threshold; KNOTE_UNLOCKED takes the knlist
	 * lock itself (we hold no lock here, dodging any pressure_mtx ordering).
	 */
	KNOTE_UNLOCKED(&pressure_knl, 0);

	callout_reset(&pressure_callout, hz, pressure_aggregate, NULL);
}

static void
pressure_init(void *arg __unused)
{

	mtx_init(&pressure_knl_mtx, "mem pressure knote", NULL, MTX_DEF);
	knlist_init_mtx(&pressure_knl, &pressure_knl_mtx);
	pressure_cdev = make_dev(&pressure_cdevsw, 0, UID_ROOT, GID_WHEEL, 0640,
	    "pressure");

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

/* Per-jail `some` (the federation-member granularity): one line per jail that has
 * stalled, "jail <jid> some_ns=<ns>" (jid 0 = host). Includes any in-flight stall. */
static int
pressure_jails_sysctl(SYSCTL_HANDLER_ARGS)
{
	struct sbuf sb;
	uint64_t v, fv;
	sbintime_t now;
	int i, error;

	sbuf_new_for_sysctl(&sb, NULL, 256, req);
	mtx_lock(&pressure_mtx);
	for (i = 0; i < PRESSURE_MAX_JAILS; i++) {
		if (!pressure_jails[i].used)
			continue;
		v = pressure_jails[i].some_ns;
		if (pressure_jails[i].nstalled > 0) {
			now = sbinuptime();
			if (now > pressure_jails[i].some_since)
				v += (uint64_t)
				    sbttons(now - pressure_jails[i].some_since);
		}
		/*
		 * `full` is definitionally a subset of `some` (both need a
		 * staller; full adds "nothing of this jail ran"). But `some` is
		 * measured precisely at the stall transitions while `full` is
		 * Riemann-sampled at the sched cadence, so a single-threaded jail
		 * whose stalls are shorter than a sample can push the sampled
		 * full_ns a tick past some_ns. Clamp to keep full <= some.
		 */
		fv = pressure_jails[i].full_ns;
		if (fv > v)
			fv = v;
		sbuf_printf(&sb,
		    "jail %d some_ns=%ju full_ns=%ju full_avg10=%d full_avg60=%d full_avg300=%d\n",
		    pressure_jails[i].jid, (uintmax_t)v, (uintmax_t)fv,
		    (int)((uint64_t)pressure_jails[i].full_avg10 * 10000 / PRESSURE_FIXED_1),
		    (int)((uint64_t)pressure_jails[i].full_avg60 * 10000 / PRESSURE_FIXED_1),
		    (int)((uint64_t)pressure_jails[i].full_avg300 * 10000 / PRESSURE_FIXED_1));
	}
	mtx_unlock(&pressure_mtx);
	error = sbuf_finish(&sb);
	sbuf_delete(&sb);
	return (error);
}
SYSCTL_PROC(_kern_pressure_memory, OID_AUTO, jails,
    CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_MPSAFE, NULL, 0,
    pressure_jails_sysctl, "A",
    "Per-jail memory stall 'some' (jid some_ns) — federation-member granularity");

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
