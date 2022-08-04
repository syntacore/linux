// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2010 Tilera Corporation. All Rights Reserved.
 * Copyright 2015 Regents of the University of California
 * Copyright 2017 SiFive
 *
 * Copied from arch/tile/kernel/ptrace.c
 */

#include <asm/vector.h>
#include <asm/ptrace.h>
#include <asm/syscall.h>
#include <asm/thread_info.h>
#include <asm/switch_to.h>
#include <linux/audit.h>
#include <linux/compat.h>
#include <linux/ptrace.h>
#include <linux/elf.h>
#include <linux/regset.h>
#include <linux/sched.h>
#include <linux/sched/task_stack.h>
#include <linux/hw_breakpoint.h>

enum riscv_regset {
	REGSET_X,
#ifdef CONFIG_FPU
	REGSET_F,
#endif
#ifdef CONFIG_RISCV_ISA_V
	REGSET_V,
#endif
#ifdef CONFIG_HAVE_HW_BREAKPOINT
	REGSET_HW_BREAK,
	REGSET_HW_WATCH,
#endif
};

static int riscv_gpr_get(struct task_struct *target,
			 const struct user_regset *regset,
			 struct membuf to)
{
	return membuf_write(&to, task_pt_regs(target),
			    sizeof(struct user_regs_struct));
}

static int riscv_gpr_set(struct task_struct *target,
			 const struct user_regset *regset,
			 unsigned int pos, unsigned int count,
			 const void *kbuf, const void __user *ubuf)
{
	struct pt_regs *regs;

	regs = task_pt_regs(target);
	return user_regset_copyin(&pos, &count, &kbuf, &ubuf, regs, 0, -1);
}

#ifdef CONFIG_FPU
static int riscv_fpr_get(struct task_struct *target,
			 const struct user_regset *regset,
			 struct membuf to)
{
	struct __riscv_d_ext_state *fstate = &target->thread.fstate;

	if (target == current)
		fstate_save(current, task_pt_regs(current));

	membuf_write(&to, fstate, offsetof(struct __riscv_d_ext_state, fcsr));
	membuf_store(&to, fstate->fcsr);
	return membuf_zero(&to, 4);	// explicitly pad
}

static int riscv_fpr_set(struct task_struct *target,
			 const struct user_regset *regset,
			 unsigned int pos, unsigned int count,
			 const void *kbuf, const void __user *ubuf)
{
	int ret;
	struct __riscv_d_ext_state *fstate = &target->thread.fstate;

	ret = user_regset_copyin(&pos, &count, &kbuf, &ubuf, fstate, 0,
				 offsetof(struct __riscv_d_ext_state, fcsr));
	if (!ret) {
		ret = user_regset_copyin(&pos, &count, &kbuf, &ubuf, fstate, 0,
					 offsetof(struct __riscv_d_ext_state, fcsr) +
					 sizeof(fstate->fcsr));
	}

	return ret;
}
#endif

#ifdef CONFIG_RISCV_ISA_V
static int riscv_vr_get(struct task_struct *target,
			const struct user_regset *regset,
			struct membuf to)
{
	struct __riscv_v_ext_state *vstate = &target->thread.vstate;
	struct __riscv_v_regset_state ptrace_vstate;

	if (!riscv_v_vstate_query(task_pt_regs(target)))
		return -EINVAL;

	/*
	 * Ensure the vector registers have been saved to the memory before
	 * copying them to membuf.
	 */
	if (target == current)
		riscv_v_vstate_save(current, task_pt_regs(current));

	ptrace_vstate.vstart = vstate->vstart;
	ptrace_vstate.vl = vstate->vl;
	ptrace_vstate.vtype = vstate->vtype;
	ptrace_vstate.vcsr = vstate->vcsr;
	ptrace_vstate.vlenb = vstate->vlenb;

	/* Copy vector header from vstate. */
	membuf_write(&to, &ptrace_vstate, sizeof(struct __riscv_v_regset_state));

