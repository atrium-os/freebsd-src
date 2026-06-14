/*
 * metronome: the deadline-lane gate test (plan P2).
 *
 * Self-sponsors via /dev/laminar with (Q, T), then each period does
 * `work_us` of busy work and YIELDs (sleeps until the kernel's
 * replenishment callout).  Under background spinner load, an admitted
 * metronome must show ZERO misses (the P1 lane.rs proof, now on real
 * kernel) — and the kernel-side stats also expose the replenish
 * callout's worst lateness (the P0.3/R2 instrumentation).
 *
 * usage: metronome <q_us> <t_us> <work_us> <n_periods> [n_spinners]
 *   audio shape:  metronome 1200 2700 1000 1000 16
 *   frame shape:  metronome 4800 16200 4000 200 16
 *
 * q_us == 0 selects the PLAIN (no-lane) arm: the identical per-frame work,
 * paced to the same t_us grid with absolute sleeps, but on plain timeshare
 * with no reservation.  A miss is a frame whose work finishes after its
 * grid deadline (= not ready by its vblank = a dropped/judder frame),
 * counted in userspace.  This is the A/B control for the deadline lane:
 * same workload, same load, lane on vs off.  (Under load the starvation
 * misses dominate any HVF timer noise -- the L6 caveat.)
 */

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#include <err.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

/* mirror of the kernel's phase-I ioctl ABI (sched_laminar.c) */
struct lam_lane_req {
	uint64_t q_us, t_us;
};
struct lam_lane_stats {
	uint64_t periods, misses, throttles, max_late_us;
	int32_t cpu, pad;
};
#define	LAMIOC_SPONSOR	_IOW('L', 1, struct lam_lane_req)
#define	LAMIOC_WITHDRAW	_IO('L', 2)
#define	LAMIOC_YIELD	_IO('L', 3)
#define	LAMIOC_STATS	_IOR('L', 4, struct lam_lane_stats)

static int
cmp_double(const void *a, const void *b)
{
	double da = *(const double *)a, db = *(const double *)b;

	return ((da < db) ? -1 : (da > db) ? 1 : 0);
}

static double
now_sec(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((double)ts.tv_sec + (double)ts.tv_nsec / 1e9);
}

/*
 * A frame's "render" = consume `work_us` of actual THREAD CPU TIME (not a
 * wall-clock sleep, not a calibrated iteration count).  Measuring CPU time
 * directly is calibration-free and load-robust: the frame always costs the same
 * CPU, but under spinner load a starved thread takes longer WALL-CLOCK to
 * accumulate it, so the frame misses its vblank — the real jank.
 */
static volatile unsigned long g_sink;
static double
thread_cpu_sec(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
	return ((double)ts.tv_sec + (double)ts.tv_nsec / 1e9);
}
static void
do_frame_work(uint64_t work_us)
{
	double start = thread_cpu_sec();
	double budget = (double)work_us / 1e6;
	unsigned long a = 0;

	while (thread_cpu_sec() - start < budget) {
		for (int i = 0; i < 256; i++)
			a += (unsigned long)i * 2654435761u;
	}
	g_sink += a;
}

static void
spinner_loop(double deadline)
{
	volatile unsigned long acc = 0;

	while (now_sec() < deadline) {
		for (int i = 0; i < 100000; i++)
			acc += i;
	}
	_exit(0);
}

