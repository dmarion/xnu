/*
 * Copyright (c) 2000-2010 Apple Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 * 
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
* @OSF_COPYRIGHT@
*/
/* 
* Mach Operating System
* Copyright (c) 1991,1990,1989,1988 Carnegie Mellon University
* All Rights Reserved.
* 
* Permission to use, copy, modify and distribute this software and its
* documentation is hereby granted, provided that both the copyright
* notice and this permission notice appear in all copies of the
* software, derivative works or modified versions, and any portions
* thereof, and that both notices appear in supporting documentation.
* 
* CARNEGIE MELLON ALLOWS FREE USE OF THIS SOFTWARE IN ITS "AS IS"
* CONDITION.  CARNEGIE MELLON DISCLAIMS ANY LIABILITY OF ANY KIND FOR
* ANY DAMAGES WHATSOEVER RESULTING FROM THE USE OF THIS SOFTWARE.
* 
* Carnegie Mellon requests users of this software to return to
* 
*  Software Distribution Coordinator  or  Software.Distribution@CS.CMU.EDU
*  School of Computer Science
*  Carnegie Mellon University
*  Pittsburgh PA 15213-3890
* 
* any improvements or extensions that they make and grant Carnegie Mellon
* the rights to redistribute these changes.
*/
/*
*/

/*
* Hardware trap/fault handler.
 */

#include <mach_kdb.h>
#include <mach_kgdb.h>
#include <mach_kdp.h>
#include <mach_ldebug.h>

#include <types.h>
#include <i386/eflags.h>
#include <i386/trap.h>
#include <i386/pmap.h>
#include <i386/fpu.h>
#include <i386/misc_protos.h> /* panic_io_port_read() */
#include <i386/lapic.h>

#include <mach/exception.h>
#include <mach/kern_return.h>
#include <mach/vm_param.h>
#include <mach/i386/thread_status.h>

#include <vm/vm_kern.h>
#include <vm/vm_fault.h>

#include <kern/kern_types.h>
#include <kern/processor.h>
#include <kern/thread.h>
#include <kern/task.h>
#include <kern/sched.h>
#include <kern/sched_prim.h>
#include <kern/exception.h>
#include <kern/spl.h>
#include <kern/misc_protos.h>
#include <kern/debug.h>

#include <sys/kdebug.h>

#if	MACH_KGDB
#include <kgdb/kgdb_defs.h>
#endif	/* MACH_KGDB */

#if 	MACH_KDB
#include <debug.h>
#include <ddb/db_watch.h>
#include <ddb/db_run.h>
#include <ddb/db_break.h>
#include <ddb/db_trap.h>
#endif	/* MACH_KDB */

#include <string.h>

#include <i386/postcode.h>
#include <i386/mp_desc.h>
#include <i386/proc_reg.h>
#if CONFIG_MCA
#include <i386/machine_check.h>
#endif
#include <mach/i386/syscall_sw.h>

#include <libkern/OSDebug.h>

#include <machine/pal_routines.h>

extern void throttle_lowpri_io(int);
extern void kprint_state(x86_saved_state64_t *saved_state);

/*
 * Forward declarations
 */
static void user_page_fault_continue(kern_return_t kret);
#ifdef __i386__
static void panic_trap(x86_saved_state32_t *saved_state);
static void set_recovery_ip(x86_saved_state32_t *saved_state, vm_offset_t ip);
extern void panic_64(x86_saved_state_t *, int, const char *, boolean_t);
#else
static void panic_trap(x86_saved_state64_t *saved_state);
static void set_recovery_ip(x86_saved_state64_t *saved_state, vm_offset_t ip);
#endif

volatile perfCallback perfTrapHook = NULL; /* Pointer to CHUD trap hook routine */

#if CONFIG_DTRACE
/* See <rdar://problem/4613924> */
perfCallback tempDTraceTrapHook = NULL; /* Pointer to DTrace fbt trap hook routine */

extern boolean_t dtrace_tally_fault(user_addr_t);
#endif

extern boolean_t pmap_smep_enabled;

void
thread_syscall_return(
        kern_return_t ret)
{
        thread_t	thr_act = current_thread();
	boolean_t	is_mach;
	int		code;

	pal_register_cache_state(thr_act, DIRTY);

        if (thread_is_64bit(thr_act)) {
	        x86_saved_state64_t	*regs;
		
		regs = USER_REGS64(thr_act);

		code = (int) (regs->rax & SYSCALL_NUMBER_MASK);
		is_mach = (regs->rax & SYSCALL_CLASS_MASK)
			    == (SYSCALL_CLASS_MACH << SYSCALL_CLASS_SHIFT);
		if (kdebug_enable && is_mach) {
		        /* Mach trap */
		        KERNEL_DEBUG_CONSTANT(
			      MACHDBG_CODE(DBG_MACH_EXCP_SC,code)|DBG_FUNC_END,
			      ret, 0, 0, 0, 0);
		}
		regs->rax = ret;
#if DEBUG
		if (is_mach)
			DEBUG_KPRINT_SYSCALL_MACH(
				"thread_syscall_return: 64-bit mach ret=%u\n",
				ret);
		else
			DEBUG_KPRINT_SYSCALL_UNIX(
				"thread_syscall_return: 64-bit unix ret=%u\n",
				ret);
#endif
	} else {
	        x86_saved_state32_t	*regs;
		
		regs = USER_REGS32(thr_act);

		code = ((int) regs->eax);
		is_mach = (code < 0);
		if (kdebug_enable && is_mach) {
		        /* Mach trap */
		        KERNEL_DEBUG_CONSTANT(
			      MACHDBG_CODE(DBG_MACH_EXCP_SC,-code)|DBG_FUNC_END,
			      ret, 0, 0, 0, 0);
		}
		regs->eax = ret;
#if DEBUG
		if (is_mach)
			DEBUG_KPRINT_SYSCALL_MACH(
				"thread_syscall_return: 32-bit mach ret=%u\n",
				ret);
		else
			DEBUG_KPRINT_SYSCALL_UNIX(
				"thread_syscall_return: 32-bit unix ret=%u\n",
				ret);
#endif
	}
	throttle_lowpri_io(TRUE);

	thread_exception_return();
        /*NOTREACHED*/
}


#if	MACH_KDB
boolean_t	debug_all_traps_with_kdb = FALSE;
extern struct db_watchpoint *db_watchpoint_list;
extern boolean_t db_watchpoints_inserted;
extern boolean_t db_breakpoints_inserted;

