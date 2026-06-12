/*
 * piperlat: pipe-wake latency with a CALLOUT-FREE wake path (P0.3 round 3).
 *
 * An rtprio REALTIME ticker busy-waits on CLOCK_MONOTONIC (never sleeps — no
 * callout, no eventtimer dependence) and every TICK_US writes the current
 * timestamp into each watchdog's pipe.  Timeshare watchdogs block in read();
 * on return they record delta = now - sent.  That delta covers exactly
 * wakeup -> setrunnable -> enqueue -> pick -> on-cpu (+ pipe copy) — the
 * scheduler's wake path with the timer path amputated.
 *
 * vs wakelat (nanosleep = callout-driven): if wakelat shows multi-second maxes
 * and piperlat does not, the P0.3 artifact lives in the callout/eventtimer
 * path; if piperlat shows them too, it is the scheduler wake path.
 *
 * usage: piperlat <n_spinners> <n_watchdogs> <duration_s> [tick_us]
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

/*
 * Ticker: busy-wait to each tick, then write the send timestamp to all.
 * `use_rt` is optional — an RT busy-looper is its own experiment (it exposes
 * whether placement is blind to RT load); timestamps are stamped at actual
 * send either way, so watchdog deltas stay honest even if the ticker is late.
 */
static void
ticker_loop(double duration, int tick_us, int *wfds, int n, int use_rt)
{
	if (use_rt) {
		struct rtprio rtp = { RTP_PRIO_REALTIME, 0 };

		if (rtprio(RTP_SET, 0, &rtp) != 0)
			err(1, "rtprio (need root)");
	}

	double end = now_sec() + duration;
	double next = now_sec() + tick_us / 1e6;

	while (now_sec() < end) {
		while (now_sec() < next)
			;	/* busy wait: NO sleep, NO callout */
		double sent = now_sec();
		for (int i = 0; i < n; i++)
			(void)write(wfds[i], &sent, sizeof(sent));
		next += tick_us / 1e6;
	}
	for (int i = 0; i < n; i++)
		close(wfds[i]);
	_exit(0);
}

static void
watchdog_loop(int rfd, int fd_out)
{
	double sent, *deltas = NULL;
	int n = 0, cap = 0;

	while (read(rfd, &sent, sizeof(sent)) == sizeof(sent)) {
		double d_us = (now_sec() - sent) * 1e6;
		if (d_us < 0)
			d_us = 0;
		if (n == cap) {
			cap = cap ? cap * 2 : 4096;
			deltas = realloc(deltas, cap * sizeof(*deltas));
		}
		deltas[n++] = d_us;
	}
	(void)write(fd_out, &n, sizeof(n));
	(void)write(fd_out, deltas, n * sizeof(*deltas));
	_exit(0);
}

int
main(int argc, char **argv)
{
	int n_spin, n_watch, tick_us = 1000;
	double duration;
	int i;

	int use_rt = 0;

	if (argc < 4 || argc > 6)
		errx(1, "usage: %s <n_spinners> <n_watchdogs> <duration_s> [tick_us] [rt]",
		    argv[0]);
	n_spin = atoi(argv[1]);
	n_watch = atoi(argv[2]);
	duration = atof(argv[3]);
	if (argc >= 5)
		tick_us = atoi(argv[4]);
	if (argc == 6 && strcmp(argv[5], "rt") == 0)
		use_rt = 1;
	if (n_spin < 0 || n_watch <= 0 || duration <= 0 || tick_us <= 0)
		errx(1, "bad args");

	pid_t *kids = calloc(n_spin + n_watch + 1, sizeof(*kids));
	int *tick_w = calloc(n_watch, sizeof(*tick_w));
	int *res_r = calloc(n_watch, sizeof(*res_r));

	for (i = 0; i < n_spin; i++) {
		pid_t p = fork();
		if (p < 0) err(1, "fork");
		if (p == 0) spinner_loop(now_sec() + duration);
		kids[i] = p;
	}
	for (i = 0; i < n_watch; i++) {
		int tp[2], rp[2];
		if (pipe(tp) < 0 || pipe(rp) < 0) err(1, "pipe");
		pid_t p = fork();
		if (p < 0) err(1, "fork");
		if (p == 0) {
			/*
			 * Close every earlier watchdog's inherited fds — a later
			 * watchdog holding an earlier one's tick write-end breaks
			 * the EOF cascade and deadlocks against the parent's
			 * in-order result reads (learned the hard way).
			 */
			for (int j = 0; j < i; j++) {
				close(tick_w[j]);
				close(res_r[j]);
			}
			close(tp[1]); close(rp[0]);
			watchdog_loop(tp[0], rp[1]);
		}
		close(tp[0]); close(rp[1]);
		tick_w[i] = tp[1];
		res_r[i] = rp[0];
		kids[n_spin + i] = p;
	}
	{
		pid_t p = fork();
		if (p < 0) err(1, "fork");
		if (p == 0) ticker_loop(duration, tick_us, tick_w, n_watch, use_rt);
		kids[n_spin + n_watch] = p;
	}
	for (i = 0; i < n_watch; i++)
		close(tick_w[i]);

	int total = 0;
	double *all = NULL;
	for (i = 0; i < n_watch; i++) {
		int n;
		if (read(res_r[i], &n, sizeof(n)) != sizeof(n))
			continue;
		all = realloc(all, (total + n) * sizeof(*all));
		size_t want = (size_t)n * sizeof(*all);
		char *buf = (char *)(all + total);
		while (want > 0) {
			ssize_t r = read(res_r[i], buf, want);
			if (r <= 0)
				break;
			buf += r;
			want -= (size_t)r;
		}
		if (want != 0)
			break;
		total += n;
	}
	for (i = 0; i < n_spin + n_watch + 1; i++)
		(void)waitpid(kids[i], NULL, 0);

	if (total == 0)
		errx(1, "no samples");
	qsort(all, total, sizeof(*all), cmp_double);
	printf("spinners=%d watchdogs=%d tick_us=%d samples=%d\n",
	    n_spin, n_watch, tick_us, total);
	printf("pipe_wake_us p50=%.1f p90=%.1f p99=%.1f p99.9=%.1f max=%.1f\n",
	    all[total / 2], all[(int)(total * 0.90)], all[(int)(total * 0.99)],
	    all[(int)(total * 0.999)], all[total - 1]);
	return (0);
}
