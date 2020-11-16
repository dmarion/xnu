/*
 * Copyright (c) 2000-2008 Apple Inc. All rights reserved.
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
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/assert.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <kern/debug.h>
#include <mach/machine/thread_status.h>
#include <mach/thread_act.h>
#include <mach/branch_predicates.h>

#include <sys/kernel.h>
#include <sys/vm.h>
#include <sys/proc_internal.h>
#include <sys/syscall.h>
#include <sys/systm.h>
#include <sys/user.h>
#include <sys/errno.h>
#include <sys/kdebug.h>
#include <sys/sysent.h>
#include <sys/sysproto.h>
#include <sys/kauth.h>
#include <sys/systm.h>

#include <security/audit/audit.h>

#include <i386/seg.h>
#include <i386/machine_routines.h>
#include <mach/i386/syscall_sw.h>

#include <machine/pal_routines.h>

#if CONFIG_DTRACE
extern int32_t dtrace_systrace_syscall(struct proc *, void *, int *);
extern void dtrace_systrace_syscall_return(unsigned short, int, int *);
#endif

extern void unix_syscall(x86_saved_state_t *);
extern void unix_syscall64(x86_saved_state_t *);
extern void *find_user_regs(thread_t);

extern void x86_toggle_sysenter_arg_store(thread_t thread, boolean_t valid);
extern boolean_t x86_sysenter_arg_store_isvalid(thread_t thread);

/* dynamically generated at build time based on syscalls.master */
extern const char *syscallnames[];

/*
 * This needs to be a single switch so that it's "all on" or "all off",
 * rather than being turned on for some code paths and not others, as this
 * has a tendency to introduce "blame the next guy" bugs.
 */
#if DEBUG
#define	FUNNEL_DEBUG	1	/* Check for funnel held on exit */
#endif

/*
 * Function:	unix_syscall
 *
 * Inputs:	regs	- pointer to i386 save area
 *
 * Outputs:	none
 */