void
thread_kdb_return(void)
{
	thread_t		thr_act = current_thread();
	x86_saved_state_t	*iss = USER_STATE(thr_act);

	pal_register_cache_state(thr_act, DIRTY);

        if (is_saved_state64(iss)) {
	        x86_saved_state64_t	*regs;
		
		regs = saved_state64(iss);

		if (kdb_trap(regs->isf.trapno, (int)regs->isf.err, (void *)regs)) {
		        thread_exception_return();
			/*NOTREACHED*/
		}

	} else {
	        x86_saved_state32_t	*regs;
		
		regs = saved_state32(iss);

		if (kdb_trap(regs->trapno, regs->err, (void *)regs)) {
		        thread_exception_return();
			/*NOTREACHED*/
		}
	}
}

#endif	/* MACH_KDB */

static inline void
user_page_fault_continue(
			 kern_return_t	kr)
{
	thread_t	thread = current_thread();
	user_addr_t	vaddr;

#if	MACH_KDB
	x86_saved_state_t *regs = USER_STATE(thread);
	int		err;
	int		trapno;

	assert((is_saved_state32(regs) && !thread_is_64bit(thread)) ||
	       (is_saved_state64(regs) &&  thread_is_64bit(thread)));
#endif

        if (thread_is_64bit(thread)) {
	        x86_saved_state64_t	*uregs;

		uregs = USER_REGS64(thread);

#if	MACH_KDB
		trapno = uregs->isf.trapno;
		err = (int)uregs->isf.err;
#endif
		vaddr = (user_addr_t)uregs->cr2;
	} else {
	        x86_saved_state32_t	*uregs;

		uregs = USER_REGS32(thread);

#if	MACH_KDB
		trapno = uregs->trapno;
		err = uregs->err;
#endif
		vaddr = uregs->cr2;
	}

	if (__probable((kr == KERN_SUCCESS) || (kr == KERN_ABORTED))) {
#if	MACH_KDB
		if (!db_breakpoints_inserted) {
			db_set_breakpoints();
		}
		if (db_watchpoint_list &&
		    db_watchpoints_inserted &&
		    (err & T_PF_WRITE) &&
		    db_find_watchpoint(thread->map,
				       (vm_offset_t)vaddr,
				       saved_state32(regs)))
			kdb_trap(T_WATCHPOINT, 0, saved_state32(regs));
#endif	/* MACH_KDB */
		thread_exception_return();
		/*NOTREACHED*/
	}

#if	MACH_KDB
	if (debug_all_traps_with_kdb &&
	    kdb_trap(trapno, err, saved_state32(regs))) {
		thread_exception_return();
		/*NOTREACHED*/
	}
#endif	/* MACH_KDB */

	/* PAL debug hook */
	pal_dbg_page_fault( thread, vaddr, kr );

	i386_exception(EXC_BAD_ACCESS, kr, vaddr);
	/*NOTREACHED*/
}

/*
 * Fault recovery in copyin/copyout routines.
 */
struct recovery {
	uintptr_t	fault_addr;
	uintptr_t	recover_addr;
};

extern struct recovery	recover_table[];
extern struct recovery	recover_table_end[];

const char *	trap_type[] = {TRAP_NAMES};
unsigned 	TRAP_TYPES = sizeof(trap_type)/sizeof(trap_type[0]);

extern void	PE_incoming_interrupt(int interrupt);

#if defined(__x86_64__) && DEBUG
void
kprint_state(x86_saved_state64_t	*saved_state)
{
	kprintf("current_cpu_datap() 0x%lx\n", (uintptr_t)current_cpu_datap());
	kprintf("Current GS base MSR 0x%llx\n", rdmsr64(MSR_IA32_GS_BASE));
	kprintf("Kernel  GS base MSR 0x%llx\n", rdmsr64(MSR_IA32_KERNEL_GS_BASE));
	kprintf("state at 0x%lx:\n", (uintptr_t) saved_state);

	kprintf("      rdi    0x%llx\n", saved_state->rdi);        
	kprintf("      rsi    0x%llx\n", saved_state->rsi);    
	kprintf("      rdx    0x%llx\n", saved_state->rdx);
	kprintf("      r10    0x%llx\n", saved_state->r10);
	kprintf("      r8     0x%llx\n", saved_state->r8);
	kprintf("      r9     0x%llx\n", saved_state->r9);     
	kprintf("      v_arg6 0x%llx\n", saved_state->v_arg6);
	kprintf("      v_arg7 0x%llx\n", saved_state->v_arg7);
	kprintf("      v_arg8 0x%llx\n", saved_state->v_arg8);

	kprintf("      cr2    0x%llx\n", saved_state->cr2);
	kprintf("real  cr2    0x%lx\n", get_cr2());
	kprintf("      r15    0x%llx\n", saved_state->r15);
	kprintf("      r14    0x%llx\n", saved_state->r14);
	kprintf("      r13    0x%llx\n", saved_state->r13);
	kprintf("      r12    0x%llx\n", saved_state->r12);
	kprintf("      r11    0x%llx\n", saved_state->r11);
	kprintf("      rbp    0x%llx\n", saved_state->rbp);
	kprintf("      rbx    0x%llx\n", saved_state->rbx);
	kprintf("      rcx    0x%llx\n", saved_state->rcx);
	kprintf("      rax    0x%llx\n", saved_state->rax);

	kprintf("      gs     0x%x\n", saved_state->gs);
	kprintf("      fs     0x%x\n", saved_state->fs);

	kprintf("  isf.trapno 0x%x\n", saved_state->isf.trapno);
	kprintf("  isf._pad   0x%x\n", saved_state->isf._pad);
	kprintf("  isf.trapfn 0x%llx\n", saved_state->isf.trapfn);
	kprintf("  isf.err    0x%llx\n", saved_state->isf.err);
	kprintf("  isf.rip    0x%llx\n", saved_state->isf.rip);
	kprintf("  isf.cs     0x%llx\n", saved_state->isf.cs);
	kprintf("  isf.rflags 0x%llx\n", saved_state->isf.rflags);
	kprintf("  isf.rsp    0x%llx\n", saved_state->isf.rsp);
	kprintf("  isf.ss     0x%llx\n", saved_state->isf.ss);
}
#endif


/*
 * Non-zero indicates latency assert is enabled and capped at valued
 * absolute time units.
 */
   
uint64_t interrupt_latency_cap = 0;
boolean_t ilat_assert = FALSE;

void
interrupt_latency_tracker_setup(void) {
	uint32_t ilat_cap_us;
	if (PE_parse_boot_argn("interrupt_latency_cap_us", &ilat_cap_us, sizeof(ilat_cap_us))) {
		interrupt_latency_cap = ilat_cap_us * NSEC_PER_USEC;
		nanoseconds_to_absolutetime(interrupt_latency_cap, &interrupt_latency_cap);
	} else {
		interrupt_latency_cap = LockTimeOut;
	}
	PE_parse_boot_argn("-interrupt_latency_assert_enable", &ilat_assert, sizeof(ilat_assert));
}

