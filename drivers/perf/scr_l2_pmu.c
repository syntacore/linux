// SPDX-License-Identifier: GPL-2.0
/*
 * Syntacore SCR7 L2 Cache PMU
 *
 * Copyright (C) 2022 YADRO
 *
 * This code is based on QCOM L3 and RISCV SBI pmu.
 */
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/bitops.h>
#include <linux/perf_event.h>
#include <linux/perf/riscv_pmu.h>
#include <linux/sysfs.h>

#include <asm/sbi.h>
#include <asm/hwcap.h>

#define SBI_SCR7_PMU_EXT	0x09000001
#define SBI_EXT_SCR_PMU_COUNTER_HW_READ	0x6
#define SCR_PMU_EVTYPE_MASK	0xf
#define SCR_PMU_BANKS_LOW_BIT	16
#define SCR_PMU_BANKS_HIGH_BIT	19
#define SCR_PMU_BANKS_SEL_MASK	GENMASK(SCR_PMU_BANKS_HIGH_BIT, SCR_PMU_BANKS_LOW_BIT)

#define sbi_scr_pmu_ecall(...) \
	sbi_ecall(SBI_SCR7_PMU_EXT, ##__VA_ARGS__)
#define sbi_scr_num_cnt() \
	sbi_scr_pmu_ecall(SBI_EXT_PMU_NUM_COUNTERS, 0, 0, 0, 0, 0, 0)
#define sbi_scr_pmu_read(idx) \
	sbi_scr_pmu_ecall(SBI_EXT_SCR_PMU_COUNTER_HW_READ, idx, 0, 0, 0, 0, 0)
#define sbi_scr_pmu_cfg_match(cbase, cmask, cflags, event_base, config) \
	sbi_scr_pmu_ecall(SBI_EXT_PMU_COUNTER_CFG_MATCH, cbase, cmask, cflags, event_base, config, 0)
#define sbi_scr_pmu_start(idx, cmask, flag, ival) \
	sbi_scr_pmu_ecall(SBI_EXT_PMU_COUNTER_START, idx, cmask, flag, ival, 0, 0)
#define sbi_scr_pmu_stop(idx, cmask, flag) \
	sbi_scr_pmu_ecall(SBI_EXT_PMU_COUNTER_STOP, idx, cmask, flag, 0, 0, 0)

#define RISCV_SCR_PMU_PDEV_NAME "scr-l2cache-pmu"

#define L2_NUM_COUNTERS	4

/*
 * Events
 */
enum scr_l2_event_types {
	SCR_L2_CACHE_HIT = 0,
	SCR_L2_CACHE_MISS,
	SCR_L2_CACHE_REFILL,
	SCR_L2_CACHE_EVICT_CLEAR,
	SCR_L2_CACHE_EVICT_DIRTY,
	SCR_L2_CACHE_EVICT_ROLLBACK,
	SCR_L2_CACHE_EVICT_COLLISION,
	SCR_L2_CACHE_EVICT_REQUEST,
	SCR_L2_CACHE_EVICT_SNOOP,
	SCR_L2_CACHE_MAX,
};

/*
 * Decoding of settings from perf_event_attr
 *
 * The config format for perf events is:
 * - config: bits 0-3:    event type
 *           bit  16-19:  bank selector (one bit for each bank)
 */

static inline u32 get_event_type(struct perf_event *event)
{
	return (event->attr.config) & SCR_PMU_EVTYPE_MASK;
}

static inline u32 get_banks_mask(struct perf_event *event)
{
	return (event->attr.config) & SCR_PMU_BANKS_SEL_MASK;
}

/*
 * Aggregate PMU. Implements the core pmu functions and manages
 * the hardware PMUs.
 */
struct scr_l2cache_pmu {
	struct pmu pmu;
	struct hlist_node node;
	int num_counters;
	cpumask_t cpumask;
	struct platform_device *pdev;
};

#define to_l2cache_pmu(p) (container_of(p, struct scr_l2cache_pmu, pmu))

static bool scr_l2_cache_validate_event_group(struct perf_event *event)
{
	struct scr_l2cache_pmu *spmu = to_l2cache_pmu(event->pmu);
	int counters = 0;

	return counters <= spmu->num_counters;
}

static int scr_l2_cache_event_init(struct perf_event *event)
{
	struct scr_l2cache_pmu *spmu = to_l2cache_pmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;
	u64 event_config = 0;

	/*
	 * Is the event for this PMU?
	 */
	if (event->attr.type != event->pmu->type)
		return -ENOENT;

	/*
	 * Sampling not supported since these events are not core-attributable.
	 */
	if (hwc->sample_period)
		return -EINVAL;

	/*
	 * Task mode not available, we run the counters as socket counters,
	 * not attributable to any CPU and therefore cannot attribute per-task.
	 */
	if (event->cpu < 0)
		return -EINVAL;

	/* Validate the group */
	if (!scr_l2_cache_validate_event_group(event))
		return -EINVAL;

	hwc->idx = -1;
	event_config = get_banks_mask(event);
	if (!event_config)
		/* if no banks were chosen we assume all banks are used */
		event_config |= SCR_PMU_BANKS_SEL_MASK;

	hwc->config = event_config;
	hwc->event_base = get_event_type(event);

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
	return 0;
}

static void scr_l2_cache_event_start(struct perf_event *event, int flags)
{
	struct hw_perf_event *hwc = &event->hw;
	unsigned long flag = SBI_PMU_START_FLAG_SET_INIT_VALUE;
	struct sbiret ret;
	u64 ival;

	hwc->state = 0;
	ival = local64_read(&hwc->prev_count);
	ret = sbi_scr_pmu_start(hwc->idx, 1, flag, ival);
	if (ret.error && (ret.error != SBI_ERR_ALREADY_STARTED))
		pr_err("Starting counter idx %d failed with error %d\n",
		       hwc->idx, sbi_err_map_linux_errno(ret.error));

	perf_event_update_userpage(event);
}

static u64 scr_pmu_event_update(struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;
	u64 prev_raw_count, new_raw_count;
	u64 oldval, delta;
	struct sbiret ret;

	do {
		prev_raw_count = local64_read(&hwc->prev_count);
		ret = sbi_scr_pmu_read(hwc->idx);
		if (ret.error)
			break;

		new_raw_count = ret.value;
		oldval = local64_cmpxchg(&hwc->prev_count, prev_raw_count,
					 new_raw_count);
	} while (oldval != prev_raw_count);

	delta = (new_raw_count - prev_raw_count);
	local64_add(delta, &event->count);
	local64_sub(delta, &hwc->period_left);

	return delta;
}

static void scr_l2_cache_event_stop(struct perf_event *event, int flags)
{
	struct hw_perf_event *hwc = &event->hw;
	struct sbiret ret;

	WARN_ON_ONCE(hwc->state & PERF_HES_STOPPED);

	if (!(hwc->state & PERF_HES_STOPPED)) {
		ret = sbi_scr_pmu_stop(hwc->idx, 1, 0);
		if (ret.error && (ret.error != SBI_ERR_ALREADY_STOPPED) &&
			flags != SBI_PMU_STOP_FLAG_RESET)
			pr_err("Stopping counter idx %d failed with error %d\n",
			       hwc->idx, sbi_err_map_linux_errno(ret.error));

		hwc->state |= PERF_HES_STOPPED;
		scr_pmu_event_update(event);
		hwc->state |= PERF_HES_UPTODATE;
	}
}

static int scr_l2_cache_event_add(struct perf_event *event, int flags)
{
	struct scr_l2cache_pmu *spmu = to_l2cache_pmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;
	struct sbiret ret;
	int idx;
	uint64_t cbase = 0;
	uint64_t cmask = GENMASK_ULL(spmu->num_counters - 1, 0);
	unsigned long cflags = 0;

	ret = sbi_scr_pmu_cfg_match(cbase, cmask, cflags, hwc->event_base, hwc->config);
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
		scr_l2_cache_event_start(event, PERF_EF_RELOAD);

	/* Propagate our changes to the userspace mapping. */
	perf_event_update_userpage(event);

	return 0;
}

static void scr_l2_cache_event_del(struct perf_event *event, int flags)
{
	struct hw_perf_event *hwc = &event->hw;
	struct sbiret ret;

	scr_l2_cache_event_stop(event, PERF_EF_UPDATE);
	/* The firmware need to reset the counter mapping */
	ret = sbi_scr_pmu_stop(hwc->idx, 1, SBI_PMU_STOP_FLAG_RESET);
	if (ret.error && (ret.error != SBI_ERR_ALREADY_STOPPED))
		pr_err("Stopping counter idx %d failed with error %d\n",
		       hwc->idx, sbi_err_map_linux_errno(ret.error));

	perf_event_update_userpage(event);
	hwc->idx = -1;
}

static void scr_l2_cache_event_read(struct perf_event *event)
{
	scr_pmu_event_update(event);
}

/* formats */
static ssize_t l2cache_pmu_format_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct dev_ext_attribute *eattr;

	eattr = container_of(attr, struct dev_ext_attribute, attr);
	return sysfs_emit(buf, "%s\n", (char *) eattr->var);
}

