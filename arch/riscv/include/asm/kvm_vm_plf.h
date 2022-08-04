/* SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2024 Syntacore
 */

#ifndef __KVM_VM_RISCV_PLF_H
#define __KVM_VM_RISCV_PLF_H

#include <asm/kvm_vcpu_sbi.h>

struct kvm_riscv_plf {
	long		vendor_id;
	void		*vm_plf;

	void (*vm_init)(struct kvm *kvm);
	void (*vm_deinit)(struct kvm *kvm);
	void (*vcpu_init)(struct kvm_vcpu *vcpu);
	void (*vcpu_deinit)(struct kvm_vcpu *vcpu);
	void (*vcpu_reset)(struct kvm_vcpu *vcpu);
	int  (*handler)(struct kvm_vcpu *vcpu, struct kvm_run *run,
					struct kvm_vcpu_sbi_return *retdata);
	unsigned long (*probe)(struct kvm_vcpu *vcpu);
};

void kvm_riscv_vm_plf_init(struct kvm *kvm);
void kvm_riscv_vm_plf_deinit(struct kvm *kvm);

#define kvm_to_plf(kvm) (&(kvm)->arch.plf)

#endif /* __KVM_VM_RISCV_PLF_H */