void
unix_syscall(x86_saved_state_t *state)
{
	thread_t		thread;
	void			*vt;
	unsigned int		code;
	struct sysent		*callp;

	int			error;
	vm_offset_t		params;
	struct proc		*p;
	struct uthread		*uthread;
	x86_saved_state32_t	*regs;
	boolean_t		args_in_uthread;
	boolean_t		is_vfork;

	assert(is_saved_state32(state));
	regs = saved_state32(state);
#if DEBUG
	if (regs->eax == 0x800)
		thread_exception_return();
#endif
	thread = current_thread();
	uthread = get_bsdthread_info(thread);

	/* Get the approriate proc; may be different from task's for vfork() */
	is_vfork = uthread->uu_flag & UT_VFORK;
	if (__improbable(is_vfork != 0))
		p = current_proc();
	else 
		p = (struct proc *)get_bsdtask_info(current_task());

	/* Verify that we are not being called from a task without a proc */
	if (__improbable(p == NULL)) {
		regs->eax = EPERM;
		regs->efl |= EFL_CF;
		task_terminate_internal(current_task());
		thread_exception_return();
		/* NOTREACHED */
	}

	code = regs->eax & I386_SYSCALL_NUMBER_MASK;
	DEBUG_KPRINT_SYSCALL_UNIX("unix_syscall: code=%d(%s) eip=%u\n",
							  code, syscallnames[code >= NUM_SYSENT ? 63 : code], (uint32_t)regs->eip);
	args_in_uthread = ((regs->eax & I386_SYSCALL_ARG_BYTES_MASK) != 0) && x86_sysenter_arg_store_isvalid(thread);
	params = (vm_offset_t) (regs->uesp + sizeof (int));

	regs->efl &= ~(EFL_CF);

	callp = (code >= NUM_SYSENT) ? &sysent[63] : &sysent[code];

	if (__improbable(callp == sysent)) {
		code = fuword(params);
		params += sizeof(int);
		callp = (code >= NUM_SYSENT) ? &sysent[63] : &sysent[code];
	}

	vt = (void *)uthread->uu_arg;

	if (callp->sy_arg_bytes != 0) {
		sy_munge_t	*mungerp;

		assert((unsigned) callp->sy_arg_bytes <= sizeof (uthread->uu_arg));
		if (!args_in_uthread)
		{
			uint32_t nargs;
			nargs = callp->sy_arg_bytes;
			error = copyin((user_addr_t) params, (char *) vt, nargs);
			if (error) {
				regs->eax = error;
				regs->efl |= EFL_CF;
				thread_exception_return();
				/* NOTREACHED */
			}
		}

		if (__probable(code != 180)) {
	        	int *ip = (int *)vt;

			KERNEL_DEBUG_CONSTANT(BSDDBG_CODE(DBG_BSD_EXCP_SC, code) | DBG_FUNC_START,
				      *ip, *(ip+1), *(ip+2), *(ip+3), 0);
		}
		mungerp = callp->sy_arg_munge32;

		/*
		 * If non-NULL, then call the syscall argument munger to
		 * copy in arguments (see xnu/bsd/dev/{i386|x86_64}/munge.s); the
		 * first argument is NULL because we are munging in place
		 * after a copyin because the ABI currently doesn't use
		 * registers to pass system call arguments.
		 */
		if (mungerp != NULL)
			(*mungerp)(NULL, vt);
	} else
		KERNEL_DEBUG_CONSTANT(BSDDBG_CODE(DBG_BSD_EXCP_SC, code) | DBG_FUNC_START,
			0, 0, 0, 0, 0);

	/*
	 * Delayed binding of thread credential to process credential, if we
	 * are not running with an explicitly set thread credential.
	 */
	kauth_cred_uthread_update(uthread, p);

	uthread->uu_rval[0] = 0;
	uthread->uu_rval[1] = regs->edx;
	uthread->uu_flag |= UT_NOTCANCELPT;


#ifdef JOE_DEBUG
        uthread->uu_iocount = 0;
        uthread->uu_vpindex = 0;
#endif

	AUDIT_SYSCALL_ENTER(code, p, uthread);
	error = (*(callp->sy_call))((void *) p, (void *) vt, &(uthread->uu_rval[0]));
        AUDIT_SYSCALL_EXIT(code, p, uthread, error);

#ifdef JOE_DEBUG
        if (uthread->uu_iocount)
                printf("system call returned with uu_iocount != 0\n");
#endif
#if CONFIG_DTRACE
	uthread->t_dtrace_errno = error;
#endif /* CONFIG_DTRACE */

	if (__improbable(error == ERESTART)) {
		/*
		 * Move the user's pc back to repeat the syscall:
		 * 5 bytes for a sysenter, or 2 for an int 8x.
		 * The SYSENTER_TF_CS covers single-stepping over a sysenter
		 * - see debug trap handler in idt.s/idt64.s
		 */

		pal_syscall_restart(thread, state);
	}
	else if (error != EJUSTRETURN) {
		if (__improbable(error)) {
		    regs->eax = error;
		    regs->efl |= EFL_CF;	/* carry bit */
		} else { /* (not error) */
		    regs->eax = uthread->uu_rval[0];
		    regs->edx = uthread->uu_rval[1];
		} 
	}

	DEBUG_KPRINT_SYSCALL_UNIX(
		"unix_syscall: error=%d retval=(%u,%u)\n",
		error, regs->eax, regs->edx);

	uthread->uu_flag &= ~UT_NOTCANCELPT;
#if FUNNEL_DEBUG
	/*
	 * if we're holding the funnel panic
	 */
	syscall_exit_funnelcheck();
#endif /* FUNNEL_DEBUG */

	if (__improbable(uthread->uu_lowpri_window)) {
	        /*
		 * task is marked as a low priority I/O type
		 * and the I/O we issued while in this system call
		 * collided with normal I/O operations... we'll
		 * delay in order to mitigate the impact of this
		 * task on the normal operation of the system
		 */
		throttle_lowpri_io(TRUE);
	}
	if (__probable(code != 180))
	        KERNEL_DEBUG_CONSTANT(BSDDBG_CODE(DBG_BSD_EXCP_SC, code) | DBG_FUNC_END,
				      error, uthread->uu_rval[0], uthread->uu_rval[1], p->p_pid, 0);

	if (__improbable(!is_vfork && callp->sy_call == (sy_call_t *)execve && !error)) {
		pal_execve_return(thread);
	}

	thread_exception_return();
	/* NOTREACHED */
}


