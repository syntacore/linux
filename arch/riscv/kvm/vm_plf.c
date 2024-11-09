// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024 Syntacore
 */

#include <linux/errno.h>
#include <linux/err.h>
#include <linux/kvm_host.h>
#include <asm/sbi.h>
#include <asm/kvm_vm_plf.h>
#include <asm/vendorid_list.h>

#include <asm/kvm_vcpu_scr.h>

void kvm_riscv_vm_plf_init(struct kvm *kvm)
{
	kvm_to_plf(kvm)->vendor_id = riscv_cached_mvendorid(smp_processor_id());

	switch (kvm_to_plf(kvm)->vendor_id) {
#ifdef CONFIG_SOC_SCR
	case SCR_VENDOR_ID:
		kvm_sbi_ext_scr_init(kvm);
		break;
#endif
	default:
		kvm_to_plf(kvm)->vendor_id = -1;
	}
	if (kvm_to_plf(kvm)->vm_init)
		kvm_to_plf(kvm)->vm_init(kvm);
}
void kvm_riscv_vm_plf_deinit(struct kvm *kvm)
{
	if (kvm_to_plf(kvm)->vm_deinit)
		kvm_to_plf(kvm)->vm_deinit(kvm);
}
