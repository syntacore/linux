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
	if (target == current) {
		get_cpu_vector_context();
		riscv_v_vstate_save(&current->thread.vstate, task_pt_regs(current));
		put_cpu_vector_context();
	}

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
	pr_debug("%s: hwbp triggered at %lx\n", __func__, regs->badaddr);
	force_sig_fault(SIGTRAP, TRAP_HWBKPT, (void __user *)regs->badaddr);
}

static inline int hw_break_empty(struct arch_hw_breakpoint const *bp)
{
	unsigned long tdata1 = bp->chain->regs->tdata1;
	/* TODO: for now adjusted to current riscv-gdb behavior */
	return tdata1 == 0 || RV_DBTR_GET_TYPE(tdata1) == 15;
}

static int hw_break_get(struct task_struct *target,
			const struct user_regset *regset,
			struct membuf to)
{
	pr_debug("%s: membuf size is %d\n", __func__,
		 membuf_write(&to, NULL, 0));

	/* send total number of h/w debug triggers */
	u64 count = hw_breakpoint_slots(regset->core_note_type);
	struct perf_event *bp;
	struct hw_breakpoint_chain *hw;
	unsigned long total = 0;

	membuf_write(&to, &count, sizeof(count));

	for (unsigned long i = 0; i < count; ++i) {
		bp = target->thread.ptrace_bps[i];
		if (!bp) {
			pr_debug("%s: skipping trigger because %luth trigger is NULL\n",
				 __func__, i);
			continue;
		}

		if (hw_break_empty(&bp->hw.info)) {
			pr_debug("%s: zeroing trigger because %luth trigger is empty\n",
				 __func__, i);
			membuf_zero(&to, sizeof(struct hw_breakpoint_regs));
			++total;
			continue;
		}

		hw = bp->hw.info.chain;
		membuf_write(&to, hw->regs, sizeof(struct hw_breakpoint_regs) * hw->len);
		total += hw->len;
	}

	pr_debug("%s: total written triggers are %lu\n", __func__, total);
	membuf_zero(&to, sizeof(struct hw_breakpoint_regs) * (HW_BP_NUM_MAX - total));

	total = 0;
	for (unsigned long i = 0; i < count; ++i) {
		bp = target->thread.ptrace_bps[i];
		if (!bp) {
			pr_debug("%s: skipping hitbit because %luth trigger is NULL\n",
				 __func__, i);
			continue;
		}

		if (hw_break_empty(&bp->hw.info)) {
			pr_debug("%s: zeroing hitbit because %luth trigger is empty\n",
				 __func__, i);
			membuf_zero(&to, sizeof(unsigned long));
			++total;
			continue;
		}

		hw = bp->hw.info.chain;
		pr_debug("%s: fires count for %luth trigger is %lu",
			 __func__, i, hw->fires);

		for (unsigned long j = 0; j < hw->len; ++j)
			membuf_write(&to, &hw->fires, sizeof(unsigned long));

		total += hw->len;
	}

	pr_debug("%s: total written hitbits are %lu\n", __func__, total);
	membuf_zero(&to, sizeof(unsigned long) * (HW_BP_NUM_MAX - total));

	return 0;
}

static int hw_breakpoint_chain_bit(unsigned long tdata1)
{
	switch (RV_DBTR_GET_TYPE(tdata1)) {
	case RV_DBTR_TRIG_MCONTROL:
		return RV_DBTR_GET_MC_CHAIN(tdata1);

	case RV_DBTR_TRIG_MCONTROL6:
		return RV_DBTR_GET_MC6_CHAIN(tdata1);
	}

	return 0;
}

static u32 hw_break_parse_type(struct hw_breakpoint_regs const *regs)
{
	u32 type = 0;
	unsigned long tdata1 = regs->tdata1;

	switch (RV_DBTR_GET_TYPE(tdata1)) {
	case RV_DBTR_TRIG_MCONTROL:
		type |= RV_DBTR_GET_MC_EXEC(tdata1) ? HW_BREAKPOINT_X : 0;
		type |= RV_DBTR_GET_MC_LOAD(tdata1) ? HW_BREAKPOINT_R : 0;
		type |= RV_DBTR_GET_MC_STORE(tdata1) ? HW_BREAKPOINT_W : 0;
		break;

	case RV_DBTR_TRIG_MCONTROL6:
		type |= RV_DBTR_GET_MC6_EXEC(tdata1) ? HW_BREAKPOINT_X : 0;
		type |= RV_DBTR_GET_MC6_LOAD(tdata1) ? HW_BREAKPOINT_R : 0;
		type |= RV_DBTR_GET_MC6_STORE(tdata1) ? HW_BREAKPOINT_W : 0;
		break;

	default:
		type |= HW_BREAKPOINT_INVALID;
	}

	return type;
}

