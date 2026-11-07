/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Atrium Project
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Per-jail system call filter ("sysfil").
 *
 * A jail may carry a filter that limits which system calls, ioctl(2)
 * commands, socket options and socket address families its processes can
 * use.  The filter is set once, when the jail is created, and is inherited
 * by every child jail, which can only narrow it further.  It is configured
 * with ordinary jail parameters:
 *
 *	sysfil.mode	 0 off, 1 log (allow, but record and log what would
 *			 have been denied), 2 deny (fail with an error),
 *			 3 kill (deny and send SIGSYS)
 *	sysfil.syscalls	 comma-separated syscall names or numbers
 *	sysfil.ioctls	 comma-separated ioctl command numbers
 *	sysfil.sockopts	 comma-separated level:name pairs
 *	sysfil.families	 comma-separated address family names or numbers
 *
 * A list that is not given leaves that class unrestricted (subject to the
 * parent's filter).  An empty list allows nothing of that class.
 *
 * Read-only parameters report what the filter has denied (or, in log mode,
 * would have denied): sysfil.denied and sysfil.seen.{syscalls,ioctls,
 * sockopts,families}.  Running an application under mode 1 and reading these
 * back is how a profile is generated.
 *
 * Cost.  A process attached to a filtered jail gets CRED_FLAG_SYSFIL in its
 * credential.  syscallenter() and the ioctl, sockopt and socket paths test
 * that flag, which lives in a word already read for capability mode, and
 * only then look at the filter.  Processes outside filtered jails pay one
 * predicted-not-taken branch per system call.  The filter of a jail never
 * changes after creation, so the check takes no locks.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/jail.h>
#include <sys/limits.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/mutex.h>
#include <sys/osd.h>
#include <sys/proc.h>
#include <sys/sbuf.h>
#include <sys/signalvar.h>
#include <sys/socket.h>
#include <sys/sx.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>
#include <sys/sysent.h>
#include <sys/syslog.h>
#include <sys/ucred.h>

static MALLOC_DEFINE(M_SYSFIL, "sysfil", "Jail system call filters");

#define	SYSFIL_NSYS		SYS_MAXSYSCALL
#define	SYSFIL_SYS_WORDS	howmany(SYSFIL_NSYS, 64)
#define	SYSFIL_SEEN_WORDS	howmany(SYSFIL_NSYS, 32)
#define	SYSFIL_MAXIOCTLS	256
#define	SYSFIL_MAXSOCKOPTS	256
#define	SYSFIL_MAXSEEN		64

/*
 * Parameter string sizes.  Each must hold the longest value the parameter
 * can read back, and together they must leave room under the 64 KB that
 * vfs_buildopts() allows a whole jail_get(2) request (VFS_MOUNTARG_SIZE_MAX),
 * or "jls -n", which asks for every parameter at once, fails.  Read-back
 * widths: a syscall name is at most 24 bytes and all of syscallnames[] is
 * under 7 KB; an ioctl command is 32 bits, "0x" and 8 hex digits; a socket
 * option is "0x7fffffff:2147483647"; a family name is under 12 bytes.
 */
#define	SYSFIL_SYSCALLS_LEN	8192
#define	SYSFIL_IOCTLS_LEN	(SYSFIL_MAXIOCTLS * 11 + 256)
#define	SYSFIL_SOCKOPTS_LEN	(SYSFIL_MAXSOCKOPTS * 22 + 256)
#define	SYSFIL_FAMILIES_LEN	1024
#define	SYSFIL_SEEN_IOCTLS_LEN	(SYSFIL_MAXSEEN * 11 + 64)
#define	SYSFIL_SEEN_SOCKOPTS_LEN (SYSFIL_MAXSEEN * 22 + 64)
_Static_assert(2 * SYSFIL_SYSCALLS_LEN + SYSFIL_IOCTLS_LEN +
    SYSFIL_SOCKOPTS_LEN + 2 * SYSFIL_FAMILIES_LEN + SYSFIL_SEEN_IOCTLS_LEN +
    SYSFIL_SEEN_SOCKOPTS_LEN <= 32 * 1024,
    "sysfil parameters must leave room in a jail_get(2) request");

_Static_assert(AF_MAX < 64, "sysfil family bitmap is one 64-bit word");

/* What a filter restricts.  A class without its bit is unrestricted. */
#define	SF_SYSCALLS	0x0001
#define	SF_IOCTLS	0x0002
#define	SF_SOCKOPTS	0x0004
#define	SF_FAMILIES	0x0008

struct sysfil_sockopt {
	int	level;
	int	name;
};

struct sysfil {
	/*
	 * Immutable once the jail is visible.  sf_sys comes first:
	 * syscallenter() tests it inline through SYSFIL_SYSBITS().  It is
	 * all ones when system calls are unrestricted, so that test needs
	 * no look at sf_flags.
	 */
	uint64_t	 sf_sys[SYSFIL_SYS_WORDS];
	int		 sf_mode;		/* JAIL_SYSFIL_* */
	u_int		 sf_flags;		/* SF_* */
	uint64_t	 sf_families;
	u_int		 sf_nioctls;
	u_int		 sf_nsockopts;
	u_long		*sf_ioctls;		/* sorted */
	struct sysfil_sockopt *sf_sockopts;	/* sorted */

	/* What was denied, or would have been in log mode. */
	struct mtx	 sf_mtx;		/* seen lists, rate limit */
	u_int		 sf_denied;		/* atomic */
	u_int		 sf_seen_sys[SYSFIL_SEEN_WORDS];	/* atomic */
	u_int		 sf_seen_families[2];	/* atomic */
	u_int		 sf_nseen_ioctls;
	u_int		 sf_nseen_sockopts;
	u_long		 sf_seen_ioctls[SYSFIL_MAXSEEN];
	struct sysfil_sockopt sf_seen_sockopts[SYSFIL_MAXSEEN];
	struct timeval	 sf_lastlog;
	int		 sf_curlog;
	int		 sf_jid;
};

static u_int sysfil_osd_slot;

static int sysfil_log_rate = 5;
SYSCTL_INT(_security_jail, OID_AUTO, sysfil_log_rate, CTLFLAG_RWTUN,
    &sysfil_log_rate, 0,
    "Most sysfil messages logged per second per jail (0 disables, -1 no limit)");

SYSCTL_JAIL_PARAM_NODE(sysfil, "Jail system call filter");
SYSCTL_JAIL_PARAM(_sysfil, mode, CTLTYPE_INT | CTLFLAG_RW, "I",
    "Filter mode: 0 off, 1 log only, 2 deny, 3 deny and send SIGSYS");
SYSCTL_JAIL_PARAM_STRING(_sysfil, syscalls, CTLFLAG_RW, SYSFIL_SYSCALLS_LEN,
    "System calls the jail may use");
SYSCTL_JAIL_PARAM_STRING(_sysfil, ioctls, CTLFLAG_RW, SYSFIL_IOCTLS_LEN,
    "ioctl(2) commands the jail may use");
SYSCTL_JAIL_PARAM_STRING(_sysfil, sockopts, CTLFLAG_RW, SYSFIL_SOCKOPTS_LEN,
    "Socket options (level:name) the jail may use");
SYSCTL_JAIL_PARAM_STRING(_sysfil, families, CTLFLAG_RW, SYSFIL_FAMILIES_LEN,
    "Socket address families the jail may use");
SYSCTL_JAIL_PARAM(_sysfil, denied, CTLTYPE_UINT | CTLFLAG_RD, "IU",
    "Number of operations the filter denied or would have denied");
SYSCTL_JAIL_PARAM_SUBNODE(sysfil, seen, "What the filter denied");
SYSCTL_JAIL_PARAM_STRING(_sysfil_seen, syscalls, CTLFLAG_RD, SYSFIL_SYSCALLS_LEN,
    "System calls denied or, in log mode, that would have been");
SYSCTL_JAIL_PARAM_STRING(_sysfil_seen, ioctls, CTLFLAG_RD, SYSFIL_SEEN_IOCTLS_LEN,
    "ioctl(2) commands denied");
SYSCTL_JAIL_PARAM_STRING(_sysfil_seen, sockopts, CTLFLAG_RD,
    SYSFIL_SEEN_SOCKOPTS_LEN,
    "Socket options denied");
SYSCTL_JAIL_PARAM_STRING(_sysfil_seen, families, CTLFLAG_RD, SYSFIL_FAMILIES_LEN,
    "Address families denied");

static const struct {
	const char	*name;
	int		 af;
} sysfil_afnames[] = {
	{ "unix",	AF_UNIX },
	{ "local",	AF_LOCAL },
	{ "inet",	AF_INET },
	{ "inet6",	AF_INET6 },
	{ "route",	AF_ROUTE },
	{ "key",	PF_KEY },
	{ "netlink",	AF_NETLINK },
	{ "divert",	AF_DIVERT },
	{ "netgraph",	AF_NETGRAPH },
	{ "bluetooth",	AF_BLUETOOTH },
	{ "hyperv",	AF_HYPERV },
	{ "inet_sdp",	AF_INET_SDP },
	{ "inet6_sdp",	AF_INET6_SDP },
};

/* ------------------------------------------------------------------ */
/* Lookup helpers.                                                     */

static __inline bool
sf_bit(const uint64_t *map, u_int bit)
{

	return ((map[bit / 64] & ((uint64_t)1 << (bit % 64))) != 0);
}

static __inline void
sf_setbit(uint64_t *map, u_int bit)
{

	map[bit / 64] |= (uint64_t)1 << (bit % 64);
}

/* The seen maps are u_int words so that 32-bit platforms can update them. */
static __inline void
sf_setbit_atomic(u_int *map, u_int bit)
{

	atomic_set_int(&map[bit / 32], 1u << (bit % 32));
}

static int
sf_ioctl_cmp(const void *a, const void *b)
{
	u_long x = *(const u_long *)a, y = *(const u_long *)b;

	return (x < y ? -1 : x > y);
}

static int
sf_sockopt_cmp(const void *a, const void *b)
{
	const struct sysfil_sockopt *x = a, *y = b;

	if (x->level != y->level)
		return (x->level < y->level ? -1 : 1);
	return (x->name < y->name ? -1 : x->name > y->name);
}

/*
 * Open-coded binary searches: these run on every allowed ioctl(2) and
 * socket-option call in a filtered jail, and bsearch(3)'s comparator
 * calls cost more than the search itself on lists this short.
 */
static bool
sf_has_ioctl(const struct sysfil *sf, u_long com)
{
	const u_long *v = sf->sf_ioctls;
	u_int lo = 0, hi = sf->sf_nioctls, mid;

	while (lo < hi) {
		mid = (lo + hi) / 2;
		if (v[mid] == com)
			return (true);
		if (v[mid] < com)
			lo = mid + 1;
		else
			hi = mid;
	}
	return (false);
}

/* Entries are non-negative, so (level, name) orders like this key. */
static __inline uint64_t
sf_sockopt_key(int level, int name)
{

	return ((uint64_t)(u_int)level << 32 | (u_int)name);
}

static bool
sf_has_sockopt(const struct sysfil *sf, int level, int name)
{
	const struct sysfil_sockopt *v = sf->sf_sockopts;
	uint64_t key, k;
	u_int lo = 0, hi = sf->sf_nsockopts, mid;

	key = sf_sockopt_key(level, name);
	while (lo < hi) {
		mid = (lo + hi) / 2;
		k = sf_sockopt_key(v[mid].level, v[mid].name);
		if (k == key)
			return (true);
		if (k < key)
			lo = mid + 1;
		else
			hi = mid;
	}
	return (false);
}

static const char *
sf_afname(int af)
{
	u_int i;

	for (i = 0; i < nitems(sysfil_afnames); i++)
		if (sysfil_afnames[i].af == af)
			return (sysfil_afnames[i].name);
	return (NULL);
}

/* ------------------------------------------------------------------ */
/* Enforcement.                                                        */

_Static_assert(offsetof(struct sysfil, sf_sys) == 0,
    "SYSFIL_SYSBITS() reads sf_sys at the start of struct sysfil");
_Static_assert(SYSFIL_NSYS <= SYSFIL_SYS_WORDS * 64, "sf_sys too small");

static struct sysfil *
sf_of(struct thread *td)
{

	return (td->td_ucred->cr_prison->pr_sysfil);
}

/*
 * Common tail of every check: count, log at a bounded rate, and decide.
 * Returns true if the operation must fail.
 */
static bool
sf_deny(struct thread *td, struct sysfil *sf, const char *what,
    const char *name, u_long a, u_long b)
{
	struct proc *p;
	bool dolog;

	atomic_add_int(&sf->sf_denied, 1);
	if (sysfil_log_rate != 0) {
		mtx_lock(&sf->sf_mtx);
		dolog = ppsratecheck(&sf->sf_lastlog, &sf->sf_curlog,
		    sysfil_log_rate);
		mtx_unlock(&sf->sf_mtx);
		if (dolog) {
			p = td->td_proc;
			if (name != NULL)
				log(LOG_NOTICE, "sysfil: jail %d pid %d (%s): "
				    "%s %s %s\n", sf->sf_jid, p->p_pid,
				    p->p_comm, sf->sf_mode == JAIL_SYSFIL_LOG ?
				    "would deny" : "denied", what, name);
			else
				log(LOG_NOTICE, "sysfil: jail %d pid %d (%s): "
				    "%s %s %#lx:%lu\n", sf->sf_jid, p->p_pid,
				    p->p_comm, sf->sf_mode == JAIL_SYSFIL_LOG ?
				    "would deny" : "denied", what, a, b);
		}
	}
	if (sf->sf_mode == JAIL_SYSFIL_LOG)
		return (false);
	if (sf->sf_mode == JAIL_SYSFIL_KILL) {
		p = td->td_proc;
		PROC_LOCK(p);
		tdsignal(td, SIGSYS);
		PROC_UNLOCK(p);
	}
	return (true);
}

/*
 * Called from syscallenter() when CRED_FLAG_SYSFIL is set.  Codes are
 * native FreeBSD syscall numbers; a process running another ABI in a
 * filtered jail is denied everything, since the filter cannot speak for
 * that ABI's table.
 */
int
sysfil_check_syscall(struct thread *td, u_int code)
{
	struct sysfil *sf;
	bool native;

	sf = sf_of(td);
	if (__predict_false(sf == NULL))
		return (ENOSYS);	/* Fail closed; cannot happen. */
	native = td->td_proc->p_sysent->sv_table == sysent;
	if (native && code < SYSFIL_NSYS) {
		if ((sf->sf_flags & SF_SYSCALLS) == 0 ||
		    sf_bit(sf->sf_sys, code))
			return (0);
		sf_setbit_atomic(sf->sf_seen_sys, code);
	}
	if (sf_deny(td, sf, native ? "syscall" : "foreign-ABI syscall",
	    native && code < SYSFIL_NSYS ? syscallnames[code] : NULL, code, 0))
		return (ENOSYS);
	return (0);
}

static void
sf_seen_ioctl(struct sysfil *sf, u_long com)
{
	u_int i;

	mtx_lock(&sf->sf_mtx);
	for (i = 0; i < sf->sf_nseen_ioctls; i++)
		if (sf->sf_seen_ioctls[i] == com)
			break;
	if (i == sf->sf_nseen_ioctls && i < SYSFIL_MAXSEEN)
		sf->sf_seen_ioctls[sf->sf_nseen_ioctls++] = com;
	mtx_unlock(&sf->sf_mtx);
}

int
sysfil_check_ioctl(struct thread *td, u_long com)
{
	struct sysfil *sf;

	sf = sf_of(td);
	if (__predict_false(sf == NULL))
		return (EPERM);
	if ((sf->sf_flags & SF_IOCTLS) == 0 || sf_has_ioctl(sf, com))
		return (0);
	sf_seen_ioctl(sf, com);
	return (sf_deny(td, sf, "ioctl", NULL, com, 0) ? EPERM : 0);
}

int
sysfil_check_sockopt(struct thread *td, int level, int name)
{
	struct sysfil *sf;
	u_int i;

	sf = sf_of(td);
	if (__predict_false(sf == NULL))
		return (EPERM);
	if ((sf->sf_flags & SF_SOCKOPTS) == 0 ||
	    sf_has_sockopt(sf, level, name))
		return (0);
	mtx_lock(&sf->sf_mtx);
	for (i = 0; i < sf->sf_nseen_sockopts; i++)
		if (sf->sf_seen_sockopts[i].level == level &&
		    sf->sf_seen_sockopts[i].name == name)
			break;
	if (i == sf->sf_nseen_sockopts && i < SYSFIL_MAXSEEN) {
		sf->sf_seen_sockopts[i].level = level;
		sf->sf_seen_sockopts[i].name = name;
		sf->sf_nseen_sockopts++;
	}
	mtx_unlock(&sf->sf_mtx);
	return (sf_deny(td, sf, "sockopt", NULL, (u_int)level, (u_int)name) ?
	    EPERM : 0);
}

int
sysfil_check_family(struct thread *td, int domain)
{
	struct sysfil *sf;

	sf = sf_of(td);
	if (__predict_false(sf == NULL))
		return (EAFNOSUPPORT);
	if ((sf->sf_flags & SF_FAMILIES) == 0)
		return (0);
	if (domain >= 0 && domain < 64) {
		if ((sf->sf_families & ((uint64_t)1 << domain)) != 0)
			return (0);
		sf_setbit_atomic(sf->sf_seen_families, domain);
	}
	return (sf_deny(td, sf, "family", sf_afname(domain), (u_int)domain,
	    0) ? EAFNOSUPPORT : 0);
}

/* ------------------------------------------------------------------ */
/* Construction.                                                       */

static struct sysfil *
sf_alloc(void)
{
	struct sysfil *sf;

	sf = malloc(sizeof(*sf), M_SYSFIL, M_WAITOK | M_ZERO);
	memset(sf->sf_sys, 0xff, sizeof(sf->sf_sys));	/* unrestricted */
	mtx_init(&sf->sf_mtx, "sysfil", NULL, MTX_DEF);
	return (sf);
}

static void
sf_free(struct sysfil *sf)
{

	if (sf == NULL)
		return;
	free(sf->sf_ioctls, M_SYSFIL);
	free(sf->sf_sockopts, M_SYSFIL);
	mtx_destroy(&sf->sf_mtx);
	free(sf, M_SYSFIL);
}

static void
sysfil_osd_destructor(void *data)
{

	sf_free(data);
}

/*
 * Split a comma-separated list in place.  Returns the next token, or NULL
 * at the end.  Leading and trailing blanks around a token are dropped.
 */
static char *
sf_token(char **cursor)
{
	char *s, *t, *end;

	s = *cursor;
	if (s == NULL)
		return (NULL);
	while (*s == ' ' || *s == '\t' || *s == '\n')
		s++;
	if (*s == '\0') {
		*cursor = NULL;
		return (NULL);
	}
	t = strchr(s, ',');
	if (t != NULL) {
		*t = '\0';
		*cursor = t + 1;
	} else
		*cursor = NULL;
	end = s + strlen(s);
	while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
	    end[-1] == '\n'))
		*--end = '\0';
	return (s);
}

