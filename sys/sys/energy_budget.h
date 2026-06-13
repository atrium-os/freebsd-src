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

typedef uint64_t (energy_demand_fn)(void *arg);	/* current demand, mW */
typedef void (energy_budget_fn)(void *arg, uint64_t mw); /* 0 = uncapped */

int	energy_member_register(const char *name, energy_demand_fn *demand,
	    energy_budget_fn *budget, void *arg, uint64_t weight);
void	energy_member_unregister(int id);

#endif /* !_SYS_ENERGY_BUDGET_H_ */
