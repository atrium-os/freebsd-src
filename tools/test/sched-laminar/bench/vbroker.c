/*
 * vbroker: phase-J broker test — a mock frescod.
 *
 * The broker (root) forks N client processes; each client opens
 * /dev/laminar for YIELD/STATS only and reports its tid over a pipe.
 * The broker SPONSOR_FORs every client thread with a frame-shaped
 * (Q,T) anchored to a shared "vblank" timestamp, so all clients tick
 * the same grid.  Clients do work_us busy work per frame and YIELD.
 * Gate: every surviving client reports 0 misses under spinner load.
 *
 * kill mode ("kill" arg): SIGKILLs client 0 mid-run — the thread-dtor
 * invalidation path must reclaim its entity with no panic and no
 * effect on the other clients; the broker's WITHDRAW_FOR for the dead
 * client must fail cleanly (ESRCH).
 *
 * usage: vbroker <n_clients> <q_us> <t_us> <work_us> <n_periods>
 *                [n_spinners] [kill]
 *   frame shape: vbroker 2 4800 16667 4000 600 16
 */

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* mirror of the kernel ABI (sched_laminar.c) */
struct lam_lane_req {
	uint64_t q_us, t_us;
};
struct lam_lane_stats {
	uint64_t periods, misses, throttles, max_late_us;
	int32_t cpu, pad;
};
struct lam_lane_req_for {
	int32_t pid, tid;
	uint64_t q_us, t_us;
	uint64_t anchor_ns;
};
#define	LAMIOC_SPONSOR		_IOW('L', 1, struct lam_lane_req)
#define	LAMIOC_WITHDRAW		_IO('L', 2)
#define	LAMIOC_YIELD		_IO('L', 3)
#define	LAMIOC_STATS		_IOR('L', 4, struct lam_lane_stats)
#define	LAMIOC_SPONSOR_FOR	_IOW('L', 5, struct lam_lane_req_for)
#define	LAMIOC_WITHDRAW_FOR	_IOW('L', 6, struct lam_lane_req_for)

extern long thr_self(long *);

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

static void
client_loop(int idx, int tid_w, uint64_t work_us, uint64_t n_periods)
{
	struct lam_lane_stats st;
	long tid;
	int fd;

	fd = open("/dev/laminar", O_RDWR);
	if (fd < 0)
		err(1, "client open /dev/laminar");
	thr_self(&tid);
	int32_t tid32 = (int32_t)tid;
	if (write(tid_w, &tid32, sizeof(tid32)) != sizeof(tid32))
		err(1, "client tid write");
	close(tid_w);

	/* wait until the broker has sponsored us */
	for (int i = 0; i < 1000; i++) {
		if (ioctl(fd, LAMIOC_STATS, &st) == 0)
			break;
		usleep(2000);
	}
	/*
	 * Sync onto the period grid before working: the entity began
	 * ticking at sponsorship, mid-poll — yield away the partial
	 * first period (a real client waits for its first frame
	 * callback the same way).
	 */
	(void)ioctl(fd, LAMIOC_YIELD);
	uint64_t base_misses = 0;
	if (ioctl(fd, LAMIOC_STATS, &st) == 0)
		base_misses = st.misses;	/* pre-sync partial period */

	for (uint64_t p = 0; p < n_periods; p++) {
		double until = now_sec() + work_us / 1e6;
		volatile unsigned long acc = 0;
		while (now_sec() < until)
			acc += p;
		if (ioctl(fd, LAMIOC_YIELD) != 0)
			err(1, "client LAMIOC_YIELD");
	}

	if (ioctl(fd, LAMIOC_STATS, &st) != 0)
		err(1, "client LAMIOC_STATS");
	uint64_t misses = st.misses - base_misses;
	printf("client %d cpu=%d: periods=%llu misses=%llu (startup %llu) "
	    "throttles=%llu max_replenish_late_us=%llu\n", idx, st.cpu,
	    (unsigned long long)st.periods, (unsigned long long)misses,
	    (unsigned long long)base_misses, (unsigned long long)st.throttles,
	    (unsigned long long)st.max_late_us);
	fflush(stdout);
	_exit(misses == 0 ? 0 : 1);
}