#define L2CACHE_PMU_FORMAT_ATTR(_name, _config)				      \
(&((struct dev_ext_attribute[]) {				      \
	{ .attr = __ATTR(_name, 0444, l2cache_pmu_format_show, NULL), \
		.var = (void *) _config, }				      \
})[0].attr.attr)

static struct attribute *scr_l2_cache_pmu_formats[] = {
	L2CACHE_PMU_FORMAT_ATTR(event, "config:0-3"),
	L2CACHE_PMU_FORMAT_ATTR(banks, "config:16-19"),
	NULL,
};

static const struct attribute_group scr_l2_cache_pmu_format_group = {
	.name = "format",
	.attrs = scr_l2_cache_pmu_formats,
};

/* events */

static ssize_t l2cache_pmu_event_show(struct device *dev,
				      struct device_attribute *attr, char *page)
{
	struct perf_pmu_events_attr *pmu_attr;

	pmu_attr = container_of(attr, struct perf_pmu_events_attr, attr);
	return sysfs_emit(page, "event=0x%02llx\n", pmu_attr->id);
}

#define L2CACHE_EVENT_ATTR(_name, _id)					     \
PMU_EVENT_ATTR_ID(_name, l2cache_pmu_event_show, _id)

static struct attribute *scr_l2_cache_pmu_events[] = {
	L2CACHE_EVENT_ATTR(hit, SCR_L2_CACHE_HIT),
	L2CACHE_EVENT_ATTR(miss, SCR_L2_CACHE_MISS),
	L2CACHE_EVENT_ATTR(refill, SCR_L2_CACHE_REFILL),
	L2CACHE_EVENT_ATTR(evict-clear, SCR_L2_CACHE_EVICT_CLEAR),
	L2CACHE_EVENT_ATTR(evict-dirty, SCR_L2_CACHE_EVICT_DIRTY),
	L2CACHE_EVENT_ATTR(evict-rollback, SCR_L2_CACHE_EVICT_ROLLBACK),
	L2CACHE_EVENT_ATTR(evict-collision, SCR_L2_CACHE_EVICT_COLLISION),
	L2CACHE_EVENT_ATTR(evict-request, SCR_L2_CACHE_EVICT_REQUEST),
	L2CACHE_EVENT_ATTR(evict-snoop, SCR_L2_CACHE_EVICT_SNOOP),
	NULL
};