static int
hw_breakpoint_alloc_chain(struct hw_breakpoint_regs const *regs,
			  struct hw_breakpoint_regs const *regs_end,
			  struct hw_breakpoint_regs const **pnext_regs,
			  struct hw_breakpoint_chain **pchain)
{
	struct hw_breakpoint_regs const *itr;
	unsigned long len, size;

	for (itr = regs; itr != regs_end && hw_breakpoint_chain_bit(itr->tdata1); ++itr)
		;
	if (itr == regs_end)
		return -EINVAL;

	len = itr - regs + 1;
	size = len * sizeof(struct hw_breakpoint_regs);
	struct hw_breakpoint_chain *chain =
		kmalloc(offsetof(struct hw_breakpoint_chain, regs) + size, GFP_KERNEL);
	if (!chain)
		return -ENOMEM;

	memcpy(chain->regs, regs, size);
	chain->len = len;
	chain->fires = 0;

	*pnext_regs = itr + 1;
	*pchain = chain;
	return 0;
}

static void hw_breakpoint_free_chain(struct hw_breakpoint_chain *chain)
{
	kfree(chain);
}

static int hw_breakpoint_verify_chain(struct hw_breakpoint_chain const *chain)
{
	// checks bits validity
	// checks that addresses are not in kernel space
	return 0; // an optimistic one!
}

static int hw_break_probe_triggers(struct arch_hw_breakpoint *bps, int count)
{
	// TODO: validate address and mode of all breakpoints
	return arch_probe_install_hw_breakpoints(bps, count);
}

static int hw_break_setup_trigger(struct task_struct *target,
				  struct arch_hw_breakpoint const *hwbp, int idx)
{
	struct perf_event *bp = ERR_PTR(-EINVAL);
	struct perf_event_attr attr;
	u32 bp_type = HW_BREAKPOINT_EMPTY;

	if (!hw_break_empty(hwbp)) {
		bp_type = hw_break_parse_type(hwbp->chain->regs);
		if (bp_type == HW_BREAKPOINT_INVALID)
			return -EINVAL;
	}

	bp = target->thread.ptrace_bps[idx];
	if (bp) {
		attr = bp->attr;

		if (bp_type == HW_BREAKPOINT_EMPTY) {
			// FIXME in SYSSW-40306: Register empty trigger to keep offset and order of
			// all triggers during installation
			attr.bp_type = HW_BREAKPOINT_RW; // does not matter
			attr.bp_addr = 0xB16B00B5;
			attr.bp_len = 0xFACEFEED;
			attr.disabled = 0;
		} else {
			attr.bp_type = bp_type;
			// hw breakpoint implementation in kernel/events/hw_breakpoint.c
			// does not worry about bp_len & bp_addr. For some kind of chain
			// triggers (like NAPOT) it is impossible to determine address and
			// length, so I pray that this variables will be unclaimed, but
			// stub them with specific values due to find out error reason rapid.
			attr.bp_addr = 0xB16B00B5;
			attr.bp_len = 0xFACEFEED;
			attr.disabled = 0;
		}

		int ret =  modify_user_hw_breakpoint(bp, &attr);

		if (!ret) {
			hw_breakpoint_free_chain(bp->hw.info.chain);
			bp->hw.info = *hwbp;
		}

		return ret;
	}

	/* FIXME
	if (bp_type == HW_BREAKPOINT_EMPTY)
		return 0;
	*/

	ptrace_breakpoint_init(&attr);
	attr.bp_type = bp_type == HW_BREAKPOINT_EMPTY ? HW_BREAKPOINT_RW : bp_type;
	attr.bp_addr = 0xB16B00B5;
	attr.bp_len = 0xFACEFEED;

	bp = register_user_hw_breakpoint(&attr, ptrace_hbptriggered,
					 NULL, target);
	if (IS_ERR(bp))
		return PTR_ERR(bp);

	target->thread.ptrace_bps[idx] = bp;
	target->thread.ptrace_bps[idx]->hw.info = *hwbp;

