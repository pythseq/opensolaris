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
 * Copyright 2004 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#pragma ident	"%Z%%M%	%I%	%E% SMI"

#include <sys/atomic.h>
#include <sys/cmn_err.h>
#include <sys/exacct.h>
#include <sys/id_space.h>
#include <sys/kmem.h>
#include <sys/modhash.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/project.h>
#include <sys/rctl.h>
#include <sys/systm.h>
#include <sys/task.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/zone.h>
#include <sys/cpuvar.h>
#include <sys/fss.h>
#include <sys/class.h>
#include <sys/project.h>

/*
 * Tasks
 *
 *   A task is a collection of processes, associated with a common project ID
 *   and related by a common initial parent.  The task primarily represents a
 *   natural process sequence with known resource usage, although it can also be
 *   viewed as a convenient grouping of processes for signal delivery, processor
 *   binding, and administrative operations.
 *
 * Membership and observership
 *   We can conceive of situations where processes outside of the task may wish
 *   to examine the resource usage of the task.  Similarly, a number of the
 *   administrative operations on a task can be performed by processes who are
 *   not members of the task.  Accordingly, we must design a locking strategy
 *   where observers of the task, who wish to examine or operate on the task,
 *   and members of task, who can perform the mentioned operations, as well as
 *   leave the task, see a consistent and correct representation of the task at
 *   all times.
 *
 * Locking
 *   Because the task membership is a new relation between processes, its
 *   locking becomes an additional responsibility of the pidlock/p_lock locking
 *   sequence; however, tasks closely resemble sessions and the session locking
 *   model is mostly appropriate for the interaction of tasks, processes, and
 *   procfs.
 *
 *   kmutex_t task_hash_lock
 *     task_hash_lock is a global lock protecting the contents of the task
 *     ID-to-task pointer hash.  Holders of task_hash_lock must not attempt to
 *     acquire pidlock or p_lock.
 *   uint_t tk_hold_count
 *     tk_hold_count, the number of members and observers of the current task,
 *     must be manipulated atomically.
 *   proc_t *tk_memb_list
 *   proc_t *p_tasknext
 *   proc_t *p_taskprev
 *     The task's membership list is protected by pidlock, and is therefore
 *     always acquired before any of its members' p_lock mutexes.  The p_task
 *     member of the proc structure is protected by pidlock or p_lock for
 *     reading, and by both pidlock and p_lock for modification, as is done for
 *     p_sessp.  The key point is that only the process can modify its p_task,
 *     and not any entity on the system.  (/proc will use prlock() to prevent
 *     the process from leaving, as opposed to pidlock.)
 *   kmutex_t tk_usage_lock
 *     tk_usage_lock is a per-task lock protecting the contents of the task
 *     usage structure and tk_nlwps counter for the task.max-lwps resource
 *     control.
 */

int task_hash_size = 256;
static kmutex_t task_hash_lock;
static mod_hash_t *task_hash;

static id_space_t *taskid_space;	/* global taskid space */
static kmem_cache_t *task_cache;	/* kmem cache for task structures */

rctl_hndl_t rc_task_lwps;
rctl_hndl_t rc_task_cpu_time;

/*
 * static rctl_qty_t task_usage_lwps(void *taskp)
 *
 * Overview
 *   task_usage_lwps() is the usage operation for the resource control
 *   associated with the number of LWPs in a task.
 *
 * Return values
 *   The number of LWPs in the given task is returned.
 *
 * Caller's context
 *   The p->p_lock must be held across the call.
 */
/*ARGSUSED*/
static rctl_qty_t
task_lwps_usage(rctl_t *r, proc_t *p)
{
	task_t *t;
	rctl_qty_t nlwps;

	ASSERT(MUTEX_HELD(&p->p_lock));

	t = p->p_task;
	mutex_enter(&p->p_zone->zone_nlwps_lock);
	nlwps = t->tk_nlwps;
	mutex_exit(&p->p_zone->zone_nlwps_lock);

	return (nlwps);
}

