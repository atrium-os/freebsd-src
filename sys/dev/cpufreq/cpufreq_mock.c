/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * cpufreq_mock -- synthetic cpufreq backend for testing.
 *
 * Registers as a cpufreq child of cpu0, exposes a fixed 5-level
 * synthetic P-state table, stores the active level in softc but
 * performs no hardware writes.  Lets the cpufreq stack be exercised
 * in environments without real DVFS (QEMU TCG/HVF, embedded boards
 * without cpufreq drivers, dev VMs).
 *
 * Build-time opt-in via `device cpufreq_mock`.  Not in GENERIC.
 *
 * Per kern_cpu.c:1096-1098 ("all CPUs must offer the same levels"),
 * a single mock instance on cpu0 represents the whole system.
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/cpu.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

#include "cpufreq_if.h"

#define MOCK_NLEVELS	5

struct cpufreq_mock_softc {
	device_t		dev;
	struct cf_setting	sets[MOCK_NLEVELS];
	int			cur;	/* index into sets[] */
};

/*
 * Synthetic P-state table.  Frequencies / voltages / power chosen to
 * be plausible for a 2.4 GHz class core; values are observability hints,
 * not load-bearing (Laminar's controller uses (freq / max_freq) ratio
 * as the capacity signal).
 *
 * Index 0 is fastest; cpufreq core sorts by total_set.freq descending,
 * so this ordering is consistent with the rest of the stack.
 */
static const struct {
	int freq;	/* MHz */
	int volts;	/* mV */
	int power;	/* mW */
	int lat;	/* us */
} mock_table[MOCK_NLEVELS] = {
	{ 2400, 1100, 8000,  50 },
	{ 1800, 1000, 5000,  50 },
	{ 1200,  900, 3000,  50 },
	{  800,  800, 1500,  50 },
	{  400,  700,  800,  50 },
};

static void	cpufreq_mock_identify(driver_t *driver, device_t parent);
static int	cpufreq_mock_probe(device_t dev);
static int	cpufreq_mock_attach(device_t dev);
static int	cpufreq_mock_detach(device_t dev);
static int	cpufreq_mock_settings(device_t dev, struct cf_setting *sets,
		    int *count);
static int	cpufreq_mock_set(device_t dev, const struct cf_setting *set);
static int	cpufreq_mock_get(device_t dev, struct cf_setting *set);
static int	cpufreq_mock_type(device_t dev, int *type);

static device_method_t cpufreq_mock_methods[] = {
	DEVMETHOD(device_identify,	cpufreq_mock_identify),
	DEVMETHOD(device_probe,		cpufreq_mock_probe),
	DEVMETHOD(device_attach,	cpufreq_mock_attach),
	DEVMETHOD(device_detach,	cpufreq_mock_detach),

	DEVMETHOD(cpufreq_drv_set,	cpufreq_mock_set),
	DEVMETHOD(cpufreq_drv_get,	cpufreq_mock_get),
	DEVMETHOD(cpufreq_drv_type,	cpufreq_mock_type),
	DEVMETHOD(cpufreq_drv_settings,	cpufreq_mock_settings),

	DEVMETHOD_END
};

static driver_t cpufreq_mock_driver = {
	"cpufreq_mock", cpufreq_mock_methods,
	sizeof(struct cpufreq_mock_softc)
};

DRIVER_MODULE(cpufreq_mock, cpu, cpufreq_mock_driver, 0, 0);

static void
cpufreq_mock_identify(driver_t *driver, device_t parent)
{

	/*
	 * Attach only to cpu0 -- cpufreq core enforces system-wide
	 * uniform P-state today, so one instance represents the whole
	 * system.
	 */
	if (device_get_unit(parent) != 0)
		return;
	if (device_find_child(parent, "cpufreq_mock", DEVICE_UNIT_ANY))
		return;
	if (BUS_ADD_CHILD(parent, 0, "cpufreq_mock", 0) == NULL)
		device_printf(parent, "cpufreq_mock: BUS_ADD_CHILD failed\n");
}

static int
cpufreq_mock_probe(device_t dev)
{

	if (resource_disabled("cpufreq_mock", 0))
		return (ENXIO);
	device_set_desc(dev, "Synthetic cpufreq backend (test only)");
	return (BUS_PROBE_NOWILDCARD);
}

static int
cpufreq_mock_attach(device_t dev)
{
	struct cpufreq_mock_softc *sc;
	int i;

	sc = device_get_softc(dev);
	sc->dev = dev;
	sc->cur = 0;	/* boot at max performance */

	for (i = 0; i < MOCK_NLEVELS; i++) {
		sc->sets[i].freq = mock_table[i].freq;
		sc->sets[i].volts = mock_table[i].volts;
		sc->sets[i].power = mock_table[i].power;
		sc->sets[i].lat = mock_table[i].lat;
		sc->sets[i].dev = dev;
	}

	cpufreq_register(dev);
	device_printf(dev,
	    "synthetic backend, %d levels (%d -> %d MHz), starting at %d MHz\n",
	    MOCK_NLEVELS, mock_table[0].freq, mock_table[MOCK_NLEVELS - 1].freq,
	    mock_table[sc->cur].freq);
	return (0);
}

static int
cpufreq_mock_detach(device_t dev)
{

	cpufreq_unregister(dev);
	return (0);
}

static int
cpufreq_mock_settings(device_t dev, struct cf_setting *sets, int *count)
{
	struct cpufreq_mock_softc *sc;
	int i;

	if (sets == NULL || count == NULL)
		return (EINVAL);
	if (*count < MOCK_NLEVELS) {
		*count = MOCK_NLEVELS;
		return (E2BIG);
	}
	sc = device_get_softc(dev);
	for (i = 0; i < MOCK_NLEVELS; i++)
		sets[i] = sc->sets[i];
	*count = MOCK_NLEVELS;
	return (0);
}

static int
cpufreq_mock_set(device_t dev, const struct cf_setting *set)
{
	struct cpufreq_mock_softc *sc;
	int i;

	if (set == NULL)
		return (EINVAL);
	sc = device_get_softc(dev);
	for (i = 0; i < MOCK_NLEVELS; i++) {
		if (sc->sets[i].freq == set->freq) {
			sc->cur = i;
			return (0);
		}
	}
	return (EINVAL);
}

static int
cpufreq_mock_get(device_t dev, struct cf_setting *set)
{
	struct cpufreq_mock_softc *sc;

	if (set == NULL)
		return (EINVAL);
	sc = device_get_softc(dev);
	*set = sc->sets[sc->cur];
	return (0);
}

static int
cpufreq_mock_type(device_t dev, int *type)
{

	if (type == NULL)
		return (EINVAL);
	*type = CPUFREQ_TYPE_ABSOLUTE;
	return (0);
}

MODULE_VERSION(cpufreq_mock, 1);
MODULE_DEPEND(cpufreq_mock, cpufreq, 1, 1, 1);