	/* Copy all the vector registers from vstate. */
	return membuf_write(&to, vstate->datap, riscv_v_vsize);
}

static int riscv_vr_set(struct task_struct *target,
			const struct user_regset *regset,
			unsigned int pos, unsigned int count,
			const void *kbuf, const void __user *ubuf)
{
	int ret;
	struct __riscv_v_ext_state *vstate = &target->thread.vstate;
	struct __riscv_v_regset_state ptrace_vstate;

	if (!riscv_v_vstate_query(task_pt_regs(target)))
		return -EINVAL;

	/* Copy rest of the vstate except datap */
	ret = user_regset_copyin(&pos, &count, &kbuf, &ubuf, &ptrace_vstate, 0,
				 sizeof(struct __riscv_v_regset_state));
	if (unlikely(ret))
		return ret;

	if (vstate->vlenb != ptrace_vstate.vlenb)
		return -EINVAL;

	vstate->vstart = ptrace_vstate.vstart;
	vstate->vl = ptrace_vstate.vl;
	vstate->vtype = ptrace_vstate.vtype;
	vstate->vcsr = ptrace_vstate.vcsr;

	/* Copy all the vector registers. */
	pos = 0;
	ret = user_regset_copyin(&pos, &count, &kbuf, &ubuf, vstate->datap,
				 0, riscv_v_vsize);
	return ret;
}
#endif

#ifdef CONFIG_HAVE_HW_BREAKPOINT
static void ptrace_hbptriggered(struct perf_event *bp,
				struct perf_sample_data *data,
				struct pt_regs *regs)
{
	struct arch_hw_breakpoint *bkpt = counter_arch_bp(bp);

	force_sig_fault(SIGTRAP, TRAP_HWBKPT, (void __user *)bkpt->address);
}

static int hw_break_get(struct task_struct *target,
			const struct user_regset *regset,
			struct membuf to)
{
	/* send total number of h/w debug triggers */
	u64 count = hw_breakpoint_slots(regset->core_note_type);

	membuf_write(&to, &count, sizeof(count));
	return 0;
}

static inline int hw_break_empty(u64 addr, u64 type, u64 size)
{
	/* TODO: for now adjusted to current riscv-gdb behavior */
	return (!addr && !size);
}

static int hw_break_setup_trigger(struct task_struct *target, u64 addr,
				  u64 type, u64 size, int idx)
{
	struct perf_event *bp = ERR_PTR(-EINVAL);
	int count = hw_breakpoint_slots(type);
	struct perf_event_attr attr;
	u32 bp_type;
	u64 bp_len;

	pr_debug("%s: pid %d, addr 0x%llx, type %llu, size %llu, idx %d\n",
		 __func__, target->pid, addr, type, size, idx);

	if (idx + 1 > count) {
		pr_warn("%s: exceeded available triggers: idx %d\n",
			__func__, idx);
		return -EINVAL;
	}

	if (hw_break_empty(addr, type, size))
		pr_debug("%s: requested empty trigger\n", __func__);

	if (!hw_break_empty(addr, type, size)) {
		/* bp size: gdb to kernel */
		switch (size) {
		case 1:
			bp_len = HW_BREAKPOINT_LEN_1;
			break;
		case 2:
			bp_len = HW_BREAKPOINT_LEN_2;
			break;
		case 4:
			bp_len = HW_BREAKPOINT_LEN_4;
			break;
		case 8:
			bp_len = HW_BREAKPOINT_LEN_8;
			break;
		default:
			pr_warn("%s: unsupported size: %llu\n", __func__, size);
			return -EINVAL;
		}

		/* bp type: gdb to kernel */
		switch (type) {
		case 0:
			bp_type = HW_BREAKPOINT_X;
			break;
		case 1:
			bp_type = HW_BREAKPOINT_R;
			break;
		case 2:
			bp_type = HW_BREAKPOINT_W;
			break;
		case 3:
			bp_type = HW_BREAKPOINT_RW;
			break;
		default:
			pr_warn("%s: unsupported type: %llu\n", __func__, type);
			return -EINVAL;
		}
	}

	bp = target->thread.ptrace_bps[idx];
	if (bp) {
		attr = bp->attr;

		if (hw_break_empty(addr, type, size)) {
			attr.disabled = 1;
		} else {
			attr.bp_type = bp_type;
			attr.bp_addr = addr;
			attr.bp_len = bp_len;
			attr.disabled = 0;
		}

		return modify_user_hw_breakpoint(bp, &attr);
	}

	if (hw_break_empty(addr, type, size))
		return 0;

	ptrace_breakpoint_init(&attr);
	attr.bp_type = bp_type;
	attr.bp_addr = addr;
	attr.bp_len = bp_len;

	bp = register_user_hw_breakpoint(&attr, ptrace_hbptriggered,
					 NULL, target);
	if (IS_ERR(bp))
		return PTR_ERR(bp);

	target->thread.ptrace_bps[idx] = bp;
	return 0;
}

