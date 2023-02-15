// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * RISC-V SCR Linux
 *
 * Linux architectural port borrowing liberally from similar works of
 * others.  All original copyrights apply as per the original source
 * declaration.
 *
 * Modifications for the OpenRISC architecture:
 * Copyright (C) 2003 Matjaz Breskvar <phoenix@bsemi.com>
 * Copyright (C) 2010-2011 Jonas Bonn <jonas@southpole.se>
 * Modifications for the RISC-V SCR architecture:
 * Copyright (C) 2016-2019 mn-sc <@syntacore.com>
 *
 */

#include <linux/genalloc.h>
#include <linux/dma-mapping.h>
#include <linux/dma-map-ops.h>

#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/of_reserved_mem.h>

#include <asm/cache.h>
#include <asm/tlbflush.h>
#include <asm/cacheflush.h>

/*
 * From https://lkml.org/lkml/2018/5/18/979
 *
 *  It's necessary.  Take a moment to think carefully about this:
 *
 *  dma_map_single(, dir)
 *
 *  dma_sync_single_for_cpu(, dir)
 *
 *  dma_sync_single_for_device(, dir)
 *
 *  dma_unmap_single(, dir)
 *
 *  In the case of a DMA-incoherent architecture, the operations done at each
 *  stage depend on the direction argument:
 *
 *          map         for_cpu     for_device  unmap
 *  TO_DEV  writeback   none        writeback   none
 *  TO_CPU  invalidate  invalidate* invalidate  invalidate*
 *  BIDIR   writeback   invalidate  writeback   invalidate
 *
 *  * - only necessary if the CPU speculatively prefetches.
 *
 *  The multiple invalidations for the TO_CPU case handles different
 *  conditions that can result in data corruption, and for some CPUs, all
 *  four are necessary.
 *
 */

#ifdef CONFIG_CPU_RV_SCR

// instruction: clinv <regn> (cache line invalidate)
#define ENC_CLINV(regn) (0x10800073 | (((regn) & 0x1f) << 15))
// instruction: clflush <regn> (cache line flush & invalidate)
#define ENC_CLFLUSH(regn) (0x10900073 | (((regn) & 0x1f) << 15))
// 32 bytes should work for both 32 and 64 bytes cache lines
#define SCR_CACHE_SYNC_SIZE 32

static unsigned long riscv_cbom_block_size;

void riscv_init_cbom_blocksize(void)
{
	unsigned long probed_block_size;
	struct device_node *node;
	int cbom_hartid;
	u32 val;
	int ret;

	probed_block_size = 0;
	for_each_of_cpu_node(node) {
		int hartid;

		hartid = riscv_of_processor_hartid(node);
		if (hartid < 0)
			continue;

		/* set block-size for cbom extension if available */
		ret = of_property_read_u32(node, "riscv,cbom-block-size", &val);
		if (ret)
			continue;

		if (!probed_block_size) {
			probed_block_size = val;
			cbom_hartid = hartid;
		} else {
			if (probed_block_size != val)
				pr_warn("cbom-block-size mismatched between harts %d and %d\n",
					cbom_hartid, hartid);
		}
	}

	if (probed_block_size)
		riscv_cbom_block_size = probed_block_size;
	else
		riscv_cbom_block_size = SCR_CACHE_SYNC_SIZE;
}

static void cache_addr_invalidate(void *vaddr, size_t size)
{
	register unsigned long a0 asm("a0") = (unsigned long)vaddr;

	a0 &= -riscv_cbom_block_size;

	asm volatile ("fence" ::: "memory");

	/* Invalidate the dcache for the requested range */
	for (; a0 < (unsigned long)vaddr + size; a0 += riscv_cbom_block_size)
		__asm__ __volatile__ (".word %0" :: "i"(ENC_CLINV(10)), "r"(a0) : "memory");
}

static inline void cache_addr_flush(void *vaddr, size_t size)
{
	register unsigned long a0 asm("a0") = (unsigned long)vaddr;

	a0 &= -riscv_cbom_block_size;

	/* Flush the cache for the requested range */
	for (; a0 < (unsigned long)vaddr + size; a0 += riscv_cbom_block_size)
		__asm__ __volatile__ (".word %0" :: "i"(ENC_CLFLUSH(10)), "r"(a0) : "memory");

	asm volatile ("fence" ::: "memory");
}

void arch_sync_dma_for_device(phys_addr_t paddr, size_t size,
			      enum dma_data_direction dir)
{
	void *vaddr = phys_to_virt(paddr);

	switch (dir) {
	case DMA_FROM_DEVICE:
		cache_addr_invalidate(vaddr, size);
		break;
	case DMA_TO_DEVICE:
		cache_addr_flush(vaddr, size);
		break;
	case DMA_BIDIRECTIONAL:
		cache_addr_flush(vaddr, size);
		break;
	default:
		BUG();
	}
}

void arch_sync_dma_for_cpu(phys_addr_t paddr, size_t size,
			   enum dma_data_direction dir)
{
	void *vaddr = phys_to_virt(paddr);

	switch (dir) {
	case DMA_TO_DEVICE:
		__asm__ __volatile__ ("fence" ::: "memory");
		break;
	case DMA_FROM_DEVICE:
	case DMA_BIDIRECTIONAL:
		cache_addr_invalidate(vaddr, size);
		break;
	default:
		BUG();
	}
}

#endif /* CONFIG_CPU_RV_SCR */