/*
 * static int task_test_lwps(void *taskp, rctl_val_t *, int64_t incr,
 *   int flags)
 *
 * Overview
 *   task_test_lwps() is the test-if-valid-increment for the resource control
 *   for the number of processes in a task.
 *
 * Return values
 *   0 if the threshold limit was not passed, 1 if the limit was passed.
 *
 * Caller's context
 *   p->p_lock must be held across the call.
 */
/*ARGSUSED*/
static int
task_lwps_test(rctl_t *r, proc_t *p, rctl_entity_p_t *e, rctl_val_t *rcntl,
    rctl_qty_t incr,
    uint_t flags)
{
	rctl_qty_t nlwps;

	ASSERT(MUTEX_HELD(&p->p_lock));
	ASSERT(e->rcep_t == RCENTITY_TASK);
	if (e->rcep_p.task == NULL)
		return (0);

	ASSERT(MUTEX_HELD(&(e->rcep_p.task->tk_zone->zone_nlwps_lock)));
	nlwps = e->rcep_p.task->tk_nlwps;

	if (nlwps + incr > rcntl->rcv_value)
		return (1);

	return (0);
}
/*ARGSUSED*/
static int
task_lwps_set(rctl_t *rctl, struct proc *p, rctl_entity_p_t *e, rctl_qty_t nv) {

	ASSERT(MUTEX_HELD(&p->p_lock));
	ASSERT(e->rcep_t == RCENTITY_TASK);
	if (e->rcep_p.task == NULL)
		return (0);

	e->rcep_p.task->tk_nlwps_ctl = nv;
	return (0);
}

/*
 * static rctl_qty_t task_usage_cpu_secs(void *taskp)
 *
 * Overview
 *   task_usage_cpu_secs() is the usage operation for the resource control
 *   associated with the total accrued CPU seconds for a task.
 *
 * Return values
 *   The number of CPU seconds consumed by the task is returned.
 *
 * Caller's context
 *   The given task must be held across the call.
 */
/*ARGSUSED*/
static rctl_qty_t
task_cpu_time_usage(rctl_t *r, proc_t *p)
{
	task_t *t = p->p_task;

	ASSERT(MUTEX_HELD(&p->p_lock));
	return (t->tk_cpu_time / hz);
}

/*
 * static int task_test_cpu_secs(void *taskp, rctl_val_t *, int64_t incr,
 *   int flags)
 *
 * Overview
 *   task_test_cpu_secs() is the test-if-valid-increment for the resource
 *   control for the total accrued CPU seconds for a task.
 *
 * Return values
 *   0 if the threshold limit was not passed, 1 if the limit was passed.
 *
 * Caller's context
 *   The given task must be held across the call.
 */
/*ARGSUSED*/
static int
task_cpu_time_test(rctl_t *r, proc_t *p, rctl_entity_p_t *e,
    struct rctl_val *rcntl, rctl_qty_t incr, uint_t flags)
{
	task_t *t;

	ASSERT(MUTEX_HELD(&p->p_lock));
	ASSERT(e->rcep_t == RCENTITY_TASK);
	if (e->rcep_p.task == NULL)
		return (0);

	t = e->rcep_p.task;
	if ((t->tk_cpu_time + incr) / hz >= rcntl->rcv_value)
		return (1);

	return (0);
}

static task_t *
task_find(taskid_t id, zoneid_t zoneid)
{
	task_t *tk;

	ASSERT(MUTEX_HELD(&task_hash_lock));

	if (mod_hash_find(task_hash, (mod_hash_key_t)(uintptr_t)id,
	    (mod_hash_val_t *)&tk) == MH_ERR_NOTFOUND ||
	    (zoneid != ALL_ZONES && zoneid != tk->tk_zone->zone_id))
		return (NULL);

	return (tk);
}

/*
 * task_hold_by_id(), task_hold_by_id_zone()
 *
 * Overview
 *   task_hold_by_id() is used to take a reference on a task by its task id,
 *   supporting the various system call interfaces for obtaining resource data,
 *   delivering signals, and so forth.
 *
 * Return values
 *   Returns a pointer to the task_t with taskid_t id.  The task is returned
 *   with its hold count incremented by one.  Returns NULL if there
 *   is no task with the requested id.
 *
 * Caller's context
 *   Caller must not be holding task_hash_lock.  No restrictions on context.
 */
