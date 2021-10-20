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

#include <linux/pci.h>
#include <linux/genalloc.h>
#include <linux/dma-mapping.h>
#include <linux/dma-map-ops.h>

#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/of_reserved_mem.h>

#include <asm/cache.h>
#include <asm/tlbflush.h>

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

#define SCR_DMA_PLF_ALLOC_COHERENT

/* #define DBG_SCR_DMA_ENABLE */

#ifdef DBG_SCR_DMA_ENABLE
#define DBG_INFO pr_info
static const char *dma_dir2str(enum dma_data_direction dir)
{
	static char unk_type_buf[64];

	switch (dir) {
	case DMA_TO_DEVICE:
		return "TO_DEV";
	case DMA_FROM_DEVICE:
		return "TO_CPU";
	case DMA_BIDIRECTIONAL:
		return "BI_DIR";
	case DMA_NONE:
		return "NO_DIR";
	}

	snprintf(unk_type_buf, sizeof(unk_type_buf), "unk dir:%d", (int)dir);

	return unk_type_buf;
}
#else
#define DBG_INFO(x, ...) do {} while (0)
#endif

// instruction: clinv <regn> (cache line invalidate)
#define ENC_CLINV(regn) (0x10800073 | (((regn) & 0x1f) << 15))
// instruction: clflush <regn> (cache line flush & invalidate)
#define ENC_CLFLUSH(regn) (0x10900073 | (((regn) & 0x1f) << 15))

#define SCR_CACHE_SYNC_SIZE ARCH_DMA_MINALIGN

static void cache_addr_invalidate(void *vaddr, size_t size)
{
	register unsigned long a0 asm("a0") = (unsigned long)vaddr;

	a0 &= -SCR_CACHE_SYNC_SIZE;

	asm volatile ("fence" ::: "memory");

	/* Invalidate the dcache for the requested range */
	for (; a0 < (unsigned long)vaddr + size; a0 += SCR_CACHE_SYNC_SIZE)
		__asm__ __volatile__ (".word %0" :: "i"(ENC_CLINV(10)), "r"(a0) : "memory");
}

static inline void cache_addr_flush(void *vaddr, size_t size)
{
	register unsigned long a0 asm("a0") = (unsigned long)vaddr;

	a0 &= -SCR_CACHE_SYNC_SIZE;

	/* Flush the cache for the requested range */
	for (; a0 < (unsigned long)vaddr + size; a0 += SCR_CACHE_SYNC_SIZE)
		__asm__ __volatile__ (".word %0" :: "i"(ENC_CLFLUSH(10)), "r"(a0) : "memory");

	asm volatile ("fence" ::: "memory");
}
#endif /* CONFIG_CPU_RV_SCR */

static inline void cache_page_invalidate(struct page *page, unsigned long offset, size_t size)
{
	cache_addr_invalidate((void *)((unsigned long)page_to_virt(page) + offset), size);
}

static inline void cache_page_flush(struct page *page, unsigned long offset, size_t size)
{
	cache_addr_flush((void *)((unsigned long)page_to_virt(page) + offset), size);
}

#if defined(CONFIG_PCI) || defined(SCR_DMA_PLF_ALLOC_COHERENT)

/* Allocates from a pool of uncached memory that was reserved at boot time */
static unsigned long scr_coherent_pool_size;
static unsigned long scr_coherent_pool_base;
static struct gen_pool *scr_coherent_pool;

static void scr_alloc_dma_pool(void)
{
	if (scr_coherent_pool == NULL && scr_coherent_pool_size) {
		scr_coherent_pool = gen_pool_create(PAGE_SHIFT, -1);

		if (scr_coherent_pool == NULL)
			panic("Can't create %s() memory pool!", __func__);
		else {
			void *va = ioremap(scr_coherent_pool_base, scr_coherent_pool_size);

			int rc = gen_pool_add_virt(scr_coherent_pool,
						   (unsigned long)va,
						   scr_coherent_pool_base,
						   scr_coherent_pool_size,
						   -1);
			if (!rc) {
				pr_info("SCRxDMA: reserved coherent memory PHYS 0x%lx - 0x%lx VA %px",
					(unsigned long)scr_coherent_pool_base,
					(unsigned long)(scr_coherent_pool_base + scr_coherent_pool_size - 1),
					va);
			} else {
				pr_info("SCRxDMA: coherent memory reservation failed (%d)", rc);
			}
		}
	}
}

