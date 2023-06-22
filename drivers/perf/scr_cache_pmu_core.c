// SPDX-License-Identifier: GPL-2.0
/*
 * Syntacore SCR7 L2 Cache PMU
 *
 * Copyright (C) 2023 YADRO
 *
 * This code is based on QCOM L3 and RISCV SBI pmu.
 */
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/bitops.h>
#include <linux/sysfs.h>
#include <asm/sbi.h>
#include <asm/hwcap.h>
#include <asm/vendorid_list.h>

#include "scr_cache_pmu_core.h"

#define SBI_EXT_VENDOR_SCR    ((SBI_EXT_VENDOR_START) | (SCR_VENDOR_ID))

#define sbi_scr_pmu_ecall(spmu, ...) \
	sbi_ecall(SBI_EXT_VENDOR_SCR, (spmu)->sbi_fn, ##__VA_ARGS__)
#define sbi_scr_pmu_probe(spmu) \
	sbi_scr_pmu_ecall(spmu, SBI_EXT_SCR_PMU_PROBE, 0, 0, 0, 0, 0)
#define sbi_scr_num_cnt(spmu) \
	sbi_scr_pmu_ecall(spmu, SBI_EXT_PMU_NUM_COUNTERS, 0, 0, 0, 0, 0)
#define sbi_scr_pmu_read(spmu, idx) \
	sbi_scr_pmu_ecall(spmu, SBI_EXT_SCR_PMU_COUNTER_HW_READ, idx, 0, 0, 0, 0)
#define sbi_scr_pmu_cfg_match(spmu, cbase, cmask, cflags, event_base, config) \
	sbi_scr_pmu_ecall(spmu, SBI_EXT_PMU_COUNTER_CFG_MATCH, \
		cbase, cmask, cflags, event_base, config)
#define sbi_scr_pmu_start(spmu, idx, cmask, flag, ival) \
	sbi_scr_pmu_ecall(spmu, SBI_EXT_PMU_COUNTER_START, idx, cmask, flag, ival, 0)
#define sbi_scr_pmu_stop(spmu, idx, cmask, flag) \
	sbi_scr_pmu_ecall(spmu, SBI_EXT_PMU_COUNTER_STOP, idx, cmask, flag, 0, 0)

#define COUNTER_MASK          ((u64)(~0))


static inline u32 get_event_type(struct perf_event *event, u32 event_mask)
{
	return (event->attr.config) & event_mask;
}

static inline u32 get_banks_mask(struct perf_event *event, u32 bank_mask)
{
	return (event->attr.config) & bank_mask;
}

static int scr_cache_pmu_event_init(struct perf_event *event)
{
	struct scr_cache_pmu *spmu = to_scr_cache_pmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;
	u64 event_config = 0;

	/*
	 * Is the event for this PMU?
	 */
	if (event->attr.type != event->pmu->type)
		return -ENOENT;

	if (hwc->sample_period)
		return -EINVAL;

	/*
	 * Task mode not available, we run the counters as shared counters,
	 * not attributable to any CPU and therefore cannot attribute per-task.
	 */
	if (!spmu->dedicated && event->cpu < 0)
		return -EINVAL;

	hwc->idx = -1;
	event_config = get_banks_mask(event, spmu->bank_mask);

	if (!event_config)
		/* if no banks were chosen we assume all banks are used */
		event_config |= spmu->bank_mask;

	hwc->config = event_config;
	hwc->event_base = get_event_type(event, spmu->event_mask);

	if (!spmu->dedicated) {
		/*
		 * Many perf core operations (eg. events rotation) operate on a
		 * single CPU context. This is obvious for CPU PMUs, where one
		 * expects the same sets of events being observed on all CPUs,
		 * but can lead to issues for off-core PMUs, like this one, where
		 * each event could be theoretically assigned to a different CPU.
		 * To mitigate this, we enforce CPU assignment to one designated
		 * processor (the one described in the "cpumask" attribute exported
		 * by the PMU device). perf user space tools honor this and avoid
		 * opening more than one copy of the events.
		 */
		event->cpu = cpumask_first(&spmu->cpumask);
	}

	return 0;
}

static void scr_cache_pmu_event_start(struct perf_event *event, int flags)
{
	struct hw_perf_event *hwc = &event->hw;
	struct scr_cache_pmu *spmu = to_scr_cache_pmu(event->pmu);
	unsigned long flag = SBI_PMU_START_FLAG_SET_INIT_VALUE;
	struct sbiret ret;
	u64 ival;

	hwc->state = 0;
	ival = local64_read(&hwc->prev_count);
	ret = sbi_scr_pmu_start(spmu, hwc->idx, 1, flag, ival);
	if (ret.error && (ret.error != SBI_ERR_ALREADY_STARTED))
		pr_err("Starting counter idx %d failed with error %d\n",
		       hwc->idx, sbi_err_map_linux_errno(ret.error));

	perf_event_update_userpage(event);
}

static u64 scr_cache_pmu_event_update(struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;
	struct scr_cache_pmu *spmu = to_scr_cache_pmu(event->pmu);
	u64 prev_raw_count, new_raw_count;
	u64 oldval, delta;
	struct sbiret ret;

	do {
		prev_raw_count = local64_read(&hwc->prev_count);
		ret = sbi_scr_pmu_read(spmu, hwc->idx);
		if (ret.error)
			break;

		new_raw_count = ret.value;
		oldval = local64_cmpxchg(&hwc->prev_count, prev_raw_count,
					 new_raw_count);
	} while (oldval != prev_raw_count);

	delta = (new_raw_count - prev_raw_count) & COUNTER_MASK;
	local64_add(delta, &event->count);
	local64_sub(delta, &hwc->period_left);

	return delta;
}

static void scr_cache_pmu_event_stop(struct perf_event *event, int flags)
{
	struct hw_perf_event *hwc = &event->hw;
	struct scr_cache_pmu *spmu = to_scr_cache_pmu(event->pmu);
	struct sbiret ret;

	WARN_ON_ONCE(hwc->state & PERF_HES_STOPPED);

	if (!(hwc->state & PERF_HES_STOPPED)) {
		ret = sbi_scr_pmu_stop(spmu, hwc->idx, 1, 0);
		if (ret.error && (ret.error != SBI_ERR_ALREADY_STOPPED) &&
			flags != SBI_PMU_STOP_FLAG_RESET)
			pr_err("Stopping counter idx %d failed with error %d\n",
			       hwc->idx, sbi_err_map_linux_errno(ret.error));

		hwc->state |= PERF_HES_STOPPED;
		scr_cache_pmu_event_update(event);
		hwc->state |= PERF_HES_UPTODATE;
	}
}

static int scr_cache_pmu_event_add(struct perf_event *event, int flags)
{
	struct scr_cache_pmu *spmu = to_scr_cache_pmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;
	struct sbiret ret;
	int idx;
	uint64_t cbase = 0;
	uint64_t cmask = GENMASK_ULL(spmu->num_counters - 1, 0);
	unsigned long cflags = 0;

	ret = sbi_scr_pmu_cfg_match(spmu, cbase, cmask, cflags, hwc->event_base, hwc->config);
	if (ret.error) {
		pr_debug("Not able to find a counter for event %lx config %llx\n",
			 hwc->event_base, hwc->config);
		return sbi_err_map_linux_errno(ret.error);
	}

	idx = ret.value;
	if (idx >= spmu->num_counters)
		return -ENOENT;

	hwc->idx = idx;
	hwc->state = PERF_HES_UPTODATE | PERF_HES_STOPPED;
	if (flags & PERF_EF_START)
		scr_cache_pmu_event_start(event, PERF_EF_RELOAD);

	/* Propagate our changes to the userspace mapping. */
	perf_event_update_userpage(event);

	return 0;
}

static void scr_cache_pmu_event_del(struct perf_event *event, int flags)
{
	struct hw_perf_event *hwc = &event->hw;
	struct scr_cache_pmu *spmu = to_scr_cache_pmu(event->pmu);
	struct sbiret ret;

	scr_cache_pmu_event_stop(event, PERF_EF_UPDATE);
	/* The firmware need to reset the counter mapping */
	ret = sbi_scr_pmu_stop(spmu, hwc->idx, 1, SBI_PMU_STOP_FLAG_RESET);
	if (ret.error && (ret.error != SBI_ERR_ALREADY_STOPPED))
		pr_err("Stopping counter idx %d failed with error %d\n",
		       hwc->idx, sbi_err_map_linux_errno(ret.error));

	perf_event_update_userpage(event);
	hwc->idx = -1;
}

static void scr_cache_pmu_event_read(struct perf_event *event)
{
	scr_cache_pmu_event_update(event);
}

/* cpumask */

static ssize_t cpumask_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct scr_cache_pmu *spmu = to_scr_cache_pmu(dev_get_drvdata(dev));

	return cpumap_print_to_pagebuf(true, buf, &spmu->cpumask);
}

