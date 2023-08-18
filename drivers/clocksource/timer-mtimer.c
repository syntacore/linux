// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Syntacore.
 *
 * Syntacore SCRx RISC-V systems use MMIO machine timer in M-mode.
 */

#include <linux/bitops.h>
#include <linux/clocksource.h>
#include <linux/clockchips.h>
#include <linux/cpu.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/sched_clock.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/interrupt.h>
#include <linux/of_irq.h>
#include <linux/regmap.h>
#include <linux/smp.h>
#include <linux/timex.h>

#include <asm/clint.h>

#define MTIMER_REG_CTRL         0x00
#define MTIMER_REG_DIV          0x04
#define MTIMER_REG_VAL_L        0x08
#define MTIMER_REG_VAL_H        0x0c
#define MTIMER_REG_CMP_L        0x10
#define MTIMER_REG_CMP_H        0x14

#define MTIMER_CTRL_ENA         BIT(0)
#define MTIMER_CTRL_SRC_EXT     BIT(1)
#define MTIMER_DIV_MASK         0x3f

static unsigned int mt_timer_irq;
struct regmap *mt_regmap;

#if !defined(CONFIG_CLINT_TIMER)
u64 __iomem *clint_time_val;
EXPORT_SYMBOL(clint_time_val);
#endif

static struct regmap_config mtimer_config = {
	.name = "mtimer-regmap",
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.val_format_endian = REGMAP_ENDIAN_LITTLE,
	.max_register = 0x14,
	.fast_io = true,
};

static u64 notrace mt_get_cycles64(void)
{
	return get_cycles64();
}

static u64 mt_rdtime(struct clocksource *cs)
{
	return get_cycles64();
}

static struct clocksource mt_clocksource = {
	.name		= "mt_clocksource",
	.rating		= 300,
	.mask		= CLOCKSOURCE_MASK(64),
	.flags		= CLOCK_SOURCE_IS_CONTINUOUS,
	.read		= mt_rdtime,
};

static int mt_clock_next_event(unsigned long delta,
			       struct clock_event_device *ce)
{
	u64 val;

	csr_set(CSR_IE, IE_TIE);
	val = mt_get_cycles64() + delta;

	regmap_write(mt_regmap, MTIMER_REG_CMP_L, val & 0xffffffff);
	regmap_write(mt_regmap, MTIMER_REG_CMP_H, val >> 32);

	return 0;
}

static DEFINE_PER_CPU(struct clock_event_device, mt_clock_event) = {
	.name		= "mt_clockevent",
	.features	= CLOCK_EVT_FEAT_ONESHOT,
	.rating		= 100,
	.set_next_event	= mt_clock_next_event,
};

static int mt_timer_starting_cpu(unsigned int cpu)
{
	struct clock_event_device *ce = per_cpu_ptr(&mt_clock_event, cpu);

	ce->cpumask = cpumask_of(cpu);
	clockevents_config_and_register(ce, riscv_timebase, 100, 0x7fffffff);
	enable_percpu_irq(mt_timer_irq, irq_get_trigger_type(mt_timer_irq));

	return 0;
}

static int mt_timer_dying_cpu(unsigned int cpu)
{
	disable_percpu_irq(mt_timer_irq);
	return 0;
}

static irqreturn_t mt_timer_interrupt(int irq, void *dev_id)
{
	struct clock_event_device *evdev = this_cpu_ptr(&mt_clock_event);

	csr_clear(CSR_IE, IE_TIE);
	evdev->event_handler(evdev);

	return IRQ_HANDLED;
}

static int mt_timer_enable(struct device_node *np)
{
	u32 rate;

	/* keep bootloader divider settings if 'clock-frequency' is not
	 * specified
	 */
	if (!of_property_read_u32(np, "clock-frequency", &rate))
		regmap_update_bits(mt_regmap, MTIMER_REG_DIV, MTIMER_DIV_MASK,
				   rate / riscv_timebase - 1);

	if (of_property_read_bool(np, "clock-external"))
		regmap_write(mt_regmap, MTIMER_REG_CTRL,
			     MTIMER_CTRL_ENA | MTIMER_CTRL_SRC_EXT);
	else
		regmap_write(mt_regmap, MTIMER_REG_CTRL,
			     MTIMER_CTRL_ENA);

	return 0;
}

static int __init mt_timer_init_dt(struct device_node *np)
{
	struct of_phandle_args oirq;
	void __iomem *base;
	u32 nr_irqs;
	int rc;

	/* ensure that MTIMER device interrupt is RV_IRQ_TIMER */

	nr_irqs = of_irq_count(np);
	if (nr_irqs != 1) {
		pr_err("%pOFP: invalid timer irq count: %d\n", np, nr_irqs);
		return -ENODEV;
	}

	if (of_irq_parse_one(np, 0, &oirq)) {
		pr_err("%pOFP: failed to parse irq\n", np);
		return -ENODEV;
	}

	if (oirq.args_count != 1 || oirq.args[0] != RV_IRQ_TIMER) {
		pr_err("%pOFP: invalid hwirq %d\n", np, oirq.args[0]);
		return -ENODEV;
	}

	/* find parent irq domain and map timer irq */
	if (irq_find_host(oirq.np))
		mt_timer_irq = irq_of_parse_and_map(np, 0);

	/* If mtimer irq not found then fail */
	if (!mt_timer_irq) {
		pr_err("%pOFP: timer irq not found\n", np);
		return -ENODEV;
	}

	base = of_iomap(np, 0);
	if (!base) {
		pr_err("%pOFP: could not map registers\n", np);
		return -ENODEV;
	}

	mt_regmap = regmap_init_mmio(NULL, base, &mtimer_config);
	if (IS_ERR(mt_regmap)) {
		pr_err("%pOFP: failed to init regmap\n", np);
		rc = PTR_ERR(mt_regmap);
		goto fail_iounmap;
	}

	/*
	 * Yes, that's an odd naming scheme.  time_val is public, but hopefully
	 * will die in favor of something cleaner.
	 */
	clint_time_val = base + MTIMER_REG_VAL_L;

	rc = mt_timer_enable(np);
	if (rc) {
		pr_err("%pOFP: failed to enable timer\n", np);
		goto fail_iounmap;
	}

	pr_info("%pOFP: timer running at %ld Hz\n", np, riscv_timebase);

	rc = clocksource_register_hz(&mt_clocksource, riscv_timebase);
	if (rc) {
		pr_err("%pOFP: clocksource register failed [%d]\n", np, rc);
		goto fail_iounmap;
	}

	sched_clock_register(mt_get_cycles64, 64, riscv_timebase);

	rc = request_percpu_irq(mt_timer_irq, mt_timer_interrupt,
				"mtimer", &mt_clock_event);
	if (rc) {
		pr_err("registering percpu irq failed [%d]\n", rc);
		goto fail_iounmap;
	}

	rc = cpuhp_setup_state(CPUHP_AP_CLINT_TIMER_STARTING,
			       "clockevents/mtimer/timer:starting",
			       mt_timer_starting_cpu,
			       mt_timer_dying_cpu);
	if (rc) {
		pr_err("%pOFP: cpuhp setup state failed [%d]\n", np, rc);
		goto fail_free_irq;
	}

	return 0;

fail_free_irq:
	free_irq(mt_timer_irq, &mt_clock_event);
fail_iounmap:
	iounmap(base);
	return rc;
}

TIMER_OF_DECLARE(mt_timer, "scr,mtimer0", mt_timer_init_dt);