void
unix_syscall64(x86_saved_state_t *state)
{
	thread_t	thread;
	unsigned int	code;
	struct sysent	*callp;
	void		*uargp;
	int		args_in_regs;
	int		error;
	struct proc	*p;
	struct uthread	*uthread;
	x86_saved_state64_t *regs;

	assert(is_saved_state64(state));
	regs = saved_state64(state);
#if	DEBUG
	if (regs->rax == 0x2000800)
		thread_exception_return();
#endif
	thread = current_thread();
	uthread = get_bsdthread_info(thread);

	/* Get the approriate proc; may be different from task's for vfork() */
	if (__probable(!(uthread->uu_flag & UT_VFORK)))
		p = (struct proc *)get_bsdtask_info(current_task());
	else 
		p = current_proc();

	/* Verify that we are not being called from a task without a proc */
	if (__improbable(p == NULL)) {
		regs->rax = EPERM;
		regs->isf.rflags |= EFL_CF;
		task_terminate_internal(current_task());
		thread_exception_return();
		/* NOTREACHED */
	}
	args_in_regs = 6;

	code = regs->rax & SYSCALL_NUMBER_MASK;
	DEBUG_KPRINT_SYSCALL_UNIX(
		"unix_syscall64: code=%d(%s) rip=%llx\n",
		code, syscallnames[code >= NUM_SYSENT ? 63 : code], regs->isf.rip);
	callp = (code >= NUM_SYSENT) ? &sysent[63] : &sysent[code];
	uargp = (void *)(&regs->rdi);

	if (__improbable(callp == sysent)) {
	        /*
		 * indirect system call... system call number
		 * passed as 'arg0'
		 */
	        code = regs->rdi;
		callp = (code >= NUM_SYSENT) ? &sysent[63] : &sysent[code];
		uargp = (void *)(&regs->rsi);
		args_in_regs = 5;
	}

	if (callp->sy_narg != 0) {
		if (code != 180) {
			uint64_t *ip = (uint64_t *)uargp;

			KERNEL_DEBUG_CONSTANT(BSDDBG_CODE(DBG_BSD_EXCP_SC, code) | DBG_FUNC_START,
					(int)(*ip), (int)(*(ip+1)), (int)(*(ip+2)), (int)(*(ip+3)), 0);
		}
		assert(callp->sy_narg <= 8);

		if (__improbable(callp->sy_narg > args_in_regs)) {
			int copyin_count;

			copyin_count = (callp->sy_narg - args_in_regs) * sizeof(uint64_t);

			error = copyin((user_addr_t)(regs->isf.rsp + sizeof(user_addr_t)), (char *)&regs->v_arg6, copyin_count);
			if (error) {
				regs->rax = error;
				regs->isf.rflags |= EFL_CF;
				thread_exception_return();
				/* NOTREACHED */
			}
		}
		/*
		 * XXX Turn 64 bit unsafe calls into nosys()
		 */
		if (__improbable(callp->sy_flags & UNSAFE_64BIT)) {
			callp = &sysent[63];
			goto unsafe;
		}
	} else
	        KERNEL_DEBUG_CONSTANT(BSDDBG_CODE(DBG_BSD_EXCP_SC, code) | DBG_FUNC_START,
				      0, 0, 0, 0, 0);
unsafe:

	/*
	 * Delayed binding of thread credential to process credential, if we
	 * are not running with an explicitly set thread credential.
	 */
	kauth_cred_uthread_update(uthread, p);

	uthread->uu_rval[0] = 0;
	uthread->uu_rval[1] = 0;

	
	uthread->uu_flag |= UT_NOTCANCELPT;

#ifdef JOE_DEBUG
        uthread->uu_iocount = 0;
        uthread->uu_vpindex = 0;
#endif

	AUDIT_SYSCALL_ENTER(code, p, uthread);
	error = (*(callp->sy_call))((void *) p, uargp, &(uthread->uu_rval[0]));
        AUDIT_SYSCALL_EXIT(code, p, uthread, error);

#ifdef JOE_DEBUG
        if (uthread->uu_iocount)
               printf("system call returned with uu_iocount != 0\n");
#endif

#if CONFIG_DTRACE
	uthread->t_dtrace_errno = error;
#endif /* CONFIG_DTRACE */
	
	if (__improbable(error == ERESTART)) {
		/*
		 * all system calls come through via the syscall instruction
		 * in 64 bit mode... its 2 bytes in length
		 * move the user's pc back to repeat the syscall:
		 */
		pal_syscall_restart( thread, state );
	}
	else if (error != EJUSTRETURN) {
		if (__improbable(error)) {
			regs->rax = error;
			regs->isf.rflags |= EFL_CF;	/* carry bit */
		} else { /* (not error) */

			switch (callp->sy_return_type) {
			case _SYSCALL_RET_INT_T:
				regs->rax = uthread->uu_rval[0];
				regs->rdx = uthread->uu_rval[1];
				break;
			case _SYSCALL_RET_UINT_T:
				regs->rax = ((u_int)uthread->uu_rval[0]);
				regs->rdx = ((u_int)uthread->uu_rval[1]);
				break;
			case _SYSCALL_RET_OFF_T:
			case _SYSCALL_RET_ADDR_T:
			case _SYSCALL_RET_SIZE_T:
			case _SYSCALL_RET_SSIZE_T:
			case _SYSCALL_RET_UINT64_T:
			        regs->rax = *((uint64_t *)(&uthread->uu_rval[0]));
				regs->rdx = 0;
				break;
			case _SYSCALL_RET_NONE:
				break;
			default:
				panic("unix_syscall: unknown return type");
				break;
			}
			regs->isf.rflags &= ~EFL_CF;
		} 
	}

	DEBUG_KPRINT_SYSCALL_UNIX(
		"unix_syscall64: error=%d retval=(%llu,%llu)\n",
		error, regs->rax, regs->rdx);
	
	uthread->uu_flag &= ~UT_NOTCANCELPT;

#if FUNNEL_DEBUG	
	/*
	 * if we're holding the funnel panic
	 */
	syscall_exit_funnelcheck();
#endif /* FUNNEL_DEBUG */

	if (__improbable(uthread->uu_lowpri_window)) {
	        /*
		 * task is marked as a low priority I/O type
		 * and the I/O we issued while in this system call
		 * collided with normal I/O operations... we'll
		 * delay in order to mitigate the impact of this
		 * task on the normal operation of the system
		 */
		throttle_lowpri_io(TRUE);
	}
	if (__probable(code != 180))
	        KERNEL_DEBUG_CONSTANT(BSDDBG_CODE(DBG_BSD_EXCP_SC, code) | DBG_FUNC_END,
				      error, uthread->uu_rval[0], uthread->uu_rval[1], p->p_pid, 0);

	thread_exception_return();
	/* NOTREACHED */
}


