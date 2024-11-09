// SPDX-License-Identifier: GPL-2.0
/*
 * Syntacore SCR7 L2 Cache PMU
 *
 * Copyright (C) 2022-2023 Syntacore
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

#include "scr_cache_pmu_core.h"

/* Version identifier is taken from L2_VID
 * description of corresponding EAS
 */
#define SCR_PMU_ID_V2		0x24062001
#define SCR_PMU_V1_STR		"v1"
#define SCR_PMU_V2_STR		"v2"
#define SCR_PMU_EVTYPE_MASK_V1	GENMASK(4, 0)
#define SCR_PMU_EVTYPE_MASK_V2	GENMASK(5, 0)
#define SCR_PMU_BANKS_LOW_BIT	16
#define SCR_PMU_BANKS_HIGH_BIT	19
#define SCR_PMU_BANKS_SEL_MASK	GENMASK(SCR_PMU_BANKS_HIGH_BIT, SCR_PMU_BANKS_LOW_BIT)

#define RISCV_SCR_L2_PMU_PDEV_NAME "scr-l2cache-pmu"

static struct pmu_scr_cfg cfg_l2;

/*
 * Aggregate PMU. Implements the core pmu functions and manages
 * the hardware PMUs.
 */
struct scr_l2cache_pmu {
	struct scr_cache_pmu spmu;
	struct hlist_node node;
	struct platform_device *pdev;
	const char *version;
};

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
	L2CACHE_PMU_FORMAT_ATTR(event, "config:0-5"),
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

/* The old version of L2 events */
static struct attribute *scr_l2_cache_pmu_events_v1[] = {
	L2CACHE_EVENT_ATTR(hit, 0x0),
	L2CACHE_EVENT_ATTR(miss, 0x1),
	L2CACHE_EVENT_ATTR(refill, 0x2),
	L2CACHE_EVENT_ATTR(evictc, 0x3),
	L2CACHE_EVENT_ATTR(evictd, 0x4),
	L2CACHE_EVENT_ATTR(rollback, 0x5),
	L2CACHE_EVENT_ATTR(collision, 0x6),
	L2CACHE_EVENT_ATTR(request, 0x7),
	L2CACHE_EVENT_ATTR(snoop, 0x8),
	NULL
};

/* The latest version of L2 events */
static struct attribute *scr_l2_cache_pmu_events_v2[] = {
	L2CACHE_EVENT_ATTR(hit, 0x0),
	L2CACHE_EVENT_ATTR(miss, 0x1),
	L2CACHE_EVENT_ATTR(refill, 0x2),
	L2CACHE_EVENT_ATTR(evictc, 0x3),
	L2CACHE_EVENT_ATTR(evictd, 0x4),
	L2CACHE_EVENT_ATTR(rollback, 0x5),
	L2CACHE_EVENT_ATTR(collision, 0x6),
	L2CACHE_EVENT_ATTR(request, 0x7),
	L2CACHE_EVENT_ATTR(snoop, 0x8),
	L2CACHE_EVENT_ATTR(tp_blk_dp_index_hit_evict, 0x9),
	L2CACHE_EVENT_ATTR(tp_blk_dp_index_hit_refill, 0xa),
	L2CACHE_EVENT_ATTR(tp_blk_all_way_locked, 0xb),
	L2CACHE_EVENT_ATTR(tp_blk_ru_index_way_evict, 0xc),
	L2CACHE_EVENT_ATTR(tp_blk_ru_index_way_refill, 0xd),
	L2CACHE_EVENT_ATTR(tp_blk_su_index_way_evict, 0xe),
	L2CACHE_EVENT_ATTR(tp_blk_su_index_way_refill, 0xf),
	L2CACHE_EVENT_ATTR(tp_blk_d1_retry, 0x10),
	L2CACHE_EVENT_ATTR(tp_blk_su_busy, 0x11),
	L2CACHE_EVENT_ATTR(tp_blk_ru_busy, 0x12),
	L2CACHE_EVENT_ATTR(tp_blk_sq_busy, 0x13),
	L2CACHE_EVENT_ATTR(tp_blk_dp_busy, 0x14),
	L2CACHE_EVENT_ATTR(tp_blk_rb_busy, 0x15),
	L2CACHE_EVENT_ATTR(dp_req, 0x16),
	L2CACHE_EVENT_ATTR(dp_blk, 0x17),
	L2CACHE_EVENT_ATTR(dp_blk_d5_acc, 0x18),
	L2CACHE_EVENT_ATTR(dp_blk_d5_slot_capture, 0x19),
	L2CACHE_EVENT_ATTR(dp_blk_d5_slot, 0x1a),
	L2CACHE_EVENT_ATTR(dp_blk_d6, 0x1b),
	L2CACHE_EVENT_ATTR(ext_snoop, 0x1c),
	L2CACHE_EVENT_ATTR(read_miss, 0x1d),
	L2CACHE_EVENT_ATTR(read_hit, 0x1e),
	L2CACHE_EVENT_ATTR(read_hit_ru, 0x1f),
	L2CACHE_EVENT_ATTR(write_miss, 0x20),
	L2CACHE_EVENT_ATTR(write_hit, 0x21),
	L2CACHE_EVENT_ATTR(write_hit_s, 0x22),
	L2CACHE_EVENT_ATTR(write_hit_ru, 0x23),
	NULL
};


