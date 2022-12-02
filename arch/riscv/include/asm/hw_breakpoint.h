/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __RISCV_HW_BREAKPOINT_H
#define __RISCV_HW_BREAKPOINT_H

struct task_struct;

#ifdef CONFIG_HAVE_HW_BREAKPOINT

#include <uapi/linux/hw_breakpoint.h>

#if __riscv_xlen == 64
#define cpu_to_lle cpu_to_le64
#define lle_to_cpu le64_to_cpu
#elif __riscv_xlen == 32
#define cpu_to_lle cpu_to_le32
#define lle_to_cpu le32_to_cpu
#else
#error "Unexpected __riscv_xlen"
#endif

enum {
	RISCV_DBTR_BREAKPOINT	= 0,
	RISCV_DBTR_WATCHPOINT	= 1,
};

enum {
	RISCV_DBTR_TRIG_NONE = 0,
	RISCV_DBTR_TRIG_LEGACY,
	RISCV_DBTR_TRIG_MCONTROL,
	RISCV_DBTR_TRIG_ICOUNT,
	RISCV_DBTR_TRIG_ITRIGGER,
	RISCV_DBTR_TRIG_ETRIGGER,
	RISCV_DBTR_TRIG_MCONTROL6,
};

union riscv_dbtr_tdata1 {
	unsigned long value;
	struct {
#if __riscv_xlen == 64
		unsigned long data:59;
#elif __riscv_xlen == 32
		unsigned long data:27;
#else
#error "Unexpected __riscv_xlen"
#endif
		unsigned long dmode:1;
		unsigned long type:4;
	};
};

union riscv_dbtr_tdata1_mcontrol {
	unsigned long value;
	struct {
		unsigned long load:1;
		unsigned long store:1;
		unsigned long execute:1;
		unsigned long u:1;
		unsigned long s:1;
		unsigned long _res2:1;
		unsigned long m:1;
		unsigned long match:4;
		unsigned long chain:1;
		unsigned long action:4;
		unsigned long sizelo:2;
		unsigned long timing:1;
		unsigned long select:1;
		unsigned long hit:1;
#if __riscv_xlen >= 64
		unsigned long sizehi:2;
		unsigned long _res1:30;
#endif
		unsigned long maskmax:6;
		unsigned long dmode:1;
		unsigned long type:4;
	};
};

union riscv_dbtr_tdata1_mcontrol6 {
	unsigned long value;
	struct {
		unsigned long load:1;
		unsigned long store:1;
		unsigned long execute:1;
		unsigned long u:1;
		unsigned long s:1;
		unsigned long _res2:1;
		unsigned long m:1;
		unsigned long match:4;
		unsigned long chain:1;
		unsigned long action:4;
		unsigned long size:4;
		unsigned long timing:1;
		unsigned long select:1;
		unsigned long hit:1;
		unsigned long vu:1;
		unsigned long vs:1;
#if __riscv_xlen == 64
		unsigned long _res1:34;
#elif __riscv_xlen == 32
		unsigned long _res1:2;
#else
#error "Unexpected __riscv_xlen"
#endif
		unsigned long dmode:1;
		unsigned long type:4;
	};
};

struct arch_hw_breakpoint {
	unsigned long address;
	unsigned long len;
	unsigned int type;

	union {
		unsigned long value;
		union riscv_dbtr_tdata1_mcontrol mcontrol;
		union riscv_dbtr_tdata1_mcontrol6 mcontrol6;
	} trig_data1;
	unsigned long trig_data2;
	unsigned long trig_data3;
};

/* Max supported HW breakpoints */
#define HBP_NUM_MAX 32

struct perf_event_attr;
struct notifier_block;
struct perf_event;
struct pt_regs;

int hw_breakpoint_slots(int type);
int arch_check_bp_in_kernelspace(struct arch_hw_breakpoint *hw);
int hw_breakpoint_arch_parse(struct perf_event *bp,
			     const struct perf_event_attr *attr,
			     struct arch_hw_breakpoint *hw);
int hw_breakpoint_exceptions_notify(struct notifier_block *unused,
				    unsigned long val, void *data);

void arch_enable_hw_breakpoint(struct perf_event *bp);
void arch_update_hw_breakpoint(struct perf_event *bp);
void arch_disable_hw_breakpoint(struct perf_event *bp);
int arch_install_hw_breakpoint(struct perf_event *bp);
void arch_uninstall_hw_breakpoint(struct perf_event *bp);
void hw_breakpoint_pmu_read(struct perf_event *bp);
void clear_ptrace_hw_breakpoint(struct task_struct *tsk);

#else

int hw_breakpoint_slots(int type)
{
	return 0;
}

static inline void clear_ptrace_hw_breakpoint(struct task_struct *tsk)
{
}

void arch_enable_hw_breakpoint(struct perf_event *bp)
{
}

void arch_update_hw_breakpoint(struct perf_event *bp)
{
}

void arch_disable_hw_breakpoint(struct perf_event *bp)
{
}

#endif /* CONFIG_HAVE_HW_BREAKPOINT */
#endif /* __RISCV_HW_BREAKPOINT_H */
