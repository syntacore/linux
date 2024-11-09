// SPDX-License-Identifier: GPL-2.0
/*
 * Syntacore SCR Cache PMU for KVM
 *
 * Copyright (C) 2024 Syntacore
 *
 */
#include <linux/kvm_host.h>
#include <scr_cache_pmu_core.h>
#include <asm/kvm_vcpu_scr.h>

static const bool l2_enabled = IS_ENABLED(CONFIG_RISCV_PMU_SCR_L2_SBI);
static const bool l3_enabled = IS_ENABLED(CONFIG_RISCV_PMU_SCR_L3_SBI);

static int kvm_sbi_handle_scr_pmu(struct kvm_scr_pmu *kvpmu,
			struct kvm_vcpu_sbi_return *retdata, struct kvm_cpu_context *cp,
			unsigned long pmu_type, int dedicated)
{
	unsigned long scr_fn_id = cp->a0;
	int ret = 0;

	switch (scr_fn_id) {
	case SBI_EXT_PMU_NUM_COUNTERS:
		retdata->out_val = kvpmu->num_hw_ctrs;
		break;
	case SBI_EXT_PMU_COUNTER_CFG_MATCH:
		ret = kvm_riscv_vcpu_scr_pmu_ctr_cfg_match(kvpmu, cp->a1,
				cp->a2, cp->a3, cp->a4, cp->a5, retdata,
				pmu_type, dedicated);
		if (ret < 0)
			retdata->err_val = SBI_ERR_NOT_SUPPORTED;
		break;
	case SBI_EXT_PMU_COUNTER_START:
		ret = kvm_riscv_vcpu_scr_pmu_ctr_start(kvpmu, cp->a1,
					       cp->a2, cp->a3, cp->a4, retdata);
		break;
	case SBI_EXT_PMU_COUNTER_STOP:
		ret = kvm_riscv_vcpu_scr_pmu_ctr_stop(kvpmu, cp->a1,
					       cp->a2, cp->a3, retdata);
		break;
	case SBI_EXT_SCR_PMU_COUNTER_HW_READ:
		ret = kvm_riscv_vcpu_scr_pmu_ctr_read(kvpmu, cp->a1, retdata);
		break;
	case SBI_EXT_SCR_PMU_PROBE:
		if (dedicated)
			retdata->out_val = CACHE_DEDICATED_FLAG;
		else
			retdata->out_val = 0;
		break;
	default:
		retdata->err_val = SBI_ERR_NOT_SUPPORTED;
	}

	return ret;
}

static int kvm_sbi_ext_scr_handler(struct kvm_vcpu *vcpu, struct kvm_run *run,
				   struct kvm_vcpu_sbi_return *retdata)
{
	struct kvm_cpu_context *cp = &vcpu->arch.guest_context;
	unsigned long scr_ext_id = cp->a6;
	int ret = 0;

	if (!vcpu_to_scr_plf(vcpu)->enable_scr_pmu) {
		retdata->err_val = SBI_ERR_NOT_SUPPORTED;
		return ret;
	}

	if (l2_enabled && scr_ext_id == SBI_SCR7_L2_PMU_FN) {
		int dedicated = kvm_to_scr_plf(vcpu->kvm)->l2_dedicated;
		struct kvm_scr_pmu *kvpmu;

		if (dedicated)
			kvpmu = vcpu_to_pmu_scr_l2(vcpu);
		else
			kvpmu = kvm_to_pmu_scr_l2(vcpu->kvm);

		if (!kvpmu->init_done) {
			retdata->err_val = SBI_ERR_NOT_SUPPORTED;
			return 0;
		}
		ret = kvm_sbi_handle_scr_pmu(kvpmu, retdata, cp,
					SBI_SCR7_L2_PMU_FN, dedicated);
	} else if (l3_enabled && scr_ext_id == SBI_SCR7_L3_PMU_FN) {

		struct kvm_scr_pmu *kvpmu = kvm_to_pmu_scr_l3(vcpu->kvm);

		if (!kvpmu->init_done) {
			retdata->err_val = SBI_ERR_NOT_SUPPORTED;
			return 0;
		}
		ret = kvm_sbi_handle_scr_pmu(kvpmu, retdata, cp,
					SBI_SCR7_L3_PMU_FN, false);
	} else {
		retdata->err_val = SBI_ERR_NOT_SUPPORTED;
	}