static int
sf_number(const char *s, u_long max, u_long *out)
{
	char *end;
	u_long v;

	if (*s == '\0' || *s == '-')
		return (EINVAL);
	v = strtoul(s, &end, 0);
	if (*end != '\0' || v > max)
		return (EINVAL);
	*out = v;
	return (0);
}

static int
sf_parse_syscalls(struct vfsoptlist *opts, char *s, uint64_t *map)
{
	char *tok;
	u_long n;
	u_int i;

	while ((tok = sf_token(&s)) != NULL) {
		if (sf_number(tok, SYSFIL_NSYS - 1, &n) != 0) {
			for (i = 0; i < SYSFIL_NSYS; i++)
				if (syscallnames[i] != NULL &&
				    strcmp(syscallnames[i], tok) == 0)
					break;
			if (i == SYSFIL_NSYS) {
				vfs_opterror(opts,
				    "sysfil.syscalls: unknown system call: %s",
				    tok);
				return (EINVAL);
			}
			n = i;
		}
		sf_setbit(map, n);
	}
	return (0);
}

static int
sf_parse_ioctls(struct vfsoptlist *opts, char *s, struct sysfil *sf)
{
	char *tok;
	u_long n, *v;
	u_int cnt, i, j;

	v = malloc(SYSFIL_MAXIOCTLS * sizeof(*v), M_SYSFIL, M_WAITOK);
	cnt = 0;
	while ((tok = sf_token(&s)) != NULL) {
		/* Commands are 32 bits: sys_ioctl() truncates wider ones. */
		if (sf_number(tok, UINT32_MAX, &n) != 0) {
			vfs_opterror(opts, "sysfil.ioctls: bad command: %s",
			    tok);
			free(v, M_SYSFIL);
			return (EINVAL);
		}
		if (cnt == SYSFIL_MAXIOCTLS) {
			vfs_opterror(opts, "sysfil.ioctls: more than %d",
			    SYSFIL_MAXIOCTLS);
			free(v, M_SYSFIL);
			return (E2BIG);
		}
		v[cnt++] = n;
	}
	qsort(v, cnt, sizeof(*v), sf_ioctl_cmp);
	for (i = j = 0; i < cnt; i++)
		if (j == 0 || v[j - 1] != v[i])
			v[j++] = v[i];
	sf->sf_ioctls = v;
	sf->sf_nioctls = j;
	return (0);
}

