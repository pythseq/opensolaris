/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
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
 * Copyright 2005 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#pragma ident	"%Z%%M%	%I%	%E% SMI"

#include <sys/sysmacros.h>
#include <sys/stack.h>
#include <sys/cpuvar.h>
#include <sys/ivintr.h>
#include <sys/intreg.h>
#include <sys/membar.h>
#include <sys/kmem.h>
#include <sys/intr.h>
#include <sys/sunndi.h>
#include <sys/cmn_err.h>
#include <sys/privregs.h>
#include <sys/systm.h>
#include <sys/archsystm.h>
#include <sys/machsystm.h>
#include <sys/x_call.h>
#include <vm/seg_kp.h>
#include <sys/debug.h>
#include <sys/cyclic.h>

#include <sys/cpu_sgnblk_defs.h>

kmutex_t soft_iv_lock;	/* protect software interrupt vector table */
/* Global locks which protect the interrupt distribution lists */
static kmutex_t intr_dist_lock;
static kmutex_t intr_dist_cpu_lock;

/* Head of the interrupt distribution lists */
static struct intr_dist *intr_dist_head = NULL;
static struct intr_dist *intr_dist_whead = NULL;

uint_t swinum_base;
uint_t maxswinum;
uint_t siron_inum;
uint_t poke_cpu_inum;
int siron_pending;

int intr_policy = INTR_WEIGHTED_DIST;	/* interrupt distribution policy */
int intr_dist_debug = 0;
int32_t intr_dist_weight_max = 1;
int32_t intr_dist_weight_maxmax = 1000;
int intr_dist_weight_maxfactor = 2;
#define	INTR_DEBUG(args) if (intr_dist_debug) cmn_err args

static void sw_ivintr_init(cpu_t *);

/*
 * intr_init() - interrupt initialization
 *	Initialize the system's software interrupt vector table and
 *	CPU's interrupt free list
 */
void
intr_init(cpu_t *cp)
{
	init_ivintr();
	sw_ivintr_init(cp);
	init_intr_pool(cp);

	mutex_init(&intr_dist_lock, NULL, MUTEX_DEFAULT, NULL);
	mutex_init(&intr_dist_cpu_lock, NULL, MUTEX_DEFAULT, NULL);

	/*
	 * A soft interrupt may have been requested prior to the initialization
	 * of soft interrupts.  Soft interrupts can't be dispatched until after
	 * init_intr_pool, so we have to wait until now before we can dispatch
	 * the pending soft interrupt (if any).
	 */
	if (siron_pending)
		setsoftint(siron_inum);
}

/*
 * poke_cpu_intr - fall through when poke_cpu calls
 */

/* ARGSUSED */
uint_t
poke_cpu_intr(caddr_t arg1, caddr_t arg2)
{
	CPU->cpu_m.poke_cpu_outstanding = B_FALSE;
	membar_stld_stst();
	return (1);
}

/*
 * sw_ivintr_init() - software interrupt vector initialization
 *	called after CPU is active
 *	the software interrupt vector table is part of the intr_vector[]
 */
static void
sw_ivintr_init(cpu_t *cp)
{
	extern uint_t softlevel1();

	mutex_init(&soft_iv_lock, NULL, MUTEX_DEFAULT, NULL);

	swinum_base = SOFTIVNUM;

	/*
	 * the maximum software interrupt == MAX_SOFT_INO
	 */
	maxswinum = swinum_base + MAX_SOFT_INO;

	REGISTER_BBUS_INTR();

	siron_inum = add_softintr(PIL_1, softlevel1, 0);
	poke_cpu_inum = add_softintr(PIL_13, poke_cpu_intr, 0);
	cp->cpu_m.poke_cpu_outstanding = B_FALSE;
}

cpuset_t intr_add_pools_inuse;

/*
 * cleanup_intr_pool()
 *	Free up the extra intr request pool for this cpu.
 */