task_t *
task_hold_by_id_zone(taskid_t id, zoneid_t zoneid)
{
	task_t *tk;

	mutex_enter(&task_hash_lock);
	if ((tk = task_find(id, zoneid)) != NULL)
		atomic_add_32(&tk->tk_hold_count, 1);
	mutex_exit(&task_hash_lock);

	return (tk);
}

task_t *
task_hold_by_id(taskid_t id)
{
	zoneid_t zoneid;

	if (INGLOBALZONE(curproc))
		zoneid = ALL_ZONES;
	else
		zoneid = getzoneid();
	return (task_hold_by_id_zone(id, zoneid));
}

/*
 * void task_hold(task_t *)
 *
 * Overview
 *   task_hold() is used to take an additional reference to the given task.
 *
 * Return values
 *   None.
 *
 * Caller's context
 *   No restriction on context.
 */
void
task_hold(task_t *tk)
{
	atomic_add_32(&tk->tk_hold_count, 1);
}

/*
 * void task_rele(task_t *)
 *
 * Overview
 *   task_rele() relinquishes a reference on the given task, which was acquired
 *   via task_hold() or task_hold_by_id().  If this is the last member or
 *   observer of the task, dispatch it for commitment via the accounting
 *   subsystem.
 *
 * Return values
 *   None.
 *
 * Caller's context
 *   Caller must not be holding the task_hash_lock.
 *   Caller's context must be acceptable for KM_SLEEP allocations.
 */
void
task_rele(task_t *tk)
{
	mutex_enter(&task_hash_lock);
	if (atomic_add_32_nv(&tk->tk_hold_count, -1) > 0) {
		mutex_exit(&task_hash_lock);
		return;
	}

	mutex_enter(&tk->tk_zone->zone_nlwps_lock);
	tk->tk_proj->kpj_ntasks--;
	mutex_exit(&tk->tk_zone->zone_nlwps_lock);

	if (mod_hash_destroy(task_hash,
	    (mod_hash_key_t)(uintptr_t)tk->tk_tkid) != 0)
		panic("unable to delete task %d", tk->tk_tkid);
	mutex_exit(&task_hash_lock);

	/*
	 * At this point, there are no members or observers of the task, so we
	 * can safely send it on for commitment to the accounting subsystem.
	 * The task will be destroyed in task_end() subsequent to commitment.
	 */
	(void) taskq_dispatch(exacct_queue, exacct_commit_task, tk, KM_SLEEP);
}

/*
 * task_t *task_create(projid_t, zone *)
 *
 * Overview
 *   A process constructing a new task calls task_create() to construct and
 *   preinitialize the task for the appropriate destination project.  Only one
 *   task, the primordial task0, is not created with task_create().
 *
 * Return values
 *   None.
 *
 * Caller's context
 *   Caller's context should be safe for KM_SLEEP allocations.
 *   The caller should appropriately bump the kpj_ntasks counter on the
 *   project that contains this task.
 */
