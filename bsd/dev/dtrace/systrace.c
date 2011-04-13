/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2006 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

/* #pragma ident	"@(#)systrace.c	1.6	06/09/19 SMI" */

#if !defined(__APPLE__)
#include <sys/dtrace.h>
#include <sys/systrace.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/atomic.h>
#define	SYSTRACE_ARTIFICIAL_FRAMES	1
#else

#ifdef KERNEL
#ifndef _KERNEL
#define _KERNEL /* Solaris vs. Darwin */
#endif
#endif

#define MACH__POSIX_C_SOURCE_PRIVATE 1 /* pulls in suitable savearea from mach/ppc/thread_status.h */
#include <kern/thread.h>
#include <mach/thread_status.h>
/* XXX All of these should really be derived from syscall_sw.h */
#if defined(__i386__) || defined (__x86_64__)
#define SYSCALL_CLASS_SHIFT 24
#define SYSCALL_CLASS_MASK  (0xFF << SYSCALL_CLASS_SHIFT)
#define SYSCALL_NUMBER_MASK (~SYSCALL_CLASS_MASK)
#define I386_SYSCALL_NUMBER_MASK (0xFFFF)

typedef x86_saved_state_t savearea_t;
#endif

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/errno.h>
#include <sys/ioctl.h>
#include <sys/conf.h>
#include <sys/fcntl.h>
#include <miscfs/devfs/devfs.h>

#include <sys/dtrace.h>
#include <sys/dtrace_impl.h>
#include "systrace.h"
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/user.h>

#if defined (__ppc__) || defined (__ppc64__)
#define	SYSTRACE_ARTIFICIAL_FRAMES	3
#define MACHTRACE_ARTIFICIAL_FRAMES 4
#elif defined(__i386__) || defined (__x86_64__)
#define	SYSTRACE_ARTIFICIAL_FRAMES	2
#define MACHTRACE_ARTIFICIAL_FRAMES 3
#else
#error Unknown Architecture
#endif

#include <sys/sysent.h>
#define sy_callc sy_call /* Map Solaris slot name to Darwin's */
#define NSYSCALL nsysent /* and is less than 500 or so */

extern const char *syscallnames[];

#include <sys/dtrace_glue.h>
#define casptr dtrace_casptr
#define membar_enter dtrace_membar_producer

#define LOADABLE_SYSCALL(a) 0 /* Not pertinent to Darwin. */
#define LOADED_SYSCALL(a) 1 /* Not pertinent to Darwin. */