void
cleanup_intr_pool(cpu_t *cp)
{
	extern struct intr_req *intr_add_head;
	int poolno;
	struct intr_req *pool;

	poolno = cp->cpu_m.intr_pool_added;
	if (poolno >= 0) {
		cp->cpu_m.intr_pool_added = -1;
		pool = (poolno * INTR_PENDING_MAX * intr_add_pools) +

			intr_add_head;	/* not byte arithmetic */
		bzero(pool, INTR_PENDING_MAX * intr_add_pools *
		    sizeof (struct intr_req));

		CPUSET_DEL(intr_add_pools_inuse, poolno);
	}
}

/*
 * init_intr_pool()
 *	initialize the intr request pool for the cpu
 * 	should be called for each cpu
 */
void
init_intr_pool(cpu_t *cp)
{
	extern struct intr_req *intr_add_head;
#ifdef	DEBUG
	extern struct intr_req *intr_add_tail;
#endif	/* DEBUG */
	int i, pool;

	cp->cpu_m.intr_pool_added = -1;

	for (i = 0; i < INTR_PENDING_MAX-1; i++) {
		cp->cpu_m.intr_pool[i].intr_next =
		    &cp->cpu_m.intr_pool[i+1];
	}
	cp->cpu_m.intr_pool[INTR_PENDING_MAX-1].intr_next = NULL;

	cp->cpu_m.intr_head[0] = &cp->cpu_m.intr_pool[0];
	cp->cpu_m.intr_tail[0] = &cp->cpu_m.intr_pool[INTR_PENDING_MAX-1];

	if (intr_add_pools != 0) {

		/*
		 * If additional interrupt pools have been allocated,
		 * initialize those too and add them to the free list.
		 */

		struct intr_req *trace;

		for (pool = 0; pool < max_ncpus; pool++) {
			if (!(CPU_IN_SET(intr_add_pools_inuse, pool)))
			    break;
		}
		if (pool >= max_ncpus) {
			/*
			 * XXX - intr pools are alloc'd, just not as
			 * much as we would like.
			 */
			cmn_err(CE_WARN, "Failed to alloc all requested intr "
			    "pools for cpu%d", cp->cpu_id);
			return;
		}
		CPUSET_ADD(intr_add_pools_inuse, pool);
		cp->cpu_m.intr_pool_added = pool;

		trace = (pool * INTR_PENDING_MAX * intr_add_pools) +
			intr_add_head;	/* not byte arithmetic */

		cp->cpu_m.intr_pool[INTR_PENDING_MAX-1].intr_next = trace;

		for (i = 1; i < intr_add_pools * INTR_PENDING_MAX; i++, trace++)
			trace->intr_next = trace + 1;
		trace->intr_next = NULL;

		ASSERT(trace >= intr_add_head && trace <= intr_add_tail);

		cp->cpu_m.intr_tail[0] = trace;
	}
}


/*
 * siron - primitive for sun/os/softint.c
 */
void
siron(void)
{
	if (!siron_pending) {
		siron_pending = 1;
		if (siron_inum != 0)
			setsoftint(siron_inum);
	}
}

/*
 * no_ivintr()
 * 	called by vec_interrupt() through sys_trap()
 *	vector interrupt received but not valid or not
 *	registered in intr_vector[]
 *	considered as a spurious mondo interrupt
 */
/* ARGSUSED */
void
no_ivintr(struct regs *rp, int inum, int pil)
{
	cmn_err(CE_WARN, "invalid vector intr: number 0x%x, pil 0x%x",
	    inum, pil);


#ifdef DEBUG_VEC_INTR
	prom_enter_mon();
#endif /* DEBUG_VEC_INTR */
}

/*
 * no_intr_pool()
 * 	called by vec_interrupt() through sys_trap()
 *	vector interrupt received but no intr_req entries
 */
