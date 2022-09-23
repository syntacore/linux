// SPDX-License-Identifier: GPL-2.0-only

#include <linux/hw_breakpoint.h>
#include <linux/perf_event.h>
#include <linux/percpu.h>
#include <linux/kdebug.h>

#include <asm/sbi.h>

/* bps/wps currently set on each debug trigger for each cpu */
static DEFINE_PER_CPU(struct perf_event *, bp_per_reg[HBP_NUM_MAX]);

static struct sbi_dbtr_data_msg __percpu *sbi_xmit;
static struct sbi_dbtr_id_msg __percpu *sbi_recv;

/* number of debug triggers on this cpu . */
static int dbtr_total_num __ro_after_init;
static int dbtr_type __ro_after_init;
static int dbtr_init __ro_after_init;

void arch_hw_breakpoint_init_sbi(void)
{
	union riscv_dbtr_tdata1 tdata1;
	struct sbiret ret;

	if (sbi_probe_extension(SBI_EXT_DBTR) <= 0) {
		pr_info("%s: SBI_EXT_DBTR is not supported\n", __func__);
		dbtr_total_num = 0;
		goto done;
	}

	ret = sbi_ecall(SBI_EXT_DBTR, SBI_EXT_DBTR_NUM_TRIGGERS,
			0, 0, 0, 0, 0, 0);
	if (ret.error) {
		pr_warn("%s: failed to detect triggers\n", __func__);
		dbtr_total_num = 0;
		goto done;
	}

	tdata1.value = 0;
	tdata1.type = RISCV_DBTR_TRIG_MCONTROL6;

	ret = sbi_ecall(SBI_EXT_DBTR, SBI_EXT_DBTR_NUM_TRIGGERS,
			tdata1.value, 0, 0, 0, 0, 0);
	if (ret.error) {
		pr_warn("%s: failed to detect mcontrol6 triggers\n", __func__);
	} else if (!ret.value) {
		pr_warn("%s: type 6 triggers not available\n", __func__);
	} else {
		dbtr_total_num = ret.value;
		dbtr_type = RISCV_DBTR_TRIG_MCONTROL6;
		goto done;
	}

	/* fallback to type 2 triggers if type 6 is not available */

	tdata1.value = 0;
	tdata1.type = RISCV_DBTR_TRIG_MCONTROL;

	ret = sbi_ecall(SBI_EXT_DBTR, SBI_EXT_DBTR_NUM_TRIGGERS,
			tdata1.value, 0, 0, 0, 0, 0);
	if (ret.error) {
		pr_warn("%s: failed to detect mcontrol triggers\n", __func__);
	} else if (!ret.value) {
		pr_warn("%s: type 2 triggers not available\n", __func__);
	} else {
		dbtr_total_num = ret.value;
		dbtr_type = RISCV_DBTR_TRIG_MCONTROL;
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
		arch_hw_breakpoint_init_sbi();

	return dbtr_total_num;
}

int arch_check_bp_in_kernelspace(struct arch_hw_breakpoint *hw)
{
	unsigned int len;
	unsigned long va;

	va = hw->address;
	len = hw->len;

	return (va >= TASK_SIZE) && ((va + len - 1) >= TASK_SIZE);
}

int arch_build_type2_trigger(const struct perf_event_attr *attr,
			     struct arch_hw_breakpoint *hw)
{
	/* type */
	switch (attr->bp_type) {
	case HW_BREAKPOINT_X:
		hw->type = RISCV_DBTR_BREAKPOINT;
		hw->trig_data1.mcontrol.execute = 1;
		break;
	case HW_BREAKPOINT_R:
		hw->type = RISCV_DBTR_WATCHPOINT;
		hw->trig_data1.mcontrol.load = 1;
		break;
	case HW_BREAKPOINT_W:
		hw->type = RISCV_DBTR_WATCHPOINT;
		hw->trig_data1.mcontrol.store = 1;
		break;
	case HW_BREAKPOINT_RW:
		hw->type = RISCV_DBTR_WATCHPOINT;
		hw->trig_data1.mcontrol.store = 1;
		hw->trig_data1.mcontrol.load = 1;
		break;
	default:
		return -EINVAL;
	}

	/* length */
	switch (attr->bp_len) {
	case HW_BREAKPOINT_LEN_1:
		hw->len = 1;
		hw->trig_data1.mcontrol.sizelo = 1;
		break;
	case HW_BREAKPOINT_LEN_2:
		hw->len = 2;
		hw->trig_data1.mcontrol.sizelo = 2;
		break;
	case HW_BREAKPOINT_LEN_4:
		hw->len = 4;
		hw->trig_data1.mcontrol.sizelo = 3;
		break;
#if __riscv_xlen >= 64
	case HW_BREAKPOINT_LEN_8:
		hw->len = 8;
		hw->trig_data1.mcontrol.sizelo = 1;
		hw->trig_data1.mcontrol.sizehi = 1;
		break;
#endif
	default:
		return -EINVAL;
	}

	hw->trig_data1.mcontrol.type = RISCV_DBTR_TRIG_MCONTROL;
	hw->trig_data1.mcontrol.dmode = 0;
	hw->trig_data1.mcontrol.timing = 0;
	hw->trig_data1.mcontrol.select = 0;
	hw->trig_data1.mcontrol.action = 0;
	hw->trig_data1.mcontrol.chain = 0;
	hw->trig_data1.mcontrol.match = 0;

	hw->trig_data1.mcontrol.m = 0;
	hw->trig_data1.mcontrol.s = 1;
	hw->trig_data1.mcontrol.u = 1;

	return 0;
}

int arch_build_type6_trigger(const struct perf_event_attr *attr,
			     struct arch_hw_breakpoint *hw)
{
	/* type */
	switch (attr->bp_type) {
	case HW_BREAKPOINT_X:
		hw->type = RISCV_DBTR_BREAKPOINT;
		hw->trig_data1.mcontrol6.execute = 1;
		break;
	case HW_BREAKPOINT_R:
		hw->type = RISCV_DBTR_WATCHPOINT;
		hw->trig_data1.mcontrol6.load = 1;
		break;
	case HW_BREAKPOINT_W:
		hw->type = RISCV_DBTR_WATCHPOINT;
		hw->trig_data1.mcontrol6.store = 1;
		break;
	case HW_BREAKPOINT_RW:
		hw->type = RISCV_DBTR_WATCHPOINT;
		hw->trig_data1.mcontrol6.store = 1;
		hw->trig_data1.mcontrol6.load = 1;
		break;
	default:
		return -EINVAL;
	}

	/* length */
	switch (attr->bp_len) {
	case HW_BREAKPOINT_LEN_1:
		hw->len = 1;
		hw->trig_data1.mcontrol6.size = 1;
		break;
	case HW_BREAKPOINT_LEN_2:
		hw->len = 2;
		hw->trig_data1.mcontrol6.size = 2;
		break;
	case HW_BREAKPOINT_LEN_4:
		hw->len = 4;
		hw->trig_data1.mcontrol6.size = 3;
		break;
	case HW_BREAKPOINT_LEN_8:
		hw->len = 8;
		hw->trig_data1.mcontrol6.size = 5;
		break;
	default:
		return -EINVAL;
	}

	hw->trig_data1.mcontrol6.type = RISCV_DBTR_TRIG_MCONTROL6;
	hw->trig_data1.mcontrol6.dmode = 0;
	hw->trig_data1.mcontrol6.timing = 0;
	hw->trig_data1.mcontrol6.select = 0;
	hw->trig_data1.mcontrol6.action = 0;
	hw->trig_data1.mcontrol6.chain = 0;
	hw->trig_data1.mcontrol6.match = 0;

	hw->trig_data1.mcontrol6.m = 0;
	hw->trig_data1.mcontrol6.s = 1;
	hw->trig_data1.mcontrol6.u = 1;
	hw->trig_data1.mcontrol6.vs = 0;
	hw->trig_data1.mcontrol6.vu = 0;

	return 0;
}

int hw_breakpoint_arch_parse(struct perf_event *bp,
			     const struct perf_event_attr *attr,
			     struct arch_hw_breakpoint *hw)
{
	int ret;

	/* address */
	hw->address = attr->bp_addr;
	hw->trig_data2 = attr->bp_addr;
	hw->trig_data3 = 0x0;

	switch (dbtr_type) {
	case RISCV_DBTR_TRIG_MCONTROL:
		ret = arch_build_type2_trigger(attr, hw);
		break;
	case RISCV_DBTR_TRIG_MCONTROL6:
		ret = arch_build_type6_trigger(attr, hw);
		break;
	default:
		pr_warn("unsupported trigger type\n");
		ret = -EOPNOTSUPP;
		break;
	}

	return ret;
}

/*
 * Handle debug exception notifications.
 */
static int hw_breakpoint_handler(struct die_args *args)
{
	int ret = NOTIFY_DONE;
	struct arch_hw_breakpoint *info;
	struct perf_event *bp;
	int i;

	for (i = 0; i < dbtr_total_num; ++i) {
		bp = this_cpu_read(bp_per_reg[i]);
		if (!bp)
			continue;

		info = counter_arch_bp(bp);
		switch (info->type) {
		case RISCV_DBTR_BREAKPOINT:
			if (info->address == args->regs->epc) {
				pr_debug("%s: breakpoint fired: pc[0x%lx]\n",
					 __func__, args->regs->epc);
				perf_bp_event(bp, args->regs);
				ret = NOTIFY_STOP;
			}

			break;
		case RISCV_DBTR_WATCHPOINT:
			if (info->address == csr_read(CSR_STVAL)) {
				pr_debug("%s: watchpoint fired: addr[0x%lx]\n",
					 __func__, info->address);
				perf_bp_event(bp, args->regs);
				ret = NOTIFY_STOP;
			}

			break;
		default:
			pr_warn("%s: unexpected breakpoint type: %u\n",
				__func__, info->type);
			break;
		}
	}

	return ret;
}

int hw_breakpoint_exceptions_notify(struct notifier_block *unused,
				    unsigned long val, void *data)
{
	if (val != DIE_DEBUG)
		return NOTIFY_DONE;

	return hw_breakpoint_handler(data);
}

/* atomic: counter->ctx->lock is held */
int arch_install_hw_breakpoint(struct perf_event *bp)
{
	struct arch_hw_breakpoint *info = counter_arch_bp(bp);
	struct sbi_dbtr_data_msg *xmit = this_cpu_ptr(sbi_xmit);
	struct sbi_dbtr_id_msg *recv = this_cpu_ptr(sbi_recv);
	struct perf_event **slot;
	unsigned long idx;
	struct sbiret ret;

	xmit->tdata1 = cpu_to_lle(info->trig_data1.value);
	xmit->tdata2 = cpu_to_lle(info->trig_data2);
	xmit->tdata3 = cpu_to_lle(info->trig_data3);

	ret = sbi_ecall(SBI_EXT_DBTR, SBI_EXT_DBTR_TRIGGER_INSTALL,
			1, __pa(xmit) >> 4, __pa(recv) >> 4,
			0, 0, 0);
	if (ret.error) {
		pr_warn("%s: failed to install trigger\n", __func__);
		return -EIO;
	}

	idx = lle_to_cpu(recv->idx);

	if (idx >= dbtr_total_num) {
		pr_warn("%s: invalid trigger index %lu\n", __func__, idx);
		return -EINVAL;
	}

	slot = this_cpu_ptr(&bp_per_reg[idx]);
	if (*slot) {
		pr_warn("%s: slot %lu is in use\n", __func__, idx);
		return -EBUSY;
	}

	*slot = bp;

	return 0;
}

/* atomic: counter->ctx->lock is held */
void arch_uninstall_hw_breakpoint(struct perf_event *bp)
{
	struct sbiret ret;
	int i;

	for (i = 0; i < dbtr_total_num; i++) {
		struct perf_event **slot = this_cpu_ptr(&bp_per_reg[i]);

		if (*slot == bp) {
			*slot = NULL;
			break;
		}
	}

	if (i == dbtr_total_num) {
		pr_warn("%s: unknown breakpoint\n", __func__);
		return;
	}

	ret = sbi_ecall(SBI_EXT_DBTR, SBI_EXT_DBTR_TRIGGER_UNINSTALL,
			i, 1, 0, 0, 0, 0);
	if (ret.error)
		pr_warn("%s: failed to uninstall trigger %d\n", __func__, i);
}

void hw_breakpoint_pmu_read(struct perf_event *bp)
{
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
	int i;
	struct thread_struct *t = &tsk->thread;

	for (i = 0; i < dbtr_total_num; i++) {
		unregister_hw_breakpoint(t->ptrace_bps[i]);
		t->ptrace_bps[i] = NULL;
	}
}

static int __init arch_hw_breakpoint_init(void)
{
	sbi_xmit = __alloc_percpu(sizeof(*sbi_xmit), SZ_16);
	if (!sbi_xmit) {
		pr_warn("failed to allocate SBI xmit message buffer\n");
		return -ENOMEM;
	}

	sbi_recv = __alloc_percpu(sizeof(*sbi_recv), SZ_16);
	if (!sbi_recv) {
		pr_warn("failed to allocate SBI recv message buffer\n");
		return -ENOMEM;
	}

	if (!dbtr_init)
		arch_hw_breakpoint_init_sbi();

	if (dbtr_total_num)
		pr_info("%s: total number of type %d triggers: %u\n",
			__func__, dbtr_type, dbtr_total_num);
	else
		pr_info("%s: no hardware triggers available\n", __func__);

	return 0;
}
arch_initcall(arch_hw_breakpoint_init);