systrace_sysent_t *systrace_sysent = NULL;
void (*systrace_probe)(dtrace_id_t, uint64_t, uint64_t,
    uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

void
systrace_stub(dtrace_id_t id, uint64_t arg0, uint64_t arg1,
    uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6, uint64_t arg7)
{
#pragma unused(id,arg0,arg1,arg2,arg3,arg4,arg5,arg6,arg7)
}


int32_t
dtrace_systrace_syscall(struct proc *pp, void *uap, int *rv)
{
	boolean_t           flavor;
	unsigned short      code;

	systrace_sysent_t *sy;
	dtrace_id_t id;
	int32_t rval;
#if 0 /* XXX */
	proc_t *p;
#endif
	syscall_arg_t *ip = (syscall_arg_t *)uap;

#if defined (__ppc__) || defined (__ppc64__)
	{
		savearea_t *regs = (savearea_t *)find_user_regs(current_thread());

		flavor = (((unsigned int)regs->save_r0) == 0)? 1: 0;

		if (flavor)
			code = regs->save_r3;
		else
			code = regs->save_r0;

		/*
		 * FIXME: unix_syscall screens for "unsafe calls" and instead calls nosys(), *not* sysent[code] !
		 */
	}
#elif defined(__i386__) || defined (__x86_64__)
#pragma unused(flavor)
	{
		x86_saved_state_t   *tagged_regs = (x86_saved_state_t *)find_user_regs(current_thread());

		if (is_saved_state64(tagged_regs)) {
			x86_saved_state64_t *regs = saved_state64(tagged_regs);
			code = regs->rax & SYSCALL_NUMBER_MASK;
			/*
			 * Check for indirect system call... system call number
			 * passed as 'arg0'
			 */
			if (code == 0) {
				code = regs->rdi;
			}
		} else {
			code = saved_state32(tagged_regs)->eax & I386_SYSCALL_NUMBER_MASK;

			if (code == 0) {
				vm_offset_t params = (vm_offset_t) (saved_state32(tagged_regs)->uesp + sizeof (int));
				code = fuword(params);
			}
		}
	}
#else
#error Unknown Architecture
#endif

	// Bounds "check" the value of code a la unix_syscall
	sy = (code >= NUM_SYSENT) ? &systrace_sysent[63] : &systrace_sysent[code];

	if ((id = sy->stsy_entry) != DTRACE_IDNONE) {
		if (ip)
			(*systrace_probe)(id, *ip, *(ip+1), *(ip+2), *(ip+3), *(ip+4), *(ip+5), *(ip+6), *(ip+7));
		else
			(*systrace_probe)(id, 0, 0, 0, 0, 0, 0, 0, 0);
	}

#if 0 /* XXX */
	/*
	 * We want to explicitly allow DTrace consumers to stop a process
	 * before it actually executes the meat of the syscall.
	 */
	p = ttoproc(curthread);
	mutex_enter(&p->p_lock);
	if (curthread->t_dtrace_stop && !curthread->t_lwp->lwp_nostop) {
		curthread->t_dtrace_stop = 0;
		stop(PR_REQUESTED, 0);
	}
	mutex_exit(&p->p_lock);
#endif

	rval = (*sy->stsy_underlying)(pp, uap, rv);

	if ((id = sy->stsy_return) != DTRACE_IDNONE) {
		uint64_t munged_rv0, munged_rv1;
    	uthread_t uthread = (uthread_t)get_bsdthread_info(current_thread());

		if (uthread)
			uthread->t_dtrace_errno = rval; /* Establish t_dtrace_errno now in case this enabling refers to it. */

		/*
	 	 * "Decode" rv for use in the call to dtrace_probe()
	 	 */
		if (rval == ERESTART) {
			munged_rv0 = -1LL; /* System call will be reissued in user mode. Make DTrace report a -1 return. */
			munged_rv1 = -1LL;
		} else if (rval != EJUSTRETURN) {
			if (rval) {
				munged_rv0 = -1LL; /* Mimic what libc will do. */
				munged_rv1 = -1LL;
			} else {
				switch (sy->stsy_return_type) {
				case _SYSCALL_RET_INT_T:
					munged_rv0 = rv[0];
					munged_rv1 = rv[1];
					break;
				case _SYSCALL_RET_UINT_T:
					munged_rv0 = ((u_int)rv[0]);
					munged_rv1 = ((u_int)rv[1]);
					break;
				case _SYSCALL_RET_OFF_T:
				case _SYSCALL_RET_UINT64_T:
					munged_rv0 = *(u_int64_t *)rv;
					munged_rv1 = 0LL;
					break;
				case _SYSCALL_RET_ADDR_T:
				case _SYSCALL_RET_SIZE_T:
				case _SYSCALL_RET_SSIZE_T:
					munged_rv0 = *(user_addr_t *)rv;
					munged_rv1 = 0LL;
					break;
				case _SYSCALL_RET_NONE:
					munged_rv0 = 0LL;
					munged_rv1 = 0LL;
					break;
				default:
					munged_rv0 = 0LL;
					munged_rv1 = 0LL;
					break;
				}
			}
		} else {
			munged_rv0 = 0LL;
			munged_rv1 = 0LL;
		}

		/*
		 * <http://mail.opensolaris.org/pipermail/dtrace-discuss/2007-January/003276.html> says:
		 *
		 * "This is a bit of an historical artifact. At first, the syscall provider just
		 * had its return value in arg0, and the fbt and pid providers had their return
		 * values in arg1 (so that we could use arg0 for the offset of the return site).
		 * 
		 * We inevitably started writing scripts where we wanted to see the return
		 * values from probes in all three providers, and we made this script easier
		 * to write by replicating the syscall return values in arg1 to match fbt and
		 * pid. We debated briefly about removing the return value from arg0, but
		 * decided that it would be less confusing to have the same data in two places
		 * than to have some non-helpful, non-intuitive value in arg0.
		 * 
		 * This change was made 4/23/2003 according to the DTrace project's putback log."
		 */ 
		(*systrace_probe)(id, munged_rv0, munged_rv0, munged_rv1, (uint64_t)rval, 0, 0, 0, 0);
	}

	return (rval);
}

void
dtrace_systrace_syscall_return(unsigned short code, int rval, int *rv)
{
	systrace_sysent_t *sy;
	dtrace_id_t id;

	// Bounds "check" the value of code a la unix_syscall_return
	sy = (code >= NUM_SYSENT) ? &systrace_sysent[63] : &systrace_sysent[code];

	if ((id = sy->stsy_return) != DTRACE_IDNONE) {
		uint64_t munged_rv0, munged_rv1;
    	uthread_t uthread = (uthread_t)get_bsdthread_info(current_thread());

		if (uthread)
			uthread->t_dtrace_errno = rval; /* Establish t_dtrace_errno now in case this enabling refers to it. */

		/*
	 	 * "Decode" rv for use in the call to dtrace_probe()
	 	 */
		if (rval == ERESTART) {
			munged_rv0 = -1LL; /* System call will be reissued in user mode. Make DTrace report a -1 return. */
			munged_rv1 = -1LL;
		} else if (rval != EJUSTRETURN) {
			if (rval) {
				munged_rv0 = -1LL; /* Mimic what libc will do. */
				munged_rv1 = -1LL;
			} else {
				switch (sy->stsy_return_type) {
				case _SYSCALL_RET_INT_T:
					munged_rv0 = rv[0];
					munged_rv1 = rv[1];
					break;
				case _SYSCALL_RET_UINT_T:
					munged_rv0 = ((u_int)rv[0]);
					munged_rv1 = ((u_int)rv[1]);
					break;
				case _SYSCALL_RET_OFF_T:
				case _SYSCALL_RET_UINT64_T:
					munged_rv0 = *(u_int64_t *)rv;
					munged_rv1 = 0LL;
					break;
				case _SYSCALL_RET_ADDR_T:
				case _SYSCALL_RET_SIZE_T:
				case _SYSCALL_RET_SSIZE_T:
					munged_rv0 = *(user_addr_t *)rv;
					munged_rv1 = 0LL;
					break;
				case _SYSCALL_RET_NONE:
					munged_rv0 = 0LL;
					munged_rv1 = 0LL;
					break;
				default:
					munged_rv0 = 0LL;
					munged_rv1 = 0LL;
					break;
				}
			}
		} else {
			munged_rv0 = 0LL;
			munged_rv1 = 0LL;
		}

		(*systrace_probe)(id, munged_rv0, munged_rv0, munged_rv1, (uint64_t)rval, 0, 0, 0, 0);
	}
}
#endif /* __APPLE__ */

#define	SYSTRACE_SHIFT			16
#define	SYSTRACE_ISENTRY(x)		((int)(x) >> SYSTRACE_SHIFT)
#define	SYSTRACE_SYSNUM(x)		((int)(x) & ((1 << SYSTRACE_SHIFT) - 1))
#define	SYSTRACE_ENTRY(id)		((1 << SYSTRACE_SHIFT) | (id))
#define	SYSTRACE_RETURN(id)		(id)

#if ((1 << SYSTRACE_SHIFT) <= NSYSCALL)
#error 1 << SYSTRACE_SHIFT must exceed number of system calls
#endif

static dev_info_t *systrace_devi;
static dtrace_provider_id_t systrace_id;

#if !defined (__APPLE__)
static void
systrace_init(struct sysent *actual, systrace_sysent_t **interposed)
{
	systrace_sysent_t *sysent = *interposed;
	int i;

	if (sysent == NULL) {
		*interposed = sysent = kmem_zalloc(sizeof (systrace_sysent_t) *
		    NSYSCALL, KM_SLEEP);
	}

	for (i = 0; i < NSYSCALL; i++) {
		struct sysent *a = &actual[i];
		systrace_sysent_t *s = &sysent[i];

		if (LOADABLE_SYSCALL(a) && !LOADED_SYSCALL(a))
			continue;

		if (a->sy_callc == dtrace_systrace_syscall)
			continue;

#ifdef _SYSCALL32_IMPL
		if (a->sy_callc == dtrace_systrace_syscall32)
			continue;
#endif

		s->stsy_underlying = a->sy_callc;
	}
}
#else
#define systrace_init _systrace_init /* Avoid name clash with Darwin automagic conf symbol */
static void
systrace_init(struct sysent *actual, systrace_sysent_t **interposed)
{

	systrace_sysent_t *ssysent = *interposed;  /* Avoid sysent shadow warning
							   from bsd/sys/sysent.h */
	int i;

	if (ssysent == NULL) {
		*interposed = ssysent = kmem_zalloc(sizeof (systrace_sysent_t) *
		    NSYSCALL, KM_SLEEP);
	}

	for (i = 0; i < NSYSCALL; i++) {
		struct sysent *a = &actual[i];
		systrace_sysent_t *s = &ssysent[i];

		if (LOADABLE_SYSCALL(a) && !LOADED_SYSCALL(a))
			continue;

		if (a->sy_callc == dtrace_systrace_syscall)
			continue;

#ifdef _SYSCALL32_IMPL
		if (a->sy_callc == dtrace_systrace_syscall32)
			continue;
#endif

		s->stsy_underlying = a->sy_callc;
		s->stsy_return_type = a->sy_return_type;
	}
}

#endif /* __APPLE__ */

/*ARGSUSED*/
static void
systrace_provide(void *arg, const dtrace_probedesc_t *desc)
{
#pragma unused(arg) /* __APPLE__ */
	int i;

	if (desc != NULL)
		return;

	systrace_init(sysent, &systrace_sysent);
#ifdef _SYSCALL32_IMPL
	systrace_init(sysent32, &systrace_sysent32);
#endif

	for (i = 0; i < NSYSCALL; i++) {
		if (systrace_sysent[i].stsy_underlying == NULL)
			continue;

		if (dtrace_probe_lookup(systrace_id, NULL,
		    syscallnames[i], "entry") != 0)
			continue;

		(void) dtrace_probe_create(systrace_id, NULL, syscallnames[i],
		    "entry", SYSTRACE_ARTIFICIAL_FRAMES,
		    (void *)((uintptr_t)SYSTRACE_ENTRY(i)));
		(void) dtrace_probe_create(systrace_id, NULL, syscallnames[i],
		    "return", SYSTRACE_ARTIFICIAL_FRAMES,
		    (void *)((uintptr_t)SYSTRACE_RETURN(i)));

		systrace_sysent[i].stsy_entry = DTRACE_IDNONE;
		systrace_sysent[i].stsy_return = DTRACE_IDNONE;
#ifdef _SYSCALL32_IMPL
		systrace_sysent32[i].stsy_entry = DTRACE_IDNONE;
		systrace_sysent32[i].stsy_return = DTRACE_IDNONE;
#endif
	}
}
#if defined(__APPLE__)
#undef systrace_init
#endif

/*ARGSUSED*/
static void
systrace_destroy(void *arg, dtrace_id_t id, void *parg)
{
#pragma unused(arg,id) /* __APPLE__ */

	int sysnum = SYSTRACE_SYSNUM((uintptr_t)parg);

#pragma unused(sysnum)  /* __APPLE__ */
	/*
	 * There's nothing to do here but assert that we have actually been
	 * disabled.
	 */
	if (SYSTRACE_ISENTRY((uintptr_t)parg)) {
		ASSERT(systrace_sysent[sysnum].stsy_entry == DTRACE_IDNONE);
#ifdef _SYSCALL32_IMPL
		ASSERT(systrace_sysent32[sysnum].stsy_entry == DTRACE_IDNONE);
#endif
	} else {
		ASSERT(systrace_sysent[sysnum].stsy_return == DTRACE_IDNONE);
#ifdef _SYSCALL32_IMPL
		ASSERT(systrace_sysent32[sysnum].stsy_return == DTRACE_IDNONE);
#endif
	}
}

/*ARGSUSED*/
static void
systrace_enable(void *arg, dtrace_id_t id, void *parg)
{
#pragma unused(arg) /* __APPLE__ */
    
	int sysnum = SYSTRACE_SYSNUM((uintptr_t)parg);
	int enabled = (systrace_sysent[sysnum].stsy_entry != DTRACE_IDNONE ||
	    systrace_sysent[sysnum].stsy_return != DTRACE_IDNONE);

	if (SYSTRACE_ISENTRY((uintptr_t)parg)) {
		systrace_sysent[sysnum].stsy_entry = id;
#ifdef _SYSCALL32_IMPL
		systrace_sysent32[sysnum].stsy_entry = id;
#endif
	} else {
		systrace_sysent[sysnum].stsy_return = id;
#ifdef _SYSCALL32_IMPL
		systrace_sysent32[sysnum].stsy_return = id;
#endif
	}

	if (enabled) {
		ASSERT(sysent[sysnum].sy_callc == dtrace_systrace_syscall);
		return;
	}

	(void) casptr(&sysent[sysnum].sy_callc,
	    (void *)systrace_sysent[sysnum].stsy_underlying,
	    (void *)dtrace_systrace_syscall);
#ifdef _SYSCALL32_IMPL
	(void) casptr(&sysent32[sysnum].sy_callc,
	    (void *)systrace_sysent32[sysnum].stsy_underlying,
	    (void *)dtrace_systrace_syscall32);
#endif
}

/*ARGSUSED*/
static void
systrace_disable(void *arg, dtrace_id_t id, void *parg)
{
#pragma unused(arg,id) /* __APPLE__ */
    
	int sysnum = SYSTRACE_SYSNUM((uintptr_t)parg);
	int disable = (systrace_sysent[sysnum].stsy_entry == DTRACE_IDNONE ||
	    systrace_sysent[sysnum].stsy_return == DTRACE_IDNONE);

	if (disable) {
		(void) casptr(&sysent[sysnum].sy_callc,
		    (void *)dtrace_systrace_syscall,
		    (void *)systrace_sysent[sysnum].stsy_underlying);

#ifdef _SYSCALL32_IMPL
		(void) casptr(&sysent32[sysnum].sy_callc,
		    (void *)dtrace_systrace_syscall32,
		    (void *)systrace_sysent32[sysnum].stsy_underlying);
#endif
	}

	if (SYSTRACE_ISENTRY((uintptr_t)parg)) {
		systrace_sysent[sysnum].stsy_entry = DTRACE_IDNONE;
#ifdef _SYSCALL32_IMPL
		systrace_sysent32[sysnum].stsy_entry = DTRACE_IDNONE;
#endif
	} else {
		systrace_sysent[sysnum].stsy_return = DTRACE_IDNONE;
#ifdef _SYSCALL32_IMPL
		systrace_sysent32[sysnum].stsy_return = DTRACE_IDNONE;
#endif
	}
}

static dtrace_pattr_t systrace_attr = {
{ DTRACE_STABILITY_EVOLVING, DTRACE_STABILITY_EVOLVING, DTRACE_CLASS_COMMON },
{ DTRACE_STABILITY_PRIVATE, DTRACE_STABILITY_PRIVATE, DTRACE_CLASS_UNKNOWN },
{ DTRACE_STABILITY_PRIVATE, DTRACE_STABILITY_PRIVATE, DTRACE_CLASS_ISA },
{ DTRACE_STABILITY_EVOLVING, DTRACE_STABILITY_EVOLVING, DTRACE_CLASS_COMMON },
{ DTRACE_STABILITY_PRIVATE, DTRACE_STABILITY_PRIVATE, DTRACE_CLASS_ISA },
};

static dtrace_pops_t systrace_pops = {
	systrace_provide,
	NULL,
	systrace_enable,
	systrace_disable,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	systrace_destroy
};

static int
systrace_attach(dev_info_t *devi, ddi_attach_cmd_t cmd)
{
	switch (cmd) {
	case DDI_ATTACH:
		break;
	case DDI_RESUME:
		return (DDI_SUCCESS);
	default:
		return (DDI_FAILURE);
	}

#if !defined(__APPLE__)	
	systrace_probe = (void (*)())dtrace_probe;
	membar_enter();

	if (ddi_create_minor_node(devi, "systrace", S_IFCHR, 0,
	    DDI_PSEUDO, NULL) == DDI_FAILURE ||
	    dtrace_register("syscall", &systrace_attr, DTRACE_PRIV_USER, NULL,
	    &systrace_pops, NULL, &systrace_id) != 0) {
		systrace_probe = systrace_stub;
		ddi_remove_minor_node(devi, NULL);
		return (DDI_FAILURE);
	}
#else
	systrace_probe = (void(*))&dtrace_probe;
	membar_enter();

	if (ddi_create_minor_node(devi, "systrace", S_IFCHR, 0,
	    DDI_PSEUDO, 0) == DDI_FAILURE ||
	    dtrace_register("syscall", &systrace_attr, DTRACE_PRIV_USER, NULL,
	    &systrace_pops, NULL, &systrace_id) != 0) {
		systrace_probe = systrace_stub;
		ddi_remove_minor_node(devi, NULL);
		return (DDI_FAILURE);
	}
#endif /* __APPLE__ */

	ddi_report_dev(devi);
	systrace_devi = devi;

	return (DDI_SUCCESS);
}

#if !defined(__APPLE__)
static int
systrace_detach(dev_info_t *devi, ddi_detach_cmd_t cmd)
{
	switch (cmd) {
	case DDI_DETACH:
		break;
	case DDI_SUSPEND:
		return (DDI_SUCCESS);
	default:
		return (DDI_FAILURE);
	}

	if (dtrace_unregister(systrace_id) != 0)
		return (DDI_FAILURE);

	ddi_remove_minor_node(devi, NULL);
	systrace_probe = systrace_stub;
	return (DDI_SUCCESS);
}

/*ARGSUSED*/
static int
systrace_info(dev_info_t *dip, ddi_info_cmd_t infocmd, void *arg, void **result)
{
	int error;

	switch (infocmd) {
	case DDI_INFO_DEVT2DEVINFO:
		*result = (void *)systrace_devi;
		error = DDI_SUCCESS;
		break;
	case DDI_INFO_DEVT2INSTANCE:
		*result = (void *)0;
		error = DDI_SUCCESS;
		break;
	default:
		error = DDI_FAILURE;
	}
	return (error);
}

/*ARGSUSED*/
static int
systrace_open(dev_t *devp, int flag, int otyp, cred_t *cred_p)
{
	return (0);
}

static struct cb_ops systrace_cb_ops = {
	systrace_open,		/* open */
	nodev,			/* close */
	nulldev,		/* strategy */
	nulldev,		/* print */
	nodev,			/* dump */
	nodev,			/* read */
	nodev,			/* write */
	nodev,			/* ioctl */
	nodev,			/* devmap */
	nodev,			/* mmap */
	nodev,			/* segmap */
	nochpoll,		/* poll */
	ddi_prop_op,		/* cb_prop_op */
	0,			/* streamtab  */
	D_NEW | D_MP		/* Driver compatibility flag */
};

static struct dev_ops systrace_ops = {
	DEVO_REV,		/* devo_rev, */
	0,			/* refcnt  */
	systrace_info,		/* get_dev_info */
	nulldev,		/* identify */
	nulldev,		/* probe */
	systrace_attach,	/* attach */
	systrace_detach,	/* detach */
	nodev,			/* reset */
	&systrace_cb_ops,	/* driver operations */
	NULL,			/* bus operations */
	nodev			/* dev power */
};

/*
 * Module linkage information for the kernel.
 */
static struct modldrv modldrv = {
	&mod_driverops,		/* module type (this is a pseudo driver) */
	"System Call Tracing",	/* name of module */
	&systrace_ops,		/* driver ops */
};

static struct modlinkage modlinkage = {
	MODREV_1,
	(void *)&modldrv,
	NULL
};

int
_init(void)
{
	return (mod_install(&modlinkage));
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&modlinkage, modinfop));
}