/* ARGSUSED */
void
no_intr_pool(struct regs *rp, int inum, int pil)
{
#ifdef DEBUG_VEC_INTR
	cmn_err(CE_WARN, "intr_req pool empty: num 0x%x, pil 0x%x",
		inum, pil);
	prom_enter_mon();
#else
	cmn_err(CE_PANIC, "intr_req pool empty: num 0x%x, pil 0x%x",
		inum, pil);
#endif /* DEBUG_VEC_INTR */
}

void
intr_dequeue_req(uint_t pil, uint32_t inum)
{
	struct intr_req *ir, *prev;
	struct machcpu *mcpu;
	uint32_t clr;
	extern uint_t getpstate(void);

	ASSERT((getpstate() & PSTATE_IE) == 0);

	mcpu = &CPU->cpu_m;

	/* Find a matching entry in the list */
	prev = NULL;
	ir = mcpu->intr_head[pil];
	while (ir != NULL) {
		if (ir->intr_number == inum)
			break;
		prev = ir;
		ir = ir->intr_next;
	}
	if (ir != NULL) {
		/*
		 * Remove entry from list
		 */
		if (prev != NULL)
			prev->intr_next = ir->intr_next;	/* non-head */
		else
			mcpu->intr_head[pil] = ir->intr_next;	/* head */

		if (ir->intr_next == NULL)
			mcpu->intr_tail[pil] = prev;		/* tail */

		/*
		 * Place on free list
		 */
		ir->intr_next = mcpu->intr_head[0];
		mcpu->intr_head[0] = ir;
	}

	/*
	 * clear pending interrupts at this level if the list is empty
	 */
	if (mcpu->intr_head[pil] == NULL) {
		clr = 1 << pil;
		if (pil == PIL_14)
			clr |= (TICK_INT_MASK | STICK_INT_MASK);
		wr_clr_softint(clr);
	}
}


/*
 * Send a directed interrupt of specified interrupt number id to a cpu.
 */
void
send_dirint(
	int cpuix,		/* cpu to be interrupted */
	int intr_id)		/* interrupt number id */
{
	xt_one(cpuix, setsoftint_tl1, intr_id, 0);
}

void
init_intr_threads(struct cpu *cp)
{
	int i;

	for (i = 0; i < NINTR_THREADS; i++)
		thread_create_intr(cp);

	cp->cpu_intr_stack = (caddr_t)segkp_get(segkp, INTR_STACK_SIZE,
		KPD_HASREDZONE | KPD_NO_ANON | KPD_LOCKED) +
		INTR_STACK_SIZE - SA(MINFRAME);
}

/*
 * Take the specified CPU out of participation in interrupts.
 *	Called by p_online(2) when a processor is being taken off-line.
 *	This allows interrupt threads being handled on the processor to
 *	complete before the processor is idled.
 */
int
cpu_disable_intr(struct cpu *cp)
{
	ASSERT(MUTEX_HELD(&cpu_lock));

	/*
	 * Turn off the CPU_ENABLE flag before calling the redistribution
	 * function, since it checks for this in the cpu flags.
	 */
	cp->cpu_flags &= ~CPU_ENABLE;

	intr_redist_all_cpus();

	return (0);
}

/*
 * Allow the specified CPU to participate in interrupts.
 *	Called by p_online(2) if a processor could not be taken off-line
 *	because of bound threads, in order to resume processing interrupts.
 *	Also called after starting a processor.
 */
void
cpu_enable_intr(struct cpu *cp)
{
	ASSERT(MUTEX_HELD(&cpu_lock));

	cp->cpu_flags |= CPU_ENABLE;

	intr_redist_all_cpus();
}

/*
 * Add function to callback list for intr_redist_all_cpus.  We keep two lists,
 * one for weighted callbacks and one for normal callbacks. Weighted callbacks
 * are issued to redirect interrupts of a specified weight, from heavy to
 * light.  This allows all the interrupts of a given weight to be redistributed
 * for all weighted nexus drivers prior to those of less weight.
 */