void interrupt_reset_latency_stats(void) {
	uint32_t i;
	for (i = 0; i < real_ncpus; i++) {
		cpu_data_ptr[i]->cpu_max_observed_int_latency =
		    cpu_data_ptr[i]->cpu_max_observed_int_latency_vector = 0;
	}
}

void interrupt_populate_latency_stats(char *buf, unsigned bufsize) {
	uint32_t i, tcpu = ~0;
	uint64_t cur_max = 0;

	for (i = 0; i < real_ncpus; i++) {
		if (cur_max < cpu_data_ptr[i]->cpu_max_observed_int_latency) {
			cur_max = cpu_data_ptr[i]->cpu_max_observed_int_latency;
			tcpu = i;
		}
	}

	if (tcpu < real_ncpus)
		snprintf(buf, bufsize, "0x%x 0x%x 0x%llx", tcpu, cpu_data_ptr[tcpu]->cpu_max_observed_int_latency_vector, cpu_data_ptr[tcpu]->cpu_max_observed_int_latency);
}

/*
 * Handle interrupts:
 *  - local APIC interrupts (IPIs, timers, etc) are handled by the kernel,
 *  - device interrupts go to the platform expert.
 */
void
interrupt(x86_saved_state_t *state)
{
	uint64_t	rip;
	uint64_t	rsp;
	int		interrupt_num;
	boolean_t	user_mode = FALSE;
	int		ipl;
	int		cnum = cpu_number();

	if (is_saved_state64(state) == TRUE) {
	        x86_saved_state64_t	*state64;

	        state64 = saved_state64(state);
		rip = state64->isf.rip;
		rsp = state64->isf.rsp;
		interrupt_num = state64->isf.trapno;
#ifdef __x86_64__
		if(state64->isf.cs & 0x03)
#endif
			user_mode = TRUE;
	} else {
		x86_saved_state32_t	*state32;

		state32 = saved_state32(state);
		if (state32->cs & 0x03)
			user_mode = TRUE;
		rip = state32->eip;
		rsp = state32->uesp;
		interrupt_num = state32->trapno;
	}

	KERNEL_DEBUG_CONSTANT(
		MACHDBG_CODE(DBG_MACH_EXCP_INTR, 0) | DBG_FUNC_START,
		interrupt_num, rip, user_mode, 0, 0);

	SCHED_STATS_INTERRUPT(current_processor());

	ipl = get_preemption_level();

	/*
	 * Handle local APIC interrupts
	 * else call platform expert for devices.
	 */
	if (!lapic_interrupt(interrupt_num, state))
		PE_incoming_interrupt(interrupt_num);

	if (__improbable(get_preemption_level() != ipl)) {
		panic("Preemption level altered by interrupt vector 0x%x: initial 0x%x, final: 0x%x\n", interrupt_num, ipl, get_preemption_level());
	}

	KERNEL_DEBUG_CONSTANT(
		MACHDBG_CODE(DBG_MACH_EXCP_INTR, 0) | DBG_FUNC_END,
		interrupt_num, 0, 0, 0, 0);

 	if (cpu_data_ptr[cnum]->cpu_nested_istack) {
 		cpu_data_ptr[cnum]->cpu_nested_istack_events++;
 	}
 	else  {
		uint64_t int_latency = mach_absolute_time() - cpu_data_ptr[cnum]->cpu_int_event_time;
		if (ilat_assert && (int_latency > interrupt_latency_cap) && !machine_timeout_suspended()) {
			panic("Interrupt vector 0x%x exceeded interrupt latency threshold, 0x%llx absolute time delta, prior signals: 0x%x, current signals: 0x%x", interrupt_num, int_latency, cpu_data_ptr[cnum]->cpu_prior_signals, cpu_data_ptr[cnum]->cpu_signals);
		}
		if (int_latency > cpu_data_ptr[cnum]->cpu_max_observed_int_latency) {
			cpu_data_ptr[cnum]->cpu_max_observed_int_latency = int_latency;
			cpu_data_ptr[cnum]->cpu_max_observed_int_latency_vector = interrupt_num;
		}
	}

	/*
	 * Having serviced the interrupt first, look at the interrupted stack depth.
	 */
	if (!user_mode) {
		uint64_t depth = cpu_data_ptr[cnum]->cpu_kernel_stack
				 + sizeof(struct x86_kernel_state)
				 + sizeof(struct i386_exception_link *)
				 - rsp;
		if (depth > kernel_stack_depth_max) {
			kernel_stack_depth_max = (vm_offset_t)depth;
			KERNEL_DEBUG_CONSTANT(
				MACHDBG_CODE(DBG_MACH_SCHED, MACH_STACK_DEPTH),
				(long) depth, (long) rip, 0, 0, 0);
		}
	}
}

static inline void
reset_dr7(void)
{
	long dr7 = 0x400; /* magic dr7 reset value; 32 bit on i386, 64 bit on x86_64 */
	__asm__ volatile("mov %0,%%dr7" : : "r" (dr7));
}
#if MACH_KDP
unsigned kdp_has_active_watchpoints = 0;
#define NO_WATCHPOINTS (!kdp_has_active_watchpoints)
#else
#define NO_WATCHPOINTS 1
#endif
/*
 * Trap from kernel mode.  Only page-fault errors are recoverable,
 * and then only in special circumstances.  All other errors are
 * fatal.  Return value indicates if trap was handled.
 */