int
_fini(void)
{
	return (mod_remove(&modlinkage));
}
#else
typedef kern_return_t (*mach_call_t)(void *);

/* XXX From #include <kern/syscall_sw.h> which may be changed for 64 bit! */
typedef void    mach_munge_t(const void *, void *);

typedef struct {
        int                     mach_trap_arg_count;
        int                     (*mach_trap_function)(void);
#if defined(__i386__)
        boolean_t               mach_trap_stack;
#else
        mach_munge_t            *mach_trap_arg_munge32; /* system call arguments for 32-bit */
        mach_munge_t            *mach_trap_arg_munge64; /* system call arguments for 64-bit */
#endif
#if     !MACH_ASSERT
        int                     mach_trap_unused;
#else
        const char*             mach_trap_name;
#endif /* !MACH_ASSERT */
} mach_trap_t;

extern mach_trap_t              mach_trap_table[];
extern int                      mach_trap_count;

extern const char *mach_syscall_name_table[];

/* XXX From osfmk/i386/bsd_i386.c */
struct mach_call_args {
        syscall_arg_t arg1;
        syscall_arg_t arg2;
        syscall_arg_t arg3;
        syscall_arg_t arg4;
        syscall_arg_t arg5;
        syscall_arg_t arg6;
        syscall_arg_t arg7;
        syscall_arg_t arg8;
        syscall_arg_t arg9;
};