static void
intr_dist_add_list(struct intr_dist **phead, void (*func)(void *), void *arg)
{
	struct intr_dist *new = kmem_alloc(sizeof (*new), KM_SLEEP);
	struct intr_dist *iptr;
	struct intr_dist **pptr;

	ASSERT(func);
	new->func = func;
	new->arg = arg;
	new->next = NULL;

	/* Add to tail so that redistribution occurs in original order. */
	mutex_enter(&intr_dist_lock);
	for (iptr = *phead, pptr = phead; iptr != NULL;
	    pptr = &iptr->next, iptr = iptr->next) {
		/* check for problems as we locate the tail */
		if ((iptr->func == func) && (iptr->arg == arg)) {
			cmn_err(CE_PANIC, "intr_dist_add_list(): duplicate");
			/*NOTREACHED*/
		}
	}
	*pptr = new;

	mutex_exit(&intr_dist_lock);
}

void
intr_dist_add(void (*func)(void *), void *arg)
{
	intr_dist_add_list(&intr_dist_head, (void (*)(void *))func, arg);
}

void
intr_dist_add_weighted(void (*func)(void *, int32_t, int32_t), void *arg)
{
	intr_dist_add_list(&intr_dist_whead, (void (*)(void *))func, arg);
}

/*
 * Search for the interrupt distribution structure with the specified
 * mondo vec reg in the interrupt distribution list. If a match is found,
 * then delete the entry from the list. The caller is responsible for
 * modifying the mondo vector registers.
 */
static void
intr_dist_rem_list(struct intr_dist **headp, void (*func)(void *), void *arg)
{
	struct intr_dist *iptr;
	struct intr_dist **vect;

	mutex_enter(&intr_dist_lock);
	for (iptr = *headp, vect = headp;
	    iptr != NULL; vect = &iptr->next, iptr = iptr->next) {
		if ((iptr->func == func) && (iptr->arg == arg)) {
			*vect = iptr->next;
			kmem_free(iptr, sizeof (struct intr_dist));
			mutex_exit(&intr_dist_lock);
			return;
		}
	}

	if (!panicstr)
		cmn_err(CE_PANIC, "intr_dist_rem_list: not found");
	mutex_exit(&intr_dist_lock);
}

void
intr_dist_rem(void (*func)(void *), void *arg)
{
	intr_dist_rem_list(&intr_dist_head, (void (*)(void *))func, arg);
}

void
intr_dist_rem_weighted(void (*func)(void *, int32_t, int32_t), void *arg)
{
	intr_dist_rem_list(&intr_dist_whead, (void (*)(void *))func, arg);
}

/*
 * Initiate interrupt redistribution.  Redistribution improves the isolation
 * associated with interrupt weights by ordering operations from heavy weight
 * to light weight.  When a CPUs orientation changes relative to interrupts,
 * there is *always* a redistribution to accommodate this change (call to
 * intr_redist_all_cpus()).  As devices (not CPUs) attach/detach it is possible
 * that a redistribution could improve the quality of an initialization. For
 * example, if you are not using a NIC it may not be attached with s10 (devfs).
 * If you then configure the NIC (ifconfig), this may cause the NIC to attach
 * and plumb interrupts.  The CPU assignment for the NIC's interrupts is
 * occurring late, so optimal "isolation" relative to weight is not occurring.
 * The same applies to detach, although in this case doing the redistribution
 * might improve "spread" for medium weight devices since the "isolation" of
 * a higher weight device may no longer be present.
 *
 * NB: We should provide a utility to trigger redistribution (ala "intradm -r").
 *
 * NB: There is risk associated with automatically triggering execution of the
 * redistribution code at arbitrary times. The risk comes from the fact that
 * there is a lot of low-level hardware interaction associated with a
 * redistribution.  At some point we may want this code to perform automatic
 * redistribution (redistribution thread; trigger timeout when add/remove
 * weight delta is large enough, and call cv_signal from timeout - causing
 * thead to call i_ddi_intr_redist_all_cpus()) but this is considered too
 * risky at this time.
 */
