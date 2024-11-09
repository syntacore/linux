// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2022 Syntacore
 */

#include <linux/bug.h>
#include <linux/kernel.h>
#include <linux/memory.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <asm/alternative.h>
#include <asm/cacheflush.h>
#include <asm/dma-noncoherent.h>
#include <asm/errata_list.h>
#include <asm/io.h>
#include <asm/patch.h>
#include <asm/vendorid_list.h>
#include <asm/vendor_extensions.h>

/*
 * scr.clfush
 * | 31 - 25 | 24 - 20 | 19 - 15 | 14 - 12 | 11 - 7 | 6 - 0 |
 *   0001000    01001      rs1       000      00000  1110011
 *
 * scr.clinv
 * | 31 - 25 | 24 - 20 | 19 - 15 | 14 - 12 | 11 - 7 | 6 - 0 |
 *   0001000    01000      rs1       000      00000  1110011
 */

#define SCR_INVAL	".long 0x10850073"
#define SCR_FLUSH	".long 0x10950073"
#define SCR_CLEAN	SCR_FLUSH

#define SCR_CMO_OP(_op, _start, _size, _cachesize)			\
asm volatile("mv a0, %1\n\t"						\
	"j 2f\n\t"							\
	"3:\n\t"							\
	SCR_##_op "\n\t"						\
	"add a0, a0, %0\n\t"						\
	"2:\n\t"							\
	"bltu a0, %2, 3b\n\t"						\
	: : "r"(_cachesize),						\
	    "r"((unsigned long)(_start) & ~((_cachesize) - 1UL)),	\
	    "r"((unsigned long)(_start) + (_size))			\
	: "a0")

static void scr_errata_cache_inv(phys_addr_t paddr, size_t size)
{
	void *vaddr = phys_to_virt(paddr);

	SCR_CMO_OP(INVAL, vaddr, size, riscv_cbom_block_size);
}

static void scr_errata_cache_wback(phys_addr_t paddr, size_t size)
{
	void *vaddr = phys_to_virt(paddr);

	SCR_CMO_OP(CLEAN, vaddr, size, riscv_cbom_block_size);
}

static void scr_errata_cache_wback_inv(phys_addr_t paddr, size_t size)
{
	void *vaddr = phys_to_virt(paddr);

	SCR_CMO_OP(FLUSH, vaddr, size, riscv_cbom_block_size);
}

static const struct riscv_nonstd_cache_ops scr_errata_cmo_ops = {
	.wback = &scr_errata_cache_wback,
	.inv = &scr_errata_cache_inv,
	.wback_inv = &scr_errata_cache_wback_inv,
};

static u32 scr_errata_probe(unsigned int stage, unsigned long archid, unsigned long impid)
{
	const unsigned int scr_min_cbom_size = 16;

	if (!riscv_cbom_block_size)
		riscv_cbom_block_size = scr_min_cbom_size;

	/* Every CPU is affected */
	if (stage == RISCV_ALTERNATIVES_BOOT) {
		riscv_noncoherent_supported();
		riscv_noncoherent_register_cache_ops(&scr_errata_cmo_ops);
	}

	return 0;
}

void __init_or_module scr_errata_patch_func(struct alt_entry *begin, struct alt_entry *end,
					    unsigned long archid, unsigned long impid,
					    unsigned int stage)
{
	BUILD_BUG_ON(ERRATA_SCR_NUMBER >= RISCV_VENDOR_EXT_ALTERNATIVES_BASE);

	scr_errata_probe(stage, archid, impid);

	if (stage == RISCV_ALTERNATIVES_EARLY_BOOT)
		local_flush_icache_all();
}
