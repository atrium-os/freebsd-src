/*
 * skew_pinned: like bench_skew, but each child is pinned to a
 * specific CPU so each CPU has an identical mix of nice classes.
 * Removes placement variance from the measurement, isolating the
 * picker's nice-weighting behaviour.
 *
 * For NCPU CPUs and the standard mix (4 each of nice -5/0/+5), pin
 * threads in round-robin so every CPU gets exactly one of each
 * nice level.  If the picker honours weights correctly, the
 * aggregate nice -5 : nice 0 : nice +5 ratio should match the
 * target (~3.05:1:0.33) regardless of how the unpinned bench
 * happens to distribute threads.
 *
 * Validates the conclusion of the picker-placer coupling
 * investigation: the picker is correct, the placer's incidental
 * distribution is what bench_skew's all-cpus path measures.
 *
 * usage: skew_pinned <duration_seconds>
 */

#include <sys/param.h>
#include <sys/types.h>
#include <sys/cpuset.h>
#include <sys/resource.h>
#include <sys/wait.h>

#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PER_CLASS 4
#define NCLASSES 3
static const int nice_vals[NCLASSES] = { -5, 0, 5 };

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

static int
pin_self(int cpu)
{
	cpuset_t mask;

	CPU_ZERO(&mask);
	CPU_SET(cpu, &mask);
	return (cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_PID, -1,
	    sizeof(mask), &mask));
}

int
main(int argc, char **argv)
{
	double duration;
	int ncpus, total;
	pid_t *kids;
	int *pipes;
	int class_idx[NCLASSES];
	unsigned long long class_iters[NCLASSES];
	int i, c, k;

	if (argc != 2) {
		fprintf(stderr, "usage: %s <duration_seconds>\n", argv[0]);
		return (1);
	}
	duration = atof(argv[1]);
	if (duration <= 0)
		errx(1, "bad duration");

	ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	if (ncpus <= 0)
		errx(1, "sysconf NPROCESSORS_ONLN");
	total = NCLASSES * PER_CLASS;
	kids = calloc(total, sizeof(*kids));
	pipes = calloc(total * 2, sizeof(*pipes));
	memset(class_iters, 0, sizeof(class_iters));

	/*
	 * Pin children round-robin so each CPU gets exactly one of each
	 * nice class (assumes PER_CLASS == ncpus; with PER_CLASS=4 and
	 * ncpus=4, this is one nice -5 + one nice 0 + one nice +5 per
	 * CPU).
	 */
	k = 0;
	for (c = 0; c < NCLASSES; c++)
		class_idx[c] = k, k += PER_CLASS;

	for (c = 0; c < NCLASSES; c++) {
		for (i = 0; i < PER_CLASS; i++) {
			int idx = class_idx[c] + i;
			int target_cpu = i % ncpus;
			pid_t p;

			if (pipe(&pipes[idx * 2]) < 0)
				err(1, "pipe");
			p = fork();
			if (p < 0)
				err(1, "fork");
			if (p == 0) {
				unsigned long long it;

				close(pipes[idx * 2]);
				if (pin_self(target_cpu) < 0)
					err(1, "cpuset_setaffinity");
				if (setpriority(PRIO_PROCESS, 0,
				    nice_vals[c]) < 0)
					err(1, "setpriority");
				it = child_spin(duration);
				(void)write(pipes[idx * 2 + 1], &it,
				    sizeof(it));
				_exit(0);
			}
			close(pipes[idx * 2 + 1]);
			kids[idx] = p;
		}
	}

	/* Collect. */
	for (c = 0; c < NCLASSES; c++) {
		for (i = 0; i < PER_CLASS; i++) {
			int idx = class_idx[c] + i;
			unsigned long long it = 0;

			if (read(pipes[idx * 2], &it, sizeof(it)) ==
			    sizeof(it))
				class_iters[c] += it;
		}
	}
	for (i = 0; i < total; i++)
		(void)waitpid(kids[i], NULL, 0);

	printf("skew_pinned ncpus=%d duration=%.1fs per_class=%d\n",
	    ncpus, duration, PER_CLASS);
	printf("nice -5: %llu\n", class_iters[0]);
	printf("nice  0: %llu\n", class_iters[1]);
	printf("nice +5: %llu\n", class_iters[2]);
	if (class_iters[1] > 0) {
		double r_neg = (double)class_iters[0] / class_iters[1];
		double r_pos = (double)class_iters[2] / class_iters[1];
		printf("ratio = %.2f:1.00:%.2f (target 3.05:1.00:0.33)\n",
		    r_neg, r_pos);
	}
	return (0);
}