void
unix_syscall_return(int error)
{
	thread_t		thread;
	struct uthread		*uthread;
	struct proc *p;
	unsigned int code;
	vm_offset_t params;
	struct sysent *callp;

	thread = current_thread();
	uthread = get_bsdthread_info(thread);

	pal_register_cache_state(thread, DIRTY);

	p = current_proc();

	if (proc_is64bit(p)) {
		x86_saved_state64_t *regs;

		regs = saved_state64(find_user_regs(thread));

		/* reconstruct code for tracing before blasting rax */
		code = regs->rax & SYSCALL_NUMBER_MASK;
		callp = (code >= NUM_SYSENT) ? &sysent[63] : &sysent[code];

		if (callp == sysent)
			/*
			 * indirect system call... system call number
			 * passed as 'arg0'
			 */
			code = regs->rdi;

#if CONFIG_DTRACE
		if (callp->sy_call == dtrace_systrace_syscall)
			dtrace_systrace_syscall_return( code, error, uthread->uu_rval );
#endif /* CONFIG_DTRACE */
		AUDIT_SYSCALL_EXIT(code, p, uthread, error);

		if (error == ERESTART) {
			/*
			 * repeat the syscall
			 */
			pal_syscall_restart( thread, find_user_regs(thread) );
		}
		else if (error != EJUSTRETURN) {
			if (error) {
				regs->rax = error;
				regs->isf.rflags |= EFL_CF;	/* carry bit */
			} else { /* (not error) */

				switch (callp->sy_return_type) {
				case _SYSCALL_RET_INT_T:
					regs->rax = uthread->uu_rval[0];
					regs->rdx = uthread->uu_rval[1];
					break;
				case _SYSCALL_RET_UINT_T:
					regs->rax = ((u_int)uthread->uu_rval[0]);
					regs->rdx = ((u_int)uthread->uu_rval[1]);
					break;
				case _SYSCALL_RET_OFF_T:
				case _SYSCALL_RET_ADDR_T:
				case _SYSCALL_RET_SIZE_T:
				case _SYSCALL_RET_SSIZE_T:
				case _SYSCALL_RET_UINT64_T:
					regs->rax = *((uint64_t *)(&uthread->uu_rval[0]));
					regs->rdx = 0;
					break;
				case _SYSCALL_RET_NONE:
					break;
				default:
					panic("unix_syscall: unknown return type");
					break;
				}
				regs->isf.rflags &= ~EFL_CF;
			} 
		}
		DEBUG_KPRINT_SYSCALL_UNIX(
			"unix_syscall_return: error=%d retval=(%llu,%llu)\n",
			error, regs->rax, regs->rdx);
	} else {
		x86_saved_state32_t	*regs;

		regs = saved_state32(find_user_regs(thread));

		regs->efl &= ~(EFL_CF);
		/* reconstruct code for tracing before blasting eax */
		code = regs->eax & I386_SYSCALL_NUMBER_MASK;
		callp = (code >= NUM_SYSENT) ? &sysent[63] : &sysent[code];

#if CONFIG_DTRACE
		if (callp->sy_call == dtrace_systrace_syscall)
			dtrace_systrace_syscall_return( code, error, uthread->uu_rval );
#endif /* CONFIG_DTRACE */
		AUDIT_SYSCALL_EXIT(code, p, uthread, error);

		if (callp == sysent) {
			params = (vm_offset_t) (regs->uesp + sizeof (int));
			code = fuword(params);
		}
		if (error == ERESTART) {
			pal_syscall_restart( thread, find_user_regs(thread) );
		}
		else if (error != EJUSTRETURN) {
			if (error) {
				regs->eax = error;
				regs->efl |= EFL_CF;	/* carry bit */
			} else { /* (not error) */
				regs->eax = uthread->uu_rval[0];
				regs->edx = uthread->uu_rval[1];
			} 
		}
		DEBUG_KPRINT_SYSCALL_UNIX(
			"unix_syscall_return: error=%d retval=(%u,%u)\n",
			error, regs->eax, regs->edx);
	}


	uthread->uu_flag &= ~UT_NOTCANCELPT;

#if FUNNEL_DEBUG	
	/*
	 * if we're holding the funnel panic
	 */
	syscall_exit_funnelcheck();
#endif /* FUNNEL_DEBUG */

	if (uthread->uu_lowpri_window) {
	        /*
		 * task is marked as a low priority I/O type
		 * and the I/O we issued while in this system call
		 * collided with normal I/O operations... we'll
		 * delay in order to mitigate the impact of this
		 * task on the normal operation of the system
		 */
		throttle_lowpri_io(TRUE);
	}
	if (code != 180)
	        KERNEL_DEBUG_CONSTANT(BSDDBG_CODE(DBG_BSD_EXCP_SC, code) | DBG_FUNC_END,
				      error, uthread->uu_rval[0], uthread->uu_rval[1], p->p_pid, 0);

	thread_exception_return();
	/* NOTREACHED */
}

