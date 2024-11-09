// SPDX-License-Identifier: GPL-2.0
/*
 * Syntacore SCR Cache PMU for KVM
 *
 * Copyright (C) 2024 Syntacore
 *
 * This code is based on riscv kvm pmu.
 */
#include <linux/kvm_host.h>
#include <scr_cache_pmu_core.h>
#include <asm/kvm_vcpu_scr.h>

static const bool l2_enabled = IS_ENABLED(CONFIG_RISCV_PMU_SCR_L2_SBI);
static const bool l3_enabled = IS_ENABLED(CONFIG_RISCV_PMU_SCR_L3_SBI);

static void scr_pmu_release_perf_event(struct kvm_scr_pmc *pmc)
{
	if (pmc->perf_event) {
		perf_event_disable(pmc->perf_event);
		perf_event_release_kernel(pmc->perf_event);
		pmc->perf_event = NULL;
	}
}
static int scr_pmu_get_free_pmc_index(struct kvm_scr_pmu *kvpmu,
					      unsigned long cbase, unsigned long cmask)
{
	int ctr_idx = -1;
	int i, pmc_idx;

	for_each_set_bit(i, &cmask, kvpmu->num_hw_ctrs) {
		pmc_idx = i + cbase;
		if ((pmc_idx >= 0 && pmc_idx < kvpmu->num_hw_ctrs) &&
		    !test_bit(pmc_idx, kvpmu->pmc_in_use)) {
			ctr_idx = pmc_idx;
			break;
		}
	}

	return ctr_idx;
}
static int scr_pmu_create_perf_event(struct kvm_scr_pmc *pmc,
		struct perf_event_attr *attr, unsigned long flags, unsigned long eidx,
		unsigned long evtdata, bool dedicated)
{
	struct perf_event *event;

	scr_pmu_release_perf_event(pmc);
	if (flags & SBI_PMU_CFG_FLAG_CLEAR_VALUE)
		pmc->counter_val = 0;

	if (dedicated)
		event = perf_event_create_kernel_counter(attr, -1, current, NULL, NULL);
	else {
		int existing_cpu = smp_processor_id();

		event = perf_event_create_kernel_counter(attr, existing_cpu, NULL, NULL, NULL);
	}

	if (IS_ERR(event)) {
		pr_err("kvm pmu event creation failed for eidx %lx: %ld\n", eidx, PTR_ERR(event));
		return PTR_ERR(event);
	}

	pmc->perf_event = event;
	if (flags & SBI_PMU_CFG_FLAG_AUTO_START)
		perf_event_enable(pmc->perf_event);

	return 0;
}

int kvm_riscv_vcpu_scr_pmu_ctr_start(struct kvm_scr_pmu *kvpmu, unsigned long ctr_base,
				 unsigned long ctr_mask, unsigned long flags, u64 ival,
				 struct kvm_vcpu_sbi_return *retdata)
{
	int i, pmc_index, sbiret = 0;
	struct kvm_scr_pmc *pmc;

	/* Start the counters that have been configured and requested by the guest */
	for_each_set_bit(i, &ctr_mask, kvpmu->num_hw_ctrs) {
		pmc_index = i + ctr_base;
		if (!test_bit(pmc_index, kvpmu->pmc_in_use))
			continue;
		pmc = &kvpmu->pmc[pmc_index];
		if (flags & SBI_PMU_START_FLAG_SET_INIT_VALUE)
			pmc->counter_val = ival;
		if (pmc->perf_event) {
			if (unlikely(pmc->started)) {
				sbiret = SBI_ERR_ALREADY_STARTED;
				continue;
			}
			perf_event_period(pmc->perf_event, 0);
			perf_event_enable(pmc->perf_event);
			pmc->started = true;
		} else {
			sbiret = SBI_ERR_INVALID_PARAM;
		}
	}

	retdata->err_val = sbiret;

	return 0;
}

int kvm_riscv_vcpu_scr_pmu_ctr_stop(struct kvm_scr_pmu *kvpmu, unsigned long ctr_base,
				unsigned long ctr_mask, unsigned long flags,
				struct kvm_vcpu_sbi_return *retdata)
{
	int i, pmc_index, sbiret = 0;
	struct kvm_scr_pmc *pmc;
	u64 enabled, running;