static int
sf_parse_sockopts(struct vfsoptlist *opts, char *s, struct sysfil *sf)
{
	struct sysfil_sockopt *v;
	char *tok, *colon;
	u_long level, name;
	u_int cnt, i, j;

	v = malloc(SYSFIL_MAXSOCKOPTS * sizeof(*v), M_SYSFIL, M_WAITOK);
	cnt = 0;
	while ((tok = sf_token(&s)) != NULL) {
		colon = strchr(tok, ':');
		if (colon == NULL)
			goto bad;
		*colon = '\0';
		if (sf_number(tok, INT_MAX, &level) != 0 ||
		    sf_number(colon + 1, INT_MAX, &name) != 0) {
			*colon = ':';
			goto bad;
		}
		if (cnt == SYSFIL_MAXSOCKOPTS) {
			vfs_opterror(opts, "sysfil.sockopts: more than %d",
			    SYSFIL_MAXSOCKOPTS);
			free(v, M_SYSFIL);
			return (E2BIG);
		}
		v[cnt].level = level;
		v[cnt].name = name;
		cnt++;
	}
	qsort(v, cnt, sizeof(*v), sf_sockopt_cmp);
	for (i = j = 0; i < cnt; i++)
		if (j == 0 || sf_sockopt_cmp(&v[j - 1], &v[i]) != 0)
			v[j++] = v[i];
	sf->sf_sockopts = v;
	sf->sf_nsockopts = j;
	return (0);
bad:
	vfs_opterror(opts, "sysfil.sockopts: expected level:name, got %s",
	    tok);
	free(v, M_SYSFIL);
	return (EINVAL);
}

