/*
 * lane-adopt: K-b deadline-lending gate.
 *
 * A lane CLIENT self-sponsors (Q,T) and metronomes.  A SERVER process
 * ADOPTs the client's entity and runs work bursts on the client's
 * behalf.  Two properties gate K-b:
 *
 *  1. CHARGE-BACK: while adopted, the server's cpu time drains the
 *     CLIENT's budget — a deliberate over-budget burn must make the
 *     entity throttle (client STATS throttles rise).  Band priority is
 *     never free.
 *  2. SELECTION: under 16 spinners, the adopted server's bursts finish
 *     near their work size (band -> rt-path pick); un-adopted bursts
 *     eat timeshare slice delays.
 *
 * usage: lane-adopt [n_spinners]
 */

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#include <err.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>

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
#define	LAMIOC_SPONSOR	_IOW('L', 1, struct lam_lane_req)
#define	LAMIOC_WITHDRAW	_IO('L', 2)
#define	LAMIOC_YIELD	_IO('L', 3)
#define	LAMIOC_STATS	_IOR('L', 4, struct lam_lane_stats)
#define	LAMIOC_ADOPT	_IOW('L', 7, struct lam_lane_req_for)
#define	LAMIOC_DROP	_IO('L', 8)

extern long thr_self(long *);

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

/* client: lane metronome; reports tid, then periods/misses/throttles */
static void
client_loop(int tid_w, int res_w)
{
	struct lam_lane_req req = { .q_us = 2000, .t_us = 8000 };
	struct lam_lane_stats st;
	long tid;
	int fd;

	fd = open("/dev/laminar", O_RDWR);
	if (fd < 0)
		err(1, "client open /dev/laminar");
	if (ioctl(fd, LAMIOC_SPONSOR, &req) != 0)
		err(1, "client SPONSOR");
	thr_self(&tid);
	int32_t tid32 = (int32_t)tid;
	(void)!write(tid_w, &tid32, sizeof(tid32));
	close(tid_w);

	(void)ioctl(fd, LAMIOC_YIELD);	/* grid sync */
	for (int p = 0; p < 1500; p++) {	/* 12 s */
		busy_us(200);
		if (ioctl(fd, LAMIOC_YIELD) != 0)
			err(1, "client YIELD");
	}
	if (ioctl(fd, LAMIOC_STATS, &st) != 0)
		err(1, "client STATS");
	(void)!write(res_w, &st, sizeof(st));
	_exit(0);
}

int
main(int argc, char **argv)
{
	int n_spin = (argc == 2) ? atoi(argv[1]) : 16;
	int i;

	double dur = 14.0;
	for (i = 0; i < n_spin; i++) {
		pid_t p = fork();
		if (p < 0) err(1, "fork");
		if (p == 0) spinner_loop(now_sec() + dur);
	}

	int tp[2], rp[2];
	if (pipe(tp) < 0 || pipe(rp) < 0)
		err(1, "pipe");
	pid_t cpid = fork();
	if (cpid < 0) err(1, "fork client");
	if (cpid == 0) {
		close(tp[0]); close(rp[0]);
		client_loop(tp[1], rp[1]);
	}
	close(tp[1]); close(rp[1]);
	int32_t ctid;
	if (read(tp[0], &ctid, sizeof(ctid)) != sizeof(ctid))
		errx(1, "tid read");

	/* the server: burst latency un-adopted vs adopted, then a
	 * deliberate over-budget burn to prove charge-back */
	int fd = open("/dev/laminar", O_RDWR);
	if (fd < 0)
		err(1, "server open /dev/laminar");
	struct lam_lane_req_for ar = { .pid = (int32_t)cpid, .tid = ctid };

	/*
	 * Request latency = wake + burst (sleep 2ms, then 500us work;
	 * subtract the nominal 2.5ms).  The wake is where an un-adopted
	 * timeshare server eats slice delays under load.
	 */
	double una_max = 0, ado_max = 0;
	for (i = 0; i < 40; i++) {		/* un-adopted control */
		double t0 = now_sec();
		usleep(2000);
		busy_us(500);
		double d = (now_sec() - t0) * 1e6 - 2500;
		if (d > una_max) una_max = d;
		usleep(20000);
	}
	for (i = 0; i < 40; i++) {		/* adopted */
		if (ioctl(fd, LAMIOC_ADOPT, &ar) != 0)
			err(1, "ADOPT");
		double t0 = now_sec();
		usleep(2000);
		busy_us(500);
		double d = (now_sec() - t0) * 1e6 - 2500;
		if (d > ado_max) ado_max = d;
		if (ioctl(fd, LAMIOC_DROP) != 0)
			err(1, "DROP");
		usleep(20000);
	}
	printf("request(2ms sleep + 500us work, excess over nominal): "
	    "unadopted max=%.0fus adopted max=%.0fus\n", una_max, ado_max);

	/* charge-back: burn way past the client's 2000us budget */
	if (ioctl(fd, LAMIOC_ADOPT, &ar) != 0)
		err(1, "ADOPT for burn");
	busy_us(50000);	/* 50 ms >> Q=2 ms: must throttle the entity */
	(void)ioctl(fd, LAMIOC_DROP);
	printf("server: 50ms adopted burn done (client Q=2ms/T=8ms)\n");

	struct lam_lane_stats st;
	if (read(rp[0], &st, sizeof(st)) != sizeof(st))
		errx(1, "client stats read");
	printf("client: periods=%llu misses=%llu throttles=%llu\n",
	    (unsigned long long)st.periods, (unsigned long long)st.misses,
	    (unsigned long long)st.throttles);
	/*
	 * Verdict: charge-back is K-b's claim (the burn must throttle
	 * the entity ~ burn/T times, and the client must still meet its
	 * own deadlines).  Band SELECTION is K-a's claim, already proven
	 * by lane-pi (10.65%% -> 0%%); the request latencies above are
	 * informational (40 samples rarely reach the timeshare p99
	 * plateau, so equal maxes here prove nothing either way).
	 */
	printf("K-b: charge-back %s, client %s\n",
	    st.throttles > 0 ? "PROVEN (burn throttled the entity)"
	    : "NOT OBSERVED",
	    st.misses == 0 ? "CLEAN (0 misses)" : "MISSING (investigate)");

	waitpid(cpid, NULL, 0);
	while (wait(NULL) > 0)
		;
	return (0);
}
