// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 Ventana Micro Systems Inc.
 */

#include <linux/hw_breakpoint.h>
#include <linux/perf_event.h>
#include <linux/spinlock.h>
#include <linux/percpu.h>
#include <linux/kdebug.h>
#include <linux/bitops.h>
#include <linux/cpu.h>
#include <linux/cpuhotplug.h>

#include <asm/sbi.h>

/* Registered per-cpu bp/wp */
static DEFINE_PER_CPU(struct perf_event *, pcpu_hw_bp_events[HW_BP_NUM_MAX]);
static DEFINE_PER_CPU(unsigned long, ecall_lock_flags);
static DEFINE_PER_CPU(raw_spinlock_t, ecall_lock);

/* Per-cpu shared memory between S and M mode */
static struct sbi_dbtr_shmem_entry __percpu *sbi_dbtr_shmem;

/* number of debug triggers on this cpu . */
static int dbtr_total_num __ro_after_init;
static int dbtr_type __ro_after_init;
static int dbtr_init __ro_after_init;

static int arch_smp_setup_sbi_shmem(unsigned int cpu)
{
	struct sbi_dbtr_shmem_entry *dbtr_shmem;
	unsigned long shmem_pa;
	struct sbiret ret;
	int rc = 0;

	dbtr_shmem = per_cpu_ptr(sbi_dbtr_shmem, cpu);
	if (!dbtr_shmem) {
		pr_err("Invalid per-cpu shared memory for debug triggers\n");
		return -ENODEV;
	}

	shmem_pa = __pa(dbtr_shmem);

	if (IS_ENABLED(CONFIG_32BIT))
		ret = sbi_ecall(SBI_EXT_DBTR, SBI_EXT_DBTR_SETUP_SHMEM,
				lower_32_bits(shmem_pa),
				upper_32_bits(shmem_pa),
				0, 0, 0, 0);
	else
		ret = sbi_ecall(SBI_EXT_DBTR, SBI_EXT_DBTR_SETUP_SHMEM,
				shmem_pa, 0, 0, 0, 0, 0);

	if (ret.error) {
		switch (ret.error) {
		case SBI_ERR_DENIED:
			pr_warn("%s: Access denied for shared memory at %lx\n",
				__func__, shmem_pa);
			rc = -EPERM;
			break;

		case SBI_ERR_INVALID_PARAM:
		case SBI_ERR_INVALID_ADDRESS:
			pr_warn("%s: Invalid address parameter (%lu)\n",
				__func__, ret.error);
			rc = -EINVAL;
			break;

		case SBI_ERR_ALREADY_AVAILABLE:
			pr_warn("%s: Shared memory is already set\n",
				__func__);
			rc = -EADDRINUSE;
			break;

		case SBI_ERR_FAILURE:
			pr_err("%s: Internal sdtrig state error\n",
			       __func__);
			rc = -ENXIO;
			break;

		default:
			pr_warn("%s: Unknown error %lu\n", __func__, ret.error);
			rc = -ENXIO;
			break;
		}
	}

	pr_warn("CPU %d: HW Breakpoint shared memory registered.\n", cpu);

	return rc;
}

static int arch_smp_teardown_sbi_shmem(unsigned int cpu)
{
	struct sbiret ret;

	/* Disable shared memory */
	ret = sbi_ecall(SBI_EXT_DBTR, SBI_EXT_DBTR_SETUP_SHMEM,
			-1UL, -1UL, 0, 0, 0, 0);

	if (ret.error) {
		switch (ret.error) {
		case SBI_ERR_DENIED:
			pr_err("%s: Access denied for shared memory.\n",
			       __func__);
			break;

		case SBI_ERR_INVALID_PARAM:
		case SBI_ERR_INVALID_ADDRESS:
			pr_err("%s: Invalid address parameter (%lu)\n",
			       __func__, ret.error);
			break;

		case SBI_ERR_ALREADY_AVAILABLE:
			pr_err("%s: Shared memory is already set\n",
			       __func__);
			break;
		case SBI_ERR_FAILURE:
			pr_err("%s: Internal sdtrig state error\n",
			       __func__);
			break;
		default:
			pr_err("%s: Unknown error %lu\n", __func__, ret.error);
			break;
		}
	}

	pr_warn("CPU %d: HW Breakpoint shared memory disabled.\n", cpu);

	return 0;
}

