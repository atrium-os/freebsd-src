/*
 * lane-pi: K-a deadline-inheritance gate (plan P4 stage K-a).
 *
 * Classic inversion shape: a LANE thread (audio-ish (Q,T)) must take a
 * shared mutex each period; a nice-20 HOLDER process grabs that mutex
 * for ~300us bursts; 16 spinners saturate the box.  Without
 * inheritance the holder — lowest priority on the system — sits
 * preempted INSIDE the critical section for whole slices while the
 * lane thread blocks: misses.  With the K-a band, the lane entity
 * lives at top-of-timeshare user priority, so a PTHREAD_PRIO_INHERIT
 * mutex lends that band to the holder via the existing umtx PI
 * machinery: the holder finishes the CS promptly and the lane thread
 * meets its deadlines.
 *
 *   lane-pi pi    <- PRIO_INHERIT mutex: expect ~0 misses
 *   lane-pi plain <- PRIO_NONE control:  expect misses >> 0
 *
 * usage: lane-pi <pi|plain> [t_us] [q_us] [n_periods] [n_spinners]
 */

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>

#include <err.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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
busy_us(double us)
{
	double until = now_sec() + us / 1e6;
	volatile unsigned long acc = 0;

	while (now_sec() < until)
		acc++;
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

/* nice-20 holder: short critical sections, mostly unlocked */
static void
holder_loop(pthread_mutex_t *m, double deadline)
{
	if (setpriority(PRIO_PROCESS, 0, 20) != 0)
		err(1, "setpriority");
	while (now_sec() < deadline) {
		pthread_mutex_lock(m);
		busy_us(300);
		pthread_mutex_unlock(m);
		busy_us(2000);
	}
	_exit(0);
}

int
main(int argc, char **argv)
{
	uint64_t t_us = 5000, q_us = 2000, n_periods = 2000;
	int n_spin = 16, i;

	if (argc < 2 || argc > 6)
		errx(1, "usage: %s <pi|plain> [t_us] [q_us] [n_periods] "
		    "[n_spinners]", argv[0]);
	int use_pi = strcmp(argv[1], "pi") == 0;
	if (argc >= 3) t_us = strtoull(argv[2], NULL, 10);
	if (argc >= 4) q_us = strtoull(argv[3], NULL, 10);
	if (argc >= 5) n_periods = strtoull(argv[4], NULL, 10);
	if (argc == 6) n_spin = atoi(argv[5]);

	/* shared PI (or plain) mutex */
	pthread_mutex_t *m = mmap(NULL, sizeof(*m), PROT_READ | PROT_WRITE,
	    MAP_SHARED | MAP_ANON, -1, 0);
	if (m == MAP_FAILED)
		err(1, "mmap");
	pthread_mutexattr_t ma;
	pthread_mutexattr_init(&ma);
	pthread_mutexattr_setpshared(&ma, PTHREAD_PROCESS_SHARED);
	pthread_mutexattr_setprotocol(&ma,
	    use_pi ? PTHREAD_PRIO_INHERIT : PTHREAD_PRIO_NONE);
	if (pthread_mutex_init(m, &ma) != 0)
		errx(1, "mutex init");

	double dur = (double)n_periods * t_us / 1e6 + 3.0;
	for (i = 0; i < n_spin; i++) {
		pid_t p = fork();
		if (p < 0) err(1, "fork");
		if (p == 0) spinner_loop(now_sec() + dur);
	}
	pid_t holder = fork();
	if (holder < 0) err(1, "fork holder");
	if (holder == 0) holder_loop(m, now_sec() + dur);

	/* lane thread: per period work + take the lock + yield */
	int fd = open("/dev/laminar", O_RDWR);
	if (fd < 0)
		err(1, "open /dev/laminar (deadline_enable=1?)");
	struct lam_lane_req req = { .q_us = q_us, .t_us = t_us };
	if (ioctl(fd, LAMIOC_SPONSOR, &req) != 0)
		err(1, "LAMIOC_SPONSOR");
	(void)ioctl(fd, LAMIOC_YIELD);	/* grid sync */
	struct lam_lane_stats base = { 0 };
	(void)ioctl(fd, LAMIOC_STATS, &base);

	for (uint64_t p = 0; p < n_periods; p++) {
		busy_us(200);
		pthread_mutex_lock(m);
		busy_us(200);
		pthread_mutex_unlock(m);
		if (ioctl(fd, LAMIOC_YIELD) != 0)
			err(1, "LAMIOC_YIELD");
	}

	struct lam_lane_stats st;
	if (ioctl(fd, LAMIOC_STATS, &st) != 0)
		err(1, "LAMIOC_STATS");
	(void)ioctl(fd, LAMIOC_WITHDRAW);
	uint64_t misses = st.misses - base.misses;
	printf("lane-pi %s: cpu=%d periods=%llu misses=%llu (%.2f%%) "
	    "throttles=%llu max_replenish_late_us=%llu\n",
	    use_pi ? "PI" : "PLAIN", st.cpu,
	    (unsigned long long)st.periods, (unsigned long long)misses,
	    100.0 * misses / (double)n_periods,
	    (unsigned long long)st.throttles,
	    (unsigned long long)st.max_late_us);

	kill(holder, SIGKILL);
	while (wait(NULL) > 0)
		;
	return (0);
}