void
munge_wwwlww(
	__unused const void	*in32,
	void			*out64)
{
	uint32_t	*arg32;
	uint64_t	*arg64;

	/* we convert in place in out64 */
	arg32 = (uint32_t *) out64;
	arg64 = (uint64_t *) out64;

	arg64[5] = arg32[6];	/* wwwlwW */
	arg64[4] = arg32[5];	/* wwwlWw */
	arg32[7] = arg32[4];	/* wwwLww (hi) */
	arg32[6] = arg32[3];	/* wwwLww (lo) */
	arg64[2] = arg32[2];	/* wwWlww */
	arg64[1] = arg32[1];	/* wWwlww */
	arg64[0] = arg32[0];	/* Wwwlww */
}	


void
munge_wwlwww(
	__unused const void	*in32,
	void			*out64)
{
	uint32_t	*arg32;
	uint64_t	*arg64;

	/* we convert in place in out64 */
	arg32 = (uint32_t *) out64;
	arg64 = (uint64_t *) out64;

	arg64[5] = arg32[6];	/* wwlwwW */
	arg64[4] = arg32[5];	/* wwlwWw */
	arg64[3] = arg32[4];	/* wwlWww  */
	arg32[5] = arg32[3];	/* wwLwww (hi) */
	arg32[4] = arg32[2];	/* wwLwww (lo) */
	arg64[1] = arg32[1];	/* wWlwww */
	arg64[0] = arg32[0];	/* Wwlwww */
}	