static void *scr_alloc_coherent(size_t size, phys_addr_t *dma_addr)
{
	void *vaddr;

	if (scr_coherent_pool == NULL) {
		vaddr = alloc_pages_exact(size, GFP_KERNEL);
		*dma_addr = __pa(vaddr);
	} else {
		vaddr = (void *)gen_pool_alloc(scr_coherent_pool, size);

		if (vaddr)
			*dma_addr = gen_pool_virt_to_phys(scr_coherent_pool, (unsigned long)vaddr);
		else
			pr_info("SCRxDMA: alloc(%llu) failed", (unsigned long long)size);
	}

	asm volatile ("fence" ::: "memory");

	return vaddr;
}

static void scr_free_coherent(void *vaddr, size_t size)
{
	if (scr_coherent_pool == NULL)
		free_pages_exact(vaddr, size);
	else
		gen_pool_free(scr_coherent_pool, (unsigned long)vaddr, size);

	asm volatile ("fence" ::: "memory");
}

#endif // CONFIG_PCI || SCR_DMA_PLF_ALLOC_COHERENT

/*
 * scr_dma_alloc_coherent - allocate memory for coherent DMA
 * @dev: device to allocate for
 * @size: size of the region
 * @dma_handle: DMA (bus) address
 * @flags: memory allocation flags
 *
 * dma_alloc_coherent() returns a pointer to a memory region suitable for
 * coherent DMA traffic to/from a PCI device.
 *
 * This interface is usually used for "command" streams (e.g. the command
 * queue for a SCSI controller).  See Documentation/DMA-API.txt for
 * more information.
 */
static void *
scr_dma_alloc_coherent(struct device *dev, size_t size,
		       dma_addr_t *dma_handle, gfp_t gfp,
		       unsigned long attrs)
{
	void *vaddr;
#ifdef SCR_DMA_PLF_ALLOC_COHERENT
	phys_addr_t pa;

	vaddr = scr_alloc_coherent(size, &pa);
#else
	vaddr = alloc_pages_exact(size, gfp);
#endif // SCR_DMA_PLF_ALLOC_COHERENT

	if (!vaddr) {
		DBG_INFO("***DMA*** scr_dma_alloc(%s, %lu) err!\n",
			 dev_name(dev), (unsigned long)size);
		return NULL;
	}

	memset(vaddr, 0, size);

	asm volatile ("fence" ::: "memory");

#ifdef SCR_DMA_PLF_ALLOC_COHERENT
	*dma_handle = (dma_addr_t)pa;
#else
	/* This gives us the real physical address of the first page. */
	*dma_handle = __pa(vaddr);
#endif // SCR_DMA_PLF_ALLOC_COHERENT

	DBG_INFO("***DMA*** scr_dma_alloc(%s, %lu): va %px dma %llx attrs %lx\n",
		 dev_name(dev), (unsigned long)size,
		 vaddr, (unsigned long long)*dma_handle, attrs);

	return vaddr;
}

static void
scr_dma_free_coherent(struct device *dev, size_t size, void *vaddr,
		      dma_addr_t dma_handle, unsigned long attrs)
{
	DBG_INFO("***DMA*** scr_dma_free(%s, %lu): va %lx dma %llx attrs %lx\n",
		 dev_name(dev), (unsigned long)size,
		 (unsigned long)vaddr, (unsigned long long)dma_handle, attrs);

#ifdef SCR_DMA_PLF_ALLOC_COHERENT
	scr_free_coherent(vaddr, size);
#else
	free_pages_exact(vaddr, size);
#endif // SCR_DMA_PLF_ALLOC_COHERENT
}

