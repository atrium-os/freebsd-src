/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Atrium Project
 */

/*
 * Tests for the per-jail system call filter (sysfil.* jail parameters).
 *
 * Each test makes a persistent jail rooted at "/", forks, attaches the
 * child to the jail, and has the child report what the filter did through
 * its exit status.  The child uses only the system calls the test's filter
 * allows: _exit(2) is always on the list, and everything else it calls is
 * the thing being tested.
 */

#include <sys/param.h>
#include <sys/filio.h>
#include <sys/ioctl.h>
#include <sys/jail.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <jail.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <atf-c.h>

#define	BUFLEN	16384

static char val1[BUFLEN], val2[BUFLEN], val3[BUFLEN], val4[BUFLEN];

static void
remove_jail(const char *name)
{
	int jid;

	jid = jail_getid(name);
	if (jid > 0)
		(void)jail_remove(jid);
}

/* Fork, attach the child to the jail, run fn there, return wait status. */
static int
run_in_jail(int jid, int (*fn)(void))
{
	struct rlimit rl = { 0, 0 };
	pid_t pid;
	int status;

	pid = fork();
	ATF_REQUIRE(pid >= 0);
	if (pid == 0) {
		/* SIGSYS would otherwise leave a core file behind. */
		(void)setrlimit(RLIMIT_CORE, &rl);
		if (jail_attach(jid) != 0)
			_exit(100);
		_exit(fn());
	}
	ATF_REQUIRE(waitpid(pid, &status, 0) == pid);
	return (status);
}

static void
require_exit(int status, int code)
{

	ATF_REQUIRE_MSG(WIFEXITED(status), "child did not exit (status %#x)",
	    status);
	ATF_REQUIRE_MSG(WEXITSTATUS(status) == code,
	    "child exited %d, wanted %d", WEXITSTATUS(status), code);
}

static void
get1(const char *name, const char *param, char *buf)
{

	buf[0] = '\0';
	ATF_REQUIRE_MSG(jail_getv(0, "name", name, param, buf, NULL) > 0,
	    "jail_getv %s %s: %s", name, param, jail_errmsg);
}

/* ------------------------------------------------------------------ */

static int
child_deny(void)
{

	if (getpid() <= 0)
		return (1);
	errno = 0;
	if (getppid() != -1 || errno != ENOSYS)
		return (2);
	/* The indirect syscall(2) path must be filtered by the real code. */
	errno = 0;
	if (syscall(SYS_getppid) != -1 || errno != ENOSYS)
		return (3);
	errno = 0;
	if (__syscall(SYS_getppid) != -1 || errno != ENOSYS)
		return (4);
	return (0);
}