int
main(int argc, char **argv)
{
	struct lam_lane_req req;
	struct lam_lane_stats st;
	uint64_t work_us, n_periods;
	int fd, n_spin = 0, i;

	if (argc < 5 || argc > 6)
		errx(1, "usage: %s <q_us> <t_us> <work_us> <n_periods> [n_spinners]",
		    argv[0]);
	req.q_us = strtoull(argv[1], NULL, 10);
	req.t_us = strtoull(argv[2], NULL, 10);
	work_us = strtoull(argv[3], NULL, 10);
	n_periods = strtoull(argv[4], NULL, 10);
	if (argc == 6)
		n_spin = atoi(argv[5]);

	double dur = (double)n_periods * req.t_us / 1e6 + 2.0;
	for (i = 0; i < n_spin; i++) {
		pid_t p = fork();
		if (p < 0) err(1, "fork");
		if (p == 0) spinner_loop(now_sec() + dur);
	}

	/*
	 * PLAIN (no-lane) arm: q_us == 0.  Same per-frame work + grid pacing,
	 * but plain timeshare and no reservation.  Count, in userspace, frames
	 * whose work finishes past their grid deadline (not ready by vblank).
	 */
	if (req.q_us == 0) {
		double T = (double)req.t_us / 1e6;
		double base = now_sec();
		uint64_t misses = 0;
		long target = 1;	/* the vblank we are rendering toward */
		for (uint64_t p = 0; p < n_periods; p++) {
			do_frame_work(work_us);	/* render this frame */
			double now = now_sec();
			double deadline = base + (double)target * T;
			if (now <= deadline) {
				/* ready in time: wait for the vblank, present */
				double dl = deadline - now;
				struct timespec ts;
				ts.tv_sec = (time_t)dl;
				ts.tv_nsec = (long)((dl - (double)ts.tv_sec) * 1e9);
				nanosleep(&ts, NULL);
				target++;
			} else {
				/* missed: count the vblank(s) that passed with no
				 * fresh frame, then resync to the next one (no
				 * cascade — exactly what vsync pacing does). */
				long elapsed = (long)((now - base) / T);
				misses += (uint64_t)(elapsed - (target - 1));
				target = elapsed + 1;
			}
		}
		printf("metronome q=0 t=%llu work=%llu spin=%d cpu=-1: "
		    "periods=%lld misses=%llu throttles=0 max_replenish_late_us=0\n",
		    (unsigned long long)req.t_us, (unsigned long long)work_us,
		    n_spin, (long long)(target - 1), (unsigned long long)misses);
		while (wait(NULL) > 0)
			;
		return (misses == 0 ? 0 : 1);
	}

	fd = open("/dev/laminar", O_RDWR);
	if (fd < 0)
		err(1, "open /dev/laminar (deadline_enable=1?)");
	if (ioctl(fd, LAMIOC_SPONSOR, &req) != 0)
		err(1, "LAMIOC_SPONSOR");

	/* one period of work + yield, n_periods times; track wake lateness
	 * (YIELD-return vs the expected period grid). */
	double *late = calloc(n_periods, sizeof(*late));
	double t0 = 0;	/* grid anchor = FIRST wake (on the kernel grid) */
	for (uint64_t p = 0; p < n_periods; p++) {
		do_frame_work(work_us);
		if (ioctl(fd, LAMIOC_YIELD) != 0)
			err(1, "LAMIOC_YIELD");
		/*
		 * Grid-RESIDUAL jitter: a missed period leaves the user loop
		 * a whole number of periods behind the kernel grid forever
		 * after, so raw (now - p*T) measures accumulated offset, not
		 * wake quality.  Distance to the NEAREST grid point is the
		 * per-wake jitter regardless of drift.
		 */
		double T = (double)req.t_us / 1e6;
		if (p == 0) {
			t0 = now_sec();	/* first wake defines the grid phase */
			late[0] = 0;
			continue;
		}
		double l = now_sec() - t0 - (double)p * T;
		double r = l - (double)(long long)(l / T + (l < 0 ? -0.5 : 0.5)) * T;
		late[p] = (r < 0 ? -r : r) * 1e6;
	}
	qsort(late, n_periods, sizeof(*late), cmp_double);
	printf("wake_late_us p50=%.1f p90=%.1f p99=%.1f p99.9=%.1f max=%.1f\n",
	    late[n_periods / 2], late[(int)(n_periods * 0.90)],
	    late[(int)(n_periods * 0.99)], late[(int)(n_periods * 0.999)],
	    late[n_periods - 1]);

	if (ioctl(fd, LAMIOC_STATS, &st) != 0)
		err(1, "LAMIOC_STATS");
	if (ioctl(fd, LAMIOC_WITHDRAW) != 0)
		err(1, "LAMIOC_WITHDRAW");
	close(fd);

	printf("metronome q=%llu t=%llu work=%llu spin=%d cpu=%d: "
	    "periods=%llu misses=%llu throttles=%llu max_replenish_late_us=%llu\n",
	    (unsigned long long)req.q_us, (unsigned long long)req.t_us,
	    (unsigned long long)work_us, n_spin, st.cpu,
	    (unsigned long long)st.periods, (unsigned long long)st.misses,
	    (unsigned long long)st.throttles,
	    (unsigned long long)st.max_late_us);

	while (wait(NULL) > 0)
		;
	return (st.misses == 0 ? 0 : 1);
}