void
kernel_trap(
	x86_saved_state_t	*state,
	uintptr_t *lo_spp)
{
#ifdef __i386__
	x86_saved_state32_t	*saved_state;
#else
	x86_saved_state64_t	*saved_state;
#endif
	int			code;
	user_addr_t		vaddr;
	int			type;
	vm_map_t		map = 0;	/* protected by T_PAGE_FAULT */
	kern_return_t		result = KERN_FAILURE;
	thread_t		thread;
	ast_t			*myast;
	boolean_t               intr;
	vm_prot_t		prot;
        struct recovery		*rp;
	vm_offset_t		kern_ip;
#if NCOPY_WINDOWS > 0
	int			fault_in_copy_window = -1;
#endif
	int			is_user = 0;
#if MACH_KDB
	pt_entry_t		*pte;
#endif /* MACH_KDB */
	
	thread = current_thread();

#ifdef __i386__
	if (__improbable(is_saved_state64(state))) {
		panic_64(state, 0, "Kernel trap with 64-bit state", FALSE);
	}

	saved_state = saved_state32(state);

	/* Record cpu where state was captured (trampolines don't set this) */
	saved_state->cpu = cpu_number();

	vaddr = (user_addr_t)saved_state->cr2;
	type  = saved_state->trapno;
	code  = saved_state->err & 0xffff;
	intr  = (saved_state->efl & EFL_IF) != 0;	/* state of ints at trap */
	kern_ip = (vm_offset_t)saved_state->eip;
#else
	if (__improbable(is_saved_state32(state)))
		panic("kernel_trap(%p) with 32-bit state", state);
	saved_state = saved_state64(state);

	/* Record cpu where state was captured */
	saved_state->isf.cpu = cpu_number();

	vaddr = (user_addr_t)saved_state->cr2;
	type  = saved_state->isf.trapno;
	code  = (int)(saved_state->isf.err & 0xffff);
	intr  = (saved_state->isf.rflags & EFL_IF) != 0;	/* state of ints at trap */
	kern_ip = (vm_offset_t)saved_state->isf.rip;
#endif

	myast = ast_pending();

	perfASTCallback astfn = perfASTHook;
	if (__improbable(astfn != NULL)) {
		if (*myast & AST_CHUD_ALL)
			astfn(AST_CHUD_ALL, myast);
	} else
		*myast &= ~AST_CHUD_ALL;

	/*
	 * Is there a hook?
	 */
	perfCallback fn = perfTrapHook;
	if (__improbable(fn != NULL)) {
	        if (fn(type, NULL, 0, 0) == KERN_SUCCESS) {
		        /*
			 * If it succeeds, we are done...
			 */
			return;
		}
	}

#if CONFIG_DTRACE
	if (__improbable(tempDTraceTrapHook != NULL)) {
		if (tempDTraceTrapHook(type, state, lo_spp, 0) == KERN_SUCCESS) {
			/*
			 * If it succeeds, we are done...
			 */
			return;
		}
	}
#endif /* CONFIG_DTRACE */

	/*
	 * we come here with interrupts off as we don't want to recurse
	 * on preemption below.  but we do want to re-enable interrupts
	 * as soon we possibly can to hold latency down
	 */
	if (__improbable(T_PREEMPT == type)) {
	        ast_taken(AST_PREEMPTION, FALSE);

		KERNEL_DEBUG_CONSTANT((MACHDBG_CODE(DBG_MACH_EXCP_KTRAP_x86, type)) | DBG_FUNC_NONE,
				      0, 0, 0, kern_ip, 0);
		return;
	}
	
	if (T_PAGE_FAULT == type) {
		/*
		 * assume we're faulting in the kernel map
		 */
		map = kernel_map;

		if (__probable(thread != THREAD_NULL && thread->map != kernel_map)) {
#if NCOPY_WINDOWS > 0
			vm_offset_t	copy_window_base;
			vm_offset_t	kvaddr;
			int		window_index;

			kvaddr = (vm_offset_t)vaddr;
			/*
			 * must determine if fault occurred in
			 * the copy window while pre-emption is
			 * disabled for this processor so that
			 * we only need to look at the window
			 * associated with this processor
			 */
			copy_window_base = current_cpu_datap()->cpu_copywindow_base;

			if (kvaddr >= copy_window_base && kvaddr < (copy_window_base + (NBPDE * NCOPY_WINDOWS)) ) {

				window_index = (int)((kvaddr - copy_window_base) / NBPDE);

				if (thread->machine.copy_window[window_index].user_base != (user_addr_t)-1) {

				        kvaddr -= (copy_window_base + (NBPDE * window_index));
				        vaddr = thread->machine.copy_window[window_index].user_base + kvaddr;

					map = thread->map;
					fault_in_copy_window = window_index;
				}
				is_user = -1;
			}
#else
			if (__probable(vaddr < VM_MAX_USER_PAGE_ADDRESS)) {
				/* fault occurred in userspace */
				map = thread->map;
				is_user = -1;

				/* Intercept a potential Supervisor Mode Execute
				 * Protection fault. These criteria identify
				 * both NX faults and SMEP faults, but both
				 * are fatal. We avoid checking PTEs (racy).
				 * (The VM could just redrive a SMEP fault, hence
				 * the intercept).
				 */
				if (__improbable((code == (T_PF_PROT | T_PF_EXECUTE)) && (pmap_smep_enabled) && (saved_state->isf.rip == vaddr))) {
					goto debugger_entry;
				}

				/*
				 * If we're not sharing cr3 with the user
				 * and we faulted in copyio,
				 * then switch cr3 here and dismiss the fault.
				 */
				if (no_shared_cr3 &&
				    (thread->machine.specFlags&CopyIOActive) &&
				    map->pmap->pm_cr3 != get_cr3_base()) {
					pmap_assert(current_cpu_datap()->cpu_pmap_pcid_enabled == FALSE);
					set_cr3_raw(map->pmap->pm_cr3);
					return;
				}
			}
#endif
		}
	}

	KERNEL_DEBUG_CONSTANT(
		(MACHDBG_CODE(DBG_MACH_EXCP_KTRAP_x86, type)) | DBG_FUNC_NONE,
		(unsigned)(vaddr >> 32), (unsigned)vaddr, is_user, kern_ip, 0);


	(void) ml_set_interrupts_enabled(intr);

	switch (type) {

	    case T_NO_FPU:
		fpnoextflt();
		return;

	    case T_FPU_FAULT:
		fpextovrflt();
		return;

	    case T_FLOATING_POINT_ERROR:
		fpexterrflt();
		return;

	    case T_SSE_FLOAT_ERROR:
	        fpSSEexterrflt();
		return;
 	    case T_DEBUG:
#ifdef __i386__
		    if ((saved_state->efl & EFL_TF) == 0 && NO_WATCHPOINTS)
#else
		    if ((saved_state->isf.rflags & EFL_TF) == 0 && NO_WATCHPOINTS)
#endif
		    {
			    /* We've somehow encountered a debug
			     * register match that does not belong
			     * to the kernel debugger.
			     * This isn't supposed to happen.
			     */
			    reset_dr7();
			    return;
		    }
		    goto debugger_entry;
#ifdef __x86_64__
	    case T_INT3:
	      goto debugger_entry;
#endif
	    case T_PAGE_FAULT:

#if	MACH_KDB
		/*
		 * Check for watchpoint on kernel static data.
		 * vm_fault would fail in this case 
		 */
		if (map == kernel_map && db_watchpoint_list && db_watchpoints_inserted &&
		    (code & T_PF_WRITE) && vaddr < vm_map_max(map) &&
		    ((*(pte = pmap_pte(kernel_pmap, (vm_map_offset_t)vaddr))) & INTEL_PTE_WRITE) == 0) {
			pmap_store_pte(
				pte,
				*pte | INTEL_PTE_VALID | INTEL_PTE_WRITE);
			/* XXX need invltlb here? */

			result = KERN_SUCCESS;
			goto look_for_watchpoints;
		}
#endif	/* MACH_KDB */

#if CONFIG_DTRACE
		if (thread != THREAD_NULL && thread->options & TH_OPT_DTRACE) {	/* Executing under dtrace_probe? */
			if (dtrace_tally_fault(vaddr)) { /* Should a fault under dtrace be ignored? */
				/*
				 * DTrace has "anticipated" the possibility of this fault, and has
				 * established the suitable recovery state. Drop down now into the
				 * recovery handling code in "case T_GENERAL_PROTECTION:". 
				 */
				goto FALL_THROUGH;
			}
		}
#endif /* CONFIG_DTRACE */

		
		prot = VM_PROT_READ;

		if (code & T_PF_WRITE)
		        prot |= VM_PROT_WRITE;
#if     PAE
		if (code & T_PF_EXECUTE)
		        prot |= VM_PROT_EXECUTE;
#endif

		result = vm_fault(map,
				  vm_map_trunc_page(vaddr),
				  prot,
				  FALSE, 
				  THREAD_UNINT, NULL, 0);

#if	MACH_KDB
		if (result == KERN_SUCCESS) {
		        /*
			 * Look for watchpoints
			 */
look_for_watchpoints:
		        if (map == kernel_map && db_watchpoint_list && db_watchpoints_inserted && (code & T_PF_WRITE) &&
			    db_find_watchpoint(map, vaddr, saved_state))
			        kdb_trap(T_WATCHPOINT, 0, saved_state);
		}
#endif	/* MACH_KDB */

		if (result == KERN_SUCCESS) {
#if NCOPY_WINDOWS > 0
			if (fault_in_copy_window != -1) {
				ml_set_interrupts_enabled(FALSE);
				copy_window_fault(thread, map,
						  fault_in_copy_window);
				(void) ml_set_interrupts_enabled(intr);
			}
#endif /* NCOPY_WINDOWS > 0 */
			return;
		}
		/*
		 * fall through
		 */
#if CONFIG_DTRACE
FALL_THROUGH:
#endif /* CONFIG_DTRACE */

	    case T_GENERAL_PROTECTION:
		/*
		 * If there is a failure recovery address
		 * for this fault, go there.
		 */
	        for (rp = recover_table; rp < recover_table_end; rp++) {
		        if (kern_ip == rp->fault_addr) {
			        set_recovery_ip(saved_state, rp->recover_addr);
				return;
			}
		}

		/*
		 * Check thread recovery address also.
		 */
		if (thread != THREAD_NULL && thread->recover) {
			set_recovery_ip(saved_state, thread->recover);
			thread->recover = 0;
			return;
		}
		/*
		 * Unanticipated page-fault errors in kernel
		 * should not happen.
		 *
		 * fall through...
		 */
	    default:
		/*
		 * Exception 15 is reserved but some chips may generate it
		 * spuriously. Seen at startup on AMD Athlon-64.
		 */
	    	if (type == 15) {
			kprintf("kernel_trap() ignoring spurious trap 15\n"); 
			return;
		}
debugger_entry:
		/* Ensure that the i386_kernel_state at the base of the
		 * current thread's stack (if any) is synchronized with the
		 * context at the moment of the trap, to facilitate
		 * access through the debugger.
		 */
		sync_iss_to_iks(state);
#if MACH_KDB
restart_debugger:
#endif /* MACH_KDB */		
#if  MACH_KDP
                if (current_debugger != KDB_CUR_DB) {
			if (kdp_i386_trap(type, saved_state, result, (vm_offset_t)vaddr))
				return;
		} else {
#endif /* MACH_KDP */
#if MACH_KDB
			if (kdb_trap(type, code, saved_state)) {
				if (switch_debugger) {
					current_debugger = KDP_CUR_DB;
					switch_debugger = 0;
					goto restart_debugger;
				}
				return;
			}
#endif /* MACH_KDB */
#if MACH_KDP
		}
#endif
	}
	__asm__ volatile("cli":::"cc");
	panic_trap(saved_state);
	/*
	 * NO RETURN
	 */
}


