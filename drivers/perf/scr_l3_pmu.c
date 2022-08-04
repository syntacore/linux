// SPDX-License-Identifier: GPL-2.0
/*
 * Syntacore SCR7 L3 Cache PMU
 *
 * Copyright (C) 2023 Syntacore Ltd.
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

/* Version identifier is taken from L3_VID
 * description of corresponding EAS
 */
#define SCR_PMU_ID_V1		0x2022010101
#define SCR_PMU_V1_STR		"v1"
#define SCR_PMU_V2_STR		"v2"
#define SCR_PMU_EVTYPE_MASK	0xff
#define SCR_PMU_BANKS_SEL_MASK	0xff00

#define RISCV_SCR_L3_PMU_PDEV_NAME "scr-l3cache-pmu"

static struct pmu_scr_cfg cfg_l3;

/*
 * Aggregate PMU. Implements the core pmu functions and manages
 * the hardware PMUs.
 */
struct scr_l3cache_pmu {
	struct scr_cache_pmu spmu;
	struct hlist_node node;
	struct platform_device *pdev;
	const char *version;
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

static struct attribute *scr_l3_cache_pmu_events_v1[] = {
	L3CACHE_EVENT_ATTR(hit, 0x1),
	L3CACHE_EVENT_ATTR(miss, 0x2),
	L3CACHE_EVENT_ATTR(retry, 0x3),
	L3CACHE_EVENT_ATTR(evict-clear, 0x4),
	L3CACHE_EVENT_ATTR(evict-dirty, 0x5),
	L3CACHE_EVENT_ATTR(rollback, 0x6),
	L3CACHE_EVENT_ATTR(collision, 0x7),
	L3CACHE_EVENT_ATTR(request, 0x8),
	L3CACHE_EVENT_ATTR(snoop, 0x9),
	L3CACHE_EVENT_ATTR(writes, 0xa),
	L3CACHE_EVENT_ATTR(reads, 0xb),
	L3CACHE_EVENT_ATTR(dat_flits, 0xc),
	L3CACHE_EVENT_ATTR(clk, 0xd),
	NULL
};

static struct attribute *scr_l3_cache_pmu_events_v2[] = {
	L3CACHE_EVENT_ATTR(hit, 0x1),
	L3CACHE_EVENT_ATTR(miss, 0x2),
	L3CACHE_EVENT_ATTR(readhit, 0x3),
	L3CACHE_EVENT_ATTR(readhazard, 0x4),
	L3CACHE_EVENT_ATTR(readmiss, 0x5),
	L3CACHE_EVENT_ATTR(writehit, 0x6),
	L3CACHE_EVENT_ATTR(writehitcoherency, 0x7),
	L3CACHE_EVENT_ATTR(writehazard, 0x8),
	L3CACHE_EVENT_ATTR(writemiss, 0x9),
	L3CACHE_EVENT_ATTR(writeback, 0xa),
	L3CACHE_EVENT_ATTR(evictclean, 0xb),
	L3CACHE_EVENT_ATTR(evictdirty, 0xc),
	L3CACHE_EVENT_ATTR(prefetchhit, 0xd),
	L3CACHE_EVENT_ATTR(prefetchhazard, 0xe),
	L3CACHE_EVENT_ATTR(prefetchmiss, 0xf),
	NULL
};

static struct attribute_group scr_l3_cache_pmu_events_group = {
	.name = "events",
};

static int scr_l3_cache_pmu_probe(struct platform_device *pdev)
{
	int ret;
	unsigned long vid = 0ul;
	struct scr_cache_pmu *spmu;
	struct scr_l3cache_pmu *l3pmu;

	l3pmu = devm_kzalloc(&pdev->dev, sizeof(*l3pmu), GFP_KERNEL);
	if (!l3pmu)
		return -ENOMEM;

	spmu = &l3pmu->spmu;
	spmu->sbi_fn = SBI_SCR7_L3_PMU_FN;
	spmu->event_mask = SCR_PMU_EVTYPE_MASK;
	spmu->bank_mask = SCR_PMU_BANKS_SEL_MASK;

	ret = scr_cache_pmu_vid(SBI_SCR7_L3_PMU_FN, &vid);
	/* As event sets are totally different
	 * return error if getting vid fails
	 */
	if (ret < 0) {
		dev_err(&pdev->dev, "Failed to get L3_VID (%d)\n", ret);
		return -ENOTSUPP;
	}

	if (vid <= SCR_PMU_ID_V1) {
		l3pmu->version = SCR_PMU_V1_STR;
		scr_l3_cache_pmu_events_group.attrs = scr_l3_cache_pmu_events_v1;
	} else {
		l3pmu->version = SCR_PMU_V2_STR;
		scr_l3_cache_pmu_events_group.attrs = scr_l3_cache_pmu_events_v2;
	}

	ret = scr_cache_pmu_init(spmu, &scr_l3_cache_pmu_format_group,
			&scr_l3_cache_pmu_events_group);
	if (ret < 0)
		return ret;

	ret = perf_pmu_register(&spmu->pmu, "scr_l3cache_pmu", -1);
	if (ret < 0) {
		dev_err(&pdev->dev, "Failed to register SCR L3 cache PMU (%d)\n", ret);
		return ret;
	}
	cfg_l3 = (struct pmu_scr_cfg) {
		.num_counters = spmu->num_counters,
		.is_dedicated = false,
		.is_available = true,
		.event_type = spmu->pmu.type,
	};

	dev_info(&pdev->dev, "Registered %s, type: %d ver: %s\n",
			RISCV_SCR_L3_PMU_PDEV_NAME, spmu->pmu.type, l3pmu->version);

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

const struct pmu_scr_cfg *scr_pmu_l3_get_config(void)
{
	return &cfg_l3;
}
EXPORT_SYMBOL(scr_pmu_l3_get_config);
