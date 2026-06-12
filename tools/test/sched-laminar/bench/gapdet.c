/*
 * gapdet: userspace execution-gap detector (the Laminar-only-stall probe).
 *
 * An rtprio REALTIME tight loop samples CLOCK_MONOTONIC and records every
 * gap between consecutive samples above a threshold.  Pinned to one CPU
 * (cpuset -l N), gaps can only be (a) host/hypervisor descheduling of the
 * vCPU or (b) the guest CPU stuck with interrupts off — long interrupt
 * handlers or spinning on a held spin mutex.  Identical runs on Laminar
 * and ULE boots under identical load separate host artifacts from
 * scheduler-side sections: gaps on both = host; gaps only on Laminar =
 * Laminar's kernel paths.
 *
 * usage: gapdet <duration_s> [thresh_us] [n_spinners]
 */

#include <sys/types.h>
#include <sys/rtprio.h>
#include <sys/wait.h>

#include <err.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define	MAXGAPS	64

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
	double duration, thresh_us = 100.0;
	int n_spin = 0, i;

	if (argc < 2 || argc > 4)
		errx(1, "usage: %s <duration_s> [thresh_us] [n_spinners]",
		    argv[0]);
	duration = atof(argv[1]);
	if (argc >= 3)
		thresh_us = atof(argv[2]);
	if (argc == 4)
		n_spin = atoi(argv[3]);

	for (i = 0; i < n_spin; i++) {
		pid_t p = fork();
		if (p < 0) err(1, "fork");
		if (p == 0) spinner_loop(now_sec() + duration + 1.0);
	}

	struct rtprio rtp = { RTP_PRIO_REALTIME, 0 };
	if (rtprio(RTP_SET, 0, &rtp) != 0)
		err(1, "rtprio (need root)");

	double gaps[MAXGAPS];
	double when[MAXGAPS];
	int n_gaps = 0;
	uint64_t total_over = 0;
	double worst = 0;

	double start = now_sec(), prev = start, end = start + duration;
	for (;;) {
		double now = now_sec();
		double gap_us = (now - prev) * 1e6;

		if (gap_us > thresh_us) {
			total_over++;
			if (gap_us > worst)
				worst = gap_us;
			if (n_gaps < MAXGAPS) {
				gaps[n_gaps] = gap_us;
				when[n_gaps] = now - start;
				n_gaps++;
			}
		}
		prev = now;
		if (now >= end)
			break;
	}

	printf("gapdet dur=%.0fs thresh=%.0fus spin=%d: gaps_over=%llu worst_us=%.0f\n",
	    duration, thresh_us, n_spin, (unsigned long long)total_over,
	    worst);
	for (i = 0; i < n_gaps; i++)
		printf("  t=%7.3fs gap=%9.0fus\n", when[i], gaps[i]);

	while (wait(NULL) > 0)
		;
	return (0);
}