static dma_addr_t
scr_map_page(struct device *dev, struct page *page,
	     unsigned long offset, size_t size,
	     enum dma_data_direction dir,
	     unsigned long attrs)
{
	dma_addr_t addr;
	void *va;

	va = page_to_virt(page);

	DBG_INFO("***DMA*** %s(%s, %s): pfn %lx va %lx offs %lx size %lu attrs %lx\n",
		 __func__, dev_name(dev), dma_dir2str(dir),
		 (unsigned long)page_to_pfn(page),
		 (unsigned long)va, offset, (unsigned long)size, attrs);

	// FIXME: RV64 memory layout, kernel HIMEM (non linear addresses)

	// ??? vm->flags & VM_IOREMAP ???
	if ((unsigned long)va < PAGE_OFFSET) {
		// outside of kernel linear space
		struct vm_struct *vm = find_vm_area(va);

		if (vm)
			addr = vm->phys_addr + ((unsigned long)va - (unsigned long)vm->addr) + offset;
		else
			addr = page_to_phys(page) + offset;

		DBG_INFO("***DMA*** <va= %lx not in kernel [%lx], vm %lx, vm.pa %llx, vm.addr %lx>"
			 " %s(%s, %s): size %lu attrs %lx: dma %llx\n",
			 (unsigned long)va, (unsigned long)PAGE_OFFSET,
			 (unsigned long)vm,
			 (vm ? (unsigned long long)vm->phys_addr : 0UL),
			 (vm ? (unsigned long)vm->addr : 0UL),
			 __func__, dev_name(dev), dma_dir2str(dir),
			 (unsigned long)size, attrs, (unsigned long long)addr);

		__asm__ __volatile__ ("fence" ::: "memory");
	} else {
		addr = page_to_phys(page) + offset;

		DBG_INFO("***DMA*** %s(%s, %s):"
			 " pfn %lx va %lx size %lu attrs %lx dma %llx\n",
			 __func__, dev_name(dev), dma_dir2str(dir),
			 (unsigned long)page_to_pfn(page),
			 (unsigned long)va, (unsigned long)size, attrs,
			 (unsigned long long)addr);

		switch (dir) {
		case DMA_FROM_DEVICE: // any RX
			/* if (!(attrs & DMA_ATTR_SKIP_CPU_SYNC)) */
			cache_page_invalidate(page, offset, size);
			break;
		case DMA_TO_DEVICE:
			/* if (!(attrs & DMA_ATTR_SKIP_CPU_SYNC)) */
			cache_page_flush(page, offset, size);
			break;
		case DMA_BIDIRECTIONAL:
			/* if (!(attrs & DMA_ATTR_SKIP_CPU_SYNC)) */
			cache_page_flush(page, offset, size);
			break;
		default:
			BUG();
		}
	}

	return addr;
}

static void
scr_unmap_page(struct device *dev, dma_addr_t dma_handle,
	       size_t size, enum dma_data_direction dir,
	       unsigned long attrs)
{
	if (dma_handle >= PFN_PHYS(riscv_pfn_base) && dma_handle < PFN_PHYS(riscv_pfn_base + max_mapnr)) {

		DBG_INFO("***DMA*** %s(%s, %s): dma %llx size %lu attrs %lx\n",
			 __func__, dev_name(dev), dma_dir2str(dir),
			 (unsigned long long)dma_handle,
			 (unsigned long)size, attrs);

		switch (dir) {
		case DMA_TO_DEVICE:
			__asm__ __volatile__ ("fence" ::: "memory");
			break;
		case DMA_FROM_DEVICE:
			// fall through
		case DMA_BIDIRECTIONAL:
			/* if (!(attrs & DMA_ATTR_SKIP_CPU_SYNC)) */
			cache_addr_invalidate(__va(dma_handle), size);
			break;
		default:
			BUG();
		}
	} else {
		DBG_INFO("***DMA*** <dma addr %llx not in range [%llx %llx]>"
			 " %s(%s, %s): dma %llx size %lu attrs %lx\n",
			 (unsigned long long)dma_handle,
			 (unsigned long long)PFN_PHYS(riscv_pfn_base),
			 (unsigned long long)PFN_PHYS(riscv_pfn_base + max_mapnr),
			 __func__, dev_name(dev), dma_dir2str(dir),
			 (unsigned long long)dma_handle, (unsigned long)size, attrs);

		__asm__ __volatile__ ("fence" ::: "memory");
	}
}

