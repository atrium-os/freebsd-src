/*
 * wakelat: measure wake-from-sleep scheduling latency under load.
 *
 * Fork B background CPU-spinners + W "watchdog" workers.  Each
 * watchdog repeats: nanosleep(SLEEP_US), record monotonic time,
 * compare to the expected wake time.  The DELTA between actual
 * wake and intended wake is the scheduler's wake latency.
 *
 * We report p50, p90, p99 of the per-wake delta over the run.
 * Spinners create scheduling pressure so wake latency is the
 * interesting measurement, not just nanosleep's own jitter.
 *
 * usage: wakelat <n_spinners> <n_watchdogs> <duration_seconds> [sleep_us]
 */

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

static void
watchdog_loop(double duration, int sleep_us, int fd_out)
{
	int max_samples = (int)(duration * 1e6 / sleep_us) + 16;
	double *deltas = calloc(max_samples, sizeof(*deltas));
	struct timespec req = {
		.tv_sec = sleep_us / 1000000,
		.tv_nsec = (sleep_us % 1000000) * 1000,
	};
	int n = 0;
	double end = now_sec() + duration;

	while (now_sec() < end && n < max_samples) {
		double expect = now_sec() + sleep_us / 1e6;
		nanosleep(&req, NULL);
		double actual = now_sec();
		double delta_us = (actual - expect) * 1e6;
		if (delta_us < 0)
			delta_us = 0;
		deltas[n++] = delta_us;
	}
	(void)write(fd_out, &n, sizeof(n));
	(void)write(fd_out, deltas, n * sizeof(*deltas));
	free(deltas);
	_exit(0);
}

int
main(int argc, char **argv)
{
	int n_spin, n_watch, sleep_us = 1000;	/* default 1ms */
	double duration;
	pid_t *kids;
	int *pipes;
	int i;

	if (argc < 4 || argc > 5)
		errx(1, "usage: %s <n_spinners> <n_watchdogs> <duration_s> [sleep_us]",
		    argv[0]);
	n_spin = atoi(argv[1]);
	n_watch = atoi(argv[2]);
	duration = atof(argv[3]);
	if (argc == 5)
		sleep_us = atoi(argv[4]);
	if (n_spin < 0 || n_watch <= 0 || duration <= 0 || sleep_us <= 0)
		errx(1, "bad args");

	kids = calloc(n_spin + n_watch, sizeof(*kids));
	pipes = calloc(n_watch * 2, sizeof(*pipes));

	for (i = 0; i < n_spin; i++) {
		pid_t p = fork();
		if (p < 0) err(1, "fork");
		if (p == 0) spinner_loop(now_sec() + duration);
		kids[i] = p;
	}
	for (i = 0; i < n_watch; i++) {
		if (pipe(&pipes[i * 2]) < 0) err(1, "pipe");
		pid_t p = fork();
		if (p < 0) err(1, "fork");
		if (p == 0) {
			close(pipes[i * 2]);
			watchdog_loop(duration, sleep_us, pipes[i * 2 + 1]);
		}
		close(pipes[i * 2 + 1]);
		kids[n_spin + i] = p;
	}

	/* Collect all wake-latency samples. */
	int total = 0;
	int per_w[n_watch];
	double *all = NULL;
	for (i = 0; i < n_watch; i++) {
		int n;
		if (read(pipes[i * 2], &n, sizeof(n)) != sizeof(n))
			continue;
		per_w[i] = n;
		all = realloc(all, (total + n) * sizeof(*all));
		size_t want = (size_t)n * sizeof(*all);
		char *buf = (char *)(all + total);
		while (want > 0) {
			ssize_t r = read(pipes[i * 2], buf, want);
			if (r <= 0)
				break;
			buf += r;
			want -= (size_t)r;
		}
		if (want != 0)
			break;
		total += n;
	}
	for (i = 0; i < n_spin + n_watch; i++)
		(void)waitpid(kids[i], NULL, 0);

	if (total == 0)
		errx(1, "no samples");
	qsort(all, total, sizeof(*all), cmp_double);
	double p50 = all[total / 2];
	double p90 = all[(int)(total * 0.90)];
	double p99 = all[(int)(total * 0.99)];
	double p999 = all[(int)(total * 0.999)];
	double mx = all[total - 1];

	printf("spinners=%d watchdogs=%d sleep_us=%d duration=%.2fs samples=%d\n",
	    n_spin, n_watch, sleep_us, duration, total);
	printf("wake_latency_us p50=%.1f p90=%.1f p99=%.1f p99.9=%.1f max=%.1f\n",
	    p50, p90, p99, p999, mx);
	free(all);
	return (0);
}
