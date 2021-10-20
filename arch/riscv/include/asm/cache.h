/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2017 Chen Liqin <liqin.chen@sunplusct.com>
 * Copyright (C) 2012 Regents of the University of California
 */

#ifndef _ASM_RISCV_CACHE_H
#define _ASM_RISCV_CACHE_H

#ifdef CONFIG_CPU_RV_SCR
#define L1_CACHE_SHIFT		4
#define L2_CACHE_SHIFT		5
#define L2_CACHE_BYTES		(1 << L2_CACHE_SHIFT)
#define ARCH_DMA_MINALIGN	L2_CACHE_BYTES
/* use the cache line size for the L2, which is where it counts */
#define SMP_CACHE_BYTES_SHIFT	L2_CACHE_SHIFT
#define SMP_CACHE_BYTES		L2_CACHE_BYTES
#define INTERNODE_CACHE_SHIFT	SMP_CACHE_BYTES_SHIFT
#else /* CONFIG_CPU_RV_SCR */
#define L1_CACHE_SHIFT		6
#endif /* CONFIG_CPU_RV_SCR */

#define L1_CACHE_BYTES		(1 << L1_CACHE_SHIFT)

/*
 * RISC-V requires the stack pointer to be 16-byte aligned, so ensure that
 * the flat loader aligns it accordingly.
 */
#ifndef CONFIG_MMU
#define ARCH_SLAB_MINALIGN	16
#endif

#ifndef SMP_CACHE_BYTES
#define SMP_CACHE_BYTES		L1_CACHE_BYTES
#endif

#ifdef CONFIG_ARCH_HAS_CACHE_LINE_SIZE
#define cache_line_size()	SMP_CACHE_BYTES
#endif

#endif /* _ASM_RISCV_CACHE_H */
