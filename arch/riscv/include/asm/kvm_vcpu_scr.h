/* SPDX-License-Identifier: GPL-2.0
 *
 * Syntacore SCR Cache PMU for KVM
 *
 * Copyright (C) 2024 Syntacore
 *
 */

#ifndef __KVM_VCPU_RISCV_SCR_H
#define __KVM_VCPU_RISCV_SCR_H

#include <linux/errno.h>
#include <linux/err.h>
#include <asm/kvm_vcpu_sbi.h>
#include <asm/sbi.h>

#define MAX(a, b) ((a) >= (b) ? (a) : (b))

#define RISCV_KVM_SCR_L2_HW_CTRS	4
#define RISCV_KVM_SCR_L3_HW_CTRS	4
#define RISCV_KVM_SCR_MAX_HW_CTRS MAX(RISCV_KVM_SCR_L2_HW_CTRS, RISCV_KVM_SCR_L3_HW_CTRS)

#define vcpu_to_scr_plf(vcpu) ((struct kvm_vcpu_scr_plf *)(vcpu)->arch.plf)
#define kvm_to_scr_plf(kvm) ((struct kvm_scr_plf *)(kvm)->arch.plf.vm_plf)
#define vcpu_to_pmu_scr_l2(vcpu) (&(vcpu_to_scr_plf(vcpu))->pmu_scr_l2)
#define kvm_to_pmu_scr_l2(kvm) (&(kvm_to_scr_plf(kvm))->pmu_scr_l2)
#define kvm_to_pmu_scr_l3(kvm) (&(kvm_to_scr_plf(kvm))->pmu_scr_l3)

/* Per virtual pmu counter data */
struct kvm_scr_pmc {
	struct perf_event *perf_event;
	u64 counter_val;
	/* Event monitoring status */
	bool started;
};

/* PMU data structure per vcpu */
struct kvm_scr_pmu {
	struct kvm_scr_pmc pmc[RISCV_KVM_SCR_MAX_HW_CTRS];
	/* Number of the virtual hardware counters available */
	int num_hw_ctrs;
	/* A flag to indicate that pmu initialization is done */
	bool init_done;
	/* Bit map of all the virtual counter used */
	DECLARE_BITMAP(pmc_in_use, RISCV_KVM_SCR_MAX_HW_CTRS);
};

struct kvm_scr_plf {
	/* SCR L2 CACHE PMU: per core or not (true - per core)*/
	bool l2_dedicated;
	struct kvm_scr_pmu pmu_scr_l2;
	struct kvm_scr_pmu pmu_scr_l3;
};

struct kvm_vcpu_scr_plf {
	unsigned long enable_scr_pmu;
	struct kvm_scr_pmu pmu_scr_l2;
};


void kvm_sbi_ext_scr_init(struct kvm *kvm);

int kvm_riscv_vcpu_scr_pmu_ctr_start(struct kvm_scr_pmu *kvpmu, unsigned long ctr_base,
				 unsigned long ctr_mask, unsigned long flags, u64 ival,
				 struct kvm_vcpu_sbi_return *retdata);
int kvm_riscv_vcpu_scr_pmu_ctr_stop(struct kvm_scr_pmu *kvpmu, unsigned long ctr_base,
				unsigned long ctr_mask, unsigned long flags,
				struct kvm_vcpu_sbi_return *retdata);
int kvm_riscv_vcpu_scr_pmu_ctr_cfg_match(struct kvm_scr_pmu *kvpmu,
			unsigned long ctr_base, unsigned long ctr_mask,
			unsigned long flags, unsigned long eidx, u64 evtdata,
			struct kvm_vcpu_sbi_return *retdata,
			unsigned long pmu_type, int dedicated);
int kvm_riscv_vcpu_scr_pmu_ctr_read(struct kvm_scr_pmu *kvpmu, unsigned long cidx,
				struct kvm_vcpu_sbi_return *retdata);

void kvm_riscv_vm_pmu_scr_init(struct kvm *kvm);
void kvm_riscv_vm_pmu_scr_deinit(struct kvm *kvm, unsigned long pmu_type);
void kvm_riscv_vcpu_pmu_scr_init(struct kvm_vcpu *vcpu, unsigned long pmu_type);
void kvm_riscv_vcpu_pmu_scr_deinit(struct kvm_vcpu *vcpu, unsigned long pmu_type);
void kvm_riscv_vcpu_pmu_scr_reset(struct kvm_vcpu *vcpu, unsigned long pmu_type);

#endif /* !__KVM_VCPU_RISCV_SCR_H */