static int
sf_parse_families(struct vfsoptlist *opts, char *s, uint64_t *map)
{
	char *tok;
	u_long n;
	u_int i;

	while ((tok = sf_token(&s)) != NULL) {
		if (sf_number(tok, AF_MAX, &n) != 0) {
			for (i = 0; i < nitems(sysfil_afnames); i++)
				if (strcmp(sysfil_afnames[i].name, tok) == 0)
					break;
			if (i == nitems(sysfil_afnames)) {
				vfs_opterror(opts,
				    "sysfil.families: unknown family: %s", tok);
				return (EINVAL);
			}
			n = sysfil_afnames[i].af;
		}
		*map |= (uint64_t)1 << n;
	}
	return (0);
}

/*
 * Fetch a string parameter.  Returns ENOENT if absent, or a private,
 * writable copy in *out.
 */
static int
sf_getstr(struct vfsoptlist *opts, const char *name, int maxlen, char **out)
{
	char *v;
	int error, len;

	error = vfs_getopt(opts, name, (void **)&v, &len);
	if (error != 0)
		return (error);
	if (v == NULL || len <= 0 || v[len - 1] != '\0') {
		vfs_opterror(opts, "%s: not a string", name);
		return (EINVAL);
	}
	if (len > maxlen) {
		vfs_opterror(opts, "%s: longer than %d", name, maxlen - 1);
		return (ENAMETOOLONG);
	}
	*out = malloc(len, M_SYSFIL, M_WAITOK);
	memcpy(*out, v, len);
	return (0);
}

