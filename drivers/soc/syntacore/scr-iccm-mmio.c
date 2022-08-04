// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2023 Syntacore
 */

#include <linux/cpu.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/irq.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/of_address.h>
#include <linux/regmap.h>
#include <linux/sched_clock.h>

#define SCR_IPI_BUFSTATUS       (0x0)
#define SCR_IPI_BUFREAD         (0x4)
#define SCR_IPI_BUFWRITE_N(N)   (0xc00 + (N) * 4)
#define SCR_IPI_SNDSTAT_N_LO(N) (0x400 + (N) * 16)

#define SCR_IPI_VERSION         (0x1000)
#define SCR_IPI_HARTS           (0x1004)
#define SCR_IPI_CONTROL         (0x1008)
#define SCR_IPI_CLEAR_LO        (0x1010)
#define SCR_IPI_CLEAR_HI        (0x1014)

#define SCR_BUFSTATUS_FULL      BIT(0)

#define SCR_IPI_RETRY	100

static unsigned int iccm_ipi_irq;

static struct regmap_config iccm_mmio_config = {
	.name = "iccm-regmap",
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.val_format_endian = REGMAP_ENDIAN_LITTLE,
	.max_register = 0x2000,
	.fast_io = true,
};

static struct regmap *map;

static void iccm_send_ipi(unsigned int cpu)
{
	unsigned int send_id = cpuid_to_hartid_map(smp_processor_id());
	unsigned int recv_id;
	unsigned int val;
	unsigned int c;

	recv_id = cpuid_to_hartid_map(cpu);
	for (c = 0; c < SCR_IPI_RETRY; c++) {
		regmap_read(map, SCR_IPI_SNDSTAT_N_LO(send_id), &val);
		if ((val >> recv_id) & 0x1)
			continue;
		regmap_write(map, SCR_IPI_BUFWRITE_N(recv_id), 1);
		break;
	}
}

static void iccm_clear_ipi(void)
{
	unsigned int val;
	unsigned int c;

	for (c = 0; c < SCR_IPI_RETRY; c++) {
		regmap_read(map, SCR_IPI_BUFSTATUS, &val);
		if (val & SCR_BUFSTATUS_FULL) {
			regmap_read(map, SCR_IPI_BUFREAD, &val);
			break;
		}
	}
}

static void iccm_ipi_interrupt(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);

	chained_irq_enter(chip, desc);

	iccm_clear_ipi();
	ipi_mux_process();

	chained_irq_exit(chip, desc);
}

static int iccm_ipi_starting_cpu(unsigned int cpu)
{
	enable_percpu_irq(iccm_ipi_irq, irq_get_trigger_type(iccm_ipi_irq));
	return 0;
}

static int __init scr_iccm_mmio_init(void)
{
	struct irq_domain *domain;
	struct device_node *np;
	void __iomem *base;
	int virq;
	int ret;

	np = of_find_compatible_node(NULL, NULL, "scr,iccm-mmio");
	if (!of_device_is_available(np))
		return 0;

	domain = irq_find_matching_fwnode(riscv_get_intc_hwnode(),
					  DOMAIN_BUS_ANY);
	if (!domain) {
		pr_err("unable to find INTC IRQ domain\n");
		return -ENODEV;
	}

	base = of_iomap(np, 0);
	if (!base) {
		pr_err("%pOFP: could not map iccm registers\n", np);
		return -ENODEV;
	}

	map = regmap_init_mmio(NULL, base, &iccm_mmio_config);
	if (IS_ERR(map)) {
		pr_err("%pOFP: failed to init regmap\n", np);
		ret = -EINVAL;
		goto fail_iounmap;
	}

	/* ignore write/full and read/empty errors */
	regmap_write(map, SCR_IPI_CONTROL, 0);

	if (riscv_ipi_have_virq_range()) {
		ret = -EBUSY;
		goto fail_iounmap;
	}

	iccm_ipi_irq = irq_create_mapping(domain, RV_IRQ_SOFT);
	if (!iccm_ipi_irq) {
		pr_err("unable to create INTC IRQ mapping\n");
		ret = -EINVAL;
		goto fail_iounmap;
	}

	virq = ipi_mux_create(BITS_PER_BYTE, iccm_send_ipi);
	if (virq <= 0) {
		pr_err("unable to create muxed IPIs\n");
		ret = -ENODEV;
		goto fail_dispose;
	}

	irq_set_chained_handler(iccm_ipi_irq, iccm_ipi_interrupt);

	/*
	 * Don't disable IPI when CPU goes offline because
	 * the masking/unmasking of virtual IPIs is done
	 * via generic IPI-Mux
	 */
	cpuhp_setup_state(CPUHP_AP_ONLINE_DYN,
			  "irqchip/iccm-ipi:starting",
			  iccm_ipi_starting_cpu, NULL);

	riscv_ipi_set_virq_range(virq, BITS_PER_BYTE, true);
	pr_info("providing IPIs using ICCM IPI extension\n");

	return 0;

fail_dispose:
	irq_dispose_mapping(iccm_ipi_irq);

fail_iounmap:
	iounmap(base);
	return ret;
}

early_initcall(scr_iccm_mmio_init);