static int
scr_map_sg(struct device *dev, struct scatterlist *sg,
	   int nents, enum dma_data_direction dir,
	   unsigned long attrs)
{
	struct scatterlist *s;
	int i;

	for_each_sg(sg, s, nents, i) {
		s->dma_address = scr_map_page(dev, sg_page(s), s->offset,
					      s->length, dir, attrs);
	}

	return nents;
}

static void
scr_unmap_sg(struct device *dev, struct scatterlist *sg,
	     int nents, enum dma_data_direction dir,
	     unsigned long attrs)
{
	struct scatterlist *s;
	int i;

	for_each_sg(sg, s, nents, i) {
		scr_unmap_page(dev, sg_dma_address(s), sg_dma_len(s), dir, attrs);
	}
}

static void
scr_sync_single_for_cpu(struct device *dev,
			dma_addr_t dma_handle, size_t size,
			enum dma_data_direction dir)
{
	/* Invalidate the dcache for the requested range */

	// FIXME: kernel HIMEM (non linear addresses)

	// ignore uncached region
	if (dma_handle >= PFN_PHYS(riscv_pfn_base) && dma_handle < PFN_PHYS(riscv_pfn_base + max_mapnr)) {
		DBG_INFO("***DMA*** %s(%s, %s): dma %llx size %lu\n",
			 __func__, dev_name(dev), dma_dir2str(dir),
			 (unsigned long long)dma_handle, (unsigned long)size);
		if (dir == DMA_TO_DEVICE)
			__asm__ __volatile__ ("fence" ::: "memory");
		else
			cache_addr_invalidate(__va(dma_handle), size);
	} else {
		DBG_INFO("***DMA*** <dma addr %llx not in range [%llx %llx]> %s(%s, %s): dma %llx size %lu\n",
			 (unsigned long long)dma_handle, (unsigned long long)PFN_PHYS(riscv_pfn_base),
			 (unsigned long long)PFN_PHYS(riscv_pfn_base + max_mapnr),
			 __func__, dev_name(dev), dma_dir2str(dir),
			 (unsigned long long)dma_handle, (unsigned long)size);

		__asm__ __volatile__ ("fence" ::: "memory");
	}
}

static void
scr_sync_single_for_device(struct device *dev,
			   dma_addr_t dma_handle, size_t size,
			   enum dma_data_direction dir)
{
	/* Flush the dcache for the requested range */

	// FIXME: kernel HIMEM (non linear addresses)

	// ignore uncached region
	if (dma_handle >= PFN_PHYS(riscv_pfn_base) && dma_handle < PFN_PHYS(riscv_pfn_base + max_mapnr)) {
		DBG_INFO("***DMA*** %s(%s, %s): dma %llx size %lu\n",
			 __func__, dev_name(dev), dma_dir2str(dir),
			 (unsigned long long)dma_handle, (unsigned long)size);
		if (dir == DMA_FROM_DEVICE)
			cache_addr_invalidate(__va(dma_handle), size);
		else
			cache_addr_flush(__va(dma_handle), size);
	} else {
		DBG_INFO("***DMA*** <dma addr %llx not in range [%llx %llx]> %s(%s, %s): dma %llx size %lu\n",
			 (unsigned long long)dma_handle, (unsigned long long)PFN_PHYS(riscv_pfn_base),
			 (unsigned long long)PFN_PHYS(riscv_pfn_base + max_mapnr),
			 __func__, dev_name(dev), dma_dir2str(dir),
			 (unsigned long long)dma_handle, (unsigned long)size);

		__asm__ __volatile__ ("fence" ::: "memory");
	}
}