task_t *
task_create(projid_t projid, zone_t *zone)
{
	task_t *tk = kmem_cache_alloc(task_cache, KM_SLEEP);
	task_t *ancestor_tk;
	taskid_t tkid;
	task_usage_t *tu = kmem_zalloc(sizeof (task_usage_t), KM_SLEEP);
	mod_hash_hndl_t hndl;
	rctl_set_t *set = rctl_set_create();
	rctl_alloc_gp_t *gp;
	rctl_entity_p_t e;

	bzero(tk, sizeof (task_t));

	tk->tk_tkid = tkid = id_alloc(taskid_space);
	tk->tk_nlwps = 0;
	tk->tk_nlwps_ctl = INT_MAX;
	tk->tk_usage = tu;
	tk->tk_proj = project_hold_by_id(projid, zone->zone_id,
	    PROJECT_HOLD_INSERT);
	tk->tk_flags = TASK_NORMAL;

	/*
	 * Copy ancestor task's resource controls.
	 */
	zone_task_hold(zone);
	mutex_enter(&curproc->p_lock);
	ancestor_tk = curproc->p_task;
	task_hold(ancestor_tk);
	tk->tk_zone = zone;
	mutex_exit(&curproc->p_lock);

	for (;;) {
		gp = rctl_set_dup_prealloc(ancestor_tk->tk_rctls);

		mutex_enter(&ancestor_tk->tk_rctls->rcs_lock);
		if (rctl_set_dup_ready(ancestor_tk->tk_rctls, gp))
			break;

		mutex_exit(&ancestor_tk->tk_rctls->rcs_lock);

		rctl_prealloc_destroy(gp);
	}

	/*
	 * At this point, curproc does not have the appropriate linkage
	 * through the task to the project. So, rctl_set_dup should only
	 * copy the rctls, and leave the callbacks for later.
	 */
	e.rcep_p.task = tk;
	e.rcep_t = RCENTITY_TASK;
	tk->tk_rctls = rctl_set_dup(ancestor_tk->tk_rctls, curproc, curproc, &e,
	    set, gp, RCD_DUP);
	mutex_exit(&ancestor_tk->tk_rctls->rcs_lock);

	rctl_prealloc_destroy(gp);

	/*
	 * Record the ancestor task's ID for use by extended accounting.
	 */
	tu->tu_anctaskid = ancestor_tk->tk_tkid;
	task_rele(ancestor_tk);

	/*
	 * Put new task structure in the hash table.
	 */
	(void) mod_hash_reserve(task_hash, &hndl);
	mutex_enter(&task_hash_lock);
	ASSERT(task_find(tkid, getzoneid()) == NULL);
	if (mod_hash_insert_reserve(task_hash, (mod_hash_key_t)(uintptr_t)tkid,
	    (mod_hash_val_t *)tk, hndl) != 0) {
		mod_hash_cancel(task_hash, &hndl);
		panic("unable to insert task %d(%p)", tkid, (void *)tk);
	}
	mutex_exit(&task_hash_lock);

	return (tk);
}

/*
 * void task_attach(task_t *, proc_t *)
 *
 * Overview
 *   task_attach() is used to attach a process to a task; this operation is only
 *   performed as a result of a fork() or settaskid() system call.  The proc_t's
 *   p_tasknext and p_taskprev fields will be set such that the proc_t is a
 *   member of the doubly-linked list of proc_t's that make up the task.
 *
 * Return values
 *   None.
 *
 * Caller's context
 *   pidlock and p->p_lock must be held on entry.
 */
void
task_attach(task_t *tk, proc_t *p)
{
	proc_t *first, *prev;
	rctl_entity_p_t e;
	ASSERT(tk != NULL);
	ASSERT(p != NULL);
	ASSERT(MUTEX_HELD(&pidlock));
	ASSERT(MUTEX_HELD(&p->p_lock));

	if (tk->tk_memb_list == NULL) {
		p->p_tasknext = p;
		p->p_taskprev = p;
	} else {
		first = tk->tk_memb_list;
		prev = first->p_taskprev;
		first->p_taskprev = p;
		p->p_tasknext = first;
		p->p_taskprev = prev;
		prev->p_tasknext = p;
	}
	tk->tk_memb_list = p;
	task_hold(tk);
	p->p_task = tk;

	/*
	 * Now that the linkage from process to task and project is
	 * complete, do the required callbacks for the task and project
	 * rctl sets.
	 */
	e.rcep_p.proj = tk->tk_proj;
	e.rcep_t = RCENTITY_PROJECT;
	(void) rctl_set_dup(NULL, NULL, p, &e, tk->tk_proj->kpj_rctls, NULL,
	    RCD_CALLBACK);

	e.rcep_p.task = tk;
	e.rcep_t = RCENTITY_TASK;
	(void) rctl_set_dup(NULL, NULL, p, &e, tk->tk_rctls, NULL,
	    RCD_CALLBACK);

}

/*
 * task_begin()
 *
 * Overview
 *   A process constructing a new task calls task_begin() to initialize the
 *   task, by attaching itself as a member.
 *
 * Return values
 *   None.
 *
 * Caller's context
 *   pidlock and p_lock must be held across the call to task_begin().
 */
