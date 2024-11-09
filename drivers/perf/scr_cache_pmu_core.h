/* SPDX-License-Identifier: GPL-2.0
 *
 * Syntacore SCR7 Cache PMU core
 *
 * Copyright (C) 2023 Syntacore
 *
 */
#ifndef _SCR_CACHE_PMU_CORE_H
#define _SCR_CACHE_PMU_CORE_H

#include <linux/perf_event.h>
#include <linux/cpumask.h>

#define SBI_SCR7_L2_PMU_FN	0x1
#define SBI_SCR7_L3_PMU_FN	0x2

#define SBI_EXT_SCR_PMU_COUNTER_HW_READ 0x6
#define SBI_EXT_SCR_PMU_PROBE           0x7
#define SBI_EXT_SCR_PMU_VID             0x8

#define CACHE_DEDICATED_FLAG  BIT(1)

#define SBI_EXT_VENDOR_SCR    ((SBI_EXT_VENDOR_START) | (SCR_VENDOR_ID))

/*
 * Aggregate PMU. Implements the core pmu functions and manages
 * the hardware PMUs.
 */
struct scr_cache_pmu {
	struct pmu pmu;
	bool dedicated;
	int num_counters;
	cpumask_t cpumask;

	/* Variables to be set before call init */
	int sbi_fn;
	u32 event_mask;
	u32 bank_mask;
};

struct pmu_scr_cfg {
	bool is_available;
	bool is_dedicated;
	int num_counters;
	int event_type;
};

#define to_scr_cache_pmu(p) (container_of(p, struct scr_cache_pmu, pmu))

int scr_cache_pmu_init(struct scr_cache_pmu *pmu,
	const struct attribute_group *format,
	const struct attribute_group *events);

int scr_cache_pmu_vid(int sbi_fn, unsigned long *vid);

const struct pmu_scr_cfg *scr_pmu_l2_get_config(void);
const struct pmu_scr_cfg *scr_pmu_l3_get_config(void);

static inline bool scr_pmu_is_available(const struct pmu_scr_cfg *cfg)
{
	return cfg->is_available;
}

static inline bool scr_pmu_is_dedicated(const struct pmu_scr_cfg *cfg)
{
	return cfg->is_dedicated;
}

static inline int scr_pmu_get_num_counters(const struct pmu_scr_cfg *cfg)
{
	return cfg->num_counters;
}

static inline int scr_pmu_get_etype(const struct pmu_scr_cfg *cfg)
{
	return cfg->event_type;
}

#endif /* _SCR_CACHE_PMU_CORE_H */
