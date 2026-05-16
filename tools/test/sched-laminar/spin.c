/*
 * spin: fork N children, each spins for `duration' seconds (wall),
 * parent collects per-child user+system CPU via wait4()/rusage and
 * prints summary statistics.  Used by fair.sh / skew.sh / bursty.sh.
 *
 * usage: spin <n_children> <duration_seconds> [nice]
 */

#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>

#include <err.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double
tv_to_sec(const struct timeval *tv)
{

	return ((double)tv->tv_sec + (double)tv->tv_usec / 1e6);
}

static double
now_sec(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((double)ts.tv_sec + (double)ts.tv_nsec / 1e9);
}

static void
child_spin(double duration)
{
	double deadline;
	volatile unsigned long acc = 0;

	deadline = now_sec() + duration;
	while (now_sec() < deadline) {
		/*
		 * Tight loop with a small back-check; volatile prevents
		 * the optimizer from collapsing.
		 */
		for (int i = 0; i < 100000; i++)
			acc += i;
	}
	_exit(0);
}

int
main(int argc, char **argv)
{
	int n, i, nicev = 0;
	double duration;
	pid_t *kids;
	double *cpu;
	int status;
	struct rusage ru;

	if (argc < 3 || argc > 4) {
		fprintf(stderr,
		    "usage: %s <n_children> <duration_seconds> [nice]\n",
		    argv[0]);
		return (1);
	}
	n = atoi(argv[1]);
	duration = atof(argv[2]);
	if (argc == 4)
		nicev = atoi(argv[3]);
	if (n <= 0 || duration <= 0)
		errx(1, "bad args");

	kids = calloc(n, sizeof(*kids));
	cpu = calloc(n, sizeof(*cpu));
	if (kids == NULL || cpu == NULL)
		err(1, "calloc");

	for (i = 0; i < n; i++) {
		pid_t p = fork();
		if (p < 0)
			err(1, "fork");
		if (p == 0) {
			if (nicev != 0)
				(void)nice(nicev);
			child_spin(duration);
		}
		kids[i] = p;
	}

	for (i = 0; i < n; i++) {
		pid_t p = wait4(-1, &status, 0, &ru);
		if (p < 0)
			err(1, "wait4");
		/* Find which slot. */
		for (int j = 0; j < n; j++) {
			if (kids[j] == p) {
				cpu[j] = tv_to_sec(&ru.ru_utime) +
				    tv_to_sec(&ru.ru_stime);
				break;
			}
		}
	}

	/* Stats. */
	double sum = 0, sq = 0, mn = cpu[0], mx = cpu[0];
	for (i = 0; i < n; i++) {
		sum += cpu[i];
		sq += cpu[i] * cpu[i];
		if (cpu[i] < mn) mn = cpu[i];
		if (cpu[i] > mx) mx = cpu[i];
	}
	double mean = sum / n;
	double var = sq / n - mean * mean;
	double sd = var > 0 ? sqrt(var) : 0;

	printf("n=%d duration=%.2fs nice=%d\n", n, duration, nicev);
	printf("per-child cpu:\n");
	for (i = 0; i < n; i++)
		printf("  child %d: %.3fs\n", i, cpu[i]);
	printf("summary: min=%.3f max=%.3f mean=%.3f sd=%.3f spread=%.3f (%.1f%%)\n",
	    mn, mx, mean, sd, mx - mn,
	    mean > 0 ? 100.0 * (mx - mn) / mean : 0.0);
	return (0);
}
