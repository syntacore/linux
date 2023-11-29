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
#include <linux/sched_clock.h>

#define SCR_CSR_IPI_MBASE   0xbd8
#define SCR_CSR_IPI_MADDR   (SCR_CSR_IPI_MBASE + 0)
#define SCR_CSR_IPI_MSTATUS (SCR_CSR_IPI_MBASE + 1)
#define SCR_CSR_IPI_MRDATA  (SCR_CSR_IPI_MBASE + 2)
#define SCR_CSR_IPI_MWDATA  (SCR_CSR_IPI_MBASE + 3)

#define SCR_IPI_RETRY	100

static unsigned int iccm_ipi_irq;

static void iccm_send_ipi(unsigned int cpu)
{
	unsigned int c;

	csr_write(SCR_CSR_IPI_MADDR, cpuid_to_hartid_map(cpu));
	for (c = 0; c < SCR_IPI_RETRY; c++) {
		if (!csr_read(SCR_CSR_IPI_MSTATUS)) {
			csr_write(SCR_CSR_IPI_MWDATA, 1);
			break;
		}
	}
}

static void iccm_clear_ipi(void)
{
	unsigned int c;

	for (c = 0; c < SCR_IPI_RETRY; c++) {
		if (csr_swap(SCR_CSR_IPI_MRDATA, 1))
			break;
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

static int __init scr_iccm_csr_init(void)
{
	struct irq_domain *domain;
	struct device_node *np;
	int virq;

	np = of_find_compatible_node(NULL, NULL, "scr,iccm-csr");
	if (!of_device_is_available(np))
		return 0;

	domain = irq_find_matching_fwnode(riscv_get_intc_hwnode(),
					  DOMAIN_BUS_ANY);
	if (!domain) {
		pr_err("unable to find INTC IRQ domain\n");
		return -ENODEV;
	}

	if (riscv_ipi_have_virq_range())
		return -EBUSY;

	iccm_ipi_irq = irq_create_mapping(domain, RV_IRQ_SOFT);
	if (!iccm_ipi_irq) {
		pr_err("unable to create INTC IRQ mapping\n");
		return -EINVAL;
	}

	virq = ipi_mux_create(BITS_PER_BYTE, iccm_send_ipi);
	if (virq <= 0) {
		pr_err("unable to create muxed IPIs\n");
		irq_dispose_mapping(iccm_ipi_irq);
		return -ENODEV;
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
}

early_initcall(scr_iccm_csr_init);