static void init_sbi_dbtr(void)
{
	unsigned long tdata1;
	struct sbiret ret;

	if (sbi_probe_extension(SBI_EXT_DBTR) <= 0) {
		pr_warn("%s: SBI_EXT_DBTR is not supported\n", __func__);
		dbtr_total_num = 0;
		goto done;
	}

	ret = sbi_ecall(SBI_EXT_DBTR, SBI_EXT_DBTR_NUM_TRIGGERS,
			0, 0, 0, 0, 0, 0);
	if (ret.error) {
		pr_warn("%s: Failed to detect triggers\n", __func__);
		dbtr_total_num = 0;
		goto done;
	}

	if (!IS_ENABLED(CONFIG_SOC_SCR)) {
		tdata1 = 0;
		RV_DBTR_SET_TDATA1_TYPE(tdata1, RV_DBTR_TRIG_MCONTROL6);

		ret = sbi_ecall(SBI_EXT_DBTR, SBI_EXT_DBTR_NUM_TRIGGERS,
				tdata1, 0, 0, 0, 0, 0);
		if (ret.error) {
			pr_warn("%s: failed to detect mcontrol6 triggers\n", __func__);
		} else if (!ret.value) {
			pr_warn("%s: type 6 triggers not available\n", __func__);
		} else {
			dbtr_total_num = ret.value;
			dbtr_type = RV_DBTR_TRIG_MCONTROL6;
			pr_warn("%s: mcontrol6 trigger available.\n", __func__);
			goto done;
		}
	}

	/* fallback to type 2 triggers if type 6 is not available */

	tdata1 = 0;
	RV_DBTR_SET_TDATA1_TYPE(tdata1, RV_DBTR_TRIG_MCONTROL);

	ret = sbi_ecall(SBI_EXT_DBTR, SBI_EXT_DBTR_NUM_TRIGGERS,
			tdata1, 0, 0, 0, 0, 0);
	if (ret.error) {
		pr_warn("%s: failed to detect mcontrol triggers\n", __func__);
	} else if (!ret.value) {
		pr_warn("%s: type 2 triggers not available\n", __func__);
	} else {
		dbtr_total_num = ret.value;
		dbtr_type = RV_DBTR_TRIG_MCONTROL;
		goto done;
	}

done:
	dbtr_init = 1;
}

int hw_breakpoint_slots(int type)
{
	/*
	 * We can be called early, so don't rely on
	 * static variables being initialised.
	 */

	if (!dbtr_init)
		init_sbi_dbtr();

	return dbtr_total_num;
}

int arch_check_bp_in_kernelspace(struct arch_hw_breakpoint *hw)
{
	return 0; // wanna be optimistic for some time
}

int hw_breakpoint_arch_parse(struct perf_event *bp,
			     const struct perf_event_attr *attr,
			     struct arch_hw_breakpoint *hw)
{
	return 0;
}

