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
 * Phase 1a: the GLOBAL signal. `some` = wall-time with >=1 thread blocked on
 * memory (tracked by the 0->1 / 1->0 transitions of the stalled count, NOT a sum
 * of per-thread stall times). Per-jail attribution and the kqueue edge-trigger are
 * Phase 1b.
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

#endif /* _KERNEL */

#endif /* !_SYS_PRESSURE_H_ */
