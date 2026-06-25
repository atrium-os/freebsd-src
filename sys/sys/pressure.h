/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Memory-pressure stall accounting (atrium-memory-pressure.md) — a PSI-equivalent
 * built on the insight that *free-page count is the wrong signal*: a healthy
 * system caches all RAM, so "low free" is normal. What is pathological is *stall
 * time* — wall-clock during which a thread is blocked waiting on memory. FreeBSD
 * already funnels those stalls through a small set of sleep points (vm_wait /
 * vm_waitpfault); bracketing them measures the faithful PSI `some` signal with no
 * refault/shadow-entry surgery.
 *
 * `some` = wall-time with >=1 thread blocked on memory (tracked by the 0->1 /
 * 1->0 transitions of the stalled count, NOT a sum of per-thread stall times);
 * `full` adds "and nothing is progressing"; per-jail attribution gives the
 * federation-member granularity; and /dev/pressure exposes a kqueue edge-trigger
 * (EVFILT_READ with a threshold) so a controller waits on a pushed pressure edge
 * instead of polling. See kern_pressure.c.
 *
 * /dev/pressure is the SINGLE grantable capability a memory governor needs: the
 * EVFILT_READ edge (above) + the PRESSURE_GET ioctl (below) deliver the complete
 * state — global and per-jail — so a *jailed* governor reads everything from one
 * device in its devfs ruleset, with no host sysctl access (the cross-jail detail
 * is TCB-sensitive; granting the device node is the access decision).
 */
#ifndef _SYS_PRESSURE_H_
#define	_SYS_PRESSURE_H_

#include <sys/types.h>
#include <sys/ioccom.h>

/* Bound on per-jail slots reported in a snapshot (matches kern_pressure.c). */
#define	PRESSURE_MAX_JAILS	16

/* Bytes of jail name reported per slot (NUL-terminated, truncated). Lets the
 * jailed memfed budgeter match snapshot entries to its by-name config without
 * resolving sibling jids (which jail isolation forbids). Atrium jail names
 * (atrium-/app-/...) are short; 64 is ample and keeps the snapshot < 8 KiB. */
#define	PRESSURE_JAIL_NAME	64

/* Decaying averages are reported in basis points: fraction x10000 (100% = 10000),
 * the same unit as the kern.pressure.memory.* sysctls. */
struct pressure_jail_stat {
	int32_t		pjs_jid;
	uint32_t	pjs_full_avg10;		/* basis points */
	uint32_t	pjs_full_avg60;
	uint32_t	pjs_full_avg300;
	uint64_t	pjs_some_ns;
	uint64_t	pjs_full_ns;		/* clamped <= some_ns */
	uint64_t	pjs_memoryuse;		/* per-jail RSS bytes (RACCT_RSS);
					 * 0 if racct disabled. The jailed
					 * memfed budgeter reads RSS here so
					 * /dev/pressure is the single jailed
					 * per-jail telemetry source. */
	char		pjs_name[PRESSURE_JAIL_NAME];	/* jail name, NUL-term */
};

/* Complete pressure state, read in one PRESSURE_GET ioctl on /dev/pressure. */
struct pressure_snapshot {
	uint64_t	ps_some_ns;		/* global cumulative stall ns */
	uint64_t	ps_full_ns;
	uint32_t	ps_some_avg10;		/* global averages, basis points */
	uint32_t	ps_some_avg60;
	uint32_t	ps_some_avg300;
	uint32_t	ps_full_avg10;
	uint32_t	ps_full_avg60;
	uint32_t	ps_full_avg300;
	int32_t		ps_nstalled;		/* threads blocked on memory right now */
	uint32_t	ps_njails;		/* valid entries in ps_jails[] */
	struct pressure_jail_stat ps_jails[PRESSURE_MAX_JAILS];
};

#define	PRESSURE_GET	_IOR('P', 1, struct pressure_snapshot)

#ifdef _KERNEL

/*
 * Bracket a memory-reclaim stall. Call _enter() immediately before the sleep on a
 * memory shortage and _exit() immediately after it returns. Cheap (a leaf mutex);
 * never call another lock-taking routine between them beyond the sleep itself.
 * The pagedaemon's own waits are NOT stalls and must not be bracketed.
 */
void	pressure_mem_enter(void);
void	pressure_mem_exit(void);

/*
 * Sample whether the system is fully memory-stalled (a thread blocked on memory
 * and no CPU doing productive work). Called from the scheduler's periodic control
 * loop — no hot-path hook. Derives PSI `full`.
 */
void	pressure_sample_cpus(void);

#endif /* _KERNEL */

#endif /* !_SYS_PRESSURE_H_ */