#ifdef __i386__
static void
set_recovery_ip(x86_saved_state32_t  *saved_state, vm_offset_t ip)
{
        saved_state->eip = ip;
}
#else
static void
set_recovery_ip(x86_saved_state64_t  *saved_state, vm_offset_t ip)
{
        saved_state->isf.rip = ip;
}
#endif


#ifdef __i386__
static void
panic_trap(x86_saved_state32_t *regs)
{
	const char *trapname = "Unknown";
	pal_cr_t	cr0, cr2, cr3, cr4;

	pal_get_control_registers( &cr0, &cr2, &cr3, &cr4 );

	/*
	 * Issue an I/O port read if one has been requested - this is an
	 * event logic analyzers can use as a trigger point.
	 */
	panic_io_port_read();

	kprintf("panic trap number 0x%x, eip 0x%x\n", regs->trapno, regs->eip);
	kprintf("cr0 0x%08x cr2 0x%08x cr3 0x%08x cr4 0x%08x\n",
		cr0, cr2, cr3, cr4);

	if (regs->trapno < TRAP_TYPES)
	        trapname = trap_type[regs->trapno];
#undef panic
	panic("Kernel trap at 0x%08x, type %d=%s, registers:\n"
	      "CR0: 0x%08x, CR2: 0x%08x, CR3: 0x%08x, CR4: 0x%08x\n"
	      "EAX: 0x%08x, EBX: 0x%08x, ECX: 0x%08x, EDX: 0x%08x\n"
	      "CR2: 0x%08x, EBP: 0x%08x, ESI: 0x%08x, EDI: 0x%08x\n"
	      "EFL: 0x%08x, EIP: 0x%08x, CS:  0x%08x, DS:  0x%08x\n"
	      "Error code: 0x%08x\n",
	      regs->eip, regs->trapno, trapname, cr0, cr2, cr3, cr4,
	      regs->eax,regs->ebx,regs->ecx,regs->edx,
	      regs->cr2,regs->ebp,regs->esi,regs->edi,
	      regs->efl,regs->eip,regs->cs & 0xFFFF, regs->ds & 0xFFFF, regs->err);
	/*
	 * This next statement is not executed,
	 * but it's needed to stop the compiler using tail call optimization
	 * for the panic call - which confuses the subsequent backtrace.
	 */
	cr0 = 0;
}
#else