#undef NSYSCALL
#define NSYSCALL mach_trap_count

#if ((1 << SYSTRACE_SHIFT) <= NSYSCALL)
#error 1 << SYSTRACE_SHIFT must exceed number of Mach traps
#endif

typedef systrace_sysent_t machtrace_sysent_t;

static machtrace_sysent_t *machtrace_sysent = NULL;

void (*machtrace_probe)(dtrace_id_t, uint64_t, uint64_t,
    uint64_t, uint64_t, uint64_t);

static dev_info_t *machtrace_devi;
static dtrace_provider_id_t machtrace_id;

static kern_return_t
dtrace_machtrace_syscall(struct mach_call_args *args)
{
	boolean_t           flavor;
	unsigned short      code;

	machtrace_sysent_t *sy;
	dtrace_id_t id;
	kern_return_t rval;
#if 0 /* XXX */
	proc_t *p;
#endif
	syscall_arg_t *ip = (syscall_arg_t *)args;
	mach_call_t mach_call;

#if defined (__ppc__) || defined (__ppc64__)
	{
		savearea_t *regs = (savearea_t *)find_user_regs(current_thread());

		flavor = (((unsigned int)regs->save_r0) == 0)? 1: 0;

		if (flavor)
			code = -regs->save_r3;
		else
			code = -regs->save_r0;
	}
#elif defined(__i386__) || defined (__x86_64__)
#pragma unused(flavor)
	{
		x86_saved_state_t   *tagged_regs = (x86_saved_state_t *)find_user_regs(current_thread());

		if (is_saved_state64(tagged_regs)) {
			code = saved_state64(tagged_regs)->rax & SYSCALL_NUMBER_MASK;
		} else {
			code = -saved_state32(tagged_regs)->eax;
		}
	}
#else
#error Unknown Architecture
#endif

	sy = &machtrace_sysent[code];

	if ((id = sy->stsy_entry) != DTRACE_IDNONE)
		(*machtrace_probe)(id, *ip, *(ip+1), *(ip+2), *(ip+3), *(ip+4));

#if 0 /* XXX */
	/*
	 * We want to explicitly allow DTrace consumers to stop a process
	 * before it actually executes the meat of the syscall.
	 */
	p = ttoproc(curthread);
	mutex_enter(&p->p_lock);
	if (curthread->t_dtrace_stop && !curthread->t_lwp->lwp_nostop) {
		curthread->t_dtrace_stop = 0;
		stop(PR_REQUESTED, 0);
	}
	mutex_exit(&p->p_lock);
#endif

	mach_call = (mach_call_t)(*sy->stsy_underlying);
	rval = mach_call(args);

	if ((id = sy->stsy_return) != DTRACE_IDNONE)
		(*machtrace_probe)(id, (uint64_t)rval, 0, 0, 0, 0);

	return (rval);
}