static bool
sf_any_param(struct vfsoptlist *opts)
{
	struct vfsopt *opt;

	TAILQ_FOREACH(opt, opts, link)
		if (strncmp(opt->name, "sysfil.", 7) == 0)
			return (true);
	return (false);
}

/*
 * Build the filter a new jail asked for, from its parameters alone.
 * *sfp is NULL if the parameters ask for no filter.
 */
static int
sf_from_opts(struct vfsoptlist *opts, struct sysfil **sfp)
{
	struct sysfil *sf;
	char *s;
	int error, mode;

	*sfp = NULL;
	error = vfs_copyopt(opts, "sysfil.mode", &mode, sizeof(mode));
	if (error == ENOENT)
		mode = JAIL_SYSFIL_OFF;
	else if (error != 0)
		return (error);
	if (mode < JAIL_SYSFIL_OFF || mode > JAIL_SYSFIL_KILL) {
		vfs_opterror(opts, "sysfil.mode: must be 0 to 3");
		return (EINVAL);
	}

	sf = sf_alloc();
	sf->sf_mode = mode;
	s = NULL;
	if ((error = sf_getstr(opts, "sysfil.syscalls", SYSFIL_SYSCALLS_LEN, &s)) == 0) {
		sf->sf_flags |= SF_SYSCALLS;
		memset(sf->sf_sys, 0, sizeof(sf->sf_sys));
		error = sf_parse_syscalls(opts, s, sf->sf_sys);
		free(s, M_SYSFIL);
	}
	/* Each class in turn; ENOENT means "not given". */
	if (error != 0 && error != ENOENT)
		goto fail;
	if ((error = sf_getstr(opts, "sysfil.ioctls", SYSFIL_IOCTLS_LEN, &s)) == 0) {
		sf->sf_flags |= SF_IOCTLS;
		error = sf_parse_ioctls(opts, s, sf);
		free(s, M_SYSFIL);
	}
	if (error != 0 && error != ENOENT)
		goto fail;
	if ((error = sf_getstr(opts, "sysfil.sockopts", SYSFIL_SOCKOPTS_LEN, &s)) == 0) {
		sf->sf_flags |= SF_SOCKOPTS;
		error = sf_parse_sockopts(opts, s, sf);
		free(s, M_SYSFIL);
	}
	if (error != 0 && error != ENOENT)
		goto fail;
	if ((error = sf_getstr(opts, "sysfil.families", SYSFIL_FAMILIES_LEN, &s)) == 0) {
		sf->sf_flags |= SF_FAMILIES;
		error = sf_parse_families(opts, s, &sf->sf_families);
		free(s, M_SYSFIL);
	}
	if (error != 0 && error != ENOENT)
		goto fail;

	if (mode == JAIL_SYSFIL_OFF) {
		if (sf->sf_flags != 0) {
			vfs_opterror(opts,
			    "sysfil lists given but sysfil.mode is 0");
			error = EINVAL;
			goto fail;
		}
		sf_free(sf);
		return (0);
	}
	*sfp = sf;
	return (0);
fail:
	sf_free(sf);
	return (error);
}