static void
panic_trap(x86_saved_state64_t *regs)
{
	const char	*trapname = "Unknown";
	pal_cr_t	cr0, cr2, cr3, cr4;
	boolean_t	potential_smep_fault = FALSE;

	pal_get_control_registers( &cr0, &cr2, &cr3, &cr4 );
	assert(ml_get_interrupts_enabled() == FALSE);
	current_cpu_datap()->cpu_fatal_trap_state = regs;
	/*
	 * Issue an I/O port read if one has been requested - this is an
	 * event logic analyzers can use as a trigger point.
	 */
	panic_io_port_read();

	kprintf("panic trap number 0x%x, rip 0x%016llx\n",
		regs->isf.trapno, regs->isf.rip);
	kprintf("cr0 0x%016llx cr2 0x%016llx cr3 0x%016llx cr4 0x%016llx\n",
		cr0, cr2, cr3, cr4);

	if (regs->isf.trapno < TRAP_TYPES)
	        trapname = trap_type[regs->isf.trapno];

	if ((regs->isf.trapno == T_PAGE_FAULT) && (regs->isf.err == (T_PF_PROT | T_PF_EXECUTE)) && (pmap_smep_enabled) && (regs->isf.rip == regs->cr2) && (regs->isf.rip < VM_MAX_USER_PAGE_ADDRESS)) {
		potential_smep_fault = TRUE;
	}

#undef panic
	panic("Kernel trap at 0x%016llx, type %d=%s, registers:\n"
	      "CR0: 0x%016llx, CR2: 0x%016llx, CR3: 0x%016llx, CR4: 0x%016llx\n"
	      "RAX: 0x%016llx, RBX: 0x%016llx, RCX: 0x%016llx, RDX: 0x%016llx\n"
	      "RSP: 0x%016llx, RBP: 0x%016llx, RSI: 0x%016llx, RDI: 0x%016llx\n"
	      "R8:  0x%016llx, R9:  0x%016llx, R10: 0x%016llx, R11: 0x%016llx\n"
	      "R12: 0x%016llx, R13: 0x%016llx, R14: 0x%016llx, R15: 0x%016llx\n"
	      "RFL: 0x%016llx, RIP: 0x%016llx, CS:  0x%016llx, SS:  0x%016llx\n"
	      "CR2: 0x%016llx, Error code: 0x%016llx, Faulting CPU: 0x%x%s\n",
	      regs->isf.rip, regs->isf.trapno, trapname,
	      cr0, cr2, cr3, cr4,
	      regs->rax, regs->rbx, regs->rcx, regs->rdx,
	      regs->isf.rsp, regs->rbp, regs->rsi, regs->rdi,
	      regs->r8,  regs->r9,  regs->r10, regs->r11,
	      regs->r12, regs->r13, regs->r14, regs->r15,
	      regs->isf.rflags, regs->isf.rip, regs->isf.cs & 0xFFFF,
	      regs->isf.ss & 0xFFFF,regs->cr2, regs->isf.err, regs->isf.cpu,
	      potential_smep_fault ? " SMEP/NX fault" : "");
	/*
	 * This next statement is not executed,
	 * but it's needed to stop the compiler using tail call optimization
	 * for the panic call - which confuses the subsequent backtrace.
	 */
	cr0 = 0;
}
#endif

#if CONFIG_DTRACE
extern kern_return_t dtrace_user_probe(x86_saved_state_t *);
#endif

/*
 *	Trap from user mode.
 */