static int hw_breakpoint_handler(struct die_args *args)
{
	struct perf_event *event;
	struct perf_event *any_triggered_event = NULL;
	int chain_len;

	struct sbi_dbtr_shmem_entry *shmem = this_cpu_ptr(sbi_dbtr_shmem);
	struct sbi_dbtr_shmem_entry *entry;
	struct sbiret ret;

	int any_trigger_fired = 0;

	pr_debug("%s: wow, looks like some ebreak stuff triggered\n", __func__);

	// The first pass - hit bit check
	for (int i = 0; i < dbtr_total_num; ++i) {
		event = this_cpu_read(pcpu_hw_bp_events[i]);
		if (!event)
			continue;

		pr_debug("%s:\tlooking for %dth breakpoint...\n", __func__, i);

		chain_len = event->hw.info.chain->len;

		ret = sbi_ecall(SBI_EXT_DBTR, SBI_EXT_DBTR_TRIG_READ,
				event->hw.info.chain->idx, chain_len, 0, 0, 0, 0);

		if (ret.error) {
			pr_err("%s: failed to read triggers\n", __func__);
			// something strange happen, let's still try to check other triggers
			continue;
		}

		for (entry = shmem; entry < shmem + chain_len; ++entry) {
			pr_debug("%s:\t\ttrigger tdata1 = %lx, hit = %lu\n",
				 __func__, entry->data.tdata1,
				 RV_DBTR_GET_MC_HIT(entry->data.tdata1));

			int last_trigger = (entry == (shmem + chain_len - 1));
			int fired = 0;

			switch (RV_DBTR_GET_TYPE(entry->data.tdata1)) {
			case RV_DBTR_TRIG_MCONTROL:
				if (RV_DBTR_GET_MC_HIT(entry->data.tdata1))
					fired = 1;
				break;

			case RV_DBTR_TRIG_MCONTROL6:
				if (RV_DBTR_GET_MC6_HIT(entry->data.tdata1))
					fired = 1;
				break;

			// TODO: other trigger types
			}

			if (fired) {
				if (!last_trigger)
					pr_err("%s: intermediate chain trigger fired, looks like hardware bug\n",
					       __func__);

				++event->hw.info.chain->fires;
				any_trigger_fired = 1;
				any_triggered_event = event;
				RV_DBTR_CLEAR_MC6_HIT(entry->data.tdata1);
				pr_debug("%s:\tyes, it triggered!\n", __func__);
			}
		}

		ret = sbi_ecall(SBI_EXT_DBTR, SBI_EXT_DBTR_TRIG_UPDATE,
				event->hw.info.chain->idx, (1ul << chain_len) - 1, 0, 0, 0, 0);

		if (ret.error)
			pr_err("%s: failed to update triggers\n", __func__);
	}

	if (any_trigger_fired) {
		perf_bp_event(any_triggered_event, args->regs);
		return NOTIFY_STOP;
	} else
		return NOTIFY_DONE;
}

int hw_breakpoint_exceptions_notify(struct notifier_block *unused,
				    unsigned long val, void *data)
{
	if (val != DIE_DEBUG)
		return NOTIFY_DONE;

	return hw_breakpoint_handler(data);
}

/* atomic: counter->ctx->lock is held */
int arch_install_hw_breakpoint(struct perf_event *event)
{
	struct arch_hw_breakpoint *bp = counter_arch_bp(event);
	struct sbi_dbtr_shmem_entry *shmem = this_cpu_ptr(sbi_dbtr_shmem);
	struct sbi_dbtr_data_msg *xmit;
	struct sbi_dbtr_id_msg *recv;
	struct perf_event **slot;
	unsigned long idx;
	struct sbiret ret;
	int err = 0;

	raw_spin_lock_irqsave(this_cpu_ptr(&ecall_lock),
			      *this_cpu_ptr(&ecall_lock_flags));

	pr_debug("%s: installing breakpoint...\n", __func__);

	for (struct hw_breakpoint_regs const *regs = bp->chain->regs;
			regs < bp->chain->regs + bp->chain->len; ++regs) {
		pr_debug("\ttrigger tdata1 = %lx, tdata2 = %lx, tdata3 = %lx\n",
			 regs->tdata1, regs->tdata2, regs->tdata3);
	}

	int i = 0;

	for (struct hw_breakpoint_regs const *regs = bp->chain->regs;
			regs < bp->chain->regs + bp->chain->len; ++regs) {
		xmit = &shmem[i].data;
		xmit->tdata1 = cpu_to_le(regs->tdata1);
		xmit->tdata2 = cpu_to_le(regs->tdata2);
		xmit->tdata3 = cpu_to_le(regs->tdata3);
		++i;
	}

	recv = &shmem->id;

	ret = sbi_ecall(SBI_EXT_DBTR, SBI_EXT_DBTR_TRIG_INSTALL,
			bp->chain->len, 0, 0, 0, 0, 0);

	if (ret.error) {
		err = -EINVAL;
		pr_err("%s: failed to install trigger\n", __func__);
		goto done;
	}

	idx = le_to_cpu(recv->idx);
	if (idx >= dbtr_total_num) { // looks paranoid
		pr_warn("%s: invalid trigger index %lu\n", __func__, idx);
		err = -EINVAL;
		goto done;
	}

	pr_debug("\tindex = %lu\n", recv->idx);

	event->hw.info.chain->idx = idx;

	slot = this_cpu_ptr(&pcpu_hw_bp_events[idx]);
	if (*slot) {
		pr_warn("%s: slot %lu is in use\n", __func__, idx);
		err = -EBUSY;
		goto done;
	}

	/* Save the event - to be looked up in handler */
	*slot = event;

done:
	raw_spin_unlock_irqrestore(this_cpu_ptr(&ecall_lock),
				   *this_cpu_ptr(&ecall_lock_flags));
	return err;
}