static struct attribute_group scr_l2_cache_pmu_events_group = {
	.name = "events",
};

static int scr_l2_cache_pmu_probe(struct platform_device *pdev)
{
	int ret;
	unsigned long vid = 0ul;
	struct scr_cache_pmu *spmu;
	struct scr_l2cache_pmu *l2pmu;

	l2pmu = devm_kzalloc(&pdev->dev, sizeof(*l2pmu), GFP_KERNEL);
	if (!l2pmu)
		return -ENOMEM;

	spmu = &l2pmu->spmu;
	spmu->sbi_fn = SBI_SCR7_L2_PMU_FN;
	spmu->bank_mask = SCR_PMU_BANKS_SEL_MASK;

	ret = scr_cache_pmu_vid(SBI_SCR7_L2_PMU_FN, &vid);
	/* Do not return err to be compatible with old SBIs
	 * If error occurs V1 will be used
	 */
	if (ret < 0)
		dev_warn(&pdev->dev, "Failed to get L2_VID (%d)\n", ret);

	if (vid < SCR_PMU_ID_V2) {
		l2pmu->version = SCR_PMU_V1_STR;
		spmu->event_mask = SCR_PMU_EVTYPE_MASK_V1;
		scr_l2_cache_pmu_events_group.attrs = scr_l2_cache_pmu_events_v1;
	} else {
		l2pmu->version = SCR_PMU_V2_STR;
		spmu->event_mask = SCR_PMU_EVTYPE_MASK_V2;
		scr_l2_cache_pmu_events_group.attrs = scr_l2_cache_pmu_events_v2;
	}

	ret = scr_cache_pmu_init(spmu, &scr_l2_cache_pmu_format_group,
				 &scr_l2_cache_pmu_events_group);
	if (ret < 0)
		return ret;

	ret = perf_pmu_register(&spmu->pmu, "scr_l2cache_pmu", -1);
	if (ret < 0) {
		dev_err(&pdev->dev, "Failed to register SCR L2 cache PMU (%d)\n", ret);
		return ret;
	}
	cfg_l2 = (struct pmu_scr_cfg) {
		.num_counters = spmu->num_counters,
		.is_dedicated = spmu->dedicated,
		.is_available = true,
		.event_type = spmu->pmu.type,
	};

	dev_info(&pdev->dev, "Registered %s, type: %d, ver: %s, impl: %s\n", RISCV_SCR_L2_PMU_PDEV_NAME,
		spmu->pmu.type, l2pmu->version, spmu->dedicated ? "dedicated" : "shared");

	return 0;
}

static void scr_l2_cache_pmu_remove(struct platform_device *pdev)
{
	struct scr_cache_pmu *spmu =
		to_scr_cache_pmu(platform_get_drvdata(pdev));

	perf_pmu_unregister(&spmu->pmu);
}

static struct platform_driver scr_l2_cache_pmu_driver = {
	.probe = scr_l2_cache_pmu_probe,
	.remove = scr_l2_cache_pmu_remove,
	.driver = {
		.name = RISCV_SCR_L2_PMU_PDEV_NAME,
	},
};

static int __init register_scr_l2_cache_pmu_driver(void)
{
	int ret;
	struct platform_device *pdev;

	ret = platform_driver_register(&scr_l2_cache_pmu_driver);
	if (ret)
		return ret;

	pdev = platform_device_register_simple(RISCV_SCR_L2_PMU_PDEV_NAME, -1, NULL, 0);
	if (IS_ERR(pdev)) {
		platform_driver_unregister(&scr_l2_cache_pmu_driver);
		return PTR_ERR(pdev);
	}

	return ret;
}
device_initcall(register_scr_l2_cache_pmu_driver);

const struct pmu_scr_cfg *scr_pmu_l2_get_config(void)
{
	return &cfg_l2;
}
EXPORT_SYMBOL(scr_pmu_l2_get_config);