/*
 * Narrow a child's filter by its parent's: the result allows only what
 * both allow, and its mode is the stricter of the two.  Either may be NULL.
 * Consumes "child"; the parent is only read.
 */
static struct sysfil *
sf_intersect(const struct sysfil *parent, struct sysfil *child)
{
	struct sysfil *sf;
	u_int i, j;

	if (parent == NULL)
		return (child);
	sf = child;
	if (sf == NULL) {
		sf = sf_alloc();
		sf->sf_mode = parent->sf_mode;
	}
	sf->sf_mode = MAX(sf->sf_mode, parent->sf_mode);

	if (parent->sf_flags & SF_SYSCALLS) {
		if (sf->sf_flags & SF_SYSCALLS) {
			for (i = 0; i < SYSFIL_SYS_WORDS; i++)
				sf->sf_sys[i] &= parent->sf_sys[i];
		} else
			memcpy(sf->sf_sys, parent->sf_sys, sizeof(sf->sf_sys));
		sf->sf_flags |= SF_SYSCALLS;
	}
	if (parent->sf_flags & SF_FAMILIES) {
		if (sf->sf_flags & SF_FAMILIES)
			sf->sf_families &= parent->sf_families;
		else
			sf->sf_families = parent->sf_families;
		sf->sf_flags |= SF_FAMILIES;
	}
	if (parent->sf_flags & SF_IOCTLS) {
		if (sf->sf_flags & SF_IOCTLS) {
			for (i = j = 0; i < sf->sf_nioctls; i++)
				if (sf_has_ioctl(parent, sf->sf_ioctls[i]))
					sf->sf_ioctls[j++] = sf->sf_ioctls[i];
			sf->sf_nioctls = j;
		} else {
			sf->sf_ioctls = malloc(MAX(parent->sf_nioctls, 1) *
			    sizeof(u_long), M_SYSFIL, M_WAITOK);
			memcpy(sf->sf_ioctls, parent->sf_ioctls,
			    parent->sf_nioctls * sizeof(u_long));
			sf->sf_nioctls = parent->sf_nioctls;
		}
		sf->sf_flags |= SF_IOCTLS;
	}
	if (parent->sf_flags & SF_SOCKOPTS) {
		if (sf->sf_flags & SF_SOCKOPTS) {
			for (i = j = 0; i < sf->sf_nsockopts; i++)
				if (sf_has_sockopt(parent,
				    sf->sf_sockopts[i].level,
				    sf->sf_sockopts[i].name))
					sf->sf_sockopts[j++] =
					    sf->sf_sockopts[i];
			sf->sf_nsockopts = j;
		} else {
			sf->sf_sockopts = malloc(MAX(parent->sf_nsockopts, 1) *
			    sizeof(struct sysfil_sockopt), M_SYSFIL, M_WAITOK);
			memcpy(sf->sf_sockopts, parent->sf_sockopts,
			    parent->sf_nsockopts *
			    sizeof(struct sysfil_sockopt));
			sf->sf_nsockopts = parent->sf_nsockopts;
		}
		sf->sf_flags |= SF_SOCKOPTS;
	}
	return (sf);
}

