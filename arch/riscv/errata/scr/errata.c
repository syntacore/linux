// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2022 Yadro
 */

#include <linux/bug.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <asm/alternative.h>
#include <asm/cacheflush.h>
#include <asm/errata_list.h>
#include <asm/patch.h>
#include <asm/vendorid_list.h>

static u32 scr_errata_probe(unsigned int stage, unsigned long archid, unsigned long impid)
{
	u32 cpu_req_errata = 0;
	const unsigned int scr_min_cbom_size = 16;

	if (!riscv_cbom_block_size)
		riscv_cbom_block_size = scr_min_cbom_size;

	/* Every CPU is affected */
	riscv_noncoherent_supported();
	cpu_req_errata |= (1U << ERRATA_SCR_CMO);

	return cpu_req_errata;
}

void __init_or_module scr_errata_patch_func(struct alt_entry *begin, struct alt_entry *end,
					    unsigned long archid, unsigned long impid,
					    unsigned int stage)
{
	struct alt_entry *alt;
	u32 cpu_req_errata = scr_errata_probe(stage, archid, impid);
	u32 tmp;

	for (alt = begin; alt < end; alt++) {
		if (alt->vendor_id != SCR_VENDOR_ID)
			continue;
		if (alt->patch_id >= ERRATA_SCR_NUMBER)
			continue;

		tmp = (1U << alt->patch_id);
		if (cpu_req_errata & tmp) {
			/* On vm-alternatives, the mmu isn't running yet */
			if (stage == RISCV_ALTERNATIVES_EARLY_BOOT)
				memcpy((void *)__pa_symbol(ALT_OLD_PTR(alt)),
				       (void *)__pa_symbol(ALT_ALT_PTR(alt)), alt->alt_len);
			else
				patch_text_nosync(ALT_OLD_PTR(alt), ALT_ALT_PTR(alt), alt->alt_len);
		}
	}

	if (stage == RISCV_ALTERNATIVES_EARLY_BOOT)
		local_flush_icache_all();
}