ATF_TC_WITH_CLEANUP(deny_syscall);
ATF_TC_HEAD(deny_syscall, tc)
{
	atf_tc_set_md_var(tc, "descr", "Mode 2 fails unlisted syscalls with ENOSYS");
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(deny_syscall, tc)
{
	int jid;

	jid = jail_setv(JAIL_CREATE, "name", "sysfil_deny", "path", "/",
	    "persist", NULL, "sysfil.mode", "2",
	    "sysfil.syscalls", "_exit,getpid", NULL);
	ATF_REQUIRE_MSG(jid > 0, "jail_setv: %s", jail_errmsg);
	require_exit(run_in_jail(jid, child_deny), 0);

	get1("sysfil_deny", "sysfil.seen.syscalls", val1);
	ATF_CHECK_STREQ(val1, "getppid");
	get1("sysfil_deny", "sysfil.denied", val2);
	ATF_CHECK_STREQ(val2, "3");
	get1("sysfil_deny", "sysfil.syscalls", val3);
	ATF_CHECK_STREQ(val3, "_exit,getpid");
	get1("sysfil_deny", "sysfil.mode", val4);
	ATF_CHECK_STREQ(val4, "2");
}
ATF_TC_CLEANUP(deny_syscall, tc)
{
	remove_jail("sysfil_deny");
}

/* ------------------------------------------------------------------ */

static int
child_log(void)
{

	return (getppid() > 0 ? 0 : 1);
}

ATF_TC_WITH_CLEANUP(log_mode);
ATF_TC_HEAD(log_mode, tc)
{
	atf_tc_set_md_var(tc, "descr", "Mode 1 allows and records");
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(log_mode, tc)
{
	int jid;

	jid = jail_setv(JAIL_CREATE, "name", "sysfil_log", "path", "/",
	    "persist", NULL, "sysfil.mode", "1",
	    "sysfil.syscalls", "_exit", NULL);
	ATF_REQUIRE_MSG(jid > 0, "jail_setv: %s", jail_errmsg);
	require_exit(run_in_jail(jid, child_log), 0);
	get1("sysfil_log", "sysfil.seen.syscalls", val1);
	ATF_CHECK_STREQ(val1, "getppid");
	get1("sysfil_log", "sysfil.denied", val2);
	ATF_CHECK_STREQ(val2, "1");
}
ATF_TC_CLEANUP(log_mode, tc)
{
	remove_jail("sysfil_log");
}

/* ------------------------------------------------------------------ */

static int
child_kill(void)
{

	(void)getppid();
	return (0);
}

ATF_TC_WITH_CLEANUP(kill_mode);
ATF_TC_HEAD(kill_mode, tc)
{
	atf_tc_set_md_var(tc, "descr", "Mode 3 sends SIGSYS");
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(kill_mode, tc)
{
	int jid, status;

	jid = jail_setv(JAIL_CREATE, "name", "sysfil_kill", "path", "/",
	    "persist", NULL, "sysfil.mode", "3",
	    "sysfil.syscalls", "_exit", NULL);
	ATF_REQUIRE_MSG(jid > 0, "jail_setv: %s", jail_errmsg);
	status = run_in_jail(jid, child_kill);
	ATF_REQUIRE_MSG(WIFSIGNALED(status) && WTERMSIG(status) == SIGSYS,
	    "status %#x", status);
}
ATF_TC_CLEANUP(kill_mode, tc)
{
	remove_jail("sysfil_kill");
}

/* ------------------------------------------------------------------ */

static int
child_fork(void)
{
	pid_t pid;
	int status;

	pid = fork();
	if (pid < 0)
		return (1);
	if (pid == 0) {
		errno = 0;
		_exit(getppid() == -1 && errno == ENOSYS ? 0 : 2);
	}
	if (wait4(pid, &status, 0, NULL) != pid)
		return (3);
	return (WIFEXITED(status) ? WEXITSTATUS(status) : 4);
}

ATF_TC_WITH_CLEANUP(fork_inherits);
ATF_TC_HEAD(fork_inherits, tc)
{
	atf_tc_set_md_var(tc, "descr", "A forked child keeps the filter");
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(fork_inherits, tc)
{
	int jid;

	jid = jail_setv(JAIL_CREATE, "name", "sysfil_fork", "path", "/",
	    "persist", NULL, "sysfil.mode", "2",
	    "sysfil.syscalls", "_exit,fork,wait4,wait6,sigprocmask,sigfastblock",
	    NULL);
	ATF_REQUIRE_MSG(jid > 0, "jail_setv: %s", jail_errmsg);
	require_exit(run_in_jail(jid, child_fork), 0);
}
ATF_TC_CLEANUP(fork_inherits, tc)
{
	remove_jail("sysfil_fork");
}

/* ------------------------------------------------------------------ */

static int
child_setuid(void)
{

	if (setuid(65534) != 0)
		return (1);
	errno = 0;
	return (getppid() == -1 && errno == ENOSYS ? 0 : 2);
}

ATF_TC_WITH_CLEANUP(setuid_keeps);
ATF_TC_HEAD(setuid_keeps, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "A credential change inside the jail keeps the filter");
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(setuid_keeps, tc)
{
	int jid;

	jid = jail_setv(JAIL_CREATE, "name", "sysfil_setuid", "path", "/",
	    "persist", NULL, "sysfil.mode", "2",
	    "sysfil.syscalls", "_exit,setuid", NULL);
	ATF_REQUIRE_MSG(jid > 0, "jail_setv: %s", jail_errmsg);
	require_exit(run_in_jail(jid, child_setuid), 0);
}
ATF_TC_CLEANUP(setuid_keeps, tc)
{
	remove_jail("sysfil_setuid");
}

/* ------------------------------------------------------------------ */

static int
child_ioctl(void)
{
	int fds[2], n, on;

	if (pipe(fds) != 0)
		return (1);
	if (ioctl(fds[0], FIONREAD, &n) != 0)
		return (2);
	on = 1;
	errno = 0;
	if (ioctl(fds[0], FIONBIO, &on) != -1 || errno != EPERM)
		return (3);
	/* Syscalls are unrestricted: no sysfil.syscalls was given. */
	if (getppid() <= 0)
		return (4);
	return (0);
}

ATF_TC_WITH_CLEANUP(ioctl_allowlist);
ATF_TC_HEAD(ioctl_allowlist, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Unlisted ioctls fail with EPERM; other classes stay open");
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(ioctl_allowlist, tc)
{
	char list[64], want[64];
	int jid;

	snprintf(list, sizeof(list), "%#lx", (u_long)FIONREAD);
	jid = jail_setv(JAIL_CREATE, "name", "sysfil_ioctl", "path", "/",
	    "persist", NULL, "sysfil.mode", "2",
	    "sysfil.ioctls", list, NULL);
	ATF_REQUIRE_MSG(jid > 0, "jail_setv: %s", jail_errmsg);
	require_exit(run_in_jail(jid, child_ioctl), 0);
	get1("sysfil_ioctl", "sysfil.seen.ioctls", val1);
	snprintf(want, sizeof(want), "%#lx", (u_long)FIONBIO);
	ATF_CHECK_STREQ(val1, want);
	get1("sysfil_ioctl", "sysfil.syscalls", val2);
	ATF_CHECK_STREQ(val2, "*");
}
ATF_TC_CLEANUP(ioctl_allowlist, tc)
{
	remove_jail("sysfil_ioctl");
}

/* ------------------------------------------------------------------ */

static int
child_sockopt(void)
{
	socklen_t len;
	int s, v;

	s = socket(AF_UNIX, SOCK_STREAM, 0);
	if (s < 0)
		return (1);
	len = sizeof(v);
	if (getsockopt(s, SOL_SOCKET, SO_SNDBUF, &v, &len) != 0)
		return (2);
	v = 4096;
	if (setsockopt(s, SOL_SOCKET, SO_SNDBUF, &v, sizeof(v)) != 0)
		return (3);
	errno = 0;
	if (setsockopt(s, SOL_SOCKET, SO_RCVBUF, &v, sizeof(v)) != -1 ||
	    errno != EPERM)
		return (4);
	len = sizeof(v);
	errno = 0;
	if (getsockopt(s, SOL_SOCKET, SO_RCVBUF, &v, &len) != -1 ||
	    errno != EPERM)
		return (5);
	return (0);
}

ATF_TC_WITH_CLEANUP(sockopt_allowlist);
ATF_TC_HEAD(sockopt_allowlist, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Unlisted socket options fail with EPERM, both directions");
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(sockopt_allowlist, tc)
{
	char list[64], want[64];
	int jid;

	snprintf(list, sizeof(list), "%#x:%d", SOL_SOCKET, SO_SNDBUF);
	jid = jail_setv(JAIL_CREATE, "name", "sysfil_sockopt", "path", "/",
	    "persist", NULL, "sysfil.mode", "2",
	    "sysfil.sockopts", list, NULL);
	ATF_REQUIRE_MSG(jid > 0, "jail_setv: %s", jail_errmsg);
	require_exit(run_in_jail(jid, child_sockopt), 0);
	get1("sysfil_sockopt", "sysfil.seen.sockopts", val1);
	snprintf(want, sizeof(want), "%#x:%d", SOL_SOCKET, SO_RCVBUF);
	ATF_CHECK_STREQ(val1, want);
}
ATF_TC_CLEANUP(sockopt_allowlist, tc)
{
	remove_jail("sysfil_sockopt");
}

/* ------------------------------------------------------------------ */

static int
child_family(void)
{
	int sv[2], s;

	s = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (s < 0)
		return (1);
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
		return (2);
	errno = 0;
	if (socket(PF_ROUTE, SOCK_RAW, 0) != -1 || errno != EAFNOSUPPORT)
		return (3);
	errno = 0;
	if (socket(AF_INET, SOCK_DGRAM, 0) != -1 || errno != EAFNOSUPPORT)
		return (4);
	return (0);
}

ATF_TC_WITH_CLEANUP(family_allowlist);
ATF_TC_HEAD(family_allowlist, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Unlisted address families fail with EAFNOSUPPORT");
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(family_allowlist, tc)
{
	int jid;

	jid = jail_setv(JAIL_CREATE, "name", "sysfil_family", "path", "/",
	    "persist", NULL, "sysfil.mode", "2",
	    "sysfil.families", "unix", NULL);
	ATF_REQUIRE_MSG(jid > 0, "jail_setv: %s", jail_errmsg);
	require_exit(run_in_jail(jid, child_family), 0);
	get1("sysfil_family", "sysfil.seen.families", val1);
	ATF_CHECK_STREQ(val1, "inet,route");	/* bit order */
	get1("sysfil_family", "sysfil.families", val2);
	ATF_CHECK_STREQ(val2, "unix");
}
ATF_TC_CLEANUP(family_allowlist, tc)
{
	remove_jail("sysfil_family");
}

/* ------------------------------------------------------------------ */

extern int __sys_shm_open2(const char *, int, mode_t, int, const char *);

static int
child_largepage(void)
{
	int fd;

	errno = 0;
	fd = __sys_shm_open2(SHM_ANON, O_RDWR | O_CREAT, 0600, SHM_LARGEPAGE,
	    NULL);
	if (fd != -1 || errno != EPERM)
		return (1);
	/* An ordinary anonymous object still works. */
	fd = __sys_shm_open2(SHM_ANON, O_RDWR | O_CREAT, 0600, 0, NULL);
	return (fd >= 0 ? 0 : 2);
}

ATF_TC_WITH_CLEANUP(largepage_gated);
ATF_TC_HEAD(largepage_gated, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Largepage shm objects need FIOSSHMLPGCNF in the ioctl list");
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(largepage_gated, tc)
{
	int jid;

	jid = jail_setv(JAIL_CREATE, "name", "sysfil_lp", "path", "/",
	    "persist", NULL, "sysfil.mode", "2",
	    "sysfil.ioctls", "", NULL);
	ATF_REQUIRE_MSG(jid > 0, "jail_setv: %s", jail_errmsg);
	require_exit(run_in_jail(jid, child_largepage), 0);
}
ATF_TC_CLEANUP(largepage_gated, tc)
{
	remove_jail("sysfil_lp");
}

/* ------------------------------------------------------------------ */

static int
child_narrow(void)
{

	if (getpid() <= 0)
		return (1);
	errno = 0;
	if (getppid() != -1 || errno != ENOSYS)	/* parent only */
		return (2);
	errno = 0;
	if (geteuid() != (uid_t)-1 || errno != ENOSYS)	/* child only */
		return (3);
	return (0);
}

ATF_TC_WITH_CLEANUP(inherit_narrow);
ATF_TC_HEAD(inherit_narrow, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "A child jail gets the intersection and the stricter mode");
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(inherit_narrow, tc)
{
	int jid;

	jid = jail_setv(JAIL_CREATE, "name", "sysfil_p", "path", "/",
	    "persist", NULL, "children.max", "2", "sysfil.mode", "2",
	    "sysfil.syscalls", "_exit,getpid,getppid",
	    "sysfil.ioctls", "0x1,0x2", NULL);
	ATF_REQUIRE_MSG(jid > 0, "jail_setv parent: %s", jail_errmsg);

	/* Asks for a weaker mode and a call the parent lacks. */
	jid = jail_setv(JAIL_CREATE, "name", "sysfil_p.c", "path", "/",
	    "persist", NULL, "sysfil.mode", "1",
	    "sysfil.syscalls", "_exit,getpid,geteuid", NULL);
	ATF_REQUIRE_MSG(jid > 0, "jail_setv child: %s", jail_errmsg);
	get1("sysfil_p.c", "sysfil.mode", val1);
	ATF_CHECK_STREQ(val1, "2");
	get1("sysfil_p.c", "sysfil.syscalls", val2);
	ATF_CHECK_STREQ(val2, "_exit,getpid");
	get1("sysfil_p.c", "sysfil.ioctls", val3);
	ATF_CHECK_STREQ(val3, "0x1,0x2");
	require_exit(run_in_jail(jid, child_narrow), 0);

	/* No sysfil parameters at all: an exact copy of the parent's. */
	jid = jail_setv(JAIL_CREATE, "name", "sysfil_p.d", "path", "/",
	    "persist", NULL, NULL);
	ATF_REQUIRE_MSG(jid > 0, "jail_setv plain child: %s", jail_errmsg);
	get1("sysfil_p.d", "sysfil.syscalls", val4);
	ATF_CHECK_STREQ(val4, "_exit,getpid,getppid");
}
ATF_TC_CLEANUP(inherit_narrow, tc)
{
	remove_jail("sysfil_p.c");
	remove_jail("sysfil_p.d");
	remove_jail("sysfil_p");
}

/* ------------------------------------------------------------------ */

ATF_TC_WITH_CLEANUP(immutable);
ATF_TC_HEAD(immutable, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "The filter cannot be added, changed or read-only params set later");
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(immutable, tc)
{
	int jid;

	jid = jail_setv(JAIL_CREATE, "name", "sysfil_imm", "path", "/",
	    "persist", NULL, "sysfil.mode", "2",
	    "sysfil.syscalls", "_exit", NULL);
	ATF_REQUIRE_MSG(jid > 0, "jail_setv: %s", jail_errmsg);
	ATF_CHECK_ERRNO(EINVAL, jail_setv(JAIL_UPDATE, "name", "sysfil_imm",
	    "sysfil.syscalls", "_exit,getpid", NULL) == -1);
	ATF_CHECK_ERRNO(EINVAL, jail_setv(JAIL_UPDATE, "name", "sysfil_imm",
	    "sysfil.mode", "1", NULL) == -1);

	jid = jail_setv(JAIL_CREATE, "name", "sysfil_imm2", "path", "/",
	    "persist", NULL, NULL);
	ATF_REQUIRE_MSG(jid > 0, "jail_setv: %s", jail_errmsg);
	ATF_CHECK_ERRNO(EINVAL, jail_setv(JAIL_UPDATE, "name", "sysfil_imm2",
	    "sysfil.mode", "2", NULL) == -1);
	/* Other parameters can still be updated. */
	ATF_CHECK(jail_setv(JAIL_UPDATE, "name", "sysfil_imm",
	    "host.hostname", "sysfil.example", NULL) > 0);
}
ATF_TC_CLEANUP(immutable, tc)
{
	remove_jail("sysfil_imm");
	remove_jail("sysfil_imm2");
}

/* ------------------------------------------------------------------ */

ATF_TC(bad_params);
ATF_TC_HEAD(bad_params, tc)
{
	atf_tc_set_md_var(tc, "descr", "Malformed filters are refused");
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(bad_params, tc)
{

	ATF_CHECK_ERRNO(EINVAL, jail_setv(JAIL_CREATE, "name", "sysfil_bad",
	    "path", "/", "persist", NULL, "sysfil.mode", "2",
	    "sysfil.syscalls", "_exit,no_such_call", NULL) == -1);
	ATF_CHECK_ERRNO(EINVAL, jail_setv(JAIL_CREATE, "name", "sysfil_bad",
	    "path", "/", "persist", NULL, "sysfil.mode", "4", NULL) == -1);
	ATF_CHECK_ERRNO(EINVAL, jail_setv(JAIL_CREATE, "name", "sysfil_bad",
	    "path", "/", "persist", NULL,
	    "sysfil.syscalls", "_exit", NULL) == -1);
	ATF_CHECK_ERRNO(EINVAL, jail_setv(JAIL_CREATE, "name", "sysfil_bad",
	    "path", "/", "persist", NULL, "sysfil.mode", "2",
	    "sysfil.sockopts", "65535", NULL) == -1);
	ATF_CHECK_ERRNO(EINVAL, jail_setv(JAIL_CREATE, "name", "sysfil_bad",
	    "path", "/", "persist", NULL, "sysfil.mode", "2",
	    "sysfil.families", "carrier-pigeon", NULL) == -1);
	/* Read-only; libjail may refuse it before the kernel does. */
	ATF_CHECK(jail_setv(JAIL_CREATE, "name", "sysfil_bad",
	    "path", "/", "persist", NULL, "sysfil.mode", "2",
	    "sysfil.denied", "0", NULL) == -1);
	ATF_CHECK(jail_getid("sysfil_bad") < 0);
}

/* ------------------------------------------------------------------ */

/*
 * What "jls -n" does: every jail parameter in one jail_get(2).  The kernel
 * caps a whole request at 64 KB, so the sysfil parameters' sizes must leave
 * room for the rest even when every list is as long as it can be.
 */
static void
get_all(const char *name)
{
	struct jailparam *jp;
	int i, n;

	n = jailparam_all(&jp);
	ATF_REQUIRE_MSG(n > 0, "jailparam_all: %s", jail_errmsg);
	for (i = 0; i < n; i++)
		if (strcmp(jp[i].jp_name, "name") == 0)
			break;
	ATF_REQUIRE(i < n);
	ATF_REQUIRE(jailparam_import(&jp[i], name) == 0);
	ATF_CHECK_MSG(jailparam_get(jp, n, 0) > 0, "jailparam_get %s: %s",
	    name, jail_errmsg);
	jailparam_free(jp, n);
	free(jp);
}

static char *
longest_lists(char **ioctls, char **sockopts)
{
	char *sys, *io, *so;
	size_t len;
	int i;

	len = 8 * SYS_MAXSYSCALL;
	sys = malloc(len);
	sys[0] = '\0';
	for (i = 0; i < SYS_MAXSYSCALL; i++)
		snprintf(sys + strlen(sys), len - strlen(sys), "%s%d",
		    i ? "," : "", i);
	io = malloc(256 * 12);
	io[0] = '\0';
	for (i = 0; i < 256; i++)
		snprintf(io + strlen(io), 256 * 12 - strlen(io), "%s%#x",
		    i ? "," : "", 0xfffff000u + i);
	so = malloc(256 * 24);
	so[0] = '\0';
	for (i = 0; i < 256; i++)
		snprintf(so + strlen(so), 256 * 24 - strlen(so), "%s%#x:%d",
		    i ? "," : "", 0x7fffff00 + i, 0x7fffff00 + i);
	*ioctls = io;
	*sockopts = so;
	return (sys);
}

ATF_TC_WITH_CLEANUP(all_params);
ATF_TC_HEAD(all_params, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "jls -n works on jails with no filter and with the longest lists; "
	    "lists over the limits are refused");
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(all_params, tc)
{
	char *sys, *io, *so, *big;
	int jid;

	jid = jail_setv(JAIL_CREATE, "name", "sysfil_all_open", "path", "/",
	    "persist", NULL, NULL);
	ATF_REQUIRE_MSG(jid > 0, "jail_setv: %s", jail_errmsg);
	get_all("sysfil_all_open");

	sys = longest_lists(&io, &so);
	jid = jail_setv(JAIL_CREATE, "name", "sysfil_all_full", "path", "/",
	    "persist", NULL, "sysfil.mode", "1", "sysfil.syscalls", sys,
	    "sysfil.ioctls", io, "sysfil.sockopts", so, "sysfil.families",
	    "0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,"
	    "25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47",
	    NULL);
	ATF_REQUIRE_MSG(jid > 0, "jail_setv: %s", jail_errmsg);
	get_all("sysfil_all_full");

	/* One more than the limit, and a command wider than 32 bits. */
	big = malloc(strlen(io) + 32);
	sprintf(big, "%s,0x1", io);
	ATF_CHECK_ERRNO(E2BIG, jail_setv(JAIL_CREATE, "name", "sysfil_all_bad",
	    "path", "/", "persist", NULL, "sysfil.mode", "2",
	    "sysfil.ioctls", big, NULL) == -1);
	ATF_CHECK_ERRNO(EINVAL, jail_setv(JAIL_CREATE, "name", "sysfil_all_bad",
	    "path", "/", "persist", NULL, "sysfil.mode", "2",
	    "sysfil.ioctls", "0x100000000", NULL) == -1);
	free(big);
	free(sys);
	free(io);
	free(so);
}
ATF_TC_CLEANUP(all_params, tc)
{
	remove_jail("sysfil_all_open");
	remove_jail("sysfil_all_full");
	remove_jail("sysfil_all_bad");
}

/* ------------------------------------------------------------------ */

static int
child_open(void)
{

	return (getppid() > 0 && geteuid() == 0 ? 0 : 1);
}

ATF_TC_WITH_CLEANUP(unfiltered);
ATF_TC_HEAD(unfiltered, tc)
{
	atf_tc_set_md_var(tc, "descr", "A jail without a filter is unchanged");
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(unfiltered, tc)
{
	int jid;

	jid = jail_setv(JAIL_CREATE, "name", "sysfil_none", "path", "/",
	    "persist", NULL, NULL);
	ATF_REQUIRE_MSG(jid > 0, "jail_setv: %s", jail_errmsg);
	require_exit(run_in_jail(jid, child_open), 0);
	get1("sysfil_none", "sysfil.mode", val1);
	ATF_CHECK_STREQ(val1, "0");
	get1("sysfil_none", "sysfil.syscalls", val2);
	ATF_CHECK_STREQ(val2, "*");
	get1("sysfil_none", "sysfil.seen.syscalls", val3);
	ATF_CHECK_STREQ(val3, "");
}
ATF_TC_CLEANUP(unfiltered, tc)
{
	remove_jail("sysfil_none");
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, deny_syscall);
	ATF_TP_ADD_TC(tp, log_mode);
	ATF_TP_ADD_TC(tp, kill_mode);
	ATF_TP_ADD_TC(tp, fork_inherits);
	ATF_TP_ADD_TC(tp, setuid_keeps);
	ATF_TP_ADD_TC(tp, ioctl_allowlist);
	ATF_TP_ADD_TC(tp, sockopt_allowlist);
	ATF_TP_ADD_TC(tp, family_allowlist);
	ATF_TP_ADD_TC(tp, largepage_gated);
	ATF_TP_ADD_TC(tp, inherit_narrow);
	ATF_TP_ADD_TC(tp, immutable);
	ATF_TP_ADD_TC(tp, bad_params);
	ATF_TP_ADD_TC(tp, all_params);
	ATF_TP_ADD_TC(tp, unfiltered);
	return (atf_no_error());
}