void
task_begin(task_t *tk, proc_t *p)
{
	timestruc_t ts;
	task_usage_t *tu;

	ASSERT(MUTEX_HELD(&pidlock));
	ASSERT(MUTEX_HELD(&p->p_lock));

	mutex_enter(&tk->tk_usage_lock);
	tu = tk->tk_usage;
	gethrestime(&ts);
	tu->tu_startsec = (uint64_t)ts.tv_sec;
	tu->tu_startnsec = (uint64_t)ts.tv_nsec;
	mutex_exit(&tk->tk_usage_lock);

	/*
	 * Join process to the task as a member.
	 */
	task_attach(tk, p);
}

/*
 * void task_detach(proc_t *)
 *
 * Overview
 *   task_detach() removes the specified process from its task.  task_detach
 *   sets the process's task membership to NULL, in anticipation of a final exit
 *   or of joining a new task.  Because task_rele() requires a context safe for
 *   KM_SLEEP allocations, a task_detach() is followed by a subsequent
 *   task_rele() once appropriate context is available.
 *
 *   Because task_detach() involves relinquishing the process's membership in
 *   the project, any observational rctls the process may have had on the task
 *   or project are destroyed.
 *
 * Return values
 *   None.
 *
 * Caller's context
 *   pidlock and p_lock held across task_detach().
 */
void
task_detach(proc_t *p)
{
	task_t *tk = p->p_task;

	ASSERT(MUTEX_HELD(&pidlock));
	ASSERT(MUTEX_HELD(&p->p_lock));
	ASSERT(p->p_task != NULL);
	ASSERT(tk->tk_memb_list != NULL);

	if (tk->tk_memb_list == p)
		tk->tk_memb_list = p->p_tasknext;
	if (tk->tk_memb_list == p)
		tk->tk_memb_list = NULL;
	p->p_taskprev->p_tasknext = p->p_tasknext;
	p->p_tasknext->p_taskprev = p->p_taskprev;

	rctl_set_tearoff(p->p_task->tk_rctls, p);
	rctl_set_tearoff(p->p_task->tk_proj->kpj_rctls, p);

	p->p_task = NULL;
	p->p_tasknext = p->p_taskprev = NULL;
}

/*
 * task_change(task_t *, proc_t *)
 *
 * Overview
 *   task_change() removes the specified process from its current task.  The
 *   process is then attached to the specified task.  This routine is called
 *   from settaskid() when process is being moved to a new task.
 *
 * Return values
 *   None.
 *
 * Caller's context
 *   pidlock and p_lock held across task_change()
 */
void
task_change(task_t *newtk, proc_t *p)
{
	task_t *oldtk = p->p_task;

	ASSERT(MUTEX_HELD(&pidlock));
	ASSERT(MUTEX_HELD(&p->p_lock));
	ASSERT(oldtk != NULL);
	ASSERT(oldtk->tk_memb_list != NULL);

	mutex_enter(&p->p_zone->zone_nlwps_lock);
	oldtk->tk_nlwps -= p->p_lwpcnt;
	mutex_exit(&p->p_zone->zone_nlwps_lock);

	mutex_enter(&newtk->tk_zone->zone_nlwps_lock);
	newtk->tk_nlwps += p->p_lwpcnt;
	mutex_exit(&newtk->tk_zone->zone_nlwps_lock);

	task_detach(p);
	task_begin(newtk, p);
}

/*
 * task_end()
 *
 * Overview
 *   task_end() contains the actions executed once the final member of
 *   a task has released the task, and all actions connected with the task, such
 *   as committing an accounting record to a file, are completed.  It is called
 *   by the known last consumer of the task information.  Additionally,
 *   task_end() must never refer to any process in the system.
 *
 * Return values
 *   None.
 *
 * Caller's context
 *   No restrictions on context, beyond that given above.
 */
void
task_end(task_t *tk)
{
	ASSERT(tk->tk_hold_count == 0);

	project_rele(tk->tk_proj);
	kmem_free(tk->tk_usage, sizeof (task_usage_t));
	if (tk->tk_prevusage != NULL)
		kmem_free(tk->tk_prevusage, sizeof (task_usage_t));
	if (tk->tk_zoneusage != NULL)
		kmem_free(tk->tk_zoneusage, sizeof (task_usage_t));
	rctl_set_free(tk->tk_rctls);
	id_free(taskid_space, tk->tk_tkid);
	zone_task_rele(tk->tk_zone);
	kmem_cache_free(task_cache, tk);
}

