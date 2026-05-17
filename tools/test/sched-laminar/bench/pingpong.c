/*
 * pingpong: two processes ping-pong a 1-byte token over a pipe pair
 * for a fixed wall-clock duration.  Reports round-trip count, mean
 * round-trip latency, and (via the bench wrapper) whether the
 * scheduler co-located the pair on one CPU.
 *
 * Each round-trip: parent writes, child reads + writes back, parent
 * reads.  Both sides block in read() between turns -- exercising
 * the wakeup path which is what IPC affinity hooks into.
 *
 * usage: pingpong <duration_seconds>
 */

#include <sys/select.h>
#include <sys/time.h>
#include <sys/wait.h>

#include <err.h>
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

int
main(int argc, char **argv)
{
	int p2c[2], c2p[2];
	pid_t child;
	double duration, t0, t1;
	uint64_t n_round_trips = 0;
	char buf = 'x';

	if (argc != 2)
		errx(1, "usage: %s <duration_seconds>", argv[0]);
	duration = atof(argv[1]);
	if (duration <= 0)
		errx(1, "bad duration");

	if (pipe(p2c) < 0 || pipe(c2p) < 0)
		err(1, "pipe");

	child = fork();
	if (child < 0)
		err(1, "fork");
	if (child == 0) {
		/* Child: read from p2c, echo to c2p, forever. */
		close(p2c[1]);
		close(c2p[0]);
		for (;;) {
			if (read(p2c[0], &buf, 1) != 1)
				break;
			if (write(c2p[1], &buf, 1) != 1)
				break;
		}
		_exit(0);
	}
	close(p2c[0]);
	close(c2p[1]);

	t0 = now_sec();
	t1 = t0 + duration;
	while (now_sec() < t1) {
		if (write(p2c[1], &buf, 1) != 1)
			break;
		if (read(c2p[0], &buf, 1) != 1)
			break;
		n_round_trips++;
	}
	double elapsed = now_sec() - t0;
	/* Stop the child by closing the pipe. */
	close(p2c[1]);
	close(c2p[0]);
	(void)waitpid(child, NULL, 0);

	double rt_us = elapsed * 1e6 / (double)n_round_trips;
	printf("round_trips=%llu elapsed=%.3fs rt_mean_us=%.2f\n",
	    (unsigned long long)n_round_trips, elapsed, rt_us);
	return (0);
}