static int scr_dma_supported(struct device *dev, u64 dma_mask)
{
	DBG_INFO("***DMA*** %s(%s %llx)? = %d",
		 __func__, dev_name(dev), dma_mask,
		 ((dma_mask & DMA_BIT_MASK(32)) == DMA_BIT_MASK(32)));

	return (dma_mask & DMA_BIT_MASK(32)) == DMA_BIT_MASK(32);
}

const struct dma_map_ops scr_dma_platform_map_ops = {
	.alloc = scr_dma_alloc_coherent,
	.free = scr_dma_free_coherent,
	.map_page = scr_map_page,
	.unmap_page = scr_unmap_page,
	.map_sg = scr_map_sg,
	.unmap_sg = scr_unmap_sg,
	.sync_single_for_cpu = scr_sync_single_for_cpu,
	.sync_single_for_device = scr_sync_single_for_device,
	.dma_supported = scr_dma_supported,
};
EXPORT_SYMBOL(scr_dma_platform_map_ops);

#ifdef CONFIG_PCI

#define PCI_BUS_BASE_ADDR 0x1000000000
#define CPU_ADDR_TO_PCI_BUS_ADDR(x) ((dma_addr_t)(x) + PCI_BUS_BASE_ADDR)
#define PCI_BUS_ADDR_TO_CPU_ADDR(x) ((phys_addr_t)((dma_addr_t)(x) - PCI_BUS_BASE_ADDR))

static void *
scr_dma_pci_alloc_coherent(struct device *dev, size_t size,
			   dma_addr_t *dma_handle, gfp_t gfp,
			   unsigned long attrs)
{
	void *vaddr;
	phys_addr_t pa;

	vaddr = scr_alloc_coherent(size, &pa);
	if (!vaddr) {
		DBG_INFO("***DMA*** %s(%s, %lu) err!\n",
			 __func__, dev_name(dev), (unsigned long)size);
		return NULL;
	}

	memset(vaddr, 0, size);

	asm volatile ("fence" ::: "memory");

	/* This gives us the PCI bus address of the first page. */
	*dma_handle = CPU_ADDR_TO_PCI_BUS_ADDR(pa);

	DBG_INFO("***DMA*** %s(%s, %lu attrs %lx): va %px pa %lx dma %llx\n",
		 __func__, dev_name(dev), (unsigned long)size, attrs,
		 vaddr, (unsigned long)pa, (unsigned long long)*dma_handle);

	return vaddr;
}

static void
scr_dma_pci_free_coherent(struct device *dev, size_t size, void *vaddr,
			  dma_addr_t dma_handle, unsigned long attrs)
{
	DBG_INFO("***DMA*** %s(%s, %lu): va %lx dma %llx attrs %lx\n",
		 __func__, dev_name(dev), (unsigned long)size,
		 (unsigned long)vaddr, (unsigned long long)dma_handle, attrs);

	scr_free_coherent(vaddr, size);
}

