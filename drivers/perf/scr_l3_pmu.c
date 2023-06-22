// SPDX-License-Identifier: GPL-2.0
/*
 * Syntacore SCR7 L3 Cache PMU
 *
 * Copyright (C) 2023 YADRO
 *
 */
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/bitops.h>
#include <linux/perf_event.h>
#include <linux/perf/riscv_pmu.h>
#include <linux/sysfs.h>

#include <asm/sbi.h>
#include <asm/hwcap.h>

#include "scr_cache_pmu_core.h"

#define SCR_PMU_EVTYPE_MASK	0xff
#define SCR_PMU_BANKS_SEL_MASK	0xff00

#define RISCV_SCR_L3_PMU_PDEV_NAME "scr-l3cache-pmu"

/*
 * Events
 */
enum scr_l3_event_types {
	SCR_L3_CACHE_HIT = 1,
	SCR_L3_CACHE_MISS,
	SCR_L3_CACHE_RETRY,
	SCR_L3_CACHE_EVICT_CLEAR,
	SCR_L3_CACHE_EVICT_DIRTY,
	SCR_L3_CACHE_ROLLBACK,
	SCR_L3_CACHE_COLLISION,
	SCR_L3_CACHE_REQUEST,
	SCR_L3_CACHE_SNOOP,
	SCR_L3_CACHE_WRITES,
	SCR_L3_CACHE_READS,
	SCR_L3_CACHE_DAT_FLITS,
	SCR_L3_CACHE_CLK,
	SCR_L3_CACHE_MAX,
};

/*
 * Aggregate PMU. Implements the core pmu functions and manages
 * the hardware PMUs.
 */
struct scr_l3cache_pmu {
	struct scr_cache_pmu spmu;
	struct hlist_node node;
	struct platform_device *pdev;
};

/* formats */
static ssize_t l3cache_pmu_format_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct dev_ext_attribute *eattr;

	eattr = container_of(attr, struct dev_ext_attribute, attr);
	return sysfs_emit(buf, "%s\n", (char *) eattr->var);
}

#define L3CACHE_PMU_FORMAT_ATTR(_name, _config)				      \
(&((struct dev_ext_attribute[]) {				      \
	{ .attr = __ATTR(_name, 0444, l3cache_pmu_format_show, NULL), \
		.var = (void *) _config, }				      \
})[0].attr.attr)

static struct attribute *scr_l3_cache_pmu_formats[] = {
	L3CACHE_PMU_FORMAT_ATTR(event, "config:0-7"),
	L3CACHE_PMU_FORMAT_ATTR(banks, "config:8-15"),
	NULL,
};

static const struct attribute_group scr_l3_cache_pmu_format_group = {
	.name = "format",
	.attrs = scr_l3_cache_pmu_formats,
};

/* events */

static ssize_t l3cache_pmu_event_show(struct device *dev,
				      struct device_attribute *attr, char *page)
{
	struct perf_pmu_events_attr *pmu_attr;

	pmu_attr = container_of(attr, struct perf_pmu_events_attr, attr);
	return sysfs_emit(page, "event=0x%02llx\n", pmu_attr->id);
}

#define L3CACHE_EVENT_ATTR(_name, _id)					     \
PMU_EVENT_ATTR_ID(_name, l3cache_pmu_event_show, _id)

static struct attribute *scr_l3_cache_pmu_events[] = {
	L3CACHE_EVENT_ATTR(hit, SCR_L3_CACHE_HIT),
	L3CACHE_EVENT_ATTR(miss, SCR_L3_CACHE_MISS),
	L3CACHE_EVENT_ATTR(retry, SCR_L3_CACHE_RETRY),
	L3CACHE_EVENT_ATTR(evict-clear, SCR_L3_CACHE_EVICT_CLEAR),
	L3CACHE_EVENT_ATTR(evict-dirty, SCR_L3_CACHE_EVICT_DIRTY),
	L3CACHE_EVENT_ATTR(rollback, SCR_L3_CACHE_ROLLBACK),
	L3CACHE_EVENT_ATTR(collision, SCR_L3_CACHE_COLLISION),
	L3CACHE_EVENT_ATTR(request, SCR_L3_CACHE_REQUEST),
	L3CACHE_EVENT_ATTR(snoop, SCR_L3_CACHE_SNOOP),
	L3CACHE_EVENT_ATTR(writes, SCR_L3_CACHE_WRITES),
	L3CACHE_EVENT_ATTR(reads, SCR_L3_CACHE_READS),
	L3CACHE_EVENT_ATTR(dat_flits, SCR_L3_CACHE_DAT_FLITS),
	L3CACHE_EVENT_ATTR(clk, SCR_L3_CACHE_DAT_FLITS),
	NULL
};

static const struct attribute_group scr_l3_cache_pmu_events_group = {
	.name = "events",
	.attrs = scr_l3_cache_pmu_events,
};

static int scr_l3_cache_pmu_probe(struct platform_device *pdev)
{
	int ret;
	struct scr_cache_pmu *spmu;
	struct scr_l3cache_pmu *l3pmu;

	l3pmu = devm_kzalloc(&pdev->dev, sizeof(*l3pmu), GFP_KERNEL);
	if (!l3pmu)
		return -ENOMEM;

	spmu = &l3pmu->spmu;
	spmu->sbi_fn = SBI_SCR7_L3_PMU_FN;
	spmu->event_mask = SCR_PMU_EVTYPE_MASK;
	spmu->bank_mask = SCR_PMU_BANKS_SEL_MASK;

	ret = scr_cache_pmu_init(spmu, &scr_l3_cache_pmu_format_group,
			&scr_l3_cache_pmu_events_group);
	if (ret < 0)
		return ret;

	ret = perf_pmu_register(&spmu->pmu, "scr_l3cache_pmu", -1);
	if (ret < 0) {
		dev_err(&pdev->dev, "Failed to register SCR L3 cache PMU (%d)\n", ret);
		return ret;
	}

	dev_info(&pdev->dev, "Registered %s, type: %d\n",
			RISCV_SCR_L3_PMU_PDEV_NAME, spmu->pmu.type);

	return 0;
}

static int scr_l3_cache_pmu_remove(struct platform_device *pdev)
{
	struct scr_cache_pmu *spmu =
		to_scr_cache_pmu(platform_get_drvdata(pdev));

	perf_pmu_unregister(&spmu->pmu);
	return 0;
}

static struct platform_driver scr_l3_cache_pmu_driver = {
	.probe = scr_l3_cache_pmu_probe,
	.remove = scr_l3_cache_pmu_remove,
	.driver = {
		.name = RISCV_SCR_L3_PMU_PDEV_NAME,
	},
};

static int __init register_scr_l3_cache_pmu_driver(void)
{
	int ret;
	struct platform_device *pdev;

	ret = platform_driver_register(&scr_l3_cache_pmu_driver);
	if (ret)
		return ret;

	pdev = platform_device_register_simple(RISCV_SCR_L3_PMU_PDEV_NAME, -1, NULL, 0);
	if (IS_ERR(pdev)) {
		platform_driver_unregister(&scr_l3_cache_pmu_driver);
		return PTR_ERR(pdev);
	}

	return ret;
}
device_initcall(register_scr_l3_cache_pmu_driver);