void
i_ddi_intr_redist_all_cpus()
{
	mutex_enter(&cpu_lock);
	INTR_DEBUG((CE_CONT, "intr_dist: i_ddi_intr_redist_all_cpus\n"));
	intr_redist_all_cpus();
	mutex_exit(&cpu_lock);
}

/*
 * Redistribute all interrupts
 *
 * This function redistributes all interrupting devices, running the
 * parent callback functions for each node.
 */
void
intr_redist_all_cpus(void)
{
	struct cpu *cp;
	struct intr_dist *iptr;
	int32_t weight, max_weight;

	ASSERT(MUTEX_HELD(&cpu_lock));
	mutex_enter(&intr_dist_lock);

	/*
	 * zero cpu_intr_weight on all cpus - it is safe to traverse
	 * cpu_list since we hold cpu_lock.
	 */
	cp = cpu_list;
	do {
		cp->cpu_intr_weight = 0;
	} while ((cp = cp->cpu_next) != cpu_list);

	/*
	 * Assume that this redistribution may encounter a device weight
	 * via driver.conf tuning of "ddi-intr-weight" that is at most
	 * intr_dist_weight_maxfactor times larger.
	 */
	max_weight = intr_dist_weight_max * intr_dist_weight_maxfactor;
	if (max_weight > intr_dist_weight_maxmax)
		max_weight = intr_dist_weight_maxmax;
	intr_dist_weight_max = 1;

	INTR_DEBUG((CE_CONT, "intr_dist: "
	    "intr_redist_all_cpus: %d-0\n", max_weight));

	/*
	 * Redistribute weighted, from heavy to light.  The callback that
	 * specifies a weight equal to weight_max should redirect all
	 * interrupts of weight weight_max or greater [weight_max, inf.).
	 * Interrupts of lesser weight should be processed on the call with
	 * the matching weight. This allows all the heaver weight interrupts
	 * on all weighted busses (multiple pci busses) to be redirected prior
	 * to any lesser weight interrupts.
	 */
	for (weight = max_weight; weight >= 0; weight--)
		for (iptr = intr_dist_whead; iptr != NULL; iptr = iptr->next)
			((void (*)(void *, int32_t, int32_t))iptr->func)
			    (iptr->arg, max_weight, weight);

	/* redistribute normal (non-weighted) interrupts */
	for (iptr = intr_dist_head; iptr != NULL; iptr = iptr->next)
		((void (*)(void *))iptr->func)(iptr->arg);
	mutex_exit(&intr_dist_lock);
}

void
intr_redist_all_cpus_shutdown(void)
{
	intr_policy = INTR_CURRENT_CPU;
	intr_redist_all_cpus();
}