int arch_probe_install_hw_breakpoints(struct arch_hw_breakpoint *bps, int count)
{
	struct sbi_dbtr_shmem_entry *shmem = this_cpu_ptr(sbi_dbtr_shmem);
	struct sbi_dbtr_data_msg *xmit;
	struct sbi_dbtr_id_msg *recv;
	struct sbiret ret;

	unsigned long idxs[HW_BP_NUM_MAX];
	int trig, chain, err = 0;

	raw_spin_lock_irqsave(this_cpu_ptr(&ecall_lock),
			      *this_cpu_ptr(&ecall_lock_flags));

	pr_debug("%s: probe installation of triggers started\n", __func__);

	for (trig = 0; trig < count; ++trig) {
		pr_info("%s: sending trig %d, chain len is %lu\n",
			__func__, trig, bps[trig].chain->len);

		recv = &shmem->id;
		for (chain = 0; chain < bps[trig].chain->len; ++chain) {
			xmit = &shmem[chain].data;

			pr_debug("%s: sending tdata1 = %lx, tdata2 = %lx\n",
				 __func__, bps[trig].chain->regs[chain].tdata1,
				 bps[trig].chain->regs[chain].tdata2);

			xmit->tdata1 = cpu_to_le(bps[trig].chain->regs[chain].tdata1);
			xmit->tdata2 = cpu_to_le(bps[trig].chain->regs[chain].tdata2);
			xmit->tdata3 = cpu_to_le(bps[trig].chain->regs[chain].tdata3);
		}

		ret = sbi_ecall(SBI_EXT_DBTR, SBI_EXT_DBTR_TRIG_INSTALL,
				bps[trig].chain->len, 0, 0, 0, 0, 0);

		if (ret.error) {
			pr_err("%s: failed to install trigger\n", __func__);
			err = -EINVAL;
			goto done;
		}

		// we worry only about the first trigger id, because chained triggers must have
		// consequent indexes according to SBI SPEC
		idxs[trig] = le_to_cpu(recv->idx);
		if (idxs[trig] >= dbtr_total_num) { // paranoid check...
			pr_err("%s: invalid trigger index %lu\n", __func__, idxs[trig]);
			err = -EINVAL;
			goto done;
		}

		pr_debug("%s: trig %d installed\n", __func__, trig);
	}

	pr_debug("%s: probe installation of triggers finished\n",
		 __func__);

done:
	for (int i = 0; i < trig; ++i) {
		ret = sbi_ecall(SBI_EXT_DBTR, SBI_EXT_DBTR_TRIG_UNINSTALL,
				idxs[i], (1 << bps[i].chain->len) - 1, 0, 0, 0, 0);

		if (ret.error)
			pr_err("%s: Failed to uninstall trigger %lu.\n", __func__, idxs[i]);
	}

	raw_spin_unlock_irqrestore(this_cpu_ptr(&ecall_lock),
				   *this_cpu_ptr(&ecall_lock_flags));

	pr_debug("%s: all probe triggers uninstalled\n", __func__);

	return err;
}