static int hw_break_set(struct task_struct *target,
			const struct user_regset *regset,
			unsigned int pos, unsigned int count,
			const void *kbuf, const void __user *ubuf)
{
	int ret, idx = 0, offset, limit;
	u64 addr;
	u64 type;
	u64 size;

#define PTRACE_HBP_ADDR_SZ	sizeof(u64)
#define PTRACE_HBP_TYPE_SZ	sizeof(u64)
#define PTRACE_HBP_SIZE_SZ	sizeof(u64)

	/* Resource info and pad */
	offset = offsetof(struct user_hwdebug_state, dbg_regs);
	user_regset_copyin_ignore(&pos, &count, &kbuf, &ubuf, 0, offset);

	/* trigger settings */
	limit = regset->n * regset->size;
	while (count && offset < limit) {
		if (count < PTRACE_HBP_ADDR_SZ)
			return -EINVAL;
		ret = user_regset_copyin(&pos, &count, &kbuf, &ubuf, &addr,
					 offset, offset + PTRACE_HBP_ADDR_SZ);
		if (ret)
			return ret;

		offset += PTRACE_HBP_ADDR_SZ;

		if (!count)
			break;
		ret = user_regset_copyin(&pos, &count, &kbuf, &ubuf, &type,
					 offset, offset + PTRACE_HBP_TYPE_SZ);
		if (ret)
			return ret;

		offset += PTRACE_HBP_TYPE_SZ;

		ret = user_regset_copyin(&pos, &count, &kbuf, &ubuf, &size,
					 offset, offset + PTRACE_HBP_SIZE_SZ);
		if (ret)
			return ret;

		offset += PTRACE_HBP_SIZE_SZ;

		ret = hw_break_setup_trigger(target, addr, type, size, idx);
		if (ret)
			return ret;

		idx++;
	}

	return 0;
}
#endif

