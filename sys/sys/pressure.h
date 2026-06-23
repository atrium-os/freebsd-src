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
 */
#ifndef _SYS_PRESSURE_H_
#define	_SYS_PRESSURE_H_

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