static dma_addr_t
scr_pci_map_page(struct device *dev, struct page *page,
		 unsigned long offset, size_t size,
		 enum dma_data_direction dir,
		 unsigned long attrs)
{
	dma_addr_t addr;
	void *va;

	va = page_to_virt(page);

	DBG_INFO("***DMA*** %s(%s, %s): pfn %lx va %lx offs %lx size %lu attrs %lx\n",
		 __func__, dev_name(dev), dma_dir2str(dir),
		 (unsigned long)page_to_pfn(page),
		 (unsigned long)va, offset, (unsigned long)size, attrs);

	// FIXME: RV64 memory layout, kernel HIMEM (non linear addresses)

	// ??? vm->flags & VM_IOREMAP ???
	if ((unsigned long)va < PAGE_OFFSET) {
		// FIXME: PCI bus mapping???
		// outside of kernel linear space
		struct vm_struct *vm = find_vm_area(va);

		if (vm)
			addr = vm->phys_addr + ((unsigned long)va - (unsigned long)vm->addr) + offset;
		else
			addr = page_to_phys(page) + offset;

		DBG_INFO("***DMA*** <va %lx not in kernel [%lx], vm %lx, vm.pa %llx, vm.addr %lx>"
			 " %s(%s, %s): size %lu attrs %lx dma %llx\n",
			 (unsigned long)va, (unsigned long)PAGE_OFFSET,
			 (unsigned long)vm,
			 (vm ? (unsigned long long)vm->phys_addr : 0ULL),
			 (vm ? (unsigned long)vm->addr : 0UL),
			 __func__, dev_name(dev), dma_dir2str(dir),
			 (unsigned long)size, attrs, (unsigned long long)addr);

		__asm__ __volatile__ ("fence" ::: "memory");
	} else {
		addr = CPU_ADDR_TO_PCI_BUS_ADDR(page_to_phys(page)) + offset;

		DBG_INFO("***DMA*** %s(%s, %s): pfn %lx va %lx size %lu attrs %lx dma %llx\n",
			 __func__, dev_name(dev), dma_dir2str(dir),
			 (unsigned long)page_to_pfn(page),
			 (unsigned long)va, (unsigned long)size,
			 attrs, (unsigned long long)addr);

		switch (dir) {
		case DMA_TO_DEVICE:
			/* if (!(attrs & DMA_ATTR_SKIP_CPU_SYNC)) */
			cache_page_flush(page, offset, size);
			break;
		case DMA_FROM_DEVICE:
			/* if (!(attrs & DMA_ATTR_SKIP_CPU_SYNC)) */
			cache_page_invalidate(page, offset, size);
			break;
		case DMA_BIDIRECTIONAL:
			/* if (!(attrs & DMA_ATTR_SKIP_CPU_SYNC)) */
			cache_page_flush(page, offset, size);
			break;
		default:
			BUG();
		}
	}

	return addr;
}

static void
scr_pci_unmap_page(struct device *dev, dma_addr_t dma_handle,
		   size_t size, enum dma_data_direction dir,
		   unsigned long attrs)
{
	phys_addr_t cpu_addr = PCI_BUS_ADDR_TO_CPU_ADDR(dma_handle);

	DBG_INFO("***DMA*** %s(%s, %s): size %lu dma %llx va %px attrs %lx\n",
		 __func__, dev_name(dev), dma_dir2str(dir),
		 (unsigned long)size, (unsigned long long)dma_handle,
		 __va(cpu_addr), attrs);

	// ignore uncached region
	if (cpu_addr >= PFN_PHYS(riscv_pfn_base) && cpu_addr < PFN_PHYS(riscv_pfn_base + max_mapnr)) {
		switch (dir) {
		case DMA_TO_DEVICE:
			__asm__ __volatile__ ("fence" ::: "memory");
			break;
		case DMA_FROM_DEVICE:
			// fall through
		case DMA_BIDIRECTIONAL:
			/* if (!(attrs & DMA_ATTR_SKIP_CPU_SYNC)) */
			cache_addr_invalidate(__va(cpu_addr), size);
			break;
		default:
			BUG();
		}
	} else {
		DBG_INFO("***DMA*** <cpu addr %llx not in range [%llx %llx]>"
			 " %s(%s, %s): size %lu dma %llx attrs %lx\n",
			 (unsigned long long)cpu_addr, (unsigned long long)PFN_PHYS(riscv_pfn_base),
			 (unsigned long long)PFN_PHYS(riscv_pfn_base + max_mapnr),
			 __func__, dev_name(dev), dma_dir2str(dir),
			 (unsigned long)size, (unsigned long long)dma_handle, attrs);

		__asm__ __volatile__ ("fence" ::: "memory");
	}
}