static void
machtrace_init(mach_trap_t *actual, machtrace_sysent_t **interposed)
{
	machtrace_sysent_t *msysent = *interposed;
	int i;

	if (msysent == NULL) {
		*interposed = msysent = kmem_zalloc(sizeof (machtrace_sysent_t) *
				NSYSCALL, KM_SLEEP);
	}

	for (i = 0; i < NSYSCALL; i++) {
		mach_trap_t *a = &actual[i];
		machtrace_sysent_t *s = &msysent[i];

		if (LOADABLE_SYSCALL(a) && !LOADED_SYSCALL(a))
			continue;

		if ((mach_call_t)(a->mach_trap_function) == (mach_call_t)(dtrace_machtrace_syscall))
			continue;

		s->stsy_underlying = (sy_call_t *)a->mach_trap_function;
	}
}

/*ARGSUSED*/
static void
machtrace_provide(void *arg, const dtrace_probedesc_t *desc)
{
#pragma unused(arg) /* __APPLE__ */
    
	int i;

	if (desc != NULL)
		return;

	machtrace_init(mach_trap_table, &machtrace_sysent);

	for (i = 0; i < NSYSCALL; i++) {
		
		if (machtrace_sysent[i].stsy_underlying == NULL)
			continue;

		if (dtrace_probe_lookup(machtrace_id, NULL,
					mach_syscall_name_table[i], "entry") != 0)
			continue;

		(void) dtrace_probe_create(machtrace_id, NULL, mach_syscall_name_table[i],
					   "entry", MACHTRACE_ARTIFICIAL_FRAMES,
					   (void *)((uintptr_t)SYSTRACE_ENTRY(i)));
		(void) dtrace_probe_create(machtrace_id, NULL, mach_syscall_name_table[i],
					   "return", MACHTRACE_ARTIFICIAL_FRAMES,
					   (void *)((uintptr_t)SYSTRACE_RETURN(i)));

		machtrace_sysent[i].stsy_entry = DTRACE_IDNONE;
		machtrace_sysent[i].stsy_return = DTRACE_IDNONE;
	}
}