/*
 * Determine what CPU to target, based on interrupt policy.
 *
 * INTR_FLAT_DIST: hold a current CPU pointer in a static variable and
 *	advance through interrupt enabled cpus (round-robin).
 *
 * INTR_WEIGHTED_DIST: search for an enabled CPU with the lowest
 *	cpu_intr_weight, round robin when all equal.
 *
 *	Weighted interrupt distribution provides two things: "spread" of weight
 *	(associated with algorithm itself) and "isolation" (associated with a
 *	particular device weight). A redistribution is what provides optimal
 *	"isolation" of heavy weight interrupts, optimal "spread" of weight
 *	(relative to what came before) is always occurring.
 *
 *	An interrupt weight is a subjective number that represents the
 *	percentage of a CPU required to service a device's interrupts: the
 *	default weight is 0% (however the algorithm still maintains
 *	round-robin), a network interface controller (NIC) may have a large
 *	weight (35%). Interrupt weight only has meaning relative to the
 *	interrupt weight of other devices: a CPU can be weighted more than
 *	100%, and a single device might consume more than 100% of a CPU.
 *
 *	A coarse interrupt weight can be defined by the parent nexus driver
 *	based on bus specific information, like pci class codes. A nexus
 *	driver that supports device interrupt weighting for its children
 *	should call intr_dist_cpuid_add/rem_device_weight(), which adds
 *	and removes the weight of a device from the CPU that an interrupt
 *	is directed at.  The quality of initialization improves when the
 *	device interrupt weights more accuracy reflect actual run-time weights,
 *	and as the assignments are ordered from is heavy to light.
 *
 *	The implementation also supports interrupt weight being specified in
 *	driver.conf files via the property "ddi-intr-weight", which takes
 *	precedence over the nexus supplied weight.  This support is added to
 *	permit possible tweaking in the product in response to customer
 *	problems. This is not a formal or committed interface.
 *
 *	While a weighted approach chooses the CPU providing the best spread
 *	given past weights, less than optimal isolation can result in cases
 *	where heavy weight devices show up last. The nexus driver's interrupt
 *	redistribution logic should use intr_dist_add/rem_weighted so that
 *	interrupts can be redistributed heavy first for optimal isolation.
 */
uint32_t
intr_dist_cpuid(void)
{
	static struct cpu	*curr_cpu;
	struct cpu		*start_cpu;
	struct cpu		*new_cpu;
	struct cpu		*cp;
	int			cpuid = -1;

	/* Establish exclusion for curr_cpu and cpu_intr_weight manipulation */
	mutex_enter(&intr_dist_cpu_lock);

	switch (intr_policy) {
	case INTR_CURRENT_CPU:
		cpuid = CPU->cpu_id;
		break;

	case INTR_BOOT_CPU:
		panic("INTR_BOOT_CPU no longer supported.");
		/*NOTREACHED*/

	case INTR_FLAT_DIST:
	case INTR_WEIGHTED_DIST:
	default:
		/*
		 * Ensure that curr_cpu is valid - cpu_next will be NULL if
		 * the cpu has been deleted (cpu structs are never freed).
		 */
		if (curr_cpu == NULL || curr_cpu->cpu_next == NULL)
			curr_cpu = CPU;

		/*
		 * Advance to online CPU after curr_cpu (round-robin). For
		 * INTR_WEIGHTED_DIST we choose the cpu with the lightest
		 * weight.  For a nexus that does not support weight the
		 * default weight of zero is used. We degrade to round-robin
		 * behavior among equal weightes.  The default weight is zero
		 * and round-robin behavior continues.
		 *
		 * Disable preemption while traversing cpu_next_onln to
		 * ensure the list does not change.  This works because
		 * modifiers of this list and other lists in a struct cpu
		 * call pause_cpus() before making changes.
		 */
		kpreempt_disable();
		cp = start_cpu = curr_cpu->cpu_next_onln;
		new_cpu = NULL;
		do {
			/* Skip CPUs with interrupts disabled */
			if ((cp->cpu_flags & CPU_ENABLE) == 0)
				continue;

			if (intr_policy == INTR_FLAT_DIST) {
				/* select CPU */
				new_cpu = cp;
				break;
			} else if ((new_cpu == NULL) ||
			    (cp->cpu_intr_weight < new_cpu->cpu_intr_weight)) {
				/* Choose if lighter weight */
				new_cpu = cp;
			}
		} while ((cp = cp->cpu_next_onln) != start_cpu);
		ASSERT(new_cpu);
		cpuid = new_cpu->cpu_id;

		INTR_DEBUG((CE_CONT, "intr_dist: cpu %2d weight %3d: "
		    "targeted\n", cpuid, new_cpu->cpu_intr_weight));

		/* update static pointer for next round-robin */
		curr_cpu = new_cpu;
		kpreempt_enable();
		break;
	}
	mutex_exit(&intr_dist_cpu_lock);
	return (cpuid);
}