int
main(int argc, char **argv)
{
	uint64_t q_us, t_us, work_us, n_periods;
	int n_clients, n_spin = 0, do_kill = 0, i;

	if (argc < 6 || argc > 8)
		errx(1, "usage: %s <n_clients> <q_us> <t_us> <work_us> "
		    "<n_periods> [n_spinners] [kill]", argv[0]);
	n_clients = atoi(argv[1]);
	q_us = strtoull(argv[2], NULL, 10);
	t_us = strtoull(argv[3], NULL, 10);
	work_us = strtoull(argv[4], NULL, 10);
	n_periods = strtoull(argv[5], NULL, 10);
	if (argc >= 7)
		n_spin = atoi(argv[6]);
	if (argc == 8 && strcmp(argv[7], "kill") == 0)
		do_kill = 1;
	if (n_clients < 1 || n_clients > 16)
		errx(1, "n_clients 1..16");

	double dur = (double)n_periods * t_us / 1e6 + 3.0;
	for (i = 0; i < n_spin; i++) {
		pid_t p = fork();
		if (p < 0) err(1, "fork spinner");
		if (p == 0) spinner_loop(now_sec() + dur);
	}

	pid_t cpid[16];
	int32_t ctid[16];
	int tid_r[16];

	for (i = 0; i < n_clients; i++) {
		int tp[2];
		if (pipe(tp) < 0) err(1, "pipe");
		pid_t p = fork();
		if (p < 0) err(1, "fork client");
		if (p == 0) {
			for (int j = 0; j < i; j++)
				close(tid_r[j]);
			close(tp[0]);
			client_loop(i, tp[1], work_us, n_periods);
		}
		close(tp[1]);
		tid_r[i] = tp[0];
		cpid[i] = p;
	}

	/* the broker: one fd owns all sponsorships */
	int bfd = open("/dev/laminar", O_RDWR);
	if (bfd < 0)
		err(1, "broker open /dev/laminar");

	/* synthetic vblank anchor: one timestamp, shared grid */
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	uint64_t anchor_ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;

	for (i = 0; i < n_clients; i++) {
		if (read(tid_r[i], &ctid[i], sizeof(ctid[i])) !=
		    sizeof(ctid[i]))
			errx(1, "tid read from client %d", i);
		struct lam_lane_req_for r = {
			.pid = (int32_t)cpid[i], .tid = ctid[i],
			.q_us = q_us, .t_us = t_us, .anchor_ns = anchor_ns,
		};
		if (ioctl(bfd, LAMIOC_SPONSOR_FOR, &r) != 0)
			err(1, "SPONSOR_FOR client %d (pid %d tid %d)", i,
			    (int)cpid[i], (int)ctid[i]);
		printf("broker: sponsored client %d (pid %d tid %d)\n", i,
		    (int)cpid[i], (int)ctid[i]);
	}
	fflush(stdout);

	if (do_kill) {
		/* mid-run SIGKILL: entity must be reclaimed by thread-dtor */
		usleep((useconds_t)(n_periods * t_us / 2));
		printf("broker: SIGKILL client 0 (pid %d)\n", (int)cpid[0]);
		fflush(stdout);
		kill(cpid[0], SIGKILL);
		usleep(100000);
		struct lam_lane_req_for r = {
			.pid = (int32_t)cpid[0], .tid = ctid[0],
		};
		int rc = ioctl(bfd, LAMIOC_WITHDRAW_FOR, &r);
		printf("broker: WITHDRAW_FOR dead client => %s\n",
		    rc == 0 ? "0 (unexpected)" : strerror(errno));
		fflush(stdout);
	}

	int fails = 0;
	for (i = 0; i < n_clients; i++) {
		int wst;
		waitpid(cpid[i], &wst, 0);
		if (do_kill && i == 0)
			continue;	/* killed by design */
		if (!WIFEXITED(wst) || WEXITSTATUS(wst) != 0)
			fails++;
	}
	close(bfd);	/* priv dtor sweeps any leftovers */
	while (wait(NULL) > 0)
		;
	printf("vbroker: clients=%d spin=%d%s => %s\n", n_clients, n_spin,
	    do_kill ? " kill-test" : "",
	    fails == 0 ? "ALL CLIENTS CLEAN" : "FAILURES");
	return (fails == 0 ? 0 : 1);
}