static int
scr_pci_map_sg(struct device *dev, struct scatterlist *sg,
	       int nents, enum dma_data_direction dir,
	       unsigned long attrs)
{
	struct scatterlist *s;
	int i;

	for_each_sg(sg, s, nents, i) {
		s->dma_address = scr_pci_map_page(dev, sg_page(s), s->offset,
						  s->length, dir, attrs);
	}

	return nents;
}

static void
scr_pci_unmap_sg(struct device *dev, struct scatterlist *sg,
		 int nents, enum dma_data_direction dir,
		 unsigned long attrs)
{
	struct scatterlist *s;
	int i;

	for_each_sg(sg, s, nents, i) {
		scr_pci_unmap_page(dev, sg_dma_address(s), sg_dma_len(s), dir, attrs);
	}
}

static void
scr_pci_sync_single_for_cpu(struct device *dev,
			    dma_addr_t dma_handle, size_t size,
			    enum dma_data_direction dir)
{
	/* Invalidate the dcache for the requested range */

	// FIXME: kernel HIMEM (non linear addresses)

	phys_addr_t cpu_addr = PCI_BUS_ADDR_TO_CPU_ADDR(dma_handle);

	// ignore uncached region
	if (cpu_addr >= PFN_PHYS(riscv_pfn_base) && cpu_addr < PFN_PHYS(riscv_pfn_base + max_mapnr)) {
		void *vaddr = __va(cpu_addr);

		DBG_INFO("***DMA*** %s(%s, %s): size %lu dma %llx va %px\n",
			 __func__, dev_name(dev), dma_dir2str(dir),
			 (unsigned long)size, (unsigned long long)dma_handle,
			 vaddr);
		if (dir == DMA_TO_DEVICE)
			__asm__ __volatile__ ("fence" ::: "memory");
		else
			cache_addr_invalidate(vaddr, size);
	} else {
		DBG_INFO("***DMA*** <cpu addr %llx not in range [%llx %llx]>"
			 " %s(%s, %s): size %lu dma %llx\n",
			 (unsigned long long)cpu_addr, (unsigned long long)PFN_PHYS(riscv_pfn_base),
			 (unsigned long long)PFN_PHYS(riscv_pfn_base + max_mapnr),
			 __func__, dev_name(dev), dma_dir2str(dir),
			 (unsigned long)size, (unsigned long long)dma_handle);

		__asm__ __volatile__ ("fence" ::: "memory");
	}
}

static void
scr_pci_sync_single_for_device(struct device *dev,
			       dma_addr_t dma_handle, size_t size,
			       enum dma_data_direction dir)
{
	/* Flush the dcache for the requested range */

	// FIXME: kernel HIMEM (non linear addresses)

	phys_addr_t cpu_addr = PCI_BUS_ADDR_TO_CPU_ADDR(dma_handle);

	// ignore uncached region
	if (cpu_addr >= PFN_PHYS(riscv_pfn_base) && cpu_addr < PFN_PHYS(riscv_pfn_base + max_mapnr)) {
		void *vaddr = __va(cpu_addr);

		DBG_INFO("***DMA*** %s(%s, %s): size %lu dma %llx va %px\n",
			 __func__, dev_name(dev), dma_dir2str(dir),
			 (unsigned long)size, (unsigned long long)dma_handle,
			 vaddr);
		if (dir == DMA_FROM_DEVICE)
			cache_addr_invalidate(vaddr, size);
		else
			cache_addr_flush(vaddr, size);
	} else {
		DBG_INFO("***DMA*** <cpu addr %llx not in range [%llx %llx]>"
			 " %s(%s, %s) size %lu dma %llx\n",
			 (unsigned long long)cpu_addr, (unsigned long long)PFN_PHYS(riscv_pfn_base),
			 (unsigned long long)PFN_PHYS(riscv_pfn_base + max_mapnr),
			 __func__, dev_name(dev), dma_dir2str(dir),
			 (unsigned long)size, (unsigned long long)dma_handle);

		__asm__ __volatile__ ("fence" ::: "memory");
	}
}