/*
 * Add or remove the the weight of a device from a CPUs interrupt weight.
 *
 * We expect nexus drivers to call intr_dist_cpuid_add/rem_device_weight for
 * their children to improve the overall quality of interrupt initialization.
 *
 * If a nexues shares the CPU returned by a single intr_dist_cpuid() call
 * among multiple devices (sharing ino) then the nexus should call
 * intr_dist_cpuid_add/rem_device_weight for each device separately. Devices
 * that share must specify the same cpuid.
 *
 * If a nexus driver is unable to determine the cpu at remove_intr time
 * for some of its interrupts, then it should not call add_device_weight -
 * intr_dist_cpuid will still provide round-robin.
 *
 * An established device weight (from dev_info node) takes precedence over
 * the weight passed in.  If a device weight is not already established
 * then the passed in nexus weight is established.
 */
void
intr_dist_cpuid_add_device_weight(uint32_t cpuid,
    dev_info_t *dip, int32_t nweight)
{
	int32_t		eweight;

	/*
	 * For non-weighted policy everything has weight of zero (and we get
	 * round-robin distribution from intr_dist_cpuid).
	 * NB: intr_policy is limited to this file. A weighted nexus driver is
	 * calls this rouitne even if intr_policy has been patched to
	 * INTR_FLAG_DIST.
	 */
	ASSERT(dip);
	if (intr_policy != INTR_WEIGHTED_DIST)
		return;

	eweight = i_ddi_get_intr_weight(dip);
	INTR_DEBUG((CE_CONT, "intr_dist: cpu %2d weight %3d: +%2d/%2d for "
	    "%s#%d/%s#%d\n", cpuid, cpu[cpuid]->cpu_intr_weight,
	    nweight, eweight, ddi_driver_name(ddi_get_parent(dip)),
	    ddi_get_instance(ddi_get_parent(dip)),
	    ddi_driver_name(dip), ddi_get_instance(dip)));

	/* if no establish weight, establish nexus weight */
	if (eweight < 0) {
		if (nweight > 0)
			(void) i_ddi_set_intr_weight(dip, nweight);
		else
			nweight = 0;
	} else
		nweight = eweight;	/* use established weight */

	/* Establish exclusion for cpu_intr_weight manipulation */
	mutex_enter(&intr_dist_cpu_lock);
	cpu[cpuid]->cpu_intr_weight += nweight;

	/* update intr_dist_weight_max */
	if (nweight > intr_dist_weight_max)
		intr_dist_weight_max = nweight;
	mutex_exit(&intr_dist_cpu_lock);
}

void
intr_dist_cpuid_rem_device_weight(uint32_t cpuid, dev_info_t *dip)
{
	struct cpu	*cp;
	int32_t		weight;

	ASSERT(dip);
	if (intr_policy != INTR_WEIGHTED_DIST)
		return;

	/* remove weight of device from cpu */
	weight = i_ddi_get_intr_weight(dip);
	if (weight < 0)
		weight = 0;
	INTR_DEBUG((CE_CONT, "intr_dist: cpu %2d weight %3d: -%2d    for "
	    "%s#%d/%s#%d\n", cpuid, cpu[cpuid]->cpu_intr_weight, weight,
	    ddi_driver_name(ddi_get_parent(dip)),
	    ddi_get_instance(ddi_get_parent(dip)),
	    ddi_driver_name(dip), ddi_get_instance(dip)));

	/* Establish exclusion for cpu_intr_weight manipulation */
	mutex_enter(&intr_dist_cpu_lock);
	cp = cpu[cpuid];
	cp->cpu_intr_weight -= weight;
	if (cp->cpu_intr_weight < 0)
		cp->cpu_intr_weight = 0;	/* sanity */
	mutex_exit(&intr_dist_cpu_lock);
}
