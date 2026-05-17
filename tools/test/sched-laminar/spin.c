/*
 * spin: fork N children, each spins for `duration' seconds (wall),
 * each child reports its iteration count (work done) back via a
 * dedicated pipe.  Parent collects them and prints summary stats.
 *
 * Iteration count is used instead of rusage CPU time because
 * FreeBSD's tick-based CPU accounting over-attributes under
 * scheduling contention (whichever process is running at the
 * 10ms tick boundary gets the whole tick credited), which inflates
 * "total CPU" beyond the physical core-seconds available.
 * Iteration count is the actual work done -- ground-truth.
 *
 * For comparison, the rusage CPU time is still printed but marked.
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

static unsigned long long
child_spin(double duration)
{
	double deadline;
	volatile unsigned long acc = 0;
	unsigned long long iters = 0;

	deadline = now_sec() + duration;
	while (now_sec() < deadline) {
		for (int i = 0; i < 100000; i++)
			acc += i;
		iters++;
	}
	return (iters);
}

int
main(int argc, char **argv)
{
	int n, i, nicev = 0;
	double duration;
	pid_t *kids;
	int *fds;
	unsigned long long *iters;
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
	fds = calloc(n * 2, sizeof(*fds));
	iters = calloc(n, sizeof(*iters));
	cpu = calloc(n, sizeof(*cpu));
	if (!kids || !fds || !iters || !cpu)
		err(1, "calloc");

	for (i = 0; i < n; i++) {
		if (pipe(&fds[i * 2]) < 0)
			err(1, "pipe");
		pid_t p = fork();
		if (p < 0)
			err(1, "fork");
		if (p == 0) {
			close(fds[i * 2]);	/* read end */
			if (nicev != 0)
				(void)nice(nicev);
			unsigned long long it = child_spin(duration);
			(void)write(fds[i * 2 + 1], &it, sizeof(it));
			_exit(0);
		}
		close(fds[i * 2 + 1]);		/* parent doesn't write */
		kids[i] = p;
	}

	for (i = 0; i < n; i++) {
		pid_t p = wait4(-1, &status, 0, &ru);
		if (p < 0)
			err(1, "wait4");
		int slot = -1;
		for (int j = 0; j < n; j++) {
			if (kids[j] == p) { slot = j; break; }
		}
		if (slot < 0)
			continue;
		cpu[slot] = tv_to_sec(&ru.ru_utime) + tv_to_sec(&ru.ru_stime);
		(void)read(fds[slot * 2], &iters[slot], sizeof(iters[slot]));
		close(fds[slot * 2]);
	}

	/* Stats on iterations (the true work metric). */
	double sum_it = 0, sq_it = 0;
	unsigned long long mn = iters[0], mx = iters[0];
	double sum_cpu = 0;
	for (i = 0; i < n; i++) {
		sum_it += iters[i];
		sq_it += (double)iters[i] * iters[i];
		if (iters[i] < mn) mn = iters[i];
		if (iters[i] > mx) mx = iters[i];
		sum_cpu += cpu[i];
	}
	double mean_it = sum_it / n;
	double var_it = sq_it / n - mean_it * mean_it;
	double sd_it = var_it > 0 ? sqrt(var_it) : 0;

	printf("n=%d duration=%.2fs nice=%d\n", n, duration, nicev);
	printf("per-child iters:\n");
	for (i = 0; i < n; i++)
		printf("  child %d: iters=%llu  cpu=%.3fs (tick-biased)\n",
		    i, iters[i], cpu[i]);
	printf("summary: min=%llu max=%llu mean=%.0f sd=%.0f spread=%llu (%.1f%% of mean)\n",
	    mn, mx, mean_it, sd_it, mx - mn,
	    mean_it > 0 ? 100.0 * (double)(mx - mn) / mean_it : 0.0);
	printf("aggregate: total_iters=%.0f total_cpu_s=%.2f (tick-biased; use iters)\n",
	    sum_it, sum_cpu);
	return (0);
}