const struct dma_map_ops scr_dma_pci_map_ops = {
	.alloc = scr_dma_pci_alloc_coherent,
	.free = scr_dma_pci_free_coherent,
	.map_page = scr_pci_map_page,
	.unmap_page = scr_pci_unmap_page,
	.map_sg = scr_pci_map_sg,
	.unmap_sg = scr_pci_unmap_sg,
	.sync_single_for_cpu = scr_pci_sync_single_for_cpu,
	.sync_single_for_device = scr_pci_sync_single_for_device,
	.dma_supported = scr_dma_supported,
};
EXPORT_SYMBOL(scr_dma_pci_map_ops);

static int __init scr_pcibios_set_cache_line_size(void)
{
	pci_dfl_cache_line_size = SMP_CACHE_BYTES >> 2;

	pr_debug("%s: pci_cache_line_size set to %d bytes\n",
		 __func__, (int)SMP_CACHE_BYTES);
	return 0;
}
arch_initcall(scr_pcibios_set_cache_line_size);
#endif /* CONFIG_PCI */

const struct dma_map_ops *get_arch_dma_ops(struct bus_type *bus)
{
	DBG_INFO("%s: \"%s\" \"%s\" root %px", __func__,
		 (bus ? bus->name : "null bus"),
		 (bus ? bus->dev_name : ""), bus ? bus->dev_root : 0);

#ifdef CONFIG_PCI
	if (bus == &pci_bus_type)
		return &scr_dma_pci_map_ops;
#endif /* CONFIG_PCI */

	return &scr_dma_platform_map_ops;
}

/*
 * Plug in coherent or noncoherent dma ops
 */
void arch_setup_dma_ops(struct device *dev, u64 dma_base, u64 size,
			const struct iommu_ops *iommu, bool coherent)
{
#ifdef CONFIG_PCI
	if (dev && dev->bus == &pci_bus_type) {
		set_dma_ops(dev, &scr_dma_pci_map_ops);
		dev_info(dev, "SCRxDMA: use pci bus dma ops\n");
		return;
	}
#endif /* CONFIG_PCI */

	set_dma_ops(dev, &scr_dma_platform_map_ops);
	dev_info(dev, "SCRxDMA: use platform dma ops\n");
}

static struct reserved_mem *scr_sdk_dma_reserved_memory __initdata;

static int scr_sdk_dma_device_init(struct reserved_mem *rmem, struct device *dev)
{
	return 0;
}

static void scr_sdk_dma_device_release(struct reserved_mem *rmem, struct device *dev)
{
}

static const struct reserved_mem_ops rmem_dma_ops = {
	.device_init    = scr_sdk_dma_device_init,
	.device_release = scr_sdk_dma_device_release,
};

static int __init scr_sdk_dma_init_memory(void)
{
	scr_alloc_dma_pool();

	return 0;
}

static int __init scr_sdk_dma_setup(struct reserved_mem *rmem)
{
	scr_sdk_dma_reserved_memory = rmem;

	scr_coherent_pool_size = rmem->size;
	scr_coherent_pool_base = rmem->base;

	rmem->ops = &rmem_dma_ops;

	if (scr_coherent_pool_size) {
		pr_info("%s: created DMA memory pool at %pa, size %ld MiB\n",
			__func__, &rmem->base, (unsigned long)rmem->size / SZ_1M);
	}

	return scr_coherent_pool_size > 0 ? 0 : -ENOMEM;
}

core_initcall(scr_sdk_dma_init_memory);

RESERVEDMEM_OF_DECLARE(dma, "scr-sdk-dma-pool", scr_sdk_dma_setup);