static const struct attribute_group scr_l2_cache_pmu_events_group = {
	.name = "events",
	.attrs = scr_l2_cache_pmu_events,
};

/* cpumask */

static ssize_t cpumask_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct scr_l2cache_pmu *l2pmu = to_l2cache_pmu(dev_get_drvdata(dev));

	return cpumap_print_to_pagebuf(true, buf, &l2pmu->cpumask);
}

static DEVICE_ATTR_RO(cpumask);

static struct attribute *scr_l2_cache_pmu_cpumask_attrs[] = {
	&dev_attr_cpumask.attr,
	NULL,
};

static const struct attribute_group scr_l2_cache_pmu_cpumask_attr_group = {
	.attrs = scr_l2_cache_pmu_cpumask_attrs,
};

/*
 * Per PMU device attribute groups
 */
static const struct attribute_group *scr_l2_cache_pmu_attr_grps[] = {
	&scr_l2_cache_pmu_format_group,
	&scr_l2_cache_pmu_events_group,
	&scr_l2_cache_pmu_cpumask_attr_group,
	NULL,
};

static int scr_l2_cache_pmu_probe(struct platform_device *pdev)
{
	int ret;
	struct sbiret sbi_ret;
	struct scr_l2cache_pmu *l2pmu;
	int cpu;

	/* probe SBI extension */
	ret = sbi_probe_extension(SBI_SCR7_PMU_EXT);
	if (ret < 0) {
		dev_err(&pdev->dev, "No SCR L2 cache PMU extension found\n");
		return ret;
	}

	sbi_ret = sbi_scr_num_cnt();
	if (sbi_ret.error || sbi_ret.value == 0) {
		dev_err(&pdev->dev, "No counters for SCR L2 cache PMU extension found\n");
		return sbi_ret.error;
	}

	l2pmu = devm_kzalloc(&pdev->dev, sizeof(*l2pmu), GFP_KERNEL);
	if (!l2pmu)
		return -ENOMEM;

	l2pmu->num_counters = sbi_ret.value;

	l2pmu->pmu = (struct pmu) {
		.task_ctx_nr	= perf_invalid_context,

		.attr_groups	= scr_l2_cache_pmu_attr_grps,

		.event_init	= scr_l2_cache_event_init,
		.add		= scr_l2_cache_event_add,
		.del		= scr_l2_cache_event_del,
		.start		= scr_l2_cache_event_start,
		.stop		= scr_l2_cache_event_stop,
		.read		= scr_l2_cache_event_read,

		.capabilities	= PERF_PMU_CAP_NO_EXCLUDE | PERF_PMU_CAP_NO_INTERRUPT,
	};

	// TODO: hotplug
	cpu = cpumask_first(cpu_online_mask);
	cpumask_set_cpu(cpu, &l2pmu->cpumask);

	ret = perf_pmu_register(&l2pmu->pmu, "scr_l2cache_pmu", -1);
	if (ret < 0) {
		dev_err(&pdev->dev, "Failed to register SCR L2 cache PMU (%d)\n", ret);
		return ret;
	}

	dev_info(&pdev->dev, "Registered %s, type: %d\n", RISCV_SCR_PMU_PDEV_NAME, l2pmu->pmu.type);

	return 0;
}

static int scr_l2_cache_pmu_remove(struct platform_device *pdev)
{
	struct scr_l2cache_pmu *scr_l2cache_pmu =
		to_l2cache_pmu(platform_get_drvdata(pdev));

	perf_pmu_unregister(&scr_l2cache_pmu->pmu);
	return 0;
}

static struct platform_driver scr_l2_cache_pmu_driver = {
	.probe = scr_l2_cache_pmu_probe,
	.remove = scr_l2_cache_pmu_remove,
	.driver = {
		.name = RISCV_SCR_PMU_PDEV_NAME,
	},
};

static int __init register_scr_l2_cache_pmu_driver(void)
{
	int ret;
	struct platform_device *pdev;

	ret = platform_driver_register(&scr_l2_cache_pmu_driver);
	if (ret)
		return ret;

	pdev = platform_device_register_simple(RISCV_SCR_PMU_PDEV_NAME, -1, NULL, 0);
	if (IS_ERR(pdev)) {
		platform_driver_unregister(&scr_l2_cache_pmu_driver);
		return PTR_ERR(pdev);
	}

	return ret;
}
device_initcall(register_scr_l2_cache_pmu_driver);
