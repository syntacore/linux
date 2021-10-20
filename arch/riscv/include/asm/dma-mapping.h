// SPDX-License-Identifier: GPL-2.0

#ifndef _RISCV_ASM_DMA_MAPPING_H
#define _RISCV_ASM_DMA_MAPPING_H 1

#ifdef CONFIG_CPU_RV_SCR

#include <linux/dma-mapping.h>

const struct dma_map_ops *get_arch_dma_ops(struct bus_type *bus);

void arch_setup_dma_ops(struct device *dev, u64 dma_base, u64 size,
			const struct iommu_ops *iommu, bool coherent);
#define arch_setup_dma_ops arch_setup_dma_ops

#endif /* CONFIG_CPU_RV_SCR */

#endif /* _RISCV_ASM_DMA_MAPPING_H */