/*ARGSUSED*/
static void
machtrace_destroy(void *arg, dtrace_id_t id, void *parg)
{
#pragma unused(arg,id) /* __APPLE__ */
	int sysnum = SYSTRACE_SYSNUM((uintptr_t)parg);
	
#pragma unused(sysnum) /* __APPLE__ */

	/*
	 * There's nothing to do here but assert that we have actually been
	 * disabled.
	 */
	if (SYSTRACE_ISENTRY((uintptr_t)parg)) {
		ASSERT(machtrace_sysent[sysnum].stsy_entry == DTRACE_IDNONE);
	} else {
		ASSERT(machtrace_sysent[sysnum].stsy_return == DTRACE_IDNONE);
	}
}

/*ARGSUSED*/
static void
machtrace_enable(void *arg, dtrace_id_t id, void *parg)
{
#pragma unused(arg) /* __APPLE__ */
    
	int sysnum = SYSTRACE_SYSNUM((uintptr_t)parg);
	int enabled = (machtrace_sysent[sysnum].stsy_entry != DTRACE_IDNONE ||
			machtrace_sysent[sysnum].stsy_return != DTRACE_IDNONE);

	if (SYSTRACE_ISENTRY((uintptr_t)parg)) {
		machtrace_sysent[sysnum].stsy_entry = id;
	} else {
		machtrace_sysent[sysnum].stsy_return = id;
	}

	if (enabled) {
	    ASSERT(sysent[sysnum].sy_callc == (void *)dtrace_machtrace_syscall);
		return;
	}

	(void) casptr(&mach_trap_table[sysnum].mach_trap_function,
		      (void *)machtrace_sysent[sysnum].stsy_underlying,
		      (void *)dtrace_machtrace_syscall);
}