void
user_trap(
	x86_saved_state_t *saved_state)
{
	int			exc;
	int			err;
	mach_exception_code_t 	code;
	mach_exception_subcode_t subcode;
	int			type;
	user_addr_t		vaddr;
	vm_prot_t		prot;
	thread_t		thread = current_thread();
	ast_t			*myast;
	kern_return_t		kret;
	user_addr_t		rip;
	unsigned long 		dr6 = 0; /* 32 bit for i386, 64 bit for x86_64 */

	assert((is_saved_state32(saved_state) && !thread_is_64bit(thread)) ||
	       (is_saved_state64(saved_state) &&  thread_is_64bit(thread)));

	if (is_saved_state64(saved_state)) {
	        x86_saved_state64_t	*regs;

		regs = saved_state64(saved_state);

		/* Record cpu where state was captured */
		regs->isf.cpu = cpu_number();

		type = regs->isf.trapno;
		err  = (int)regs->isf.err & 0xffff;
		vaddr = (user_addr_t)regs->cr2;
		rip   = (user_addr_t)regs->isf.rip;
	} else {
		x86_saved_state32_t	*regs;

		regs = saved_state32(saved_state);

		/* Record cpu where state was captured */
		regs->cpu = cpu_number();

		type  = regs->trapno;
		err   = regs->err & 0xffff;
		vaddr = (user_addr_t)regs->cr2;
		rip   = (user_addr_t)regs->eip;
	}

	if ((type == T_DEBUG) && thread->machine.ids) {
		unsigned long clear = 0;
		/* Stash and clear this processor's DR6 value, in the event
		 * this was a debug register match
		 */
		__asm__ volatile ("mov %%db6, %0" : "=r" (dr6)); 
		__asm__ volatile ("mov %0, %%db6" : : "r" (clear));
	}

	pal_sti();

	KERNEL_DEBUG_CONSTANT(
		(MACHDBG_CODE(DBG_MACH_EXCP_UTRAP_x86, type)) | DBG_FUNC_NONE,
		(unsigned)(vaddr>>32), (unsigned)vaddr,
		(unsigned)(rip>>32), (unsigned)rip, 0);

	code = 0;
	subcode = 0;
	exc = 0;

#if DEBUG_TRACE
	kprintf("user_trap(0x%08x) type=%d vaddr=0x%016llx\n",
		saved_state, type, vaddr);
#endif

	perfASTCallback astfn = perfASTHook;
	if (__improbable(astfn != NULL)) {
		myast = ast_pending();
		if (*myast & AST_CHUD_ALL) {
			astfn(AST_CHUD_ALL, myast);
		}
	}

	/* Is there a hook? */
	perfCallback fn = perfTrapHook;
	if (__improbable(fn != NULL)) {
		if (fn(type, saved_state, 0, 0) == KERN_SUCCESS)
			return;	/* If it succeeds, we are done... */
	}

	/*
	 * DTrace does not consume all user traps, only INT_3's for now.
	 * Avoid needlessly calling tempDTraceTrapHook here, and let the
	 * INT_3 case handle them.
	 */
	DEBUG_KPRINT_SYSCALL_MASK(1,
		"user_trap: type=0x%x(%s) err=0x%x cr2=%p rip=%p\n",
		type, trap_type[type], err, (void *)(long) vaddr, (void *)(long) rip);
	
	switch (type) {

	    case T_DIVIDE_ERROR:
		exc = EXC_ARITHMETIC;
		code = EXC_I386_DIV;
		break;

	    case T_DEBUG:
		{
			pcb_t	pcb;
			/*
			 * Update the PCB with this processor's DR6 value
			 * in the event this was a debug register match.
			 */
			pcb = THREAD_TO_PCB(thread);
			if (pcb->ids) {
				/*
				 * We can get and set the status register
				 * in 32-bit mode even on a 64-bit thread
				 * because the high order bits are not
				 * used on x86_64
				 */
				if (thread_is_64bit(thread)) {
					x86_debug_state64_t *ids = pcb->ids;
					ids->dr6 = dr6;
				} else { /* 32 bit thread */
					x86_debug_state32_t *ids = pcb->ids;
					ids->dr6 = (uint32_t) dr6;
				}
			}
			exc = EXC_BREAKPOINT;
			code = EXC_I386_SGL;
			break;
		}
	    case T_INT3:
#if CONFIG_DTRACE
		if (dtrace_user_probe(saved_state) == KERN_SUCCESS)
			return; /* If it succeeds, we are done... */
#endif
		exc = EXC_BREAKPOINT;
		code = EXC_I386_BPT;
		break;

	    case T_OVERFLOW:
		exc = EXC_ARITHMETIC;
		code = EXC_I386_INTO;
		break;

	    case T_OUT_OF_BOUNDS:
		exc = EXC_SOFTWARE;
		code = EXC_I386_BOUND;
		break;

	    case T_INVALID_OPCODE:
		exc = EXC_BAD_INSTRUCTION;
		code = EXC_I386_INVOP;
		break;

	    case T_NO_FPU:
		fpnoextflt();
		return;

	    case T_FPU_FAULT:
		fpextovrflt(); /* Propagates exception directly, doesn't return */
		return;

	    case T_INVALID_TSS:	/* invalid TSS == iret with NT flag set */
		exc = EXC_BAD_INSTRUCTION;
		code = EXC_I386_INVTSSFLT;
		subcode = err;
		break;

	    case T_SEGMENT_NOT_PRESENT:
		exc = EXC_BAD_INSTRUCTION;
		code = EXC_I386_SEGNPFLT;
		subcode = err;
		break;

	    case T_STACK_FAULT:
		exc = EXC_BAD_INSTRUCTION;
		code = EXC_I386_STKFLT;
		subcode = err;
		break;

	    case T_GENERAL_PROTECTION:
		/*
		 * There's a wide range of circumstances which generate this
		 * class of exception. From user-space, many involve bad
		 * addresses (such as a non-canonical 64-bit address).
		 * So we map this to EXC_BAD_ACCESS (and thereby SIGSEGV).
		 * The trouble is cr2 doesn't contain the faulting address;
		 * we'd need to decode the faulting instruction to really
		 * determine this. We'll leave that to debuggers.
		 * However, attempted execution of privileged instructions
		 * (e.g. cli) also generate GP faults and so we map these to
		 * to EXC_BAD_ACCESS (and thence SIGSEGV) also - rather than
		 * EXC_BAD_INSTRUCTION which is more accurate. We just can't
		 * win!
		 */ 
		exc = EXC_BAD_ACCESS;
		code = EXC_I386_GPFLT;
		subcode = err;
		break;

	    case T_PAGE_FAULT:
		prot = VM_PROT_READ;

		if (err & T_PF_WRITE)
		        prot |= VM_PROT_WRITE;
#if     PAE
		if (__improbable(err & T_PF_EXECUTE))
		        prot |= VM_PROT_EXECUTE;
#endif
		kret = vm_fault(thread->map, vm_map_trunc_page(vaddr),
				 prot, FALSE,
				 THREAD_ABORTSAFE, NULL, 0);

	        user_page_fault_continue(kret);
	
		/* NOTREACHED */
		break;

	    case T_SSE_FLOAT_ERROR:
		fpSSEexterrflt(); /* Propagates exception directly, doesn't return */
		return;


	    case T_FLOATING_POINT_ERROR:
		fpexterrflt(); /* Propagates exception directly, doesn't return */
		return;

	    case T_DTRACE_RET:
#if CONFIG_DTRACE
		if (dtrace_user_probe(saved_state) == KERN_SUCCESS)
			return; /* If it succeeds, we are done... */
#endif
		/*
		 * If we get an INT 0x7f when we do not expect to,
		 * treat it as an illegal instruction
		 */
		exc = EXC_BAD_INSTRUCTION;
		code = EXC_I386_INVOP;
		break;

	    default:
#if     MACH_KGDB
		Debugger("Unanticipated user trap");
		return;
#endif  /* MACH_KGDB */
#if	MACH_KDB
		if (kdb_trap(type, err, saved_state32(saved_state)))
		    return;
#endif	/* MACH_KDB */
		panic("Unexpected user trap, type %d", type);
		return;
	}
	/* Note: Codepaths that directly return from user_trap() have pending
	 * ASTs processed in locore
	 */
	i386_exception(exc, code, subcode);
	/* NOTREACHED */
}


/*
 * Handle AST traps for i386.
 */

extern void     log_thread_action (thread_t, char *);

void
i386_astintr(int preemption)
{
	ast_t		mask = AST_ALL;
	spl_t		s;

	if (preemption)
	        mask = AST_PREEMPTION;

	s = splsched();

	ast_taken(mask, s);

	splx(s);
}

/*
 * Handle exceptions for i386.
 *
 * If we are an AT bus machine, we must turn off the AST for a
 * delayed floating-point exception.
 *
 * If we are providing floating-point emulation, we may have
 * to retrieve the real register values from the floating point
 * emulator.
 */
void
i386_exception(
	int	exc,
	mach_exception_code_t code,
	mach_exception_subcode_t subcode)
{
	mach_exception_data_type_t   codes[EXCEPTION_CODE_MAX];

	DEBUG_KPRINT_SYSCALL_MACH("i386_exception: exc=%d code=0x%llx subcode=0x%llx\n",
							  exc, code, subcode);
	codes[0] = code;		/* new exception interface */
	codes[1] = subcode;
	exception_triage(exc, codes, 2);
	/*NOTREACHED*/
}


#if	MACH_KDB

extern void 	db_i386_state(x86_saved_state32_t *regs);

#include <ddb/db_output.h>