	/* Stop the counters that have been configured and requested by the guest */
	for_each_set_bit(i, &ctr_mask, kvpmu->num_hw_ctrs) {
		pmc_index = i + ctr_base;
		if (!test_bit(pmc_index, kvpmu->pmc_in_use))
			continue;
		pmc = &kvpmu->pmc[pmc_index];
		if (pmc->perf_event) {
			if (pmc->started) {
				/* Stop counting the counter */
				perf_event_disable(pmc->perf_event);
				pmc->started = false;
			} else {
				sbiret = SBI_ERR_ALREADY_STOPPED;
			}

			if (flags & SBI_PMU_STOP_FLAG_RESET) {
				/* Relase the counter if this is a reset request */
				pmc->counter_val += perf_event_read_value(pmc->perf_event,
									  &enabled, &running);
				scr_pmu_release_perf_event(pmc);
			}
		} else {
			sbiret = SBI_ERR_INVALID_PARAM;
		}
		if (flags & SBI_PMU_STOP_FLAG_RESET)
			clear_bit(pmc_index, kvpmu->pmc_in_use);
	}

	retdata->err_val = sbiret;

	return 0;
}

int kvm_riscv_vcpu_scr_pmu_ctr_cfg_match(struct kvm_scr_pmu *kvpmu,
			unsigned long ctr_base, unsigned long ctr_mask,
			unsigned long flags, unsigned long eidx, u64 evtdata,
			struct kvm_vcpu_sbi_return *retdata,
			unsigned long pmu_type, int dedicated)
{
	int ctr_idx, ret, sbiret = 0;
	struct kvm_scr_pmc *pmc = NULL;
	struct perf_event_attr attr;
	int etype;

	if (l2_enabled && pmu_type == SBI_SCR7_L2_PMU_FN) {
		etype = scr_pmu_get_etype(scr_pmu_l2_get_config());
	} else if (l3_enabled && pmu_type == SBI_SCR7_L3_PMU_FN) {
		etype = scr_pmu_get_etype(scr_pmu_l3_get_config());
	} else {
		sbiret = SBI_ERR_NOT_SUPPORTED;
		goto out;
	}

	attr = (struct perf_event_attr) {
		.type = etype,
		.size = sizeof(struct perf_event_attr),
		.pinned = true,
		.config = eidx | evtdata,
	};

	ctr_idx = scr_pmu_get_free_pmc_index(kvpmu, ctr_base, ctr_mask);
	if (ctr_idx < 0) {
		sbiret = SBI_ERR_NOT_SUPPORTED;
		goto out;
	}
	pmc = &kvpmu->pmc[ctr_idx];

	ret = scr_pmu_create_perf_event(pmc, &attr, flags, eidx, evtdata, dedicated);
	if (ret)
		return ret;

	set_bit(ctr_idx, kvpmu->pmc_in_use);
	retdata->out_val = ctr_idx;
out:
	retdata->err_val = sbiret;

	return 0;
}

int kvm_riscv_vcpu_scr_pmu_ctr_read(struct kvm_scr_pmu *kvpmu, unsigned long cidx,
				struct kvm_vcpu_sbi_return *retdata)
{
	struct kvm_scr_pmc *pmc;
	u64 enabled, running;

	pmc = &kvpmu->pmc[cidx];

	if (pmc->perf_event) {
		pmc->counter_val += perf_event_read_value(pmc->perf_event, &enabled, &running);
	} else {
		retdata->err_val = SBI_ERR_INVALID_PARAM;
		return -EINVAL;
	}
	retdata->out_val = pmc->counter_val;

	return 0;
}

