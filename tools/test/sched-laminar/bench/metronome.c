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

static double
now_sec(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((double)ts.tv_sec + (double)ts.tv_nsec / 1e9);
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

	fd = open("/dev/laminar", O_RDWR);
	if (fd < 0)
		err(1, "open /dev/laminar (deadline_enable=1?)");
	if (ioctl(fd, LAMIOC_SPONSOR, &req) != 0)
		err(1, "LAMIOC_SPONSOR");

	/* one period of work + yield, n_periods times */
	for (uint64_t p = 0; p < n_periods; p++) {
		double until = now_sec() + work_us / 1e6;
		volatile unsigned long acc = 0;
		while (now_sec() < until)
			acc += p;
		if (ioctl(fd, LAMIOC_YIELD) != 0)
			err(1, "LAMIOC_YIELD");
	}

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