static const struct user_regset riscv_user_regset[] = {
	[REGSET_X] = {
		.core_note_type = NT_PRSTATUS,
		.n = ELF_NGREG,
		.size = sizeof(elf_greg_t),
		.align = sizeof(elf_greg_t),
		.regset_get = riscv_gpr_get,
		.set = riscv_gpr_set,
	},
#ifdef CONFIG_FPU
	[REGSET_F] = {
		.core_note_type = NT_PRFPREG,
		.n = ELF_NFPREG,
		.size = sizeof(elf_fpreg_t),
		.align = sizeof(elf_fpreg_t),
		.regset_get = riscv_fpr_get,
		.set = riscv_fpr_set,
	},
#endif
#ifdef CONFIG_RISCV_ISA_V
	[REGSET_V] = {
		.core_note_type = NT_RISCV_VECTOR,
		.align = 16,
		.n = ((32 * RISCV_MAX_VLENB) +
		      sizeof(struct __riscv_v_regset_state)) / sizeof(__u32),
		.size = sizeof(__u32),
		.regset_get = riscv_vr_get,
		.set = riscv_vr_set,
	},
#endif
#ifdef CONFIG_HAVE_HW_BREAKPOINT
	[REGSET_HW_BREAK] = {
		.core_note_type = NT_ARM_HW_BREAK,
		.n = sizeof(struct user_hwdebug_state) / sizeof(u32),
		.size = sizeof(u32),
		.align = sizeof(u32),
		.regset_get = hw_break_get,
		.set = hw_break_set,
	},
	[REGSET_HW_WATCH] = {
		.core_note_type = NT_ARM_HW_WATCH,
		.n = sizeof(struct user_hwdebug_state) / sizeof(u32),
		.size = sizeof(u32),
		.align = sizeof(u32),
		.regset_get = hw_break_get,
		.set = hw_break_set,
	},
#endif
};

static const struct user_regset_view riscv_user_native_view = {
	.name = "riscv",
	.e_machine = EM_RISCV,
	.regsets = riscv_user_regset,
	.n = ARRAY_SIZE(riscv_user_regset),
};

struct pt_regs_offset {
	const char *name;
	int offset;
};

#define REG_OFFSET_NAME(r) {.name = #r, .offset = offsetof(struct pt_regs, r)}
#define REG_OFFSET_END {.name = NULL, .offset = 0}

static const struct pt_regs_offset regoffset_table[] = {
	REG_OFFSET_NAME(epc),
	REG_OFFSET_NAME(ra),
	REG_OFFSET_NAME(sp),
	REG_OFFSET_NAME(gp),
	REG_OFFSET_NAME(tp),
	REG_OFFSET_NAME(t0),
	REG_OFFSET_NAME(t1),
	REG_OFFSET_NAME(t2),
	REG_OFFSET_NAME(s0),
	REG_OFFSET_NAME(s1),
	REG_OFFSET_NAME(a0),
	REG_OFFSET_NAME(a1),
	REG_OFFSET_NAME(a2),
	REG_OFFSET_NAME(a3),
	REG_OFFSET_NAME(a4),
	REG_OFFSET_NAME(a5),
	REG_OFFSET_NAME(a6),
	REG_OFFSET_NAME(a7),
	REG_OFFSET_NAME(s2),
	REG_OFFSET_NAME(s3),
	REG_OFFSET_NAME(s4),
	REG_OFFSET_NAME(s5),
	REG_OFFSET_NAME(s6),
	REG_OFFSET_NAME(s7),
	REG_OFFSET_NAME(s8),
	REG_OFFSET_NAME(s9),
	REG_OFFSET_NAME(s10),
	REG_OFFSET_NAME(s11),
	REG_OFFSET_NAME(t3),
	REG_OFFSET_NAME(t4),
	REG_OFFSET_NAME(t5),
	REG_OFFSET_NAME(t6),
	REG_OFFSET_NAME(status),
	REG_OFFSET_NAME(badaddr),
	REG_OFFSET_NAME(cause),
	REG_OFFSET_NAME(orig_a0),
	REG_OFFSET_END,
};

/**
 * regs_query_register_offset() - query register offset from its name
 * @name:	the name of a register
 *
 * regs_query_register_offset() returns the offset of a register in struct
 * pt_regs from its name. If the name is invalid, this returns -EINVAL;
 */
int regs_query_register_offset(const char *name)
{
	const struct pt_regs_offset *roff;

	for (roff = regoffset_table; roff->name != NULL; roff++)
		if (!strcmp(roff->name, name))
			return roff->offset;
	return -EINVAL;
}

/**
 * regs_within_kernel_stack() - check the address in the stack
 * @regs:      pt_regs which contains kernel stack pointer.
 * @addr:      address which is checked.
 *
 * regs_within_kernel_stack() checks @addr is within the kernel stack page(s).
 * If @addr is within the kernel stack, it returns true. If not, returns false.
 */