	return ret;
}

static unsigned long kvm_sbi_ext_scr_probe(struct kvm_vcpu *vcpu)
{
	unsigned long ret = 0;

	if (l2_enabled) {
		int dedicated = kvm_to_scr_plf(vcpu->kvm)->l2_dedicated;
		struct kvm_scr_pmu *kvpmu;

		if (dedicated)
			kvpmu = vcpu_to_pmu_scr_l2(vcpu);
		else
			kvpmu = kvm_to_pmu_scr_l2(vcpu->kvm);

		ret |= kvpmu->init_done;
	}
	if (l3_enabled) {
		struct kvm_scr_pmu *kvpmu = kvm_to_pmu_scr_l3(vcpu->kvm);

		ret |= kvpmu->init_done;
	}

	return ret;
}

static void kvm_riscv_vm_scr_init(struct kvm *kvm)
{
	kvm->arch.plf.vm_plf = (struct kvm_scr_plf *)
			kzalloc(sizeof(struct kvm_scr_plf), GFP_KERNEL);

	if (!kvm->arch.plf.vm_plf)
		return;

	if (l2_enabled || l3_enabled)
		kvm_riscv_vm_pmu_scr_init(kvm);
}

static void kvm_riscv_vm_scr_deinit(struct kvm *kvm)
{
	if (l2_enabled)
		kvm_riscv_vm_pmu_scr_deinit(kvm, SBI_SCR7_L2_PMU_FN);
	if (l3_enabled)
		kvm_riscv_vm_pmu_scr_deinit(kvm, SBI_SCR7_L3_PMU_FN);

	kfree(kvm->arch.plf.vm_plf);
}

static void kvm_riscv_vcpu_scr_reset(struct kvm_vcpu *vcpu)
{
	if (l2_enabled)
		kvm_riscv_vcpu_pmu_scr_reset(vcpu, SBI_SCR7_L2_PMU_FN);
}

static void kvm_riscv_vcpu_scr_init(struct kvm_vcpu *vcpu)
{
	vcpu->arch.plf = (struct kvm_vcpu_scr_plf *)
			kzalloc(sizeof(struct kvm_vcpu_scr_plf), GFP_KERNEL);

	if (!vcpu->arch.plf)
		return;

	if (l2_enabled)
		/* setup performance monitoring for SCR l2 cache*/
		kvm_riscv_vcpu_pmu_scr_init(vcpu, SBI_SCR7_L2_PMU_FN);
}

static void kvm_riscv_vcpu_scr_deinit(struct kvm_vcpu *vcpu)
{
	if (l2_enabled)
		kvm_riscv_vcpu_pmu_scr_deinit(vcpu, SBI_SCR7_L2_PMU_FN);

	kfree(vcpu->arch.plf);
}

void kvm_sbi_ext_scr_init(struct kvm *kvm)
{
	struct kvm_riscv_plf *plf = kvm_to_plf(kvm);

	plf->vm_init = kvm_riscv_vm_scr_init;
	plf->vm_deinit = kvm_riscv_vm_scr_deinit;
	plf->vcpu_init = kvm_riscv_vcpu_scr_init;
	plf->vcpu_deinit = kvm_riscv_vcpu_scr_deinit;
	plf->vcpu_reset = kvm_riscv_vcpu_scr_reset;
	plf->handler = kvm_sbi_ext_scr_handler;
	plf->probe = kvm_sbi_ext_scr_probe;
}