/*ARGSUSED*/
static void
machtrace_disable(void *arg, dtrace_id_t id, void *parg)
{
#pragma unused(arg,id) /* __APPLE__ */
      
	int sysnum = SYSTRACE_SYSNUM((uintptr_t)parg);
	int disable = (machtrace_sysent[sysnum].stsy_entry == DTRACE_IDNONE ||
			machtrace_sysent[sysnum].stsy_return == DTRACE_IDNONE);

	if (disable) {
		(void) casptr(&mach_trap_table[sysnum].mach_trap_function,
			      (void *)dtrace_machtrace_syscall,
			      (void *)machtrace_sysent[sysnum].stsy_underlying);

	}

	if (SYSTRACE_ISENTRY((uintptr_t)parg)) {
		machtrace_sysent[sysnum].stsy_entry = DTRACE_IDNONE;
	} else {
		machtrace_sysent[sysnum].stsy_return = DTRACE_IDNONE;
	}
}

static dtrace_pattr_t machtrace_attr = {
{ DTRACE_STABILITY_EVOLVING, DTRACE_STABILITY_EVOLVING, DTRACE_CLASS_COMMON },
{ DTRACE_STABILITY_PRIVATE, DTRACE_STABILITY_PRIVATE, DTRACE_CLASS_UNKNOWN },
{ DTRACE_STABILITY_PRIVATE, DTRACE_STABILITY_PRIVATE, DTRACE_CLASS_ISA },
{ DTRACE_STABILITY_EVOLVING, DTRACE_STABILITY_EVOLVING, DTRACE_CLASS_COMMON },
{ DTRACE_STABILITY_PRIVATE, DTRACE_STABILITY_PRIVATE, DTRACE_CLASS_ISA },
};