void 
db_i386_state(
	x86_saved_state32_t *regs)
{
  	db_printf("eip	%8x\n", regs->eip);
  	db_printf("trap	%8x\n", regs->trapno);
  	db_printf("err	%8x\n", regs->err);
  	db_printf("efl	%8x\n", regs->efl);
  	db_printf("ebp	%8x\n", regs->ebp);
  	db_printf("esp	%8x\n", regs->cr2);
  	db_printf("uesp	%8x\n", regs->uesp);
  	db_printf("cs	%8x\n", regs->cs & 0xff);
  	db_printf("ds	%8x\n", regs->ds & 0xff);
  	db_printf("es	%8x\n", regs->es & 0xff);
  	db_printf("fs	%8x\n", regs->fs & 0xff);
  	db_printf("gs	%8x\n", regs->gs & 0xff);
  	db_printf("ss	%8x\n", regs->ss & 0xff);
  	db_printf("eax	%8x\n", regs->eax);
  	db_printf("ebx	%8x\n", regs->ebx);
   	db_printf("ecx	%8x\n", regs->ecx);
  	db_printf("edx	%8x\n", regs->edx);
  	db_printf("esi	%8x\n", regs->esi);
  	db_printf("edi	%8x\n", regs->edi);
}

#endif	/* MACH_KDB */

/* Synchronize a thread's i386_kernel_state (if any) with the given
 * i386_saved_state_t obtained from the trap/IPI handler; called in
 * kernel_trap() prior to entering the debugger, and when receiving
 * an "MP_KDP" IPI.
 */
  
void
sync_iss_to_iks(x86_saved_state_t *saved_state)
{
	struct x86_kernel_state *iks;
	vm_offset_t kstack;
	boolean_t record_active_regs = FALSE;

	/* The PAL may have a special way to sync registers */
	if( saved_state->flavor == THREAD_STATE_NONE )
		pal_get_kern_regs( saved_state );

	if ((kstack = current_thread()->kernel_stack) != 0) {
#ifdef __i386__
		x86_saved_state32_t	*regs = saved_state32(saved_state);
#else
		x86_saved_state64_t	*regs = saved_state64(saved_state);
#endif

		iks = STACK_IKS(kstack);

		/* Did we take the trap/interrupt in kernel mode? */
#ifdef __i386__
		if (regs == USER_REGS32(current_thread()))
		        record_active_regs = TRUE;
		else {
		        iks->k_ebx = regs->ebx;
			iks->k_esp = (int)regs;
			iks->k_ebp = regs->ebp;
			iks->k_edi = regs->edi;
			iks->k_esi = regs->esi;
			iks->k_eip = regs->eip;
		}
#else
		if (regs == USER_REGS64(current_thread()))
		        record_active_regs = TRUE;
		else {
			iks->k_rbx = regs->rbx;
			iks->k_rsp = regs->isf.rsp;
			iks->k_rbp = regs->rbp;
			iks->k_r12 = regs->r12;
			iks->k_r13 = regs->r13;
			iks->k_r14 = regs->r14;
			iks->k_r15 = regs->r15;
			iks->k_rip = regs->isf.rip;
		}
#endif
	}

	if (record_active_regs == TRUE) {
#ifdef __i386__
		/* Show the trap handler path */
		__asm__ volatile("movl %%ebx, %0" : "=m" (iks->k_ebx));
		__asm__ volatile("movl %%esp, %0" : "=m" (iks->k_esp));
		__asm__ volatile("movl %%ebp, %0" : "=m" (iks->k_ebp));
		__asm__ volatile("movl %%edi, %0" : "=m" (iks->k_edi));
		__asm__ volatile("movl %%esi, %0" : "=m" (iks->k_esi));
		/* "Current" instruction pointer */
		__asm__ volatile("movl $1f, %0\n1:" : "=m" (iks->k_eip));
#else
		/* Show the trap handler path */
		__asm__ volatile("movq %%rbx, %0" : "=m" (iks->k_rbx));
		__asm__ volatile("movq %%rsp, %0" : "=m" (iks->k_rsp));
		__asm__ volatile("movq %%rbp, %0" : "=m" (iks->k_rbp));
		__asm__ volatile("movq %%r12, %0" : "=m" (iks->k_r12));
		__asm__ volatile("movq %%r13, %0" : "=m" (iks->k_r13));
		__asm__ volatile("movq %%r14, %0" : "=m" (iks->k_r14));
		__asm__ volatile("movq %%r15, %0" : "=m" (iks->k_r15));
		/* "Current" instruction pointer */
		__asm__ volatile("leaq 1f(%%rip), %%rax; mov %%rax, %0\n1:"
				 : "=m" (iks->k_rip)
				 :
				 : "rax");
#endif
	}
}

/*
 * This is used by the NMI interrupt handler (from mp.c) to
 * uncondtionally sync the trap handler context to the IKS
 * irrespective of whether the NMI was fielded in kernel
 * or user space.
 */
void
sync_iss_to_iks_unconditionally(__unused x86_saved_state_t *saved_state) {
	struct x86_kernel_state *iks;
	vm_offset_t kstack;

	if ((kstack = current_thread()->kernel_stack) != 0) {
		iks = STACK_IKS(kstack);
#ifdef __i386__
		/* Display the trap handler path */
		__asm__ volatile("movl %%ebx, %0" : "=m" (iks->k_ebx));
		__asm__ volatile("movl %%esp, %0" : "=m" (iks->k_esp));
		__asm__ volatile("movl %%ebp, %0" : "=m" (iks->k_ebp));
		__asm__ volatile("movl %%edi, %0" : "=m" (iks->k_edi));
		__asm__ volatile("movl %%esi, %0" : "=m" (iks->k_esi));
		/* "Current" instruction pointer */
		__asm__ volatile("movl $1f, %0\n1:" : "=m" (iks->k_eip));
#else
		/* Display the trap handler path */
		__asm__ volatile("movq %%rbx, %0" : "=m" (iks->k_rbx));
		__asm__ volatile("movq %%rsp, %0" : "=m" (iks->k_rsp));
		__asm__ volatile("movq %%rbp, %0" : "=m" (iks->k_rbp));
		__asm__ volatile("movq %%r12, %0" : "=m" (iks->k_r12));
		__asm__ volatile("movq %%r13, %0" : "=m" (iks->k_r13));
		__asm__ volatile("movq %%r14, %0" : "=m" (iks->k_r14));
		__asm__ volatile("movq %%r15, %0" : "=m" (iks->k_r15));
		/* "Current" instruction pointer */
		__asm__ volatile("leaq 1f(%%rip), %%rax; mov %%rax, %0\n1:" : "=m" (iks->k_rip)::"rax");
#endif
	}
}