	return 0;
}
static int hw_break_set(struct task_struct *target,
			const struct user_regset *regset,
			unsigned int pos, unsigned int count,
			const void *kbuf, const void __user *ubuf)
{
	int ret, trigs_read = 0, bps_read = 0, offset, limit, i;

	struct hw_breakpoint_regs regs[HW_BP_NUM_MAX];
	struct hw_breakpoint_regs const *cur_regs;
	struct hw_breakpoint_regs const *next_regs;
	struct arch_hw_breakpoint bps[HW_BP_NUM_MAX];

	unsigned long tdata1;
	unsigned long tdata2;
	unsigned long tdata3;

#define PTRACE_HBP_SIZE_TDATA sizeof(unsigned long)

	pr_debug("%s: triggers parsing started\n", __func__);

	/* Resource info and pad */
	offset = offsetof(struct user_hwdebug_state, dbg_regs);
	user_regset_copyin_ignore(&pos, &count, &kbuf, &ubuf, 0, offset);

	/* trigger settings */
	limit = regset->n * regset->size;
	while (count && offset < limit) {
		if (count < PTRACE_HBP_SIZE_TDATA)
			return -EINVAL;
		ret = user_regset_copyin(&pos, &count, &kbuf, &ubuf, &tdata1,
					 offset, offset + PTRACE_HBP_SIZE_TDATA);
		if (ret)
			return ret;

		offset += PTRACE_HBP_SIZE_TDATA;

		if (!count)
			break;
		ret = user_regset_copyin(&pos, &count, &kbuf, &ubuf, &tdata2,
					 offset, offset + PTRACE_HBP_SIZE_TDATA);
		if (ret)
			return ret;

		offset += PTRACE_HBP_SIZE_TDATA;

		ret = user_regset_copyin(&pos, &count, &kbuf, &ubuf, &tdata3,
					 offset, offset + PTRACE_HBP_SIZE_TDATA);
		if (ret)
			return ret;

		offset += PTRACE_HBP_SIZE_TDATA;

		regs[trigs_read].tdata1 = tdata1;
		regs[trigs_read].tdata2 = tdata2;
		regs[trigs_read].tdata3 = tdata3;

		pr_debug("%s: trig %d read, tdata1 = %lx, tdata2 = %lx\n",
			 __func__, trigs_read, tdata1, tdata2);

		trigs_read++;

		if (trigs_read > HW_BP_NUM_MAX)
			return -EINVAL;
	}

	pr_debug("%s: %d triggers read, start parsing...\n", __func__,
		 trigs_read);

	cur_regs = regs;
	while (cur_regs != regs + trigs_read) {
		ret = hw_breakpoint_alloc_chain(cur_regs, regs + trigs_read,
						&next_regs, &bps[bps_read].chain);
		if (ret)
			goto cleanup;

		pr_debug("%s: breakpoint %d parsed, chain_len = %lu\n",
			 __func__, bps_read, bps[bps_read].chain->len);

		ret = hw_breakpoint_verify_chain(bps[bps_read].chain);

		++bps_read;

		if (ret) {
			pr_debug("%s: breakpoint verification failed\n", __func__);
			goto cleanup;
		}

		cur_regs = next_regs;
	}

	pr_debug("%s: probe %d breakpoints...\n", __func__, bps_read);

	ret = hw_break_probe_triggers(bps, bps_read);
	if (ret) {
		pr_debug("%s: probe failed...\n", __func__);
		goto cleanup;
	}

	pr_debug("%s: probe sucessfull...\n", __func__);
	pr_debug("%s: install triggers...\n", __func__);

	for (i = 0; i < bps_read; ++i) {
		ret = hw_break_setup_trigger(target, bps + i, i);
		if (ret) {
			pr_debug("%s: install FAILED...\n", __func__);
			goto cleanup;
		}
	}

	pr_debug("%s: install sucessfull, we are realy cool!...\n", __func__);

	return 0;

cleanup:
	for (unsigned long i = 0; i < bps_read; ++i)
		hw_breakpoint_free_chain(bps[i].chain);

	return ret;
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
		.n = sizeof(struct user_hwdebug_state) / sizeof(u64),
		.size = sizeof(u64),
		.align = sizeof(u64),
		.regset_get = hw_break_get,
		.set = hw_break_set,
	},
	[REGSET_HW_WATCH] = {
		.core_note_type = NT_ARM_HW_WATCH,
		.n = sizeof(struct user_hwdebug_state) / sizeof(u64),
		.size = sizeof(u64),
		.align = sizeof(u64),
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
#else
static const struct user_regset_view compat_riscv_user_native_view = {};
#endif /* CONFIG_COMPAT */

const struct user_regset_view *task_user_regset_view(struct task_struct *task)
{
	if (is_compat_thread(&task->thread_info))
		return &compat_riscv_user_native_view;
	else
		return &riscv_user_native_view;
}