static DEVICE_ATTR_RO(cpumask);

static struct attribute *scr_cache_pmu_cpumask_attrs[] = {
	&dev_attr_cpumask.attr,
	NULL,
};

static const struct attribute_group scr_cache_pmu_cpumask_attr_group = {
	.attrs = scr_cache_pmu_cpumask_attrs,
};

/*
 * Per PMU device attribute groups
 */

/*
 * attributes:
 *    format,
 *    events,
 *    cpumask(not used for dedicated cache),
 *    santinel
 */
static const struct attribute_group *scr_cache_pmu_attr_grps[4];


int scr_cache_pmu_init(struct scr_cache_pmu *spmu,
	const struct attribute_group *format, const struct attribute_group *events)
{
	int ret;
	int cnt = 0;
	struct sbiret sbi_ret;
	bool dedicated;
	int ctx = 0;
	int caps;
	int cpu;

	/* probe SBI vendor extension */
	ret = sbi_probe_extension(SBI_EXT_VENDOR_SCR);
	if (ret < 0) {
		pr_err("SBI has no support for Syntacore vendor extension\n");
		return ret;
	}

	/* probe SCR PMU extension features */
	sbi_ret = sbi_scr_pmu_probe(spmu);
	if (sbi_ret.error) {
		pr_err("Failed to get SCR PMU features: %ld\n", sbi_ret.error);
		return sbi_ret.error;
	}

	dedicated = !!(sbi_ret.value & CACHE_DEDICATED_FLAG);

	sbi_ret = sbi_scr_num_cnt(spmu);
	if (sbi_ret.error || sbi_ret.value == 0) {
		pr_err("No counters for SCR cache PMU found\n");
		return sbi_ret.error;
	}

	spmu->dedicated = dedicated;
	spmu->num_counters = sbi_ret.value;

	if (format)
		scr_cache_pmu_attr_grps[cnt++] = format;

	if (events)
		scr_cache_pmu_attr_grps[cnt++] = events;

	if (spmu->dedicated)
		caps = PERF_PMU_CAP_NO_INTERRUPT;
	else {
		caps = PERF_PMU_CAP_NO_EXCLUDE | PERF_PMU_CAP_NO_INTERRUPT;
		scr_cache_pmu_attr_grps[cnt++] = &scr_cache_pmu_cpumask_attr_group;

		// TODO: hotplug
		cpu = cpumask_first(cpu_online_mask);
		cpumask_set_cpu(cpu, &spmu->cpumask);

		ctx = perf_invalid_context;
	}

	spmu->pmu = (struct pmu) {
		.task_ctx_nr	= ctx,

		.attr_groups	= scr_cache_pmu_attr_grps,

		.event_init	= scr_cache_pmu_event_init,
		.add		= scr_cache_pmu_event_add,
		.del		= scr_cache_pmu_event_del,
		.start		= scr_cache_pmu_event_start,
		.stop		= scr_cache_pmu_event_stop,
		.read		= scr_cache_pmu_event_read,

		.capabilities   = caps,
	};


	return 0;
}