static struct kvm_scr_pmu *choose_kvm_scr_kvpmu(struct kvm *kvm, unsigned long pmu_type)
{
	if (l2_enabled && pmu_type == SBI_SCR7_L2_PMU_FN)
		return kvm_to_pmu_scr_l2(kvm);
	else if (l3_enabled && pmu_type == SBI_SCR7_L3_PMU_FN)
		return kvm_to_pmu_scr_l3(kvm);
	else
		return NULL;
}
static struct kvm_scr_pmu *choose_vcpu_scr_kvpmu(struct kvm_vcpu *vcpu, unsigned long pmu_type)
{
	if (l2_enabled && pmu_type == SBI_SCR7_L2_PMU_FN)
		return vcpu_to_pmu_scr_l2(vcpu);
	else
		return NULL;
}
static void init_scr_pmu(struct kvm_scr_pmu *kvpmu, unsigned long pmu_type)
{
	int num_ctrs;

	if (l2_enabled && pmu_type == SBI_SCR7_L2_PMU_FN)
		num_ctrs = scr_pmu_get_num_counters(scr_pmu_l2_get_config());
	else if (l3_enabled && pmu_type == SBI_SCR7_L3_PMU_FN)
		num_ctrs = scr_pmu_get_num_counters(scr_pmu_l3_get_config());
	else
		return;

	if (num_ctrs == 0) {
		pr_err("No counters for SCR cache PMU found in KVM\n");
		return;
	}
	if (pmu_type == SBI_SCR7_L2_PMU_FN && num_ctrs > RISCV_KVM_SCR_L2_HW_CTRS) {
		pr_err("Limit SCR L2 cache PMU hardware counters is %d\n",
						RISCV_KVM_SCR_L2_HW_CTRS);
		return;
	}
	if (pmu_type == SBI_SCR7_L3_PMU_FN && num_ctrs > RISCV_KVM_SCR_L3_HW_CTRS) {
		pr_err("Limit SCR L3 cache PMU hardware counters is %d\n",
						RISCV_KVM_SCR_L3_HW_CTRS);
		return;
	}

	kvpmu->num_hw_ctrs = num_ctrs;
	kvpmu->init_done = true;
}
static void deinit_scr_pmu(struct kvm_scr_pmu *kvpmu)
{
	struct kvm_scr_pmc *pmc;
	int i;

	if (!kvpmu)
		return;

	for_each_set_bit(i, kvpmu->pmc_in_use, kvpmu->num_hw_ctrs) {
		pmc = &kvpmu->pmc[i];
		pmc->counter_val = 0;
		if (pmc->perf_event) {
			perf_event_disable(pmc->perf_event);
			perf_event_release_kernel(pmc->perf_event);
			pmc->perf_event = NULL;
		}
	}
	bitmap_zero(kvpmu->pmc_in_use, kvpmu->num_hw_ctrs);
}
void kvm_riscv_vm_pmu_scr_init(struct kvm *kvm)
{
	/* Caches can be dedicated and shared; in the first case, per-vcpu pmu
	 * is used, in the second, a common one for vm is used. For each of
	 * these cases, memory is allocated, but in the case of dedicated:
	 * per-vcpu pmu is initialized, and common for vm is set to zero;
	 * in the case of shared it’s the other way around
	 */
	if (l2_enabled) {
		struct kvm_scr_plf *kvplf = kvm_to_scr_plf(kvm);
		struct kvm_scr_pmu *kvpmu = choose_kvm_scr_kvpmu(kvm, SBI_SCR7_L2_PMU_FN);
		int dedicated;
		int ret = scr_pmu_is_available(scr_pmu_l2_get_config());

		if (!ret || !kvpmu) {
			pr_err("Failed to get SCR PMU L2 features in KVM");
			return;
		}
		dedicated = scr_pmu_is_dedicated(scr_pmu_l2_get_config());

		kvplf->l2_dedicated = dedicated;
		if (dedicated)
			memset(kvpmu, 0, sizeof(struct kvm_scr_pmu));
		else
			init_scr_pmu(kvpmu, SBI_SCR7_L2_PMU_FN);
	}
	if (l3_enabled) {
		struct kvm_scr_pmu *kvpmu = choose_kvm_scr_kvpmu(kvm, SBI_SCR7_L3_PMU_FN);
		int ret = scr_pmu_is_available(scr_pmu_l3_get_config());

		if (!ret || !kvpmu) {
			pr_err("Failed to get SCR PMU L3 features in KVM");
			return;
		}
		init_scr_pmu(kvpmu, SBI_SCR7_L3_PMU_FN);
	}
}
void kvm_riscv_vm_pmu_scr_deinit(struct kvm *kvm, unsigned long pmu_type)
{
	struct kvm_scr_pmu *kvpmu = choose_kvm_scr_kvpmu(kvm, pmu_type);

	if (l2_enabled)
		if (kvm_to_scr_plf(kvm)->l2_dedicated && pmu_type == SBI_SCR7_L2_PMU_FN)
			return;

	deinit_scr_pmu(kvpmu);
	kvpmu->init_done = false;
}
void kvm_riscv_vcpu_pmu_scr_init(struct kvm_vcpu *vcpu, unsigned long pmu_type)
{
	struct kvm_scr_pmu *kvpmu = choose_vcpu_scr_kvpmu(vcpu, pmu_type);

	if (l2_enabled)
		if (!kvm_to_scr_plf(vcpu->kvm)->l2_dedicated) {
			memset(kvpmu, 0, sizeof(struct kvm_scr_pmu));
			return;
		}

	init_scr_pmu(kvpmu, pmu_type);
}
void kvm_riscv_vcpu_pmu_scr_deinit(struct kvm_vcpu *vcpu, unsigned long pmu_type)
{
	struct kvm_scr_pmu *kvpmu = choose_vcpu_scr_kvpmu(vcpu, pmu_type);

	if (l2_enabled)
		if (!kvm_to_scr_plf(vcpu->kvm)->l2_dedicated)
			return;

	deinit_scr_pmu(kvpmu);
	kvpmu->init_done = false;
}
void kvm_riscv_vcpu_pmu_scr_reset(struct kvm_vcpu *vcpu, unsigned long pmu_type)
{
	struct kvm_scr_pmu *kvpmu = choose_vcpu_scr_kvpmu(vcpu, pmu_type);

	if (l2_enabled)
		if (!kvm_to_scr_plf(vcpu->kvm)->l2_dedicated)
			return;

	deinit_scr_pmu(kvpmu);
}