static void
changeproj(proc_t *p, kproject_t *kpj, zone_t *zone, void *projbuf,
    void *zonebuf)
{
	kproject_t *oldkpj;
	kthread_t *t;

	ASSERT(MUTEX_HELD(&pidlock));
	ASSERT(MUTEX_HELD(&p->p_lock));

	if ((t = p->p_tlist) != NULL) {
		do {
			(void) project_hold(kpj);

			thread_lock(t);
			oldkpj = ttoproj(t);
			t->t_proj = kpj;
			t->t_pre_sys = 1;		/* For cred update */
			thread_unlock(t);
			fss_changeproj(t, kpj, zone, projbuf, zonebuf);

			project_rele(oldkpj);
		} while ((t = t->t_forw) != p->p_tlist);
	}
}

/*
 * task_join()
 *
 * Overview
 *   task_join() contains the actions that must be executed when the first
 *   member (curproc) of a newly created task joins it.  It may never fail.
 *
 *   The caller must make sure holdlwps() is called so that all other lwps are
 *   stopped prior to calling this function.
 *
 *   NB: It returns with curproc->p_lock held.
 *
 * Return values
 *   Pointer to the old task.
 *
 * Caller's context
 *   cpu_lock must be held entering the function.  It will acquire pidlock,
 *   p_crlock and p_lock during execution.
 */
task_t *
task_join(task_t *tk, uint_t flags)
{
	proc_t *p = ttoproc(curthread);
	task_t *prev_tk;
	void *projbuf, *zonebuf;
	zone_t *zone = tk->tk_zone;
	projid_t projid = tk->tk_proj->kpj_id;
	cred_t *oldcr;

	/*
	 * We can't know for sure if holdlwps() was called, but we can check to
	 * ensure we're single-threaded.
	 */
	ASSERT(curthread == p->p_agenttp || p->p_lwprcnt == 1);

	/*
	 * Changing the credential is always hard because we cannot
	 * allocate memory when holding locks but we don't know whether
	 * we need to change it.  We first get a reference to the current
	 * cred if we need to change it.  Then we create a credential
	 * with an updated project id.  Finally we install it, first
	 * releasing the reference we had on the p_cred at the time we
	 * acquired the lock the first time and later we release the
	 * reference to p_cred at the time we acquired the lock the
	 * second time.
	 */
	mutex_enter(&p->p_crlock);
	if (crgetprojid(p->p_cred) == projid)
		oldcr = NULL;
	else
		crhold(oldcr = p->p_cred);
	mutex_exit(&p->p_crlock);

	if (oldcr != NULL) {
		cred_t *newcr = crdup(oldcr);
		crsetprojid(newcr, projid);
		crfree(oldcr);

		mutex_enter(&p->p_crlock);
		oldcr = p->p_cred;
		p->p_cred = newcr;
		mutex_exit(&p->p_crlock);
		crfree(oldcr);
	}

	/*
	 * Make sure that the number of processor sets is constant
	 * across this operation.
	 */
	ASSERT(MUTEX_HELD(&cpu_lock));

	projbuf = fss_allocbuf(FSS_NPSET_BUF, FSS_ALLOC_PROJ);
	zonebuf = fss_allocbuf(FSS_NPSET_BUF, FSS_ALLOC_ZONE);

	mutex_enter(&pidlock);
	mutex_enter(&p->p_lock);

	prev_tk = p->p_task;
	task_change(tk, p);

	/*
	 * Now move threads one by one to their new project.
	 */
	changeproj(p, tk->tk_proj, zone, projbuf, zonebuf);
	if (flags & TASK_FINAL)
		p->p_task->tk_flags |= TASK_FINAL;

	mutex_exit(&pidlock);

	fss_freebuf(zonebuf, FSS_ALLOC_ZONE);
	fss_freebuf(projbuf, FSS_ALLOC_PROJ);
	return (prev_tk);
}

/*
 * rctl ops vectors
 */