/* ------------------------------------------------------------------ */
/* Jail parameter methods.                                             */

static int
sysfil_prison_check(void *obj, void *data)
{
	struct prison *pr = obj;
	struct vfsoptlist *opts = data;
	struct sysfil *sf;
	struct vfsopt *opt;
	int error;

	if (!sf_any_param(opts))
		return (0);
	TAILQ_FOREACH(opt, opts, link) {
		if (strcmp(opt->name, "sysfil.denied") == 0 ||
		    strncmp(opt->name, "sysfil.seen.", 12) == 0) {
			vfs_opterror(opts, "%s is read-only", opt->name);
			return (EINVAL);
		}
	}
	/*
	 * The filter is fixed when the jail is created.  Changing it later
	 * would leave running processes with credentials that predate it,
	 * and widening it would break the inherit-and-narrow rule.
	 */
	if (pr->pr_state != PRISON_STATE_INVALID) {
		vfs_opterror(opts,
		    "sysfil parameters can only be set when a jail is created");
		return (EINVAL);
	}
	/* Parse now, so that bad lists fail before anything is changed. */
	error = sf_from_opts(opts, &sf);
	sf_free(sf);
	return (error);
}

static int
sysfil_prison_create(void *obj, void *data)
{
	struct prison *pr = obj;
	struct vfsoptlist *opts = data;
	struct sysfil *sf;
	void **rsv;
	int error;

	sx_assert(&allprison_lock, SA_XLOCKED);
	error = sf_from_opts(opts, &sf);
	if (error != 0)
		return (error);
	/* The parent's filter is immutable; no lock is needed to read it. */
	sf = sf_intersect(pr->pr_parent->pr_sysfil, sf);
	if (sf == NULL)
		return (0);
	sf->sf_jid = pr->pr_id;

	rsv = osd_reserve(sysfil_osd_slot);
	mtx_lock(&pr->pr_mtx);
	(void)osd_jail_set_reserved(pr, sysfil_osd_slot, rsv, sf);
	/*
	 * Published before the prison is alive, so before any process can
	 * attach and before any child can inherit it.
	 */
	pr->pr_sysfil = sf;
	mtx_unlock(&pr->pr_mtx);
	return (0);
}

static void
sf_put_syscalls(struct sbuf *sb, const uint64_t *map)
{
	u_int i;
	bool first = true;

	for (i = 0; i < SYSFIL_NSYS; i++) {
		if (!sf_bit(map, i))
			continue;
		if (!first)
			sbuf_putc(sb, ',');
		first = false;
		if (syscallnames[i] != NULL)
			sbuf_cat(sb, syscallnames[i]);
		else
			sbuf_printf(sb, "%u", i);
	}
}

static void
sf_put_families(struct sbuf *sb, uint64_t map)
{
	const char *name;
	u_int i;
	bool first = true;

	for (i = 0; i < 64; i++) {
		if ((map & ((uint64_t)1 << i)) == 0)
			continue;
		if (!first)
			sbuf_putc(sb, ',');
		first = false;
		name = sf_afname(i);
		if (name != NULL)
			sbuf_cat(sb, name);
		else
			sbuf_printf(sb, "%u", i);
	}
}

static void
sf_put_ioctls(struct sbuf *sb, const u_long *v, u_int n)
{
	u_int i;

	for (i = 0; i < n; i++)
		sbuf_printf(sb, "%s%#lx", i ? "," : "", v[i]);
}

static void
sf_put_sockopts(struct sbuf *sb, const struct sysfil_sockopt *v, u_int n)
{
	u_int i;

	for (i = 0; i < n; i++)
		sbuf_printf(sb, "%s%#x:%d", i ? "," : "", (u_int)v[i].level,
		    v[i].name);
}

/*
 * Set one string parameter of a jail_get(2) request, if it was asked for.
 * The value is built only when needed.
 */
static int
sf_get_one(struct vfsoptlist *opts, const char *name, struct sysfil *sf,
    void (*put)(struct sbuf *, struct sysfil *))
{
	struct vfsopt *opt;
	struct sbuf *sb;
	int error;

	TAILQ_FOREACH(opt, opts, link)
		if (strcmp(opt->name, name) == 0)
			break;
	if (opt == NULL)
		return (0);
	sb = sbuf_new(NULL, NULL, 256, SBUF_AUTOEXTEND);
	put(sb, sf);
	error = sbuf_finish(sb);
	if (error == 0)
		error = vfs_setopts(opts, name, sbuf_data(sb));
	sbuf_delete(sb);
	return (error == ENOENT ? 0 : error);
}

/*
 * The configured lists read back as "*" when that class is unrestricted
 * (including in a jail with no filter); the seen lists read back empty.
 */
static void
put_syscalls(struct sbuf *sb, struct sysfil *sf)
{

	if (sf != NULL && sf->sf_flags & SF_SYSCALLS)
		sf_put_syscalls(sb, sf->sf_sys);
	else
		sbuf_cat(sb, "*");
}