static bool regs_within_kernel_stack(struct pt_regs *regs, unsigned long addr)
{
	return (addr & ~(THREAD_SIZE - 1))  ==
		(kernel_stack_pointer(regs) & ~(THREAD_SIZE - 1));
}

/**
 * regs_get_kernel_stack_nth() - get Nth entry of the stack
 * @regs:	pt_regs which contains kernel stack pointer.
 * @n:		stack entry number.
 *
 * regs_get_kernel_stack_nth() returns @n th entry of the kernel stack which
 * is specified by @regs. If the @n th entry is NOT in the kernel stack,
 * this returns 0.
 */
unsigned long regs_get_kernel_stack_nth(struct pt_regs *regs, unsigned int n)
{
	unsigned long *addr = (unsigned long *)kernel_stack_pointer(regs);

	addr += n;
	if (regs_within_kernel_stack(regs, (unsigned long)addr))
		return *addr;
	else
		return 0;
}

void ptrace_disable(struct task_struct *child)
{
}

long arch_ptrace(struct task_struct *child, long request,
		 unsigned long addr, unsigned long data)
{
	long ret = -EIO;

	switch (request) {
	default:
		ret = ptrace_request(child, request, addr, data);
		break;
	}

	return ret;
}

#ifdef CONFIG_COMPAT
static int compat_riscv_gpr_get(struct task_struct *target,
				const struct user_regset *regset,
				struct membuf to)
{
	struct compat_user_regs_struct cregs;

	regs_to_cregs(&cregs, task_pt_regs(target));

	return membuf_write(&to, &cregs,
			    sizeof(struct compat_user_regs_struct));
}

static int compat_riscv_gpr_set(struct task_struct *target,
				const struct user_regset *regset,
				unsigned int pos, unsigned int count,
				const void *kbuf, const void __user *ubuf)
{
	int ret;
	struct compat_user_regs_struct cregs;

	ret = user_regset_copyin(&pos, &count, &kbuf, &ubuf, &cregs, 0, -1);

	cregs_to_regs(&cregs, task_pt_regs(target));

	return ret;
}

static const struct user_regset compat_riscv_user_regset[] = {
	[REGSET_X] = {
		.core_note_type = NT_PRSTATUS,
		.n = ELF_NGREG,
		.size = sizeof(compat_elf_greg_t),
		.align = sizeof(compat_elf_greg_t),
		.regset_get = compat_riscv_gpr_get,
		.set = compat_riscv_gpr_set,
	},
#ifdef CONFIG_FPU
	[REGSET_F] = {
		.core_note_type = NT_PRFPREG,
		.n = ELF_NFPREG,
		.size = sizeof(elf_fpreg_t),
		.align = sizeof(elf_fpreg_t),
		.regset_get = riscv_fpr_get,
		.set = riscv_fpr_set,
	},
#endif
};

static const struct user_regset_view compat_riscv_user_native_view = {
	.name = "riscv",
	.e_machine = EM_RISCV,
	.regsets = compat_riscv_user_regset,
	.n = ARRAY_SIZE(compat_riscv_user_regset),
};

long compat_arch_ptrace(struct task_struct *child, compat_long_t request,
			compat_ulong_t caddr, compat_ulong_t cdata)
{
	long ret = -EIO;

	switch (request) {
	default:
		ret = compat_ptrace_request(child, request, caddr, cdata);
		break;
	}

	return ret;
}
#endif /* CONFIG_COMPAT */

const struct user_regset_view *task_user_regset_view(struct task_struct *task)
{
#ifdef CONFIG_COMPAT
	if (test_tsk_thread_flag(task, TIF_32BIT))
		return &compat_riscv_user_native_view;
	else
#endif
		return &riscv_user_native_view;
}
