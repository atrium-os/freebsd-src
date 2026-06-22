/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Energy-budget federation (scheduler federation doc §4): heterogeneous
 * members (CPU scheduler, GPU, ...) coordinate ONLY through the shared
 * power cap, allocated max-min fair (water_fill) in WATTS — the one
 * commensurable currency across members.  Members register a demand
 * probe and a budget actuator; the allocator runs in the Laminar
 * control loop.  "Coordinated, not coupled": no member sees another's
 * internals, only its own budget.
 */
#ifndef _SYS_ENERGY_BUDGET_H_
#define	_SYS_ENERGY_BUDGET_H_

#include <sys/types.h>

/*
 * posture is the system power posture, 0..10 (0 = powersave, 5 = balanced,
 * 10 = performance): the SOFT preference for how eagerly available headroom
 * is spent on speed (atrium-power-posture.md).  It is orthogonal to the cap
 * (the HARD ceiling): demand() sizes the ask AT a posture, actuate() seeks the
 * posture target clamped to the granted budget.  Both are threaded through the
 * one federation loop so there is no second control path (invariant #2).
 */
typedef uint64_t (energy_demand_fn)(void *arg, int posture);	/* demand, mW */
typedef void (energy_actuate_fn)(void *arg, uint64_t mw, int posture);
						/* mw: 0 = uncapped; + posture */

int	energy_member_register(const char *name, energy_demand_fn *demand,
	    energy_actuate_fn *actuate, void *arg, uint64_t weight);
void	energy_member_unregister(int id);

#endif /* !_SYS_ENERGY_BUDGET_H_ */