static void
put_ioctls(struct sbuf *sb, struct sysfil *sf)
{

	if (sf != NULL && sf->sf_flags & SF_IOCTLS)
		sf_put_ioctls(sb, sf->sf_ioctls, sf->sf_nioctls);
	else
		sbuf_cat(sb, "*");
}

static void
put_sockopts(struct sbuf *sb, struct sysfil *sf)
{

	if (sf != NULL && sf->sf_flags & SF_SOCKOPTS)
		sf_put_sockopts(sb, sf->sf_sockopts, sf->sf_nsockopts);
	else
		sbuf_cat(sb, "*");
}

static void
put_families(struct sbuf *sb, struct sysfil *sf)
{

	if (sf != NULL && sf->sf_flags & SF_FAMILIES)
		sf_put_families(sb, sf->sf_families);
	else
		sbuf_cat(sb, "*");
}

static void
put_seen_syscalls(struct sbuf *sb, struct sysfil *sf)
{
	uint64_t map[SYSFIL_SYS_WORDS];
	u_int i, w;

	if (sf == NULL)
		return;
	memset(map, 0, sizeof(map));
	for (i = 0; i < SYSFIL_SEEN_WORDS; i++) {
		w = atomic_load_int(&sf->sf_seen_sys[i]);
		map[i / 2] |= (uint64_t)w << (32 * (i % 2));
	}
	sf_put_syscalls(sb, map);
}

static void
put_seen_ioctls(struct sbuf *sb, struct sysfil *sf)
{
	u_long v[SYSFIL_MAXSEEN];
	u_int n;

	if (sf == NULL)
		return;
	mtx_lock(&sf->sf_mtx);
	n = sf->sf_nseen_ioctls;
	memcpy(v, sf->sf_seen_ioctls, n * sizeof(v[0]));
	mtx_unlock(&sf->sf_mtx);
	qsort(v, n, sizeof(v[0]), sf_ioctl_cmp);
	sf_put_ioctls(sb, v, n);
}

static void
put_seen_sockopts(struct sbuf *sb, struct sysfil *sf)
{
	struct sysfil_sockopt v[SYSFIL_MAXSEEN];
	u_int n;

	if (sf == NULL)
		return;
	mtx_lock(&sf->sf_mtx);
	n = sf->sf_nseen_sockopts;
	memcpy(v, sf->sf_seen_sockopts, n * sizeof(v[0]));
	mtx_unlock(&sf->sf_mtx);
	qsort(v, n, sizeof(v[0]), sf_sockopt_cmp);
	sf_put_sockopts(sb, v, n);
}

static void
put_seen_families(struct sbuf *sb, struct sysfil *sf)
{

	if (sf == NULL)
		return;
	sf_put_families(sb, atomic_load_int(&sf->sf_seen_families[0]) |
	    (uint64_t)atomic_load_int(&sf->sf_seen_families[1]) << 32);
}

static int
sysfil_prison_get(void *obj, void *data)
{
	struct prison *pr = obj;
	struct vfsoptlist *opts = data;
	struct sysfil *sf;
	int error, mode;
	u_int denied;

	sf = pr->pr_sysfil;
	mode = sf != NULL ? sf->sf_mode : JAIL_SYSFIL_OFF;
	denied = sf != NULL ? atomic_load_int(&sf->sf_denied) : 0;
	error = vfs_setopt(opts, "sysfil.mode", &mode, sizeof(mode));
	if (error != 0 && error != ENOENT)
		return (error);
	error = vfs_setopt(opts, "sysfil.denied", &denied, sizeof(denied));
	if (error != 0 && error != ENOENT)
		return (error);
	if ((error = sf_get_one(opts, "sysfil.syscalls", sf,
	    put_syscalls)) != 0 ||
	    (error = sf_get_one(opts, "sysfil.ioctls", sf, put_ioctls)) != 0 ||
	    (error = sf_get_one(opts, "sysfil.sockopts", sf,
	    put_sockopts)) != 0 ||
	    (error = sf_get_one(opts, "sysfil.families", sf,
	    put_families)) != 0 ||
	    (error = sf_get_one(opts, "sysfil.seen.syscalls", sf,
	    put_seen_syscalls)) != 0 ||
	    (error = sf_get_one(opts, "sysfil.seen.ioctls", sf,
	    put_seen_ioctls)) != 0 ||
	    (error = sf_get_one(opts, "sysfil.seen.sockopts", sf,
	    put_seen_sockopts)) != 0 ||
	    (error = sf_get_one(opts, "sysfil.seen.families", sf,
	    put_seen_families)) != 0)
		return (error);
	return (0);
}

static void
sysfil_sysinit(void *arg __unused)
{
	osd_method_t methods[PR_MAXMETHOD] = {
		[PR_METHOD_CHECK] =	sysfil_prison_check,
		[PR_METHOD_CREATE] =	sysfil_prison_create,
		[PR_METHOD_GET] =	sysfil_prison_get,
	};

	sysfil_osd_slot = osd_jail_register(sysfil_osd_destructor, methods);
}
SYSINIT(sysfil, SI_SUB_DRIVERS, SI_ORDER_ANY, sysfil_sysinit, NULL);