/* atomic: counter->ctx->lock is held */
void arch_uninstall_hw_breakpoint(struct perf_event *event)
{
	struct sbiret ret;
	int i;

	pr_debug("%s: looking for the trigger required %p\n", __func__, event);

	for (i = 0; i < dbtr_total_num; i++) {
		struct perf_event **slot = this_cpu_ptr(&pcpu_hw_bp_events[i]);

		pr_debug("%s:\tlooking for slot %d at %p\n", __func__, i, slot);

		if (!slot)
			continue;

		if (*slot == event) {
			*slot = NULL;
			break;
		}

	}

	pr_debug("%s: uninstalling %lu triggers starting from %lu\n",
		 __func__, event->hw.info.chain->len, event->hw.info.chain->idx);

	if (i == dbtr_total_num) {
		pr_warn("%s: Breakpoint not installed.\n", __func__);
		return;
	}

	pr_debug("%s: idx %d\n", __func__, i);

	ret = sbi_ecall(SBI_EXT_DBTR, SBI_EXT_DBTR_TRIG_UNINSTALL,
			event->hw.info.chain->idx,
			(1ul << event->hw.info.chain->len) - 1, 0, 0, 0, 0);
	if (ret.error)
		pr_err("%s: Failed to uninstall trigger %d.\n", __func__, i);
}

void hw_breakpoint_pmu_read(struct perf_event *bp)
{
	/* TODO */
}

/*
 * Set ptrace breakpoint pointers to zero for this task.
 * This is required in order to prevent child processes from unregistering
 * breakpoints held by their parent.
 */
void clear_ptrace_hw_breakpoint(struct task_struct *tsk)
{
	memset(tsk->thread.ptrace_bps, 0, sizeof(tsk->thread.ptrace_bps));
}

/*
 * Unregister breakpoints from this task and reset the pointers in
 * the thread_struct.
 */
void flush_ptrace_hw_breakpoint(struct task_struct *tsk)
{
	struct thread_struct *t = &tsk->thread;
	int i;

	for (i = 0; i < dbtr_total_num; i++) {
		unregister_hw_breakpoint(t->ptrace_bps[i]);
		t->ptrace_bps[i] = NULL;
	}
}

static int __init arch_hw_breakpoint_init(void)
{
	unsigned int cpu;
	int rc = 0;

	for_each_possible_cpu(cpu)
		raw_spin_lock_init(&per_cpu(ecall_lock, cpu));

	if (!dbtr_init)
		init_sbi_dbtr();

	if (dbtr_total_num) {
		pr_info("%s: total number of type %d triggers: %u\n",
			__func__, dbtr_type, dbtr_total_num);
	} else {
		pr_info("%s: No hardware triggers available\n", __func__);
		goto out;
	}

	/* Allocate per-cpu shared memory */
	sbi_dbtr_shmem = __alloc_percpu(sizeof(*sbi_dbtr_shmem) * dbtr_total_num,
					PAGE_SIZE);

	if (!sbi_dbtr_shmem) {
		pr_warn("%s: Failed to allocate shared memory.\n", __func__);
		rc = -ENOMEM;
		goto out;
	}

	/* Hotplug handler to register/unregister shared memory with SBI */
	rc = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN,
			       "riscv/hw_breakpoint:prepare",
			       arch_smp_setup_sbi_shmem,
			       arch_smp_teardown_sbi_shmem);

	if (rc < 0) {
		pr_warn("%s: Failed to setup CPU hotplug state\n", __func__);
		free_percpu(sbi_dbtr_shmem);
		return rc;
	}
 out:
	return rc;
}
arch_initcall(arch_hw_breakpoint_init);
