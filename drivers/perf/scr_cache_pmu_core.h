/* SPDX-License-Identifier: GPL-2.0
 *
 * Syntacore SCR7 Cache PMU core
 *
 * Copyright (C) 2023 YADRO
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

#define CACHE_DEDICATED_FLAG  BIT(1)

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

#define to_scr_cache_pmu(p) (container_of(p, struct scr_cache_pmu, pmu))

int scr_cache_pmu_init(struct scr_cache_pmu *pmu,
	const struct attribute_group *format,
	const struct attribute_group *events);

#endif /* _SCR_CACHE_PMU_CORE_H */
