/*
 * metronome-posix: the lane A/B counterpart of metronome.c that runs on ANY
 * scheduler (no /dev/laminar).  Sleeps to an absolute period grid with
 * clock_nanosleep(TIMER_ABSTIME), does work_us of busy work per period, and
 * reports wake-lateness percentiles vs the grid plus deadline misses (work
 * not finished by period end).  Optional final arg "rt" uses rtprio
 * REALTIME — ULE's best latency mechanism, the honest baseline to beat.
 *
 * usage: metronome-posix <t_us> <work_us> <n_periods> [n_spinners] [rt]
 *   audio shape:  metronome-posix 2700 1000 10000 16
 *   vs lane:      metronome 1200 2700 1000 10000 16
 */

#include <sys/types.h>
#include <sys/rtprio.h>
#include <sys/wait.h>

#include <err.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double
now_sec(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((double)ts.tv_sec + (double)ts.tv_nsec / 1e9);
}

static int
cmp_double(const void *a, const void *b)
{
	double da = *(const double *)a, db = *(const double *)b;

	return ((da < db) ? -1 : (da > db) ? 1 : 0);
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
	uint64_t t_us, work_us, n_periods;
	int n_spin = 0, use_rt = 0, i;

	if (argc < 4 || argc > 6)
		errx(1, "usage: %s <t_us> <work_us> <n_periods> [n_spinners] [rt]",
		    argv[0]);
	t_us = strtoull(argv[1], NULL, 10);
	work_us = strtoull(argv[2], NULL, 10);
	n_periods = strtoull(argv[3], NULL, 10);
	if (argc >= 5)
		n_spin = atoi(argv[4]);
	if (argc == 6 && strcmp(argv[5], "rt") == 0)
		use_rt = 1;

	double dur = (double)n_periods * t_us / 1e6 + 2.0;
	for (i = 0; i < n_spin; i++) {
		pid_t p = fork();
		if (p < 0) err(1, "fork");
		if (p == 0) spinner_loop(now_sec() + dur);
	}

	if (use_rt) {
		struct rtprio rtp = { RTP_PRIO_REALTIME, 0 };

		if (rtprio(RTP_SET, 0, &rtp) != 0)
			err(1, "rtprio (need root)");
	}

	double *late = calloc(n_periods, sizeof(*late));
	uint64_t misses = 0;
	struct timespec grid;

	clock_gettime(CLOCK_MONOTONIC, &grid);
	double t0 = (double)grid.tv_sec + (double)grid.tv_nsec / 1e9;
	for (uint64_t p = 0; p < n_periods; p++) {
		/* advance the absolute grid by one period */
		grid.tv_nsec += (long)(t_us * 1000);
		while (grid.tv_nsec >= 1000000000L) {
			grid.tv_nsec -= 1000000000L;
			grid.tv_sec++;
		}
		while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &grid,
		    NULL) != 0)
			;
		double gp = (double)(p + 1) * t_us / 1e6;
		late[p] = (now_sec() - t0 - gp) * 1e6;
		if (late[p] < 0)
			late[p] = 0;
		double until = t0 + gp + (double)work_us / 1e6;
		volatile unsigned long acc = 0;
		while (now_sec() < until)
			acc += p;
		/* miss = work not done by period end */
		if (now_sec() - t0 > gp + (double)t_us / 1e6)
			misses++;
	}

	qsort(late, n_periods, sizeof(*late), cmp_double);
	printf("metronome-posix t=%llu work=%llu n=%llu spin=%d%s: misses=%llu\n",
	    (unsigned long long)t_us, (unsigned long long)work_us,
	    (unsigned long long)n_periods, n_spin, use_rt ? " RT" : "",
	    (unsigned long long)misses);
	printf("wake_late_us p50=%.1f p90=%.1f p99=%.1f p99.9=%.1f max=%.1f\n",
	    late[n_periods / 2], late[(int)(n_periods * 0.90)],
	    late[(int)(n_periods * 0.99)], late[(int)(n_periods * 0.999)],
	    late[n_periods - 1]);

	while (wait(NULL) > 0)
		;
	return (misses == 0 ? 0 : 1);
}