static dtrace_pops_t machtrace_pops = {
	machtrace_provide,
	NULL,
	machtrace_enable,
	machtrace_disable,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	machtrace_destroy
};

static int
machtrace_attach(dev_info_t *devi, ddi_attach_cmd_t cmd)
{
	switch (cmd) {
		case DDI_ATTACH:
			break;
		case DDI_RESUME:
			return (DDI_SUCCESS);
		default:
			return (DDI_FAILURE);
	}

#if !defined(__APPLE__)
	machtrace_probe = (void (*)())dtrace_probe;
	membar_enter();

	if (ddi_create_minor_node(devi, "machtrace", S_IFCHR, 0,
				DDI_PSEUDO, NULL) == DDI_FAILURE ||
			dtrace_register("mach_trap", &machtrace_attr, DTRACE_PRIV_USER, NULL,
				&machtrace_pops, NULL, &machtrace_id) != 0) {
		machtrace_probe = systrace_stub;
#else
	machtrace_probe = dtrace_probe;
	membar_enter();
	
	if (ddi_create_minor_node(devi, "machtrace", S_IFCHR, 0,
				DDI_PSEUDO, 0) == DDI_FAILURE ||
			dtrace_register("mach_trap", &machtrace_attr, DTRACE_PRIV_USER, NULL,
				&machtrace_pops, NULL, &machtrace_id) != 0) {
                machtrace_probe = (void (*))&systrace_stub;
#endif /* __APPLE__ */		
		ddi_remove_minor_node(devi, NULL);
		return (DDI_FAILURE);
	}

	ddi_report_dev(devi);
	machtrace_devi = devi;

	return (DDI_SUCCESS);
}

d_open_t _systrace_open;

int _systrace_open(dev_t dev, int flags, int devtype, struct proc *p)
{
#pragma unused(dev,flags,devtype,p)
	return 0;
}

#define SYSTRACE_MAJOR  -24 /* let the kernel pick the device number */

/*
 * A struct describing which functions will get invoked for certain
 * actions.
 */
static struct cdevsw systrace_cdevsw =
{
	_systrace_open,		/* open */
	eno_opcl,		/* close */
	eno_rdwrt,			/* read */
	eno_rdwrt,			/* write */
	eno_ioctl,		/* ioctl */
	(stop_fcn_t *)nulldev, /* stop */
	(reset_fcn_t *)nulldev, /* reset */
	NULL,				/* tty's */
	eno_select,			/* select */
	eno_mmap,			/* mmap */
	eno_strat,			/* strategy */
	eno_getc,			/* getc */
	eno_putc,			/* putc */
	0					/* type */
};

static int gSysTraceInited = 0;

void systrace_init( void );

void systrace_init( void )
{
	if (0 == gSysTraceInited) {
		int majdevno = cdevsw_add(SYSTRACE_MAJOR, &systrace_cdevsw);

		if (majdevno < 0) {
			printf("systrace_init: failed to allocate a major number!\n");
			gSysTraceInited = 0;
			return;
		}

		systrace_attach( (dev_info_t	*)(uintptr_t)majdevno, DDI_ATTACH );
		machtrace_attach( (dev_info_t	*)(uintptr_t)majdevno, DDI_ATTACH );

		gSysTraceInited = 1;
	} else
		panic("systrace_init: called twice!\n");
}
#undef SYSTRACE_MAJOR
#endif /* __APPLE__ */