static rctl_ops_t task_lwps_ops = {
	rcop_no_action,
	task_lwps_usage,
	task_lwps_set,
	task_lwps_test
};

static rctl_ops_t task_cpu_time_ops = {
	rcop_no_action,
	task_cpu_time_usage,
	rcop_no_set,
	task_cpu_time_test
};

/*ARGSUSED*/
/*
 * void task_init(void)
 *
 * Overview
 *   task_init() initializes task-related hashes, caches, and the task id
 *   space.  Additionally, task_init() establishes p0 as a member of task0.
 *   Called by main().
 *
 * Return values
 *   None.
 *
 * Caller's context
 *   task_init() must be called prior to MP startup.
 */
void
task_init(void)
{
	proc_t *p = &p0;
	mod_hash_hndl_t hndl;
	rctl_set_t *set;
	rctl_alloc_gp_t *gp;
	rctl_entity_p_t e;
	/*
	 * Initialize task_cache and taskid_space.
	 */
	task_cache = kmem_cache_create("task_cache", sizeof (task_t),
	    0, NULL, NULL, NULL, NULL, NULL, 0);
	taskid_space = id_space_create("taskid_space", 0, MAX_TASKID);

	/*
	 * Initialize task hash table.
	 */
	task_hash = mod_hash_create_idhash("task_hash", task_hash_size,
	    mod_hash_null_valdtor);

	/*
	 * Initialize task-based rctls.
	 */
	rc_task_lwps = rctl_register("task.max-lwps", RCENTITY_TASK,
	    RCTL_GLOBAL_NOACTION | RCTL_GLOBAL_COUNT, INT_MAX, INT_MAX,
	    &task_lwps_ops);
	rc_task_cpu_time = rctl_register("task.max-cpu-time", RCENTITY_TASK,
	    RCTL_GLOBAL_NOACTION | RCTL_GLOBAL_DENY_NEVER |
	    RCTL_GLOBAL_CPU_TIME | RCTL_GLOBAL_INFINITE |
	    RCTL_GLOBAL_UNOBSERVABLE | RCTL_GLOBAL_SECONDS, UINT64_MAX,
	    UINT64_MAX, &task_cpu_time_ops);

	/*
	 * Create task0 and place p0 in it as a member.
	 */
	task0p = kmem_cache_alloc(task_cache, KM_SLEEP);
	bzero(task0p, sizeof (task_t));

	task0p->tk_tkid = id_alloc(taskid_space);
	task0p->tk_usage = kmem_zalloc(sizeof (task_usage_t), KM_SLEEP);
	task0p->tk_proj = project_hold_by_id(0, GLOBAL_ZONEID,
	    PROJECT_HOLD_INSERT);
	task0p->tk_flags = TASK_NORMAL;
	task0p->tk_nlwps = p->p_lwpcnt;
	task0p->tk_zone = global_zone;

	set = rctl_set_create();
	gp = rctl_set_init_prealloc(RCENTITY_TASK);
	mutex_enter(&curproc->p_lock);
	e.rcep_p.task = task0p;
	e.rcep_t = RCENTITY_TASK;
	task0p->tk_rctls = rctl_set_init(RCENTITY_TASK, curproc, &e, set, gp);
	mutex_exit(&curproc->p_lock);
	rctl_prealloc_destroy(gp);

	(void) mod_hash_reserve(task_hash, &hndl);
	mutex_enter(&task_hash_lock);
	ASSERT(task_find(task0p->tk_tkid, GLOBAL_ZONEID) == NULL);
	if (mod_hash_insert_reserve(task_hash,
	    (mod_hash_key_t)(uintptr_t)task0p->tk_tkid,
	    (mod_hash_val_t *)task0p, hndl) != 0) {
		mod_hash_cancel(task_hash, &hndl);
		panic("unable to insert task %d(%p)", task0p->tk_tkid,
		    (void *)task0p);
	}
	mutex_exit(&task_hash_lock);

	task0p->tk_memb_list = p;

	/*
	 * Initialize task pointers for p0, including doubly linked list of task
	 * members.
	 */
	p->p_task = task0p;
	p->p_taskprev = p->p_tasknext = p;
	task_hold(task0p);
}
