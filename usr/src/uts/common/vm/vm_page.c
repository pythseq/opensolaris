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

/*	Copyright (c) 1983, 1984, 1985, 1986, 1987, 1988, 1989  AT&T	*/
/*	  All Rights Reserved  	*/

/*
 * University Copyright- Copyright (c) 1982, 1986, 1988
 * The Regents of the University of California
 * All Rights Reserved
 *
 * University Acknowledgment- Portions of this document are derived from
 * software developed by the University of California, Berkeley, and its
 * contributors.
 */

#pragma ident	"%Z%%M%	%I%	%E% SMI"

/*
 * VM - physical page management.
 */

#include <sys/types.h>
#include <sys/t_lock.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/time.h>
#include <sys/vnode.h>
#include <sys/vm.h>
#include <sys/vtrace.h>
#include <sys/swap.h>
#include <sys/cmn_err.h>
#include <sys/tuneable.h>
#include <sys/sysmacros.h>
#include <sys/cpuvar.h>
#include <sys/callb.h>
#include <sys/debug.h>
#include <sys/tnf_probe.h>
#include <sys/condvar_impl.h>
#include <sys/mem_config.h>
#include <sys/mem_cage.h>
#include <sys/kmem.h>
#include <sys/atomic.h>
#include <sys/strlog.h>
#include <sys/mman.h>
#include <sys/ontrap.h>
#include <sys/lgrp.h>
#include <sys/vfs.h>

#include <vm/hat.h>
#include <vm/anon.h>
#include <vm/page.h>
#include <vm/seg.h>
#include <vm/pvn.h>
#include <vm/seg_kmem.h>
#include <vm/vm_dep.h>

#include <fs/fs_subr.h>

static int nopageage = 0;

static pgcnt_t max_page_get;	/* max page_get request size in pages */
pgcnt_t total_pages = 0;	/* total number of pages (used by /proc) */

/*
 * vnode for all pages which are retired from the VM system;
 * such as pages with Uncorrectable Errors.
 */
struct vnode retired_ppages;

static void	page_retired_init(void);
static void	retired_dispose(vnode_t *vp, page_t *pp, int flag,
			int dn, cred_t *cr);
static void	retired_inactive(vnode_t *vp, cred_t *cr);
static void	page_retired(page_t *pp);
static void	retired_page_removed(page_t *pp);
void		page_unretire_pages(void);

/*
 * The maximum number of pages that will be unretired in one iteration.
 * This number is totally arbitrary.
 */
#define	UNRETIRE_PAGES		256

/*
 * We limit the number of pages that may be retired to
 * a percentage of the total physical memory. Note that
 * the percentage values are  stored as 'basis points',
 * ie, 100 basis points is 1%.
 */
#define	MAX_PAGES_RETIRED_BPS_DEFAULT	10	/* .1% */

uint64_t max_pages_retired_bps = MAX_PAGES_RETIRED_BPS_DEFAULT;

static int	pages_retired_limit_exceeded(void);

/*
 * operations vector for vnode with retired pages. Only VOP_DISPOSE
 * and VOP_INACTIVE are intercepted.
 */
struct vnodeops retired_vnodeops = {
	"retired_vnodeops",
	fs_nosys,	/* open */
	fs_nosys,	/* close */
	fs_nosys,	/* read */
	fs_nosys,	/* write */
	fs_nosys,	/* ioctl */
	fs_nosys,	/* setfl */
	fs_nosys,	/* getattr */
	fs_nosys,	/* setattr */
	fs_nosys,	/* access */
	fs_nosys,	/* lookup */
	fs_nosys,	/* create */
	fs_nosys,	/* remove */
	fs_nosys,	/* link */
	fs_nosys,	/* rename */
	fs_nosys,	/* mkdir */
	fs_nosys,	/* rmdir */
	fs_nosys,	/* readdir */
	fs_nosys,	/* symlink */
	fs_nosys,	/* readlink */
	fs_nosys,	/* fsync */
	retired_inactive,
	fs_nosys,	/* fid */
	fs_rwlock,	/* rwlock */
	fs_rwunlock,	/* rwunlock */
	fs_nosys,	/* seek */
	fs_nosys,	/* cmp */
	fs_nosys,	/* frlock */
	fs_nosys,	/* space */
	fs_nosys,	/* realvp */
	fs_nosys,	/* getpage */
	fs_nosys,	/* putpage */
	fs_nosys_map,
	fs_nosys_addmap,
	fs_nosys,	/* delmap */
	fs_nosys_poll,
	fs_nosys,	/* dump */
	fs_nosys,	/* l_pathconf */
	fs_nosys,	/* pageio */
	fs_nosys,	/* dumpctl */
	retired_dispose,
	fs_nosys,	/* setsecattr */
	fs_nosys,	/* getsecatt */
	fs_nosys,	/* shrlock */
	fs_vnevent_nosupport	/* vnevent */
};

/*
 * freemem_lock protects all freemem variables:
 * availrmem. Also this lock protects the globals which track the
 * availrmem changes for accurate kernel footprint calculation.
 * See below for an explanation of these
 * globals.
 */
kmutex_t freemem_lock;
pgcnt_t availrmem;
pgcnt_t availrmem_initial;

/*
 * These globals track availrmem changes to get a more accurate
 * estimate of tke kernel size. Historically pp_kernel is used for
 * kernel size and is based on availrmem. But availrmem is adjusted for
 * locked pages in the system not just for kernel locked pages.
 * These new counters will track the pages locked through segvn and
 * by explicit user locking.
 *
 * segvn_pages_locked : This keeps track on a global basis how many pages
 * are currently locked because of I/O.
 *
 * pages_locked : How many pages are locked becuase of user specified
 * locking through mlock or plock.
 *
 * pages_useclaim,pages_claimed : These two variables track the
 * cliam adjustments because of the protection changes on a segvn segment.
 *
 * All these globals are protected by the same lock which protects availrmem.
 */
pgcnt_t segvn_pages_locked;
pgcnt_t pages_locked;
pgcnt_t pages_useclaim;
pgcnt_t pages_claimed;


/*
 * new_freemem_lock protects freemem, freemem_wait & freemem_cv.
 */
static kmutex_t	new_freemem_lock;
static uint_t	freemem_wait;	/* someone waiting for freemem */
static kcondvar_t freemem_cv;

/*
 * The logical page free list is maintained as two lists, the 'free'
 * and the 'cache' lists.
 * The free list contains those pages that should be reused first.
 *
 * The implementation of the lists is machine dependent.
 * page_get_freelist(), page_get_cachelist(),
 * page_list_sub(), and page_list_add()
 * form the interface to the machine dependent implementation.
 *
 * Pages with p_free set are on the cache list.
 * Pages with p_free and p_age set are on the free list,
 *
 * A page may be locked while on either list.
 */

/*
 * free list accounting stuff.
 *
 *
 * Spread out the value for the number of pages on the
 * page free and page cache lists.  If there is just one
 * value, then it must be under just one lock.
 * The lock contention and cache traffic are a real bother.
 *
 * When we acquire and then drop a single pcf lock
 * we can start in the middle of the array of pcf structures.
 * If we acquire more than one pcf lock at a time, we need to
 * start at the front to avoid deadlocking.
 *
 * pcf_count holds the number of pages in each pool.
 *
 * pcf_block is set when page_create_get_something() has asked the
 * PSM page freelist and page cachelist routines without specifying
 * a color and nothing came back.  This is used to block anything
 * else from moving pages from one list to the other while the
 * lists are searched again.  If a page is freeed while pcf_block is
 * set, then pcf_reserve is incremented.  pcgs_unblock() takes care
 * of clearning pcf_block, doing the wakeups, etc.
 */

#if NCPU <= 4
#define	PAD	1
#define	PCF_FANOUT	4
static	uint_t	pcf_mask = PCF_FANOUT - 1;
#else
#define	PAD	9
#ifdef sun4v
#define	PCF_FANOUT	32
#else
#define	PCF_FANOUT	128
#endif
static	uint_t	pcf_mask = PCF_FANOUT - 1;
#endif

struct pcf {
	uint_t		pcf_touch;	/* just to help the cache */
	uint_t		pcf_count;	/* page count */
	kmutex_t	pcf_lock;	/* protects the structure */
	uint_t		pcf_wait;	/* number of waiters */
	uint_t		pcf_block; 	/* pcgs flag to page_free() */
	uint_t		pcf_reserve; 	/* pages freed after pcf_block set */
	uint_t		pcf_fill[PAD];	/* to line up on the caches */
};

static struct	pcf	pcf[PCF_FANOUT];
#define	PCF_INDEX()	((CPU->cpu_id) & (pcf_mask))

kmutex_t	pcgs_lock;		/* serializes page_create_get_ */
kmutex_t	pcgs_cagelock;		/* serializes NOSLEEP cage allocs */
kmutex_t	pcgs_wait_lock;		/* used for delay in pcgs */
static kcondvar_t	pcgs_cv;	/* cv for delay in pcgs */

#define	PAGE_LOCK_MAXIMUM \
	((1 << (sizeof (((page_t *)0)->p_lckcnt) * NBBY)) - 1)

/*
 * Control over the verbosity of page retirement.  When set to zero, no messages
 * will be printed.  A value of one will trigger messages for retirement
 * operations, and is intended for processors which don't yet support FMA
 * (spitfire).  Two will cause verbose messages to be printed when retirements
 * complete, and is intended only for debugging purposes.
 */
int page_retire_messages = 0;

#ifdef VM_STATS

/*
 * No locks, but so what, they are only statistics.
 */

static struct page_tcnt {
	int	pc_free_cache;		/* free's into cache list */
	int	pc_free_dontneed;	/* free's with dontneed */
	int	pc_free_pageout;	/* free's from pageout */
	int	pc_free_free;		/* free's into free list */
	int	pc_free_pages;		/* free's into large page free list */
	int	pc_destroy_pages;	/* large page destroy's */
	int	pc_get_cache;		/* get's from cache list */
	int	pc_get_free;		/* get's from free list */
	int	pc_reclaim;		/* reclaim's */
	int	pc_abortfree;		/* abort's of free pages */
	int	pc_find_hit;		/* find's that find page */
	int	pc_find_miss;		/* find's that don't find page */
	int	pc_destroy_free;	/* # of free pages destroyed */
#define	PC_HASH_CNT	(4*PAGE_HASHAVELEN)
	int	pc_find_hashlen[PC_HASH_CNT+1];
	int	pc_addclaim_pages;
	int	pc_subclaim_pages;
	int	pc_free_replacement_page[2];
	int	pc_try_demote_pages[6];
	int	pc_demote_pages[2];
} pagecnt;

uint_t	hashin_count;
uint_t	hashin_not_held;
uint_t	hashin_already;

uint_t	hashout_count;
uint_t	hashout_not_held;

uint_t	page_create_count;
uint_t	page_create_not_enough;
uint_t	page_create_not_enough_again;
uint_t	page_create_zero;
uint_t	page_create_hashout;
uint_t	page_create_page_lock_failed;
uint_t	page_create_trylock_failed;
uint_t	page_create_found_one;
uint_t	page_create_hashin_failed;
uint_t	page_create_dropped_phm;

uint_t	page_create_new;
uint_t	page_create_exists;
uint_t	page_create_putbacks;
uint_t	page_create_overshoot;

uint_t	page_reclaim_zero;
uint_t	page_reclaim_zero_locked;

uint_t	page_rename_exists;
uint_t	page_rename_count;

uint_t	page_lookup_cnt[20];
uint_t	page_lookup_nowait_cnt[10];
uint_t	page_find_cnt;
uint_t	page_exists_cnt;
uint_t	page_exists_forreal_cnt;
uint_t	page_lookup_dev_cnt;
uint_t	get_cachelist_cnt;
uint_t	page_create_cnt[10];
uint_t	alloc_pages[8];
uint_t	page_exphcontg[19];
uint_t  page_create_large_cnt[10];

/*
 * Collects statistics.
 */
#define	PAGE_HASH_SEARCH(index, pp, vp, off) { \
	uint_t	mylen = 0; \
			\
	for ((pp) = page_hash[(index)]; (pp); (pp) = (pp)->p_hash, mylen++) { \
		if ((pp)->p_vnode == (vp) && (pp)->p_offset == (off)) \
			break; \
	} \
	if ((pp) != NULL) \
		pagecnt.pc_find_hit++; \
	else \
		pagecnt.pc_find_miss++; \
	if (mylen > PC_HASH_CNT) \
		mylen = PC_HASH_CNT; \
	pagecnt.pc_find_hashlen[mylen]++; \
}

#else	/* VM_STATS */

/*
 * Don't collect statistics
 */
#define	PAGE_HASH_SEARCH(index, pp, vp, off) { \
	for ((pp) = page_hash[(index)]; (pp); (pp) = (pp)->p_hash) { \
		if ((pp)->p_vnode == (vp) && (pp)->p_offset == (off)) \
			break; \
	} \
}

#endif	/* VM_STATS */



#ifdef DEBUG
#define	MEMSEG_SEARCH_STATS
#endif

#ifdef MEMSEG_SEARCH_STATS
struct memseg_stats {
    uint_t nsearch;
    uint_t nlastwon;
    uint_t nhashwon;
    uint_t nnotfound;
} memseg_stats;

#define	MEMSEG_STAT_INCR(v) \
	atomic_add_32(&memseg_stats.v, 1)
#else
#define	MEMSEG_STAT_INCR(x)
#endif

struct memseg *memsegs;		/* list of memory segments */


static void page_init_mem_config(void);
static int page_do_hashin(page_t *, vnode_t *, u_offset_t);
static void page_do_hashout(page_t *);

static void page_demote_vp_pages(page_t *);

/*
 * vm subsystem related initialization
 */
void
vm_init(void)
{
	boolean_t callb_vm_cpr(void *, int);

	(void) callb_add(callb_vm_cpr, 0, CB_CL_CPR_VM, "vm");
	page_init_mem_config();

	/*
	 * initialise the vnode for retired pages
	 */
	page_retired_init();
}

/*
 * This function is called at startup and when memory is added or deleted.
 */
void
init_pages_pp_maximum()
{
	static pgcnt_t p_min;
	static pgcnt_t pages_pp_maximum_startup;
	static pgcnt_t avrmem_delta;
	static int init_done;
	static int user_set;	/* true if set in /etc/system */

	if (init_done == 0) {

		/* If the user specified a value, save it */
		if (pages_pp_maximum != 0) {
			user_set = 1;
			pages_pp_maximum_startup = pages_pp_maximum;
		}

		/*
		 * Setting of pages_pp_maximum is based first time
		 * on the value of availrmem just after the start-up
		 * allocations. To preserve this relationship at run
		 * time, use a delta from availrmem_initial.
		 */
		ASSERT(availrmem_initial >= availrmem);
		avrmem_delta = availrmem_initial - availrmem;

		/* The allowable floor of pages_pp_maximum */
		p_min = tune.t_minarmem + 100;

		/* Make sure we don't come through here again. */
		init_done = 1;
	}
	/*
	 * Determine pages_pp_maximum, the number of currently available
	 * pages (availrmem) that can't be `locked'. If not set by
	 * the user, we set it to 4% of the currently available memory
	 * plus 4MB.
	 * But we also insist that it be greater than tune.t_minarmem;
	 * otherwise a process could lock down a lot of memory, get swapped
	 * out, and never have enough to get swapped back in.
	 */
	if (user_set)
		pages_pp_maximum = pages_pp_maximum_startup;
	else
		pages_pp_maximum = ((availrmem_initial - avrmem_delta) / 25)
		    + btop(4 * 1024 * 1024);

	if (pages_pp_maximum <= p_min) {
		pages_pp_maximum = p_min;
	}
}

void
set_max_page_get(pgcnt_t target_total_pages)
{
	max_page_get = target_total_pages / 2;
}

static pgcnt_t pending_delete;

/*ARGSUSED*/
static void
page_mem_config_post_add(
	void *arg,
	pgcnt_t delta_pages)
{
	set_max_page_get(total_pages - pending_delete);
	init_pages_pp_maximum();
}

/*ARGSUSED*/
static int
page_mem_config_pre_del(
	void *arg,
	pgcnt_t delta_pages)
{
	pgcnt_t nv;

	nv = atomic_add_long_nv(&pending_delete, (spgcnt_t)delta_pages);
	set_max_page_get(total_pages - nv);
	return (0);
}

/*ARGSUSED*/
static void
page_mem_config_post_del(
	void *arg,
	pgcnt_t delta_pages,
	int cancelled)
{
	pgcnt_t nv;

	nv = atomic_add_long_nv(&pending_delete, -(spgcnt_t)delta_pages);
	set_max_page_get(total_pages - nv);
	if (!cancelled)
		init_pages_pp_maximum();
}

static kphysm_setup_vector_t page_mem_config_vec = {
	KPHYSM_SETUP_VECTOR_VERSION,
	page_mem_config_post_add,
	page_mem_config_pre_del,
	page_mem_config_post_del,
};

static void
page_init_mem_config(void)
{
	int ret;

	ret = kphysm_setup_func_register(&page_mem_config_vec, (void *)NULL);
	ASSERT(ret == 0);
}

/*
 * Evenly spread out the PCF counters for large free pages
 */
static void
page_free_large_ctr(pgcnt_t npages)
{
	static struct pcf	*p = pcf;
	pgcnt_t			lump;

	freemem += npages;

	lump = roundup(npages, PCF_FANOUT) / PCF_FANOUT;

	while (npages > 0) {

		ASSERT(!p->pcf_block);

		if (lump < npages) {
			p->pcf_count += (uint_t)lump;
			npages -= lump;
		} else {
			p->pcf_count += (uint_t)npages;
			npages = 0;
		}

		ASSERT(!p->pcf_wait);

		if (++p > &pcf[PCF_FANOUT - 1])
			p = pcf;
	}

	ASSERT(npages == 0);
}

/*
 * Add a physical chunk of memory to the system freee lists during startup.
 * Platform specific startup() allocates the memory for the page structs.
 *
 * num	- number of page structures
 * base - page number (pfn) to be associated with the first page.
 *
 * Since we are doing this during startup (ie. single threaded), we will
 * use shortcut routines to avoid any locking overhead while putting all
 * these pages on the freelists.
 *
 * NOTE: Any changes performed to page_free(), must also be performed to
 *	 add_physmem() since this is how we initialize all page_t's at
 *	 boot time.
 */
void
add_physmem(
	page_t	*pp,
	pgcnt_t	num,
	pfn_t	pnum)
{
	page_t	*root = NULL;
	uint_t	szc = page_num_pagesizes() - 1;
	pgcnt_t	large = page_get_pagecnt(szc);
	pgcnt_t	cnt = 0;

	TRACE_2(TR_FAC_VM, TR_PAGE_INIT,
		"add_physmem:pp %p num %lu", pp, num);

	/*
	 * Arbitrarily limit the max page_get request
	 * to 1/2 of the page structs we have.
	 */
	total_pages += num;
	set_max_page_get(total_pages);

	/*
	 * The physical space for the pages array
	 * representing ram pages has already been
	 * allocated.  Here we initialize each lock
	 * in the page structure, and put each on
	 * the free list
	 */
	for (; num; pp++, pnum++, num--) {

		/*
		 * this needs to fill in the page number
		 * and do any other arch specific initialization
		 */
		add_physmem_cb(pp, pnum);

		/*
		 * Initialize the page lock as unlocked, since nobody
		 * can see or access this page yet.
		 */
		pp->p_selock = 0;

		/*
		 * Initialize IO lock
		 */
		page_iolock_init(pp);

		/*
		 * initialize other fields in the page_t
		 */
		PP_SETFREE(pp);
		page_clr_all_props(pp);
		PP_SETAGED(pp);
		pp->p_offset = (u_offset_t)-1;
		pp->p_next = pp;
		pp->p_prev = pp;

		/*
		 * Simple case: System doesn't support large pages.
		 */
		if (szc == 0) {
			pp->p_szc = 0;
			page_free_at_startup(pp);
			continue;
		}

		/*
		 * Handle unaligned pages, we collect them up onto
		 * the root page until we have a full large page.
		 */
		if (!IS_P2ALIGNED(pnum, large)) {

			/*
			 * If not in a large page,
			 * just free as small page.
			 */
			if (root == NULL) {
				pp->p_szc = 0;
				page_free_at_startup(pp);
				continue;
			}

			/*
			 * Link a constituent page into the large page.
			 */
			pp->p_szc = szc;
			page_list_concat(&root, &pp);

			/*
			 * When large page is fully formed, free it.
			 */
			if (++cnt == large) {
				page_free_large_ctr(cnt);
				page_list_add_pages(root, PG_LIST_ISINIT);
				root = NULL;
				cnt = 0;
			}
			continue;
		}

		/*
		 * At this point we have a page number which
		 * is aligned. We assert that we aren't already
		 * in a different large page.
		 */
		ASSERT(IS_P2ALIGNED(pnum, large));
		ASSERT(root == NULL && cnt == 0);

		/*
		 * If insufficient number of pages left to form
		 * a large page, just free the small page.
		 */
		if (num < large) {
			pp->p_szc = 0;
			page_free_at_startup(pp);
			continue;
		}

		/*
		 * Otherwise start a new large page.
		 */
		pp->p_szc = szc;
		cnt++;
		root = pp;
	}
	ASSERT(root == NULL && cnt == 0);
}

/*
 * Find a page representing the specified [vp, offset].
 * If we find the page but it is intransit coming in,
 * it will have an "exclusive" lock and we wait for
 * the i/o to complete.  A page found on the free list
 * is always reclaimed and then locked.  On success, the page
 * is locked, its data is valid and it isn't on the free
 * list, while a NULL is returned if the page doesn't exist.
 */
page_t *
page_lookup(vnode_t *vp, u_offset_t off, se_t se)
{
	return (page_lookup_create(vp, off, se, NULL, NULL, 0));
}

/*
 * Find a page representing the specified [vp, offset].
 * We either return the one we found or, if passed in,
 * create one with identity of [vp, offset] of the
 * pre-allocated page. If we find exsisting page but it is
 * intransit coming in, it will have an "exclusive" lock
 * and we wait for the i/o to complete.  A page found on
 * the free list is always reclaimed and then locked.
 * On success, the page is locked, its data is valid and
 * it isn't on the free list, while a NULL is returned
 * if the page doesn't exist and newpp is NULL;
 */
page_t *
page_lookup_create(
	vnode_t *vp,
	u_offset_t off,
	se_t se,
	page_t *newpp,
	spgcnt_t *nrelocp,
	int flags)
{
	page_t		*pp;
	kmutex_t	*phm;
	ulong_t		index;
	uint_t		hash_locked;
	uint_t		es;

	ASSERT(MUTEX_NOT_HELD(page_vnode_mutex(vp)));
	VM_STAT_ADD(page_lookup_cnt[0]);
	ASSERT(newpp ? PAGE_EXCL(newpp) : 1);

	/*
	 * Acquire the appropriate page hash lock since
	 * we have to search the hash list.  Pages that
	 * hash to this list can't change identity while
	 * this lock is held.
	 */
	hash_locked = 0;
	index = PAGE_HASH_FUNC(vp, off);
	phm = NULL;
top:
	PAGE_HASH_SEARCH(index, pp, vp, off);
	if (pp != NULL) {
		VM_STAT_ADD(page_lookup_cnt[1]);
		es = (newpp != NULL) ? 1 : 0;
		es |= flags;
		if (!hash_locked) {
			VM_STAT_ADD(page_lookup_cnt[2]);
			if (!page_try_reclaim_lock(pp, se, es)) {
				/*
				 * On a miss, acquire the phm.  Then
				 * next time, page_lock() will be called,
				 * causing a wait if the page is busy.
				 * just looping with page_trylock() would
				 * get pretty boring.
				 */
				VM_STAT_ADD(page_lookup_cnt[3]);
				phm = PAGE_HASH_MUTEX(index);
				mutex_enter(phm);
				hash_locked = 1;
				goto top;
			}
		} else {
			VM_STAT_ADD(page_lookup_cnt[4]);
			if (!page_lock_es(pp, se, phm, P_RECLAIM, es)) {
				VM_STAT_ADD(page_lookup_cnt[5]);
				goto top;
			}
		}

		/*
		 * Since `pp' is locked it can not change identity now.
		 * Reconfirm we locked the correct page.
		 *
		 * Both the p_vnode and p_offset *must* be cast volatile
		 * to force a reload of their values: The PAGE_HASH_SEARCH
		 * macro will have stuffed p_vnode and p_offset into
		 * registers before calling page_trylock(); another thread,
		 * actually holding the hash lock, could have changed the
		 * page's identity in memory, but our registers would not
		 * be changed, fooling the reconfirmation.  If the hash
		 * lock was held during the search, the casting would
		 * not be needed.
		 */
		VM_STAT_ADD(page_lookup_cnt[6]);
		if (((volatile struct vnode *)(pp->p_vnode) != vp) ||
		    ((volatile u_offset_t)(pp->p_offset) != off)) {
			VM_STAT_ADD(page_lookup_cnt[7]);
			if (hash_locked) {
				panic("page_lookup_create: lost page %p",
				    (void *)pp);
				/*NOTREACHED*/
			}
			page_unlock(pp);
			phm = PAGE_HASH_MUTEX(index);
			mutex_enter(phm);
			hash_locked = 1;
			goto top;
		}

		/*
		 * If page_trylock() was called, then pp may still be on
		 * the cachelist (can't be on the free list, it would not
		 * have been found in the search).  If it is on the
		 * cachelist it must be pulled now. To pull the page from
		 * the cachelist, it must be exclusively locked.
		 *
		 * The other big difference between page_trylock() and
		 * page_lock(), is that page_lock() will pull the
		 * page from whatever free list (the cache list in this
		 * case) the page is on.  If page_trylock() was used
		 * above, then we have to do the reclaim ourselves.
		 */
		if ((!hash_locked) && (PP_ISFREE(pp))) {
			ASSERT(PP_ISAGED(pp) == 0);
			VM_STAT_ADD(page_lookup_cnt[8]);

			/*
			 * page_relcaim will insure that we
			 * have this page exclusively
			 */

			if (!page_reclaim(pp, NULL)) {
				/*
				 * Page_reclaim dropped whatever lock
				 * we held.
				 */
				VM_STAT_ADD(page_lookup_cnt[9]);
				phm = PAGE_HASH_MUTEX(index);
				mutex_enter(phm);
				hash_locked = 1;
				goto top;
			} else if (se == SE_SHARED && newpp == NULL) {
				VM_STAT_ADD(page_lookup_cnt[10]);
				page_downgrade(pp);
			}
		}

		if (hash_locked) {
			mutex_exit(phm);
		}

		if (newpp != NULL && pp->p_szc < newpp->p_szc &&
		    PAGE_EXCL(pp) && nrelocp != NULL) {
			ASSERT(nrelocp != NULL);
			(void) page_relocate(&pp, &newpp, 1, 1, nrelocp,
			    NULL);
			if (*nrelocp > 0) {
				VM_STAT_COND_ADD(*nrelocp == 1,
				    page_lookup_cnt[11]);
				VM_STAT_COND_ADD(*nrelocp > 1,
				    page_lookup_cnt[12]);
				pp = newpp;
				se = SE_EXCL;
			} else {
				if (se == SE_SHARED) {
					page_downgrade(pp);
				}
				VM_STAT_ADD(page_lookup_cnt[13]);
			}
		} else if (newpp != NULL && nrelocp != NULL) {
			if (PAGE_EXCL(pp) && se == SE_SHARED) {
				page_downgrade(pp);
			}
			VM_STAT_COND_ADD(pp->p_szc < newpp->p_szc,
			    page_lookup_cnt[14]);
			VM_STAT_COND_ADD(pp->p_szc == newpp->p_szc,
			    page_lookup_cnt[15]);
			VM_STAT_COND_ADD(pp->p_szc > newpp->p_szc,
			    page_lookup_cnt[16]);
		} else if (newpp != NULL && PAGE_EXCL(pp)) {
			se = SE_EXCL;
		}
	} else if (!hash_locked) {
		VM_STAT_ADD(page_lookup_cnt[17]);
		phm = PAGE_HASH_MUTEX(index);
		mutex_enter(phm);
		hash_locked = 1;
		goto top;
	} else if (newpp != NULL) {
		/*
		 * If we have a preallocated page then
		 * insert it now and basically behave like
		 * page_create.
		 */
		VM_STAT_ADD(page_lookup_cnt[18]);
		/*
		 * Since we hold the page hash mutex and
		 * just searched for this page, page_hashin
		 * had better not fail.  If it does, that
		 * means some thread did not follow the
		 * page hash mutex rules.  Panic now and
		 * get it over with.  As usual, go down
		 * holding all the locks.
		 */
		ASSERT(MUTEX_HELD(phm));
		if (!page_hashin(newpp, vp, off, phm)) {
			ASSERT(MUTEX_HELD(phm));
			panic("page_lookup_create: hashin failed %p %p %llx %p",
			    (void *)newpp, (void *)vp, off, (void *)phm);
			/*NOTREACHED*/
		}
		ASSERT(MUTEX_HELD(phm));
		mutex_exit(phm);
		phm = NULL;
		page_set_props(newpp, P_REF);
		page_io_lock(newpp);
		pp = newpp;
		se = SE_EXCL;
	} else {
		VM_STAT_ADD(page_lookup_cnt[19]);
		mutex_exit(phm);
	}

	ASSERT(pp ? PAGE_LOCKED_SE(pp, se) : 1);

	ASSERT(pp ? ((PP_ISFREE(pp) == 0) && (PP_ISAGED(pp) == 0)) : 1);

	return (pp);
}

/*
 * Search the hash list for the page representing the
 * specified [vp, offset] and return it locked.  Skip
 * free pages and pages that cannot be locked as requested.
 * Used while attempting to kluster pages.
 */
page_t *
page_lookup_nowait(vnode_t *vp, u_offset_t off, se_t se)
{
	page_t		*pp;
	kmutex_t	*phm;
	ulong_t		index;
	uint_t		locked;

	ASSERT(MUTEX_NOT_HELD(page_vnode_mutex(vp)));
	VM_STAT_ADD(page_lookup_nowait_cnt[0]);

	index = PAGE_HASH_FUNC(vp, off);
	PAGE_HASH_SEARCH(index, pp, vp, off);
	locked = 0;
	if (pp == NULL) {
top:
		VM_STAT_ADD(page_lookup_nowait_cnt[1]);
		locked = 1;
		phm = PAGE_HASH_MUTEX(index);
		mutex_enter(phm);
		PAGE_HASH_SEARCH(index, pp, vp, off);
	}

	if (pp == NULL || PP_ISFREE(pp)) {
		VM_STAT_ADD(page_lookup_nowait_cnt[2]);
		pp = NULL;
	} else {
		if (!page_trylock(pp, se)) {
			VM_STAT_ADD(page_lookup_nowait_cnt[3]);
			pp = NULL;
		} else {
			VM_STAT_ADD(page_lookup_nowait_cnt[4]);
			/*
			 * See the comment in page_lookup()
			 */
			if (((volatile struct vnode *)(pp->p_vnode) != vp) ||
			    ((u_offset_t)(pp->p_offset) != off)) {
				VM_STAT_ADD(page_lookup_nowait_cnt[5]);
				if (locked) {
					panic("page_lookup_nowait %p",
					    (void *)pp);
					/*NOTREACHED*/
				}
				page_unlock(pp);
				goto top;
			}
			if (PP_ISFREE(pp)) {
				VM_STAT_ADD(page_lookup_nowait_cnt[6]);
				page_unlock(pp);
				pp = NULL;
			}
		}
	}
	if (locked) {
		VM_STAT_ADD(page_lookup_nowait_cnt[7]);
		mutex_exit(phm);
	}

	ASSERT(pp ? PAGE_LOCKED_SE(pp, se) : 1);

	return (pp);
}

/*
 * Search the hash list for a page with the specified [vp, off]
 * that is known to exist and is already locked.  This routine
 * is typically used by segment SOFTUNLOCK routines.
 */
page_t *
page_find(vnode_t *vp, u_offset_t off)
{
	page_t		*pp;
	kmutex_t	*phm;
	ulong_t		index;

	ASSERT(MUTEX_NOT_HELD(page_vnode_mutex(vp)));
	VM_STAT_ADD(page_find_cnt);

	index = PAGE_HASH_FUNC(vp, off);
	phm = PAGE_HASH_MUTEX(index);

	mutex_enter(phm);
	PAGE_HASH_SEARCH(index, pp, vp, off);
	mutex_exit(phm);

	ASSERT(pp != NULL);
	ASSERT(PAGE_LOCKED(pp) || panicstr);
	return (pp);
}

/*
 * Determine whether a page with the specified [vp, off]
 * currently exists in the system.  Obviously this should
 * only be considered as a hint since nothing prevents the
 * page from disappearing or appearing immediately after
 * the return from this routine. Subsequently, we don't
 * even bother to lock the list.
 */
page_t *
page_exists(vnode_t *vp, u_offset_t off)
{
	page_t	*pp;
	ulong_t		index;

	ASSERT(MUTEX_NOT_HELD(page_vnode_mutex(vp)));
	VM_STAT_ADD(page_exists_cnt);

	index = PAGE_HASH_FUNC(vp, off);
	PAGE_HASH_SEARCH(index, pp, vp, off);

	return (pp);
}

/*
 * Determine if physically contiguous pages exist for [vp, off] - [vp, off +
 * page_size(szc)) range.  if they exist and ppa is not NULL fill ppa array
 * with these pages locked SHARED. If necessary reclaim pages from
 * freelist. Return 1 if contiguous pages exist and 0 otherwise.
 *
 * If we fail to lock pages still return 1 if pages exist and contiguous.
 * But in this case return value is just a hint. ppa array won't be filled.
 * Caller should initialize ppa[0] as NULL to distinguish return value.
 *
 * Returns 0 if pages don't exist or not physically contiguous.
 *
 * This routine doesn't work for anonymous(swapfs) pages.
 */
int
page_exists_physcontig(vnode_t *vp, u_offset_t off, uint_t szc, page_t *ppa[])
{
	pgcnt_t pages;
	pfn_t pfn;
	page_t *rootpp;
	pgcnt_t i;
	pgcnt_t j;
	u_offset_t save_off = off;
	ulong_t index;
	kmutex_t *phm;
	page_t *pp;
	uint_t pszc;
	int loopcnt = 0;

	ASSERT(szc != 0);
	ASSERT(vp != NULL);
	ASSERT(!IS_SWAPFSVP(vp));
	ASSERT(vp != &kvp);

again:
	if (++loopcnt > 3) {
		VM_STAT_ADD(page_exphcontg[0]);
		return (0);
	}

	index = PAGE_HASH_FUNC(vp, off);
	phm = PAGE_HASH_MUTEX(index);

	mutex_enter(phm);
	PAGE_HASH_SEARCH(index, pp, vp, off);
	mutex_exit(phm);

	VM_STAT_ADD(page_exphcontg[1]);

	if (pp == NULL) {
		VM_STAT_ADD(page_exphcontg[2]);
		return (0);
	}

	pages = page_get_pagecnt(szc);
	rootpp = pp;
	pfn = rootpp->p_pagenum;

	if ((pszc = pp->p_szc) >= szc && ppa != NULL) {
		VM_STAT_ADD(page_exphcontg[3]);
		if (!page_trylock(pp, SE_SHARED)) {
			VM_STAT_ADD(page_exphcontg[4]);
			return (1);
		}
		if (pp->p_szc != pszc || pp->p_vnode != vp ||
		    pp->p_offset != off) {
			VM_STAT_ADD(page_exphcontg[5]);
			page_unlock(pp);
			off = save_off;
			goto again;
		}
		/*
		 * szc was non zero and vnode and offset matched after we
		 * locked the page it means it can't become free on us.
		 */
		ASSERT(!PP_ISFREE(pp));
		if (!IS_P2ALIGNED(pfn, pages)) {
			page_unlock(pp);
			return (0);
		}
		ppa[0] = pp;
		pp++;
		off += PAGESIZE;
		pfn++;
		for (i = 1; i < pages; i++, pp++, off += PAGESIZE, pfn++) {
			if (!page_trylock(pp, SE_SHARED)) {
				VM_STAT_ADD(page_exphcontg[6]);
				pp--;
				while (i-- > 0) {
					page_unlock(pp);
					pp--;
				}
				ppa[0] = NULL;
				return (1);
			}
			if (pp->p_szc != pszc) {
				VM_STAT_ADD(page_exphcontg[7]);
				page_unlock(pp);
				pp--;
				while (i-- > 0) {
					page_unlock(pp);
					pp--;
				}
				ppa[0] = NULL;
				off = save_off;
				goto again;
			}
			/*
			 * szc the same as for previous already locked pages
			 * with right identity. Since this page had correct
			 * szc after we locked it can't get freed or destroyed
			 * and therefore must have the expected identity.
			 */
			ASSERT(!PP_ISFREE(pp));
			if (pp->p_vnode != vp ||
			    pp->p_offset != off) {
				panic("page_exists_physcontig: "
				    "large page identity doesn't match");
			}
			ppa[i] = pp;
			ASSERT(pp->p_pagenum == pfn);
		}
		VM_STAT_ADD(page_exphcontg[8]);
		ppa[pages] = NULL;
		return (1);
	} else if (pszc >= szc) {
		VM_STAT_ADD(page_exphcontg[9]);
		if (!IS_P2ALIGNED(pfn, pages)) {
			return (0);
		}
		return (1);
	}

	if (!IS_P2ALIGNED(pfn, pages)) {
		VM_STAT_ADD(page_exphcontg[10]);
		return (0);
	}

	if (page_numtomemseg_nolock(pfn) !=
	    page_numtomemseg_nolock(pfn + pages - 1)) {
		VM_STAT_ADD(page_exphcontg[11]);
		return (0);
	}

	/*
	 * We loop up 4 times across pages to promote page size.
	 * We're extra cautious to promote page size atomically with respect
	 * to everybody else.  But we can probably optimize into 1 loop if
	 * this becomes an issue.
	 */

	for (i = 0; i < pages; i++, pp++, off += PAGESIZE, pfn++) {
		ASSERT(pp->p_pagenum == pfn);
		if (!page_trylock(pp, SE_EXCL)) {
			VM_STAT_ADD(page_exphcontg[12]);
			break;
		}
		if (pp->p_vnode != vp ||
		    pp->p_offset != off) {
			VM_STAT_ADD(page_exphcontg[13]);
			page_unlock(pp);
			break;
		}
		if (pp->p_szc >= szc) {
			ASSERT(i == 0);
			page_unlock(pp);
			off = save_off;
			goto again;
		}
	}

	if (i != pages) {
		VM_STAT_ADD(page_exphcontg[14]);
		--pp;
		while (i-- > 0) {
			page_unlock(pp);
			--pp;
		}
		return (0);
	}

	pp = rootpp;
	for (i = 0; i < pages; i++, pp++) {
		if (PP_ISFREE(pp)) {
			VM_STAT_ADD(page_exphcontg[15]);
			ASSERT(!PP_ISAGED(pp));
			ASSERT(pp->p_szc == 0);
			if (!page_reclaim(pp, NULL)) {
				break;
			}
		} else {
			ASSERT(pp->p_szc < szc);
			VM_STAT_ADD(page_exphcontg[16]);
			(void) hat_pageunload(pp, HAT_FORCE_PGUNLOAD);
		}
	}
	if (i < pages) {
		VM_STAT_ADD(page_exphcontg[17]);
		/*
		 * page_reclaim failed because we were out of memory.
		 * drop the rest of the locks and return because this page
		 * must be already reallocated anyway.
		 */
		pp = rootpp;
		for (j = 0; j < pages; j++, pp++) {
			if (j != i) {
				page_unlock(pp);
			}
		}
		return (0);
	}

	off = save_off;
	pp = rootpp;
	for (i = 0; i < pages; i++, pp++, off += PAGESIZE) {
		ASSERT(PAGE_EXCL(pp));
		ASSERT(!PP_ISFREE(pp));
		ASSERT(!hat_page_is_mapped(pp));
		ASSERT(pp->p_vnode == vp);
		ASSERT(pp->p_offset == off);
		pp->p_szc = szc;
	}
	pp = rootpp;
	for (i = 0; i < pages; i++, pp++) {
		if (ppa == NULL) {
			page_unlock(pp);
		} else {
			ppa[i] = pp;
			page_downgrade(ppa[i]);
		}
	}
	if (ppa != NULL) {
		ppa[pages] = NULL;
	}
	VM_STAT_ADD(page_exphcontg[18]);
	ASSERT(vp->v_pages != NULL);
	return (1);
}

/*
 * Determine whether a page with the specified [vp, off]
 * currently exists in the system and if so return its
 * size code. Obviously this should only be considered as
 * a hint since nothing prevents the page from disappearing
 * or appearing immediately after the return from this routine.
 */
int
page_exists_forreal(vnode_t *vp, u_offset_t off, uint_t *szc)
{
	page_t		*pp;
	kmutex_t	*phm;
	ulong_t		index;
	int		rc = 0;

	ASSERT(MUTEX_NOT_HELD(page_vnode_mutex(vp)));
	ASSERT(szc != NULL);
	VM_STAT_ADD(page_exists_forreal_cnt);

	index = PAGE_HASH_FUNC(vp, off);
	phm = PAGE_HASH_MUTEX(index);

	mutex_enter(phm);
	PAGE_HASH_SEARCH(index, pp, vp, off);
	if (pp != NULL) {
		*szc = pp->p_szc;
		rc = 1;
	}
	mutex_exit(phm);
	return (rc);
}

/* wakeup threads waiting for pages in page_create_get_something() */
void
wakeup_pcgs(void)
{
	if (!CV_HAS_WAITERS(&pcgs_cv))
		return;
	cv_broadcast(&pcgs_cv);
}

/*
 * 'freemem' is used all over the kernel as an indication of how many
 * pages are free (either on the cache list or on the free page list)
 * in the system.  In very few places is a really accurate 'freemem'
 * needed.  To avoid contention of the lock protecting a the
 * single freemem, it was spread out into NCPU buckets.  Set_freemem
 * sets freemem to the total of all NCPU buckets.  It is called from
 * clock() on each TICK.
 */
void
set_freemem()
{
	struct pcf	*p;
	ulong_t		t;
	uint_t		i;

	t = 0;
	p = pcf;
	for (i = 0;  i < PCF_FANOUT; i++) {
		t += p->pcf_count;
		p++;
	}
	freemem = t;

	/*
	 * Don't worry about grabbing mutex.  It's not that
	 * critical if we miss a tick or two.  This is
	 * where we wakeup possible delayers in
	 * page_create_get_something().
	 */
	wakeup_pcgs();
}

ulong_t
get_freemem()
{
	struct pcf	*p;
	ulong_t		t;
	uint_t		i;

	t = 0;
	p = pcf;
	for (i = 0; i < PCF_FANOUT; i++) {
		t += p->pcf_count;
		p++;
	}
	/*
	 * We just calculated it, might as well set it.
	 */
	freemem = t;
	return (t);
}

/*
 * Acquire all of the page cache & free (pcf) locks.
 */
void
pcf_acquire_all()
{
	struct pcf	*p;
	uint_t		i;

	p = pcf;
	for (i = 0; i < PCF_FANOUT; i++) {
		p->pcf_touch = 1;
		mutex_enter(&p->pcf_lock);
		p++;
	}
}

/*
 * Release all the pcf_locks.
 */
void
pcf_release_all()
{
	struct pcf	*p;
	uint_t		i;

	p = pcf;
	for (i = 0; i < PCF_FANOUT; i++) {
		mutex_exit(&p->pcf_lock);
		p++;
	}
}

/*
 * Inform the VM system that we need some pages freed up.
 * Calls must be symmetric, e.g.:
 *
 *	page_needfree(100);
 *	wait a bit;
 *	page_needfree(-100);
 */
void
page_needfree(spgcnt_t npages)
{
	mutex_enter(&new_freemem_lock);
	needfree += npages;
	mutex_exit(&new_freemem_lock);
}

/*
 * Throttle for page_create(): try to prevent freemem from dropping
 * below throttlefree.  We can't provide a 100% guarantee because
 * KM_NOSLEEP allocations, page_reclaim(), and various other things
 * nibble away at the freelist.  However, we can block all PG_WAIT
 * allocations until memory becomes available.  The motivation is
 * that several things can fall apart when there's no free memory:
 *
 * (1) If pageout() needs memory to push a page, the system deadlocks.
 *
 * (2) By (broken) specification, timeout(9F) can neither fail nor
 *     block, so it has no choice but to panic the system if it
 *     cannot allocate a callout structure.
 *
 * (3) Like timeout(), ddi_set_callback() cannot fail and cannot block;
 *     it panics if it cannot allocate a callback structure.
 *
 * (4) Untold numbers of third-party drivers have not yet been hardened
 *     against KM_NOSLEEP and/or allocb() failures; they simply assume
 *     success and panic the system with a data fault on failure.
 *     (The long-term solution to this particular problem is to ship
 *     hostile fault-injecting DEBUG kernels with the DDK.)
 *
 * It is theoretically impossible to guarantee success of non-blocking
 * allocations, but in practice, this throttle is very hard to break.
 */
static int
page_create_throttle(pgcnt_t npages, int flags)
{
	ulong_t	fm;
	uint_t	i;
	pgcnt_t tf;	/* effective value of throttlefree */

	/*
	 * Never deny pages when:
	 * - it's a thread that cannot block [NOMEMWAIT()]
	 * - the allocation cannot block and must not fail
	 * - the allocation cannot block and is pageout dispensated
	 */
	if (NOMEMWAIT() ||
	    ((flags & (PG_WAIT | PG_PANIC)) == PG_PANIC) ||
	    ((flags & (PG_WAIT | PG_PUSHPAGE)) == PG_PUSHPAGE))
		return (1);

	/*
	 * If the allocation can't block, we look favorably upon it
	 * unless we're below pageout_reserve.  In that case we fail
	 * the allocation because we want to make sure there are a few
	 * pages available for pageout.
	 */
	if ((flags & PG_WAIT) == 0)
		return (freemem >= npages + pageout_reserve);

	/* Calculate the effective throttlefree value */
	tf = throttlefree -
	    ((flags & PG_PUSHPAGE) ? pageout_reserve : 0);

	cv_signal(&proc_pageout->p_cv);

	while (freemem < npages + tf) {
		pcf_acquire_all();
		mutex_enter(&new_freemem_lock);
		fm = 0;
		for (i = 0; i < PCF_FANOUT; i++) {
			fm += pcf[i].pcf_count;
			pcf[i].pcf_wait++;
			mutex_exit(&pcf[i].pcf_lock);
		}
		freemem = fm;
		needfree += npages;
		freemem_wait++;
		cv_wait(&freemem_cv, &new_freemem_lock);
		freemem_wait--;
		needfree -= npages;
		mutex_exit(&new_freemem_lock);
	}
	return (1);
}

/*
 * page_create_wait() is called to either coalecse pages from the
 * different pcf buckets or to wait because there simply are not
 * enough pages to satisfy the caller's request.
 *
 * Sadly, this is called from platform/vm/vm_machdep.c
 */
int
page_create_wait(size_t npages, uint_t flags)
{
	pgcnt_t		total;
	uint_t		i;
	struct pcf	*p;

	/*
	 * Wait until there are enough free pages to satisfy our
	 * entire request.
	 * We set needfree += npages before prodding pageout, to make sure
	 * it does real work when npages > lotsfree > freemem.
	 */
	VM_STAT_ADD(page_create_not_enough);

	ASSERT(!kcage_on ? !(flags & PG_NORELOC) : 1);
checkagain:
	if ((flags & PG_NORELOC) &&
	    kcage_freemem < kcage_throttlefree + npages)
		(void) kcage_create_throttle(npages, flags);

	if (freemem < npages + throttlefree)
		if (!page_create_throttle(npages, flags))
			return (0);

	/*
	 * Since page_create_va() looked at every
	 * bucket, assume we are going to have to wait.
	 * Get all of the pcf locks.
	 */
	total = 0;
	p = pcf;
	for (i = 0; i < PCF_FANOUT; i++) {
		p->pcf_touch = 1;
		mutex_enter(&p->pcf_lock);
		total += p->pcf_count;
		if (total >= npages) {
			/*
			 * Wow!  There are enough pages laying around
			 * to satisfy the request.  Do the accounting,
			 * drop the locks we acquired, and go back.
			 *
			 * freemem is not protected by any lock. So,
			 * we cannot have any assertion containing
			 * freemem.
			 */
			freemem -= npages;

			while (p >= pcf) {
				if (p->pcf_count <= npages) {
					npages -= p->pcf_count;
					p->pcf_count = 0;
				} else {
					p->pcf_count -= (uint_t)npages;
					npages = 0;
				}
				mutex_exit(&p->pcf_lock);
				p--;
			}
			ASSERT(npages == 0);
			return (1);
		}
		p++;
	}

	/*
	 * All of the pcf locks are held, there are not enough pages
	 * to satisfy the request (npages < total).
	 * Be sure to acquire the new_freemem_lock before dropping
	 * the pcf locks.  This prevents dropping wakeups in page_free().
	 * The order is always pcf_lock then new_freemem_lock.
	 *
	 * Since we hold all the pcf locks, it is a good time to set freemem.
	 *
	 * If the caller does not want to wait, return now.
	 * Else turn the pageout daemon loose to find something
	 * and wait till it does.
	 *
	 */
	freemem = total;

	if ((flags & PG_WAIT) == 0) {
		pcf_release_all();

		TRACE_2(TR_FAC_VM, TR_PAGE_CREATE_NOMEM,
		"page_create_nomem:npages %ld freemem %ld", npages, freemem);
		return (0);
	}

	ASSERT(proc_pageout != NULL);
	cv_signal(&proc_pageout->p_cv);

	TRACE_2(TR_FAC_VM, TR_PAGE_CREATE_SLEEP_START,
	    "page_create_sleep_start: freemem %ld needfree %ld",
	    freemem, needfree);

	/*
	 * We are going to wait.
	 * We currently hold all of the pcf_locks,
	 * get the new_freemem_lock (it protects freemem_wait),
	 * before dropping the pcf_locks.
	 */
	mutex_enter(&new_freemem_lock);

	p = pcf;
	for (i = 0; i < PCF_FANOUT; i++) {
		p->pcf_wait++;
		mutex_exit(&p->pcf_lock);
		p++;
	}

	needfree += npages;
	freemem_wait++;

	cv_wait(&freemem_cv, &new_freemem_lock);

	freemem_wait--;
	needfree -= npages;

	mutex_exit(&new_freemem_lock);

	TRACE_2(TR_FAC_VM, TR_PAGE_CREATE_SLEEP_END,
	    "page_create_sleep_end: freemem %ld needfree %ld",
	    freemem, needfree);

	VM_STAT_ADD(page_create_not_enough_again);
	goto checkagain;
}

/*
 * A routine to do the opposite of page_create_wait().
 */
void
page_create_putback(spgcnt_t npages)
{
	struct pcf	*p;
	pgcnt_t		lump;
	uint_t		*which;

	/*
	 * When a contiguous lump is broken up, we have to
	 * deal with lots of pages (min 64) so lets spread
	 * the wealth around.
	 */
	lump = roundup(npages, PCF_FANOUT) / PCF_FANOUT;
	freemem += npages;

	for (p = pcf; (npages > 0) && (p < &pcf[PCF_FANOUT]); p++) {
		which = &p->pcf_count;

		mutex_enter(&p->pcf_lock);

		if (p->pcf_block) {
			which = &p->pcf_reserve;
		}

		if (lump < npages) {
			*which += (uint_t)lump;
			npages -= lump;
		} else {
			*which += (uint_t)npages;
			npages = 0;
		}

		if (p->pcf_wait) {
			mutex_enter(&new_freemem_lock);
			/*
			 * Check to see if some other thread
			 * is actually waiting.  Another bucket
			 * may have woken it up by now.  If there
			 * are no waiters, then set our pcf_wait
			 * count to zero to avoid coming in here
			 * next time.
			 */
			if (freemem_wait) {
				if (npages > 1) {
					cv_broadcast(&freemem_cv);
				} else {
					cv_signal(&freemem_cv);
				}
				p->pcf_wait--;
			} else {
				p->pcf_wait = 0;
			}
			mutex_exit(&new_freemem_lock);
		}
		mutex_exit(&p->pcf_lock);
	}
	ASSERT(npages == 0);
}

/*
 * A helper routine for page_create_get_something.
 * The indenting got to deep down there.
 * Unblock the pcf counters.  Any pages freed after
 * pcf_block got set are moved to pcf_count and
 * wakeups (cv_broadcast() or cv_signal()) are done as needed.
 */
static void
pcgs_unblock(void)
{
	int		i;
	struct pcf	*p;

	/* Update freemem while we're here. */
	freemem = 0;
	p = pcf;
	for (i = 0; i < PCF_FANOUT; i++) {
		mutex_enter(&p->pcf_lock);
		ASSERT(p->pcf_count == 0);
		p->pcf_count = p->pcf_reserve;
		p->pcf_block = 0;
		freemem += p->pcf_count;
		if (p->pcf_wait) {
			mutex_enter(&new_freemem_lock);
			if (freemem_wait) {
				if (p->pcf_reserve > 1) {
					cv_broadcast(&freemem_cv);
					p->pcf_wait = 0;
				} else {
					cv_signal(&freemem_cv);
					p->pcf_wait--;
				}
			} else {
				p->pcf_wait = 0;
			}
			mutex_exit(&new_freemem_lock);
		}
		p->pcf_reserve = 0;
		mutex_exit(&p->pcf_lock);
		p++;
	}
}

/*
 * Called from page_create_va() when both the cache and free lists
 * have been checked once.
 *
 * Either returns a page or panics since the accounting was done
 * way before we got here.
 *
 * We don't come here often, so leave the accounting on permanently.
 */

#define	MAX_PCGS	100

#ifdef	DEBUG
#define	PCGS_TRIES	100
#else	/* DEBUG */
#define	PCGS_TRIES	10
#endif	/* DEBUG */

#ifdef	VM_STATS
uint_t	pcgs_counts[PCGS_TRIES];
uint_t	pcgs_too_many;
uint_t	pcgs_entered;
uint_t	pcgs_entered_noreloc;
uint_t	pcgs_locked;
uint_t	pcgs_cagelocked;
#endif	/* VM_STATS */

static page_t *
page_create_get_something(vnode_t *vp, u_offset_t off, struct seg *seg,
    caddr_t vaddr, uint_t flags)
{
	uint_t		count;
	page_t		*pp;
	uint_t		locked, i;
	struct	pcf	*p;
	lgrp_t		*lgrp;
	int		cagelocked = 0;

	VM_STAT_ADD(pcgs_entered);

	/*
	 * Tap any reserve freelists: if we fail now, we'll die
	 * since the page(s) we're looking for have already been
	 * accounted for.
	 */
	flags |= PG_PANIC;

	if ((flags & PG_NORELOC) != 0) {
		VM_STAT_ADD(pcgs_entered_noreloc);
		/*
		 * Requests for free pages from critical threads
		 * such as pageout still won't throttle here, but
		 * we must try again, to give the cageout thread
		 * another chance to catch up. Since we already
		 * accounted for the pages, we had better get them
		 * this time.
		 *
		 * N.B. All non-critical threads acquire the pcgs_cagelock
		 * to serialize access to the freelists. This implements a
		 * turnstile-type synchornization to avoid starvation of
		 * critical requests for PG_NORELOC memory by non-critical
		 * threads: all non-critical threads must acquire a 'ticket'
		 * before passing through, which entails making sure
		 * kcage_freemem won't fall below minfree prior to grabbing
		 * pages from the freelists.
		 */
		if (kcage_create_throttle(1, flags) == KCT_NONCRIT) {
			mutex_enter(&pcgs_cagelock);
			cagelocked = 1;
			VM_STAT_ADD(pcgs_cagelocked);
		}
	}

	/*
	 * Time to get serious.
	 * We failed to get a `correctly colored' page from both the
	 * free and cache lists.
	 * We escalate in stage.
	 *
	 * First try both lists without worring about color.
	 *
	 * Then, grab all page accounting locks (ie. pcf[]) and
	 * steal any pages that they have and set the pcf_block flag to
	 * stop deletions from the lists.  This will help because
	 * a page can get added to the free list while we are looking
	 * at the cache list, then another page could be added to the cache
	 * list allowing the page on the free list to be removed as we
	 * move from looking at the cache list to the free list. This
	 * could happen over and over. We would never find the page
	 * we have accounted for.
	 *
	 * Noreloc pages are a subset of the global (relocatable) page pool.
	 * They are not tracked separately in the pcf bins, so it is
	 * impossible to know when doing pcf accounting if the available
	 * page(s) are noreloc pages or not. When looking for a noreloc page
	 * it is quite easy to end up here even if the global (relocatable)
	 * page pool has plenty of free pages but the noreloc pool is empty.
	 *
	 * When the noreloc pool is empty (or low), additional noreloc pages
	 * are created by converting pages from the global page pool. This
	 * process will stall during pcf accounting if the pcf bins are
	 * already locked. Such is the case when a noreloc allocation is
	 * looping here in page_create_get_something waiting for more noreloc
	 * pages to appear.
	 *
	 * Short of adding a new field to the pcf bins to accurately track
	 * the number of free noreloc pages, we instead do not grab the
	 * pcgs_lock, do not set the pcf blocks and do not timeout when
	 * allocating a noreloc page. This allows noreloc allocations to
	 * loop without blocking global page pool allocations.
	 *
	 * NOTE: the behaviour of page_create_get_something has not changed
	 * for the case of global page pool allocations.
	 */

	flags &= ~PG_MATCH_COLOR;
	locked = 0;
#ifndef __sparc
	/*
	 * page_create_get_something may be called because 4g memory may be
	 * depleted. Set flags to allow for relocation of base page below
	 * 4g if necessary.
	 */
	if (physmax4g)
		flags |= (PGI_PGCPSZC0 | PGI_PGCPHIPRI);
#endif

	lgrp = lgrp_mem_choose(seg, vaddr, PAGESIZE);

	for (count = 0; kcage_on || count < MAX_PCGS; count++) {
		pp = page_get_freelist(vp, off, seg, vaddr, PAGESIZE,
		    flags, lgrp);
		if (pp == NULL) {
			pp = page_get_cachelist(vp, off, seg, vaddr,
				flags, lgrp);
		}
		if (pp == NULL) {
			/*
			 * Serialize.  Don't fight with other pcgs().
			 */
			if (!locked && (!kcage_on || !(flags & PG_NORELOC))) {
				mutex_enter(&pcgs_lock);
				VM_STAT_ADD(pcgs_locked);
				locked = 1;
				p = pcf;
				for (i = 0; i < PCF_FANOUT; i++) {
					mutex_enter(&p->pcf_lock);
					ASSERT(p->pcf_block == 0);
					p->pcf_block = 1;
					p->pcf_reserve = p->pcf_count;
					p->pcf_count = 0;
					mutex_exit(&p->pcf_lock);
					p++;
				}
				freemem = 0;
			}

			if (count) {
				/*
				 * Since page_free() puts pages on
				 * a list then accounts for it, we
				 * just have to wait for page_free()
				 * to unlock any page it was working
				 * with. The page_lock()-page_reclaim()
				 * path falls in the same boat.
				 *
				 * We don't need to check on the
				 * PG_WAIT flag, we have already
				 * accounted for the page we are
				 * looking for in page_create_va().
				 *
				 * We just wait a moment to let any
				 * locked pages on the lists free up,
				 * then continue around and try again.
				 *
				 * Will be awakened by set_freemem().
				 */
				mutex_enter(&pcgs_wait_lock);
				cv_wait(&pcgs_cv, &pcgs_wait_lock);
				mutex_exit(&pcgs_wait_lock);
			}
		} else {
#ifdef VM_STATS
			if (count >= PCGS_TRIES) {
				VM_STAT_ADD(pcgs_too_many);
			} else {
				VM_STAT_ADD(pcgs_counts[count]);
			}
#endif
			if (locked) {
				pcgs_unblock();
				mutex_exit(&pcgs_lock);
			}
			if (cagelocked)
				mutex_exit(&pcgs_cagelock);
			return (pp);
		}
	}
	/*
	 * we go down holding the pcf locks.
	 */
	panic("no %spage found %d",
	    ((flags & PG_NORELOC) ? "non-reloc " : ""), count);
	/*NOTREACHED*/
}

/*
 * Create enough pages for "bytes" worth of data starting at
 * "off" in "vp".
 *
 *	Where flag must be one of:
 *
 *		PG_EXCL:	Exclusive create (fail if any page already
 *				exists in the page cache) which does not
 *				wait for memory to become available.
 *
 *		PG_WAIT:	Non-exclusive create which can wait for
 *				memory to become available.
 *
 *		PG_PHYSCONTIG:	Allocate physically contiguous pages.
 *				(Not Supported)
 *
 * A doubly linked list of pages is returned to the caller.  Each page
 * on the list has the "exclusive" (p_selock) lock and "iolock" (p_iolock)
 * lock.
 *
 * Unable to change the parameters to page_create() in a minor release,
 * we renamed page_create() to page_create_va(), changed all known calls
 * from page_create() to page_create_va(), and created this wrapper.
 *
 * Upon a major release, we should break compatibility by deleting this
 * wrapper, and replacing all the strings "page_create_va", with "page_create".
 *
 * NOTE: There is a copy of this interface as page_create_io() in
 *	 i86/vm/vm_machdep.c. Any bugs fixed here should be applied
 *	 there.
 */
page_t *
page_create(vnode_t *vp, u_offset_t off, size_t bytes, uint_t flags)
{
	caddr_t random_vaddr;
	struct seg kseg;

#ifdef DEBUG
	cmn_err(CE_WARN, "Using deprecated interface page_create: caller %p",
	    (void *)caller());
#endif

	random_vaddr = (caddr_t)(((uintptr_t)vp >> 7) ^
	    (uintptr_t)(off >> PAGESHIFT));
	kseg.s_as = &kas;

	return (page_create_va(vp, off, bytes, flags, &kseg, random_vaddr));
}

#ifdef DEBUG
uint32_t pg_alloc_pgs_mtbf = 0;
#endif

/*
 * Used for large page support. It will attempt to allocate
 * a large page(s) off the freelist.
 *
 * Returns non zero on failure.
 */
int
page_alloc_pages(struct vnode *vp, struct seg *seg, caddr_t addr,
    page_t **basepp, page_t *ppa[], uint_t szc, int anypgsz)
{
	pgcnt_t		npgs, curnpgs, totpgs;
	size_t		pgsz;
	page_t		*pplist = NULL, *pp;
	int		err = 0;
	lgrp_t		*lgrp;

	ASSERT(szc != 0 && szc <= (page_num_pagesizes() - 1));

	VM_STAT_ADD(alloc_pages[0]);

#ifdef DEBUG
	if (pg_alloc_pgs_mtbf && !(gethrtime() % pg_alloc_pgs_mtbf)) {
		return (ENOMEM);
	}
#endif

	pgsz = page_get_pagesize(szc);
	totpgs = curnpgs = npgs = pgsz >> PAGESHIFT;

	ASSERT(((uintptr_t)addr & (pgsz - 1)) == 0);
	/*
	 * One must be NULL but not both.
	 * And one must be non NULL but not both.
	 */
	ASSERT(basepp != NULL || ppa != NULL);
	ASSERT(basepp == NULL || ppa == NULL);

	(void) page_create_wait(npgs, PG_WAIT);

	while (npgs && szc) {
		lgrp = lgrp_mem_choose(seg, addr, pgsz);
		pp = page_get_freelist(vp, 0, seg, addr, pgsz, 0, lgrp);
		if (pp != NULL) {
			VM_STAT_ADD(alloc_pages[1]);
			page_list_concat(&pplist, &pp);
			ASSERT(npgs >= curnpgs);
			npgs -= curnpgs;
		} else if (anypgsz) {
			VM_STAT_ADD(alloc_pages[2]);
			szc--;
			pgsz = page_get_pagesize(szc);
			curnpgs = pgsz >> PAGESHIFT;
		} else {
			VM_STAT_ADD(alloc_pages[3]);
			ASSERT(npgs == totpgs);
			page_create_putback(npgs);
			return (ENOMEM);
		}
	}
	if (szc == 0) {
		VM_STAT_ADD(alloc_pages[4]);
		ASSERT(npgs != 0);
		page_create_putback(npgs);
		err = ENOMEM;
	} else if (basepp != NULL) {
		ASSERT(npgs == 0);
		ASSERT(ppa == NULL);
		*basepp = pplist;
	}

	npgs = totpgs - npgs;
	pp = pplist;

	/*
	 * Clear the free and age bits. Also if we were passed in a ppa then
	 * fill it in with all the constituent pages from the large page. But
	 * if we failed to allocate all the pages just free what we got.
	 */
	while (npgs != 0) {
		ASSERT(PP_ISFREE(pp));
		ASSERT(PP_ISAGED(pp));
		if (ppa != NULL || err != 0) {
			if (err == 0) {
				VM_STAT_ADD(alloc_pages[5]);
				PP_CLRFREE(pp);
				PP_CLRAGED(pp);
				page_sub(&pplist, pp);
				*ppa++ = pp;
				npgs--;
			} else {
				VM_STAT_ADD(alloc_pages[6]);
				ASSERT(pp->p_szc != 0);
				curnpgs = page_get_pagecnt(pp->p_szc);
				page_list_break(&pp, &pplist, curnpgs);
				page_list_add_pages(pp, 0);
				page_create_putback(curnpgs);
				ASSERT(npgs >= curnpgs);
				npgs -= curnpgs;
			}
			pp = pplist;
		} else {
			VM_STAT_ADD(alloc_pages[7]);
			PP_CLRFREE(pp);
			PP_CLRAGED(pp);
			pp = pp->p_next;
			npgs--;
		}
	}
	return (err);
}

/*
 * Get a single large page off of the freelists, and set it up for use.
 * Number of bytes requested must be a supported page size.
 *
 * Note that this call may fail even if there is sufficient
 * memory available or PG_WAIT is set, so the caller must
 * be willing to fallback on page_create_va(), block and retry,
 * or fail the requester.
 */
page_t *
page_create_va_large(vnode_t *vp, u_offset_t off, size_t bytes, uint_t flags,
    struct seg *seg, caddr_t vaddr, void *arg)
{
	pgcnt_t		npages, pcftotal;
	page_t		*pp;
	page_t		*rootpp;
	lgrp_t		*lgrp;
	uint_t		enough;
	uint_t		pcf_index;
	uint_t		i;
	struct pcf	*p;
	struct pcf	*q;
	lgrp_id_t	*lgrpid = (lgrp_id_t *)arg;

	ASSERT(vp != NULL);

	ASSERT((flags & ~(PG_EXCL | PG_WAIT |
		    PG_NORELOC | PG_PANIC | PG_PUSHPAGE)) == 0);
	/* but no others */

	ASSERT((flags & PG_EXCL) == PG_EXCL);

	npages = btop(bytes);

	if (!kcage_on || panicstr) {
		/*
		 * Cage is OFF, or we are single threaded in
		 * panic, so make everything a RELOC request.
		 */
		flags &= ~PG_NORELOC;
	}

	/*
	 * Make sure there's adequate physical memory available.
	 * Note: PG_WAIT is ignored here.
	 */
	if (freemem <= throttlefree + npages) {
		VM_STAT_ADD(page_create_large_cnt[1]);
		return (NULL);
	}

	/*
	 * If cage is on, dampen draw from cage when available
	 * cage space is low.
	 */
	if ((flags & (PG_NORELOC | PG_WAIT)) ==  (PG_NORELOC | PG_WAIT) &&
	    kcage_freemem < kcage_throttlefree + npages) {

		/*
		 * The cage is on, the caller wants PG_NORELOC
		 * pages and available cage memory is very low.
		 * Call kcage_create_throttle() to attempt to
		 * control demand on the cage.
		 */
		if (kcage_create_throttle(npages, flags) == KCT_FAILURE) {
			VM_STAT_ADD(page_create_large_cnt[2]);
			return (NULL);
		}
	}

	enough = 0;
	pcf_index = PCF_INDEX();
	p = &pcf[pcf_index];
	p->pcf_touch = 1;
	q = &pcf[PCF_FANOUT];
	for (pcftotal = 0, i = 0; i < PCF_FANOUT; i++) {
		if (p->pcf_count > npages) {
			/*
			 * a good one to try.
			 */
			mutex_enter(&p->pcf_lock);
			if (p->pcf_count > npages) {
				p->pcf_count -= (uint_t)npages;
				/*
				 * freemem is not protected by any lock.
				 * Thus, we cannot have any assertion
				 * containing freemem here.
				 */
				freemem -= npages;
				enough = 1;
				mutex_exit(&p->pcf_lock);
				break;
			}
			mutex_exit(&p->pcf_lock);
		}
		pcftotal += p->pcf_count;
		p++;
		if (p >= q) {
			p = pcf;
		}
		p->pcf_touch = 1;
	}

	if (!enough) {
		/* If there isn't enough memory available, give up. */
		if (pcftotal < npages) {
			VM_STAT_ADD(page_create_large_cnt[3]);
			return (NULL);
		}

		/* try to collect pages from several pcf bins */
		for (p = pcf, pcftotal = 0, i = 0; i < PCF_FANOUT; i++) {
			p->pcf_touch = 1;
			mutex_enter(&p->pcf_lock);
			pcftotal += p->pcf_count;
			if (pcftotal >= npages) {
				/*
				 * Wow!  There are enough pages laying around
				 * to satisfy the request.  Do the accounting,
				 * drop the locks we acquired, and go back.
				 *
				 * freemem is not protected by any lock. So,
				 * we cannot have any assertion containing
				 * freemem.
				 */
				pgcnt_t	tpages = npages;
				freemem -= npages;
				while (p >= pcf) {
					if (p->pcf_count <= tpages) {
						tpages -= p->pcf_count;
						p->pcf_count = 0;
					} else {
						p->pcf_count -= (uint_t)tpages;
						tpages = 0;
					}
					mutex_exit(&p->pcf_lock);
					p--;
				}
				ASSERT(tpages == 0);
				break;
			}
			p++;
		}
		if (i == PCF_FANOUT) {
			/* failed to collect pages - release the locks */
			while (--p >= pcf) {
				mutex_exit(&p->pcf_lock);
			}
			VM_STAT_ADD(page_create_large_cnt[4]);
			return (NULL);
		}
	}

	/*
	 * This is where this function behaves fundamentally differently
	 * than page_create_va(); since we're intending to map the page
	 * with a single TTE, we have to get it as a physically contiguous
	 * hardware pagesize chunk.  If we can't, we fail.
	 */
	if (lgrpid != NULL && *lgrpid >= 0 && *lgrpid <= lgrp_alloc_max &&
		LGRP_EXISTS(lgrp_table[*lgrpid]))
		lgrp = lgrp_table[*lgrpid];
	else
		lgrp = lgrp_mem_choose(seg, vaddr, bytes);

	if ((rootpp = page_get_freelist(&kvp, off, seg, vaddr,
	    bytes, flags & ~PG_MATCH_COLOR, lgrp)) == NULL) {
		page_create_putback(npages);
		VM_STAT_ADD(page_create_large_cnt[5]);
		return (NULL);
	}

	/*
	 * if we got the page with the wrong mtype give it back this is a
	 * workaround for CR 6249718. When CR 6249718 is fixed we never get
	 * inside "if" and the workaround becomes just a nop
	 */
	if (kcage_on && (flags & PG_NORELOC) && !PP_ISNORELOC(rootpp)) {
		page_list_add_pages(rootpp, 0);
		page_create_putback(npages);
		VM_STAT_ADD(page_create_large_cnt[6]);
		return (NULL);
	}

	/*
	 * If satisfying this request has left us with too little
	 * memory, start the wheels turning to get some back.  The
	 * first clause of the test prevents waking up the pageout
	 * daemon in situations where it would decide that there's
	 * nothing to do.
	 */
	if (nscan < desscan && freemem < minfree) {
		TRACE_1(TR_FAC_VM, TR_PAGEOUT_CV_SIGNAL,
		    "pageout_cv_signal:freemem %ld", freemem);
		cv_signal(&proc_pageout->p_cv);
	}

	pp = rootpp;
	while (npages--) {
		ASSERT(PAGE_EXCL(pp));
		ASSERT(pp->p_vnode == NULL);
		ASSERT(!hat_page_is_mapped(pp));
		PP_CLRFREE(pp);
		PP_CLRAGED(pp);
		if (!page_hashin(pp, vp, off, NULL))
			panic("page_create_large: hashin failed: page %p",
			    (void *)pp);
		page_io_lock(pp);
		off += PAGESIZE;
		pp = pp->p_next;
	}

	VM_STAT_ADD(page_create_large_cnt[0]);
	return (rootpp);
}

page_t *
page_create_va(vnode_t *vp, u_offset_t off, size_t bytes, uint_t flags,
    struct seg *seg, caddr_t vaddr)
{
	page_t		*plist = NULL;
	pgcnt_t		npages;
	pgcnt_t		found_on_free = 0;
	pgcnt_t		pages_req;
	page_t		*npp = NULL;
	uint_t		enough;
	uint_t		i;
	uint_t		pcf_index;
	struct pcf	*p;
	struct pcf	*q;
	lgrp_t		*lgrp;

	TRACE_4(TR_FAC_VM, TR_PAGE_CREATE_START,
		"page_create_start:vp %p off %llx bytes %lu flags %x",
		vp, off, bytes, flags);

	ASSERT(bytes != 0 && vp != NULL);

	if ((flags & PG_EXCL) == 0 && (flags & PG_WAIT) == 0) {
		panic("page_create: invalid flags");
		/*NOTREACHED*/
	}
	ASSERT((flags & ~(PG_EXCL | PG_WAIT |
	    PG_NORELOC | PG_PANIC | PG_PUSHPAGE)) == 0);
	    /* but no others */

	pages_req = npages = btopr(bytes);
	/*
	 * Try to see whether request is too large to *ever* be
	 * satisfied, in order to prevent deadlock.  We arbitrarily
	 * decide to limit maximum size requests to max_page_get.
	 */
	if (npages >= max_page_get) {
		if ((flags & PG_WAIT) == 0) {
			TRACE_4(TR_FAC_VM, TR_PAGE_CREATE_TOOBIG,
			    "page_create_toobig:vp %p off %llx npages "
			    "%lu max_page_get %lu",
			    vp, off, npages, max_page_get);
			return (NULL);
		} else {
			cmn_err(CE_WARN,
			    "Request for too much kernel memory "
			    "(%lu bytes), will hang forever", bytes);
			for (;;)
				delay(1000000000);
		}
	}

	if (!kcage_on || panicstr) {
		/*
		 * Cage is OFF, or we are single threaded in
		 * panic, so make everything a RELOC request.
		 */
		flags &= ~PG_NORELOC;
	}

	if (freemem <= throttlefree + npages)
		if (!page_create_throttle(npages, flags))
			return (NULL);

	/*
	 * If cage is on, dampen draw from cage when available
	 * cage space is low.
	 */
	if ((flags & PG_NORELOC) &&
		kcage_freemem < kcage_throttlefree + npages) {

		/*
		 * The cage is on, the caller wants PG_NORELOC
		 * pages and available cage memory is very low.
		 * Call kcage_create_throttle() to attempt to
		 * control demand on the cage.
		 */
		if (kcage_create_throttle(npages, flags) == KCT_FAILURE)
			return (NULL);
	}

	VM_STAT_ADD(page_create_cnt[0]);

	enough = 0;
	pcf_index = PCF_INDEX();

	p = &pcf[pcf_index];
	p->pcf_touch = 1;
	q = &pcf[PCF_FANOUT];
	for (i = 0; i < PCF_FANOUT; i++) {
		if (p->pcf_count > npages) {
			/*
			 * a good one to try.
			 */
			mutex_enter(&p->pcf_lock);
			if (p->pcf_count > npages) {
				p->pcf_count -= (uint_t)npages;
				/*
				 * freemem is not protected by any lock.
				 * Thus, we cannot have any assertion
				 * containing freemem here.
				 */
				freemem -= npages;
				enough = 1;
				mutex_exit(&p->pcf_lock);
				break;
			}
			mutex_exit(&p->pcf_lock);
		}
		p++;
		if (p >= q) {
			p = pcf;
		}
		p->pcf_touch = 1;
	}

	if (!enough) {
		/*
		 * Have to look harder.  If npages is greater than
		 * one, then we might have to coalecse the counters.
		 *
		 * Go wait.  We come back having accounted
		 * for the memory.
		 */
		VM_STAT_ADD(page_create_cnt[1]);
		if (!page_create_wait(npages, flags)) {
			VM_STAT_ADD(page_create_cnt[2]);
			return (NULL);
		}
	}

	TRACE_2(TR_FAC_VM, TR_PAGE_CREATE_SUCCESS,
		"page_create_success:vp %p off %llx", vp, off);

	/*
	 * If satisfying this request has left us with too little
	 * memory, start the wheels turning to get some back.  The
	 * first clause of the test prevents waking up the pageout
	 * daemon in situations where it would decide that there's
	 * nothing to do.
	 */
	if (nscan < desscan && freemem < minfree) {
		TRACE_1(TR_FAC_VM, TR_PAGEOUT_CV_SIGNAL,
			"pageout_cv_signal:freemem %ld", freemem);
		cv_signal(&proc_pageout->p_cv);
	}

	/*
	 * Loop around collecting the requested number of pages.
	 * Most of the time, we have to `create' a new page. With
	 * this in mind, pull the page off the free list before
	 * getting the hash lock.  This will minimize the hash
	 * lock hold time, nesting, and the like.  If it turns
	 * out we don't need the page, we put it back at the end.
	 */
	while (npages--) {
		page_t		*pp;
		kmutex_t	*phm = NULL;
		ulong_t		index;

		index = PAGE_HASH_FUNC(vp, off);
top:
		ASSERT(phm == NULL);
		ASSERT(index == PAGE_HASH_FUNC(vp, off));
		ASSERT(MUTEX_NOT_HELD(page_vnode_mutex(vp)));

		if (npp == NULL) {
			/*
			 * Try to get a page from the freelist (ie,
			 * a page with no [vp, off] tag).  If that
			 * fails, use the cachelist.
			 *
			 * During the first attempt at both the free
			 * and cache lists we try for the correct color.
			 */
			/*
			 * XXXX-how do we deal with virtual indexed
			 * caches and and colors?
			 */
			VM_STAT_ADD(page_create_cnt[4]);
			/*
			 * Get lgroup to allocate next page of shared memory
			 * from and use it to specify where to allocate
			 * the physical memory
			 */
			lgrp = lgrp_mem_choose(seg, vaddr, PAGESIZE);
			npp = page_get_freelist(vp, off, seg, vaddr, PAGESIZE,
			    flags | PG_MATCH_COLOR, lgrp);
			if (npp == NULL) {
				npp = page_get_cachelist(vp, off, seg,
				    vaddr, flags | PG_MATCH_COLOR, lgrp);
				if (npp == NULL) {
					npp = page_create_get_something(vp,
					    off, seg, vaddr,
					    flags & ~PG_MATCH_COLOR);
				}

				if (PP_ISAGED(npp) == 0) {
					/*
					 * Since this page came from the
					 * cachelist, we must destroy the
					 * old vnode association.
					 */
					page_hashout(npp, NULL);
				}
			}
		}

		/*
		 * We own this page!
		 */
		ASSERT(PAGE_EXCL(npp));
		ASSERT(npp->p_vnode == NULL);
		ASSERT(!hat_page_is_mapped(npp));
		PP_CLRFREE(npp);
		PP_CLRAGED(npp);

		/*
		 * Here we have a page in our hot little mits and are
		 * just waiting to stuff it on the appropriate lists.
		 * Get the mutex and check to see if it really does
		 * not exist.
		 */
		phm = PAGE_HASH_MUTEX(index);
		mutex_enter(phm);
		PAGE_HASH_SEARCH(index, pp, vp, off);
		if (pp == NULL) {
			VM_STAT_ADD(page_create_new);
			pp = npp;
			npp = NULL;
			if (!page_hashin(pp, vp, off, phm)) {
				/*
				 * Since we hold the page hash mutex and
				 * just searched for this page, page_hashin
				 * had better not fail.  If it does, that
				 * means somethread did not follow the
				 * page hash mutex rules.  Panic now and
				 * get it over with.  As usual, go down
				 * holding all the locks.
				 */
				ASSERT(MUTEX_HELD(phm));
				panic("page_create: "
				    "hashin failed %p %p %llx %p",
				    (void *)pp, (void *)vp, off, (void *)phm);
				/*NOTREACHED*/
			}
			ASSERT(MUTEX_HELD(phm));
			mutex_exit(phm);
			phm = NULL;

			/*
			 * Hat layer locking need not be done to set
			 * the following bits since the page is not hashed
			 * and was on the free list (i.e., had no mappings).
			 *
			 * Set the reference bit to protect
			 * against immediate pageout
			 *
			 * XXXmh modify freelist code to set reference
			 * bit so we don't have to do it here.
			 */
			page_set_props(pp, P_REF);
			found_on_free++;
		} else {
			VM_STAT_ADD(page_create_exists);
			if (flags & PG_EXCL) {
				/*
				 * Found an existing page, and the caller
				 * wanted all new pages.  Undo all of the work
				 * we have done.
				 */
				mutex_exit(phm);
				phm = NULL;
				while (plist != NULL) {
					pp = plist;
					page_sub(&plist, pp);
					page_io_unlock(pp);
					/* large pages should not end up here */
					ASSERT(pp->p_szc == 0);
					/*LINTED: constant in conditional ctx*/
					VN_DISPOSE(pp, B_INVAL, 0, kcred);
				}
				VM_STAT_ADD(page_create_found_one);
				goto fail;
			}
			ASSERT(flags & PG_WAIT);
			if (!page_lock(pp, SE_EXCL, phm, P_NO_RECLAIM)) {
				/*
				 * Start all over again if we blocked trying
				 * to lock the page.
				 */
				mutex_exit(phm);
				VM_STAT_ADD(page_create_page_lock_failed);
				phm = NULL;
				goto top;
			}
			mutex_exit(phm);
			phm = NULL;

			if (PP_ISFREE(pp)) {
				ASSERT(PP_ISAGED(pp) == 0);
				VM_STAT_ADD(pagecnt.pc_get_cache);
				page_list_sub(pp, PG_CACHE_LIST);
				PP_CLRFREE(pp);
				found_on_free++;
			}
		}

		/*
		 * Got a page!  It is locked.  Acquire the i/o
		 * lock since we are going to use the p_next and
		 * p_prev fields to link the requested pages together.
		 */
		page_io_lock(pp);
		page_add(&plist, pp);
		plist = plist->p_next;
		off += PAGESIZE;
		vaddr += PAGESIZE;
	}

	ASSERT((flags & PG_EXCL) ? (found_on_free == pages_req) : 1);
fail:
	if (npp != NULL) {
		/*
		 * Did not need this page after all.
		 * Put it back on the free list.
		 */
		VM_STAT_ADD(page_create_putbacks);
		PP_SETFREE(npp);
		PP_SETAGED(npp);
		npp->p_offset = (u_offset_t)-1;
		page_list_add(npp, PG_FREE_LIST | PG_LIST_TAIL);
		page_unlock(npp);

	}

	ASSERT(pages_req >= found_on_free);

	{
		uint_t overshoot = (uint_t)(pages_req - found_on_free);

		if (overshoot) {
			VM_STAT_ADD(page_create_overshoot);
			p = &pcf[pcf_index];
			p->pcf_touch = 1;
			mutex_enter(&p->pcf_lock);
			if (p->pcf_block) {
				p->pcf_reserve += overshoot;
			} else {
				p->pcf_count += overshoot;
				if (p->pcf_wait) {
					mutex_enter(&new_freemem_lock);
					if (freemem_wait) {
						cv_signal(&freemem_cv);
						p->pcf_wait--;
					} else {
						p->pcf_wait = 0;
					}
					mutex_exit(&new_freemem_lock);
				}
			}
			mutex_exit(&p->pcf_lock);
			/* freemem is approximate, so this test OK */
			if (!p->pcf_block)
				freemem += overshoot;
		}
	}

	return (plist);
}

/*
 * One or more constituent pages of this large page has been marked
 * toxic. Simply demote the large page to PAGESIZE pages and let
 * page_free() handle it. This routine should only be called by
 * large page free routines (page_free_pages() and page_destroy_pages().
 * All pages are locked SE_EXCL and have already been marked free.
 */
static void
page_free_toxic_pages(page_t *rootpp)
{
	page_t	*tpp;
	pgcnt_t	i, pgcnt = page_get_pagecnt(rootpp->p_szc);
	uint_t	szc = rootpp->p_szc;

	for (i = 0, tpp = rootpp; i < pgcnt; i++, tpp = tpp->p_next) {
		ASSERT(tpp->p_szc == szc);
		ASSERT((PAGE_EXCL(tpp) &&
		    !page_iolock_assert(tpp)) || panicstr);
		tpp->p_szc = 0;
	}

	while (rootpp != NULL) {
		tpp = rootpp;
		page_sub(&rootpp, tpp);
		ASSERT(PP_ISFREE(tpp));
		PP_CLRFREE(tpp);
		page_free(tpp, 1);
	}
}

/*
 * Put page on the "free" list.
 * The free list is really two lists maintained by
 * the PSM of whatever machine we happen to be on.
 */
void
page_free(page_t *pp, int dontneed)
{
	struct pcf	*p;
	uint_t		pcf_index;

	ASSERT((PAGE_EXCL(pp) &&
	    !page_iolock_assert(pp)) || panicstr);

	if (page_deteriorating(pp)) {
		volatile int i = 0;
		char *kaddr;
		volatile int rb, wb;
		uint64_t pa;
		volatile int ue = 0;
		on_trap_data_t otd;

		if (pp->p_vnode != NULL) {
			/*
			 * Let page_destroy() do its bean counting and
			 * hash out the page; it will then call back
			 * into page_free() with pp->p_vnode == NULL.
			 */
			page_destroy(pp, 0);
			return;
		}

		if (page_isfailing(pp)) {
			/*
			 * If we have already exceeded the limit for
			 * pages retired, we will treat this page as
			 * 'toxic' rather than failing. That will ensure
			 * that the page is at least cleaned, and if
			 * a UE is detected, the page will be retired
			 * anyway.
			 */
			if (pages_retired_limit_exceeded()) {
				/*
				 * clear the flag and reset to toxic
				 */
				page_clrtoxic(pp);
				page_settoxic(pp, PAGE_IS_TOXIC);
			} else {
				pa = ptob((uint64_t)page_pptonum(pp));
				if (page_retire_messages) {
					cmn_err(CE_NOTE, "Page 0x%08x.%08x "
					    "removed from service",
					    (uint32_t)(pa >> 32), (uint32_t)pa);
				}
				goto page_failed;
			}
		}

		pagescrub(pp, 0, PAGESIZE);

		/*
		 * We want to determine whether the error that occurred on
		 * this page is transient or persistent, so we get a mapping
		 * to the page and try every possible bit pattern to compare
		 * what we write with what we read back.  A smaller number
		 * of bit patterns might suffice, but there's no point in
		 * getting fancy.  If this is the hot path on your system,
		 * you've got bigger problems.
		 */
		kaddr = ppmapin(pp, PROT_READ | PROT_WRITE, (caddr_t)-1);
		for (wb = 0xff; wb >= 0; wb--) {
			if (on_trap(&otd, OT_DATA_EC)) {
				pa = ptob((uint64_t)page_pptonum(pp)) + i;
				page_settoxic(pp, PAGE_IS_FAILING);

				if (page_retire_messages) {
					cmn_err(CE_WARN, "Uncorrectable Error "
					    "occurred at PA 0x%08x.%08x while "
					    "attempting to clear previously "
					    "reported error; page removed from "
					    "service", (uint32_t)(pa >> 32),
					    (uint32_t)pa);
				}

				ue++;
				break;
			}

			/*
			 * Write out the bit pattern, flush it to memory, and
			 * read it back while under on_trap() protection.
			 */
			for (i = 0; i < PAGESIZE; i++)
				kaddr[i] = wb;

			sync_data_memory(kaddr, PAGESIZE);

			for (i = 0; i < PAGESIZE; i++) {
				if ((rb = (uchar_t)kaddr[i]) != wb) {
					page_settoxic(pp, PAGE_IS_FAILING);
					goto out;
				}
			}
		}
out:
		no_trap();
		ppmapout(kaddr);

		if (wb >= 0 && !ue) {
			pa = ptob((uint64_t)page_pptonum(pp)) + i;
			if (page_retire_messages) {
				cmn_err(CE_WARN, "Data Mismatch occurred at PA "
				    "0x%08x.%08x [ 0x%x != 0x%x ] while "
				    "attempting to clear previously reported "
				    "error; page removed from service",
				    (uint32_t)(pa >> 32), (uint32_t)pa, rb, wb);
			}
		}
page_failed:
		/*
		 * DR operations change the association between a page_t
		 * and the physical page it represents. Check if the
		 * page is still bad. If it is, then retire it.
		 */
		if (page_isfaulty(pp) && page_isfailing(pp)) {
			/*
			 * In the future, it might be useful to have a platform
			 * callback here to tell the hardware to fence off this
			 * page during the next reboot.
			 *
			 * We move the page to the retired_vnode here
			 */
			(void) page_hashin(pp, &retired_ppages,
			    (u_offset_t)ptob((uint64_t)page_pptonum(pp)), NULL);
			mutex_enter(&freemem_lock);
			availrmem--;
			mutex_exit(&freemem_lock);
			page_retired(pp);
			page_downgrade(pp);

			/*
			 * If DR raced with the above page retirement code,
			 * we might have retired a good page. If so, unretire
			 * the page.
			 */
			if (!page_isfaulty(pp))
				page_unretire_pages();
			return;
		}

		pa = ptob((uint64_t)page_pptonum(pp));

		if (page_retire_messages) {
			cmn_err(CE_NOTE, "Previously reported error on page "
			    "0x%08x.%08x cleared", (uint32_t)(pa >> 32),
			    (uint32_t)pa);
		}

		page_clrtoxic(pp);
	}

	if (PP_ISFREE(pp)) {
		panic("page_free: page %p is free", (void *)pp);
	}

	if (pp->p_szc != 0) {
		if (pp->p_vnode == NULL || IS_SWAPFSVP(pp->p_vnode) ||
		    pp->p_vnode == &kvp) {
			panic("page_free: anon or kernel "
			    "or no vnode large page %p", (void *)pp);
		}
		page_demote_vp_pages(pp);
		ASSERT(pp->p_szc == 0);
	}

	/*
	 * The page_struct_lock need not be acquired to examine these
	 * fields since the page has an "exclusive" lock.
	 */
	if (hat_page_is_mapped(pp) || pp->p_lckcnt != 0 || pp->p_cowcnt != 0) {
		panic("page_free pp=%p, pfn=%lx, lckcnt=%d, cowcnt=%d",
		    pp, page_pptonum(pp), pp->p_lckcnt, pp->p_cowcnt);
		/*NOTREACHED*/
	}

	ASSERT(!hat_page_getshare(pp));

	PP_SETFREE(pp);
	ASSERT(pp->p_vnode == NULL || !IS_VMODSORT(pp->p_vnode) ||
	    !hat_ismod(pp));
	page_clr_all_props(pp);
	ASSERT(!hat_page_getshare(pp));

	/*
	 * Now we add the page to the head of the free list.
	 * But if this page is associated with a paged vnode
	 * then we adjust the head forward so that the page is
	 * effectively at the end of the list.
	 */
	if (pp->p_vnode == NULL) {
		/*
		 * Page has no identity, put it on the free list.
		 */
		PP_SETAGED(pp);
		pp->p_offset = (u_offset_t)-1;
		page_list_add(pp, PG_FREE_LIST | PG_LIST_TAIL);
		VM_STAT_ADD(pagecnt.pc_free_free);
		TRACE_1(TR_FAC_VM, TR_PAGE_FREE_FREE,
		    "page_free_free:pp %p", pp);
	} else {
		PP_CLRAGED(pp);

		if (!dontneed || nopageage) {
			/* move it to the tail of the list */
			page_list_add(pp, PG_CACHE_LIST | PG_LIST_TAIL);

			VM_STAT_ADD(pagecnt.pc_free_cache);
			TRACE_1(TR_FAC_VM, TR_PAGE_FREE_CACHE_TAIL,
			    "page_free_cache_tail:pp %p", pp);
		} else {
			page_list_add(pp, PG_CACHE_LIST | PG_LIST_HEAD);

			VM_STAT_ADD(pagecnt.pc_free_dontneed);
			TRACE_1(TR_FAC_VM, TR_PAGE_FREE_CACHE_HEAD,
			    "page_free_cache_head:pp %p", pp);
		}
	}
	page_unlock(pp);

	/*
	 * Now do the `freemem' accounting.
	 */
	pcf_index = PCF_INDEX();
	p = &pcf[pcf_index];
	p->pcf_touch = 1;

	mutex_enter(&p->pcf_lock);
	if (p->pcf_block) {
		p->pcf_reserve += 1;
	} else {
		p->pcf_count += 1;
		if (p->pcf_wait) {
			mutex_enter(&new_freemem_lock);
			/*
			 * Check to see if some other thread
			 * is actually waiting.  Another bucket
			 * may have woken it up by now.  If there
			 * are no waiters, then set our pcf_wait
			 * count to zero to avoid coming in here
			 * next time.  Also, since only one page
			 * was put on the free list, just wake
			 * up one waiter.
			 */
			if (freemem_wait) {
				cv_signal(&freemem_cv);
				p->pcf_wait--;
			} else {
				p->pcf_wait = 0;
			}
			mutex_exit(&new_freemem_lock);
		}
	}
	mutex_exit(&p->pcf_lock);

	/* freemem is approximate, so this test OK */
	if (!p->pcf_block)
		freemem += 1;
}

/*
 * Put page on the "free" list during intial startup.
 * This happens during initial single threaded execution.
 */
void
page_free_at_startup(page_t *pp)
{
	struct pcf	*p;
	uint_t		pcf_index;

	page_list_add(pp, PG_FREE_LIST | PG_LIST_HEAD | PG_LIST_ISINIT);
	VM_STAT_ADD(pagecnt.pc_free_free);

	/*
	 * Now do the `freemem' accounting.
	 */
	pcf_index = PCF_INDEX();
	p = &pcf[pcf_index];
	p->pcf_touch = 1;

	ASSERT(p->pcf_block == 0);
	ASSERT(p->pcf_wait == 0);
	p->pcf_count += 1;

	/* freemem is approximate, so this is OK */
	freemem += 1;
}

void
page_free_pages(page_t *pp)
{
	page_t	*tpp, *rootpp = NULL;
	pgcnt_t	pgcnt = page_get_pagecnt(pp->p_szc);
	pgcnt_t	i;
	uint_t	szc = pp->p_szc;
	int	toxic = 0;

	VM_STAT_ADD(pagecnt.pc_free_pages);
	TRACE_1(TR_FAC_VM, TR_PAGE_FREE_FREE,
	    "page_free_free:pp %p", pp);

	ASSERT(pp->p_szc != 0 && pp->p_szc < page_num_pagesizes());
	if ((page_pptonum(pp) & (pgcnt - 1)) != 0) {
		panic("page_free_pages: not root page %p", (void *)pp);
		/*NOTREACHED*/
	}

	for (i = 0, tpp = pp; i < pgcnt; i++, tpp++) {
		ASSERT((PAGE_EXCL(tpp) &&
		    !page_iolock_assert(tpp)) || panicstr);
		if (PP_ISFREE(tpp)) {
			panic("page_free_pages: page %p is free", (void *)tpp);
			/*NOTREACHED*/
		}
		if (hat_page_is_mapped(tpp) || tpp->p_lckcnt != 0 ||
		    tpp->p_cowcnt != 0) {
			panic("page_free_pages %p", (void *)tpp);
			/*NOTREACHED*/
		}

		ASSERT(!hat_page_getshare(tpp));
		ASSERT(tpp->p_vnode == NULL);
		ASSERT(tpp->p_szc == szc);

		if (page_deteriorating(tpp))
			toxic = 1;

		PP_SETFREE(tpp);
		page_clr_all_props(tpp);
		PP_SETAGED(tpp);
		tpp->p_offset = (u_offset_t)-1;
		ASSERT(tpp->p_next == tpp);
		ASSERT(tpp->p_prev == tpp);
		page_list_concat(&rootpp, &tpp);
	}
	ASSERT(rootpp == pp);

	if (toxic) {
		page_free_toxic_pages(rootpp);
		return;
	}
	page_list_add_pages(rootpp, 0);
	page_create_putback(pgcnt);
}

int free_pages = 1;

/*
 * This routine attempts to return pages to the cachelist via page_release().
 * It does not *have* to be successful in all cases, since the pageout scanner
 * will catch any pages it misses.  It does need to be fast and not introduce
 * too much overhead.
 *
 * If a page isn't found on the unlocked sweep of the page_hash bucket, we
 * don't lock and retry.  This is ok, since the page scanner will eventually
 * find any page we miss in free_vp_pages().
 */
void
free_vp_pages(vnode_t *vp, u_offset_t off, size_t len)
{
	page_t *pp;
	u_offset_t eoff;
	extern int swap_in_range(vnode_t *, u_offset_t, size_t);

	eoff = off + len;

	if (free_pages == 0)
		return;
	if (swap_in_range(vp, off, len))
		return;

	for (; off < eoff; off += PAGESIZE) {

		/*
		 * find the page using a fast, but inexact search. It'll be OK
		 * if a few pages slip through the cracks here.
		 */
		pp = page_exists(vp, off);

		/*
		 * If we didn't find the page (it may not exist), the page
		 * is free, looks still in use (shared), or we can't lock it,
		 * just give up.
		 */
		if (pp == NULL ||
		    PP_ISFREE(pp) ||
		    page_share_cnt(pp) > 0 ||
		    !page_trylock(pp, SE_EXCL))
			continue;

		/*
		 * Once we have locked pp, verify that it's still the
		 * correct page and not already free
		 */
		ASSERT(PAGE_LOCKED_SE(pp, SE_EXCL));
		if (pp->p_vnode != vp || pp->p_offset != off || PP_ISFREE(pp)) {
			page_unlock(pp);
			continue;
		}

		/*
		 * try to release the page...
		 */
		(void) page_release(pp, 1);
	}
}

/*
 * Reclaim the given page from the free list.
 * Returns 1 on success or 0 on failure.
 *
 * The page is unlocked if it can't be reclaimed (when freemem == 0).
 * If `lock' is non-null, it will be dropped and re-acquired if
 * the routine must wait while freemem is 0.
 *
 * As it turns out, boot_getpages() does this.  It picks a page,
 * based on where OBP mapped in some address, gets its pfn, searches
 * the memsegs, locks the page, then pulls it off the free list!
 */
int
page_reclaim(page_t *pp, kmutex_t *lock)
{
	struct pcf	*p;
	uint_t		pcf_index;
	struct cpu	*cpup;
	int		enough;
	uint_t		i;

	ASSERT(lock != NULL ? MUTEX_HELD(lock) : 1);
	ASSERT(PAGE_EXCL(pp) && PP_ISFREE(pp));
	ASSERT(pp->p_szc == 0);

	/*
	 * If `freemem' is 0, we cannot reclaim this page from the
	 * freelist, so release every lock we might hold: the page,
	 * and the `lock' before blocking.
	 *
	 * The only way `freemem' can become 0 while there are pages
	 * marked free (have their p->p_free bit set) is when the
	 * system is low on memory and doing a page_create().  In
	 * order to guarantee that once page_create() starts acquiring
	 * pages it will be able to get all that it needs since `freemem'
	 * was decreased by the requested amount.  So, we need to release
	 * this page, and let page_create() have it.
	 *
	 * Since `freemem' being zero is not supposed to happen, just
	 * use the usual hash stuff as a starting point.  If that bucket
	 * is empty, then assume the worst, and start at the beginning
	 * of the pcf array.  If we always start at the beginning
	 * when acquiring more than one pcf lock, there won't be any
	 * deadlock problems.
	 */

	/* TODO: Do we need to test kcage_freemem if PG_NORELOC(pp)? */

	if (freemem <= throttlefree && !page_create_throttle(1l, 0)) {
		pcf_acquire_all();
		goto page_reclaim_nomem;
	}

	enough = 0;
	pcf_index = PCF_INDEX();
	p = &pcf[pcf_index];
	p->pcf_touch = 1;
	mutex_enter(&p->pcf_lock);
	if (p->pcf_count >= 1) {
		enough = 1;
		p->pcf_count--;
	}
	mutex_exit(&p->pcf_lock);

	if (!enough) {
		VM_STAT_ADD(page_reclaim_zero);
		/*
		 * Check again. Its possible that some other thread
		 * could have been right behind us, and added one
		 * to a list somewhere.  Acquire each of the pcf locks
		 * until we find a page.
		 */
		p = pcf;
		for (i = 0; i < PCF_FANOUT; i++) {
			p->pcf_touch = 1;
			mutex_enter(&p->pcf_lock);
			if (p->pcf_count >= 1) {
				p->pcf_count -= 1;
				enough = 1;
				break;
			}
			p++;
		}

		if (!enough) {
page_reclaim_nomem:
			/*
			 * We really can't have page `pp'.
			 * Time for the no-memory dance with
			 * page_free().  This is just like
			 * page_create_wait().  Plus the added
			 * attraction of releasing whatever mutex
			 * we held when we were called with in `lock'.
			 * Page_unlock() will wakeup any thread
			 * waiting around for this page.
			 */
			if (lock) {
				VM_STAT_ADD(page_reclaim_zero_locked);
				mutex_exit(lock);
			}
			page_unlock(pp);

			/*
			 * get this before we drop all the pcf locks.
			 */
			mutex_enter(&new_freemem_lock);

			p = pcf;
			for (i = 0; i < PCF_FANOUT; i++) {
				p->pcf_wait++;
				mutex_exit(&p->pcf_lock);
				p++;
			}

			freemem_wait++;
			cv_wait(&freemem_cv, &new_freemem_lock);
			freemem_wait--;

			mutex_exit(&new_freemem_lock);

			if (lock) {
				mutex_enter(lock);
			}
			return (0);
		}

		/*
		 * There was a page to be found.
		 * The pcf accounting has been done,
		 * though none of the pcf_wait flags have been set,
		 * drop the locks and continue on.
		 */
		while (p >= pcf) {
			mutex_exit(&p->pcf_lock);
			p--;
		}
	}

	/*
	 * freemem is not protected by any lock. Thus, we cannot
	 * have any assertion containing freemem here.
	 */
	freemem -= 1;

	VM_STAT_ADD(pagecnt.pc_reclaim);
	if (PP_ISAGED(pp)) {
		page_list_sub(pp, PG_FREE_LIST);
		TRACE_1(TR_FAC_VM, TR_PAGE_UNFREE_FREE,
		    "page_reclaim_free:pp %p", pp);
	} else {
		page_list_sub(pp, PG_CACHE_LIST);
		TRACE_1(TR_FAC_VM, TR_PAGE_UNFREE_CACHE,
		    "page_reclaim_cache:pp %p", pp);
	}

	/*
	 * clear the p_free & p_age bits since this page is no longer
	 * on the free list.  Notice that there was a brief time where
	 * a page is marked as free, but is not on the list.
	 *
	 * Set the reference bit to protect against immediate pageout.
	 */
	PP_CLRFREE(pp);
	PP_CLRAGED(pp);
	page_set_props(pp, P_REF);

	CPU_STATS_ENTER_K();
	cpup = CPU;	/* get cpup now that CPU cannot change */
	CPU_STATS_ADDQ(cpup, vm, pgrec, 1);
	CPU_STATS_ADDQ(cpup, vm, pgfrec, 1);
	CPU_STATS_EXIT_K();

	return (1);
}



/*
 * Destroy identity of the page and put it back on
 * the page free list.  Assumes that the caller has
 * acquired the "exclusive" lock on the page.
 */
void
page_destroy(page_t *pp, int dontfree)
{
	ASSERT((PAGE_EXCL(pp) &&
	    !page_iolock_assert(pp)) || panicstr);

	if (pp->p_szc != 0) {
		if (pp->p_vnode == NULL || IS_SWAPFSVP(pp->p_vnode) ||
		    pp->p_vnode == &kvp) {
			panic("page_destroy: anon or kernel or no vnode "
			    "large page %p", (void *)pp);
		}
		page_demote_vp_pages(pp);
		ASSERT(pp->p_szc == 0);
	}

	TRACE_1(TR_FAC_VM, TR_PAGE_DESTROY, "page_destroy:pp %p", pp);

	/*
	 * Unload translations, if any, then hash out the
	 * page to erase its identity.
	 */
	(void) hat_pageunload(pp, HAT_FORCE_PGUNLOAD);
	page_hashout(pp, NULL);

	if (!dontfree) {
		/*
		 * Acquire the "freemem_lock" for availrmem.
		 * The page_struct_lock need not be acquired for lckcnt
		 * and cowcnt since the page has an "exclusive" lock.
		 */
		if ((pp->p_lckcnt != 0) || (pp->p_cowcnt != 0)) {
			mutex_enter(&freemem_lock);
			if (pp->p_lckcnt != 0) {
				availrmem++;
				pp->p_lckcnt = 0;
			}
			if (pp->p_cowcnt != 0) {
				availrmem += pp->p_cowcnt;
				pp->p_cowcnt = 0;
			}
			mutex_exit(&freemem_lock);
		}
		/*
		 * Put the page on the "free" list.
		 */
		page_free(pp, 0);
	}
}

void
page_destroy_pages(page_t *pp)
{

	page_t	*tpp, *rootpp = NULL;
	pgcnt_t	pgcnt = page_get_pagecnt(pp->p_szc);
	pgcnt_t	i, pglcks = 0;
	uint_t	szc = pp->p_szc;
	int	toxic = 0;

	ASSERT(pp->p_szc != 0 && pp->p_szc < page_num_pagesizes());

	VM_STAT_ADD(pagecnt.pc_destroy_pages);

	TRACE_1(TR_FAC_VM, TR_PAGE_DESTROY, "page_destroy_pages:pp %p", pp);

	if ((page_pptonum(pp) & (pgcnt - 1)) != 0) {
		panic("page_destroy_pages: not root page %p", (void *)pp);
		/*NOTREACHED*/
	}

	for (i = 0, tpp = pp; i < pgcnt; i++, tpp++) {
		ASSERT((PAGE_EXCL(tpp) &&
		    !page_iolock_assert(tpp)) || panicstr);
		(void) hat_pageunload(tpp, HAT_FORCE_PGUNLOAD);
		page_hashout(tpp, NULL);
		ASSERT(tpp->p_offset == (u_offset_t)-1);
		if (tpp->p_lckcnt != 0) {
			pglcks++;
			tpp->p_lckcnt = 0;
		} else if (tpp->p_cowcnt != 0) {
			pglcks += tpp->p_cowcnt;
			tpp->p_cowcnt = 0;
		}
		ASSERT(!hat_page_getshare(tpp));
		ASSERT(tpp->p_vnode == NULL);
		ASSERT(tpp->p_szc == szc);

		if (page_deteriorating(tpp))
			toxic = 1;

		PP_SETFREE(tpp);
		page_clr_all_props(tpp);
		PP_SETAGED(tpp);
		ASSERT(tpp->p_next == tpp);
		ASSERT(tpp->p_prev == tpp);
		page_list_concat(&rootpp, &tpp);
	}

	ASSERT(rootpp == pp);
	if (pglcks != 0) {
		mutex_enter(&freemem_lock);
		availrmem += pglcks;
		mutex_exit(&freemem_lock);
	}

	if (toxic) {
		page_free_toxic_pages(rootpp);
		return;
	}
	page_list_add_pages(rootpp, 0);
	page_create_putback(pgcnt);
}

/*
 * Similar to page_destroy(), but destroys pages which are
 * locked and known to be on the page free list.  Since
 * the page is known to be free and locked, no one can access
 * it.
 *
 * Also, the number of free pages does not change.
 */
void
page_destroy_free(page_t *pp)
{
	ASSERT(PAGE_EXCL(pp));
	ASSERT(PP_ISFREE(pp));
	ASSERT(pp->p_vnode);
	ASSERT(hat_page_getattr(pp, P_MOD | P_REF | P_RO) == 0);
	ASSERT(!hat_page_is_mapped(pp));
	ASSERT(PP_ISAGED(pp) == 0);
	ASSERT(pp->p_szc == 0);

	VM_STAT_ADD(pagecnt.pc_destroy_free);
	page_list_sub(pp, PG_CACHE_LIST);

	page_hashout(pp, NULL);
	ASSERT(pp->p_vnode == NULL);
	ASSERT(pp->p_offset == (u_offset_t)-1);
	ASSERT(pp->p_hash == NULL);

	PP_SETAGED(pp);
	page_list_add(pp, PG_FREE_LIST | PG_LIST_TAIL);
	page_unlock(pp);

	mutex_enter(&new_freemem_lock);
	if (freemem_wait) {
		cv_signal(&freemem_cv);
	}
	mutex_exit(&new_freemem_lock);
}

/*
 * Rename the page "opp" to have an identity specified
 * by [vp, off].  If a page already exists with this name
 * it is locked and destroyed.  Note that the page's
 * translations are not unloaded during the rename.
 *
 * This routine is used by the anon layer to "steal" the
 * original page and is not unlike destroying a page and
 * creating a new page using the same page frame.
 *
 * XXX -- Could deadlock if caller 1 tries to rename A to B while
 * caller 2 tries to rename B to A.
 */
void
page_rename(page_t *opp, vnode_t *vp, u_offset_t off)
{
	page_t		*pp;
	int		olckcnt = 0;
	int		ocowcnt = 0;
	kmutex_t	*phm;
	ulong_t		index;

	ASSERT(PAGE_EXCL(opp) && !page_iolock_assert(opp));
	ASSERT(MUTEX_NOT_HELD(page_vnode_mutex(vp)));
	ASSERT(PP_ISFREE(opp) == 0);

	VM_STAT_ADD(page_rename_count);

	TRACE_3(TR_FAC_VM, TR_PAGE_RENAME,
		"page rename:pp %p vp %p off %llx", opp, vp, off);

	/*
	 * CacheFS may call page_rename for a large NFS page
	 * when both CacheFS and NFS mount points are used
	 * by applications. Demote this large page before
	 * renaming it, to ensure that there are no "partial"
	 * large pages left lying around.
	 */
	if (opp->p_szc != 0) {
		vnode_t *ovp = opp->p_vnode;
		ASSERT(ovp != NULL);
		ASSERT(!IS_SWAPFSVP(ovp));
		ASSERT(ovp != &kvp);
		page_demote_vp_pages(opp);
		ASSERT(opp->p_szc == 0);
	}

	page_hashout(opp, NULL);
	PP_CLRAGED(opp);

	/*
	 * Acquire the appropriate page hash lock, since
	 * we're going to rename the page.
	 */
	index = PAGE_HASH_FUNC(vp, off);
	phm = PAGE_HASH_MUTEX(index);
	mutex_enter(phm);
top:
	/*
	 * Look for an existing page with this name and destroy it if found.
	 * By holding the page hash lock all the way to the page_hashin()
	 * call, we are assured that no page can be created with this
	 * identity.  In the case when the phm lock is dropped to undo any
	 * hat layer mappings, the existing page is held with an "exclusive"
	 * lock, again preventing another page from being created with
	 * this identity.
	 */
	PAGE_HASH_SEARCH(index, pp, vp, off);
	if (pp != NULL) {
		VM_STAT_ADD(page_rename_exists);

		/*
		 * As it turns out, this is one of only two places where
		 * page_lock() needs to hold the passed in lock in the
		 * successful case.  In all of the others, the lock could
		 * be dropped as soon as the attempt is made to lock
		 * the page.  It is tempting to add yet another arguement,
		 * PL_KEEP or PL_DROP, to let page_lock know what to do.
		 */
		if (!page_lock(pp, SE_EXCL, phm, P_RECLAIM)) {
			/*
			 * Went to sleep because the page could not
			 * be locked.  We were woken up when the page
			 * was unlocked, or when the page was destroyed.
			 * In either case, `phm' was dropped while we
			 * slept.  Hence we should not just roar through
			 * this loop.
			 */
			goto top;
		}

		/*
		 * If an existing page is a large page, then demote
		 * it to ensure that no "partial" large pages are
		 * "created" after page_rename. An existing page
		 * can be a CacheFS page, and can't belong to swapfs.
		 */
		if (hat_page_is_mapped(pp)) {
			/*
			 * Unload translations.  Since we hold the
			 * exclusive lock on this page, the page
			 * can not be changed while we drop phm.
			 * This is also not a lock protocol violation,
			 * but rather the proper way to do things.
			 */
			mutex_exit(phm);
			(void) hat_pageunload(pp, HAT_FORCE_PGUNLOAD);
			if (pp->p_szc != 0) {
				ASSERT(!IS_SWAPFSVP(vp));
				ASSERT(vp != &kvp);
				page_demote_vp_pages(pp);
				ASSERT(pp->p_szc == 0);
			}
			mutex_enter(phm);
		} else if (pp->p_szc != 0) {
			ASSERT(!IS_SWAPFSVP(vp));
			ASSERT(vp != &kvp);
			mutex_exit(phm);
			page_demote_vp_pages(pp);
			ASSERT(pp->p_szc == 0);
			mutex_enter(phm);
		}
		page_hashout(pp, phm);
	}
	/*
	 * Hash in the page with the new identity.
	 */
	if (!page_hashin(opp, vp, off, phm)) {
		/*
		 * We were holding phm while we searched for [vp, off]
		 * and only dropped phm if we found and locked a page.
		 * If we can't create this page now, then some thing
		 * is really broken.
		 */
		panic("page_rename: Can't hash in page: %p", (void *)pp);
		/*NOTREACHED*/
	}

	ASSERT(MUTEX_HELD(phm));
	mutex_exit(phm);

	/*
	 * Now that we have dropped phm, lets get around to finishing up
	 * with pp.
	 */
	if (pp != NULL) {
		ASSERT(!hat_page_is_mapped(pp));
		/* for now large pages should not end up here */
		ASSERT(pp->p_szc == 0);
		/*
		 * Save the locks for transfer to the new page and then
		 * clear them so page_free doesn't think they're important.
		 * The page_struct_lock need not be acquired for lckcnt and
		 * cowcnt since the page has an "exclusive" lock.
		 */
		olckcnt = pp->p_lckcnt;
		ocowcnt = pp->p_cowcnt;
		pp->p_lckcnt = pp->p_cowcnt = 0;

		/*
		 * Put the page on the "free" list after we drop
		 * the lock.  The less work under the lock the better.
		 */
		/*LINTED: constant in conditional context*/
		VN_DISPOSE(pp, B_FREE, 0, kcred);
	}

	/*
	 * Transfer the lock count from the old page (if any).
	 * The page_struct_lock need not be acquired for lckcnt and
	 * cowcnt since the page has an "exclusive" lock.
	 */
	opp->p_lckcnt += olckcnt;
	opp->p_cowcnt += ocowcnt;
}

/*
 * low level routine to add page `pp' to the hash and vp chains for [vp, offset]
 *
 * Pages are normally inserted at the start of a vnode's v_pages list.
 * If the vnode is VMODSORT and the page is modified, it goes at the end.
 * This can happen when a modified page is relocated for DR.
 *
 * Returns 1 on success and 0 on failure.
 */
static int
page_do_hashin(page_t *pp, vnode_t *vp, u_offset_t offset)
{
	page_t		**listp;
	page_t		*tp;
	ulong_t		index;

	ASSERT(PAGE_EXCL(pp));
	ASSERT(vp != NULL);
	ASSERT(MUTEX_HELD(page_vnode_mutex(vp)));

	/*
	 * Be sure to set these up before the page is inserted on the hash
	 * list.  As soon as the page is placed on the list some other
	 * thread might get confused and wonder how this page could
	 * possibly hash to this list.
	 */
	pp->p_vnode = vp;
	pp->p_offset = offset;

	/*
	 * record if this page is on a swap vnode
	 */
	if ((vp->v_flag & VISSWAP) != 0)
		PP_SETSWAP(pp);

	index = PAGE_HASH_FUNC(vp, offset);
	ASSERT(MUTEX_HELD(PAGE_HASH_MUTEX(index)));
	listp = &page_hash[index];

	/*
	 * If this page is already hashed in, fail this attempt to add it.
	 */
	for (tp = *listp; tp != NULL; tp = tp->p_hash) {
		if (tp->p_vnode == vp && tp->p_offset == offset) {
			pp->p_vnode = NULL;
			pp->p_offset = (u_offset_t)(-1);
			return (0);
		}
	}
	pp->p_hash = *listp;
	*listp = pp;

	/*
	 * Add the page to the vnode's list of pages
	 */
	if (vp->v_pages != NULL && IS_VMODSORT(vp) && hat_ismod(pp))
		listp = &vp->v_pages->p_vpprev->p_vpnext;
	else
		listp = &vp->v_pages;

	page_vpadd(listp, pp);

	return (1);
}

/*
 * Add page `pp' to both the hash and vp chains for [vp, offset].
 *
 * Returns 1 on success and 0 on failure.
 * If hold is passed in, it is not dropped.
 */
int
page_hashin(page_t *pp, vnode_t *vp, u_offset_t offset, kmutex_t *hold)
{
	kmutex_t	*phm = NULL;
	kmutex_t	*vphm;
	int		rc;

	ASSERT(MUTEX_NOT_HELD(page_vnode_mutex(vp)));

	TRACE_3(TR_FAC_VM, TR_PAGE_HASHIN,
		"page_hashin:pp %p vp %p offset %llx",
		pp, vp, offset);

	VM_STAT_ADD(hashin_count);

	if (hold != NULL)
		phm = hold;
	else {
		VM_STAT_ADD(hashin_not_held);
		phm = PAGE_HASH_MUTEX(PAGE_HASH_FUNC(vp, offset));
		mutex_enter(phm);
	}

	vphm = page_vnode_mutex(vp);
	mutex_enter(vphm);
	rc = page_do_hashin(pp, vp, offset);
	mutex_exit(vphm);
	if (hold == NULL)
		mutex_exit(phm);
	if (rc == 0)
		VM_STAT_ADD(hashin_already);
	return (rc);
}

/*
 * Remove page ``pp'' from the hash and vp chains and remove vp association.
 * All mutexes must be held
 */
static void
page_do_hashout(page_t *pp)
{
	page_t	**hpp;
	page_t	*hp;
	vnode_t	*vp = pp->p_vnode;

	ASSERT(vp != NULL);
	ASSERT(MUTEX_HELD(page_vnode_mutex(vp)));

	/*
	 * First, take pp off of its hash chain.
	 */
	hpp = &page_hash[PAGE_HASH_FUNC(vp, pp->p_offset)];

	for (;;) {
		hp = *hpp;
		if (hp == pp)
			break;
		if (hp == NULL) {
			panic("page_do_hashout");
			/*NOTREACHED*/
		}
		hpp = &hp->p_hash;
	}
	*hpp = pp->p_hash;

	/*
	 * Now remove it from its associated vnode.
	 */
	if (vp->v_pages)
		page_vpsub(&vp->v_pages, pp);

	pp->p_hash = NULL;
	page_clr_all_props(pp);
	PP_CLRSWAP(pp);
	pp->p_vnode = NULL;
	pp->p_offset = (u_offset_t)-1;
}

/*
 * Remove page ``pp'' from the hash and vp chains and remove vp association.
 *
 * When `phm' is non-NULL it contains the address of the mutex protecting the
 * hash list pp is on.  It is not dropped.
 */
void
page_hashout(page_t *pp, kmutex_t *phm)
{
	vnode_t		*vp;
	ulong_t		index;
	kmutex_t	*nphm;
	kmutex_t	*vphm;
	kmutex_t	*sep;

	ASSERT(phm != NULL ? MUTEX_HELD(phm) : 1);
	ASSERT(pp->p_vnode != NULL);
	ASSERT((PAGE_EXCL(pp) && !page_iolock_assert(pp)) || panicstr);
	ASSERT(MUTEX_NOT_HELD(page_vnode_mutex(pp->p_vnode)));

	vp = pp->p_vnode;

	TRACE_2(TR_FAC_VM, TR_PAGE_HASHOUT,
		"page_hashout:pp %p vp %p", pp, vp);

	/* Kernel probe */
	TNF_PROBE_2(page_unmap, "vm pagefault", /* CSTYLED */,
	    tnf_opaque, vnode, vp,
	    tnf_offset, offset, pp->p_offset);

	/*
	 *
	 */
	VM_STAT_ADD(hashout_count);
	index = PAGE_HASH_FUNC(vp, pp->p_offset);
	if (phm == NULL) {
		VM_STAT_ADD(hashout_not_held);
		nphm = PAGE_HASH_MUTEX(index);
		mutex_enter(nphm);
	}
	ASSERT(phm ? phm == PAGE_HASH_MUTEX(index) : 1);


	/*
	 * grab page vnode mutex and remove it...
	 */
	vphm = page_vnode_mutex(vp);
	mutex_enter(vphm);

	page_do_hashout(pp);

	mutex_exit(vphm);
	if (phm == NULL)
		mutex_exit(nphm);

	/*
	 * If the page was retired, update the pages_retired
	 * total and clear the page flag
	 */
	if (page_isretired(pp)) {
		retired_page_removed(pp);
	}

	/*
	 * Wake up processes waiting for this page.  The page's
	 * identity has been changed, and is probably not the
	 * desired page any longer.
	 */
	sep = page_se_mutex(pp);
	mutex_enter(sep);
	pp->p_selock &= ~SE_EWANTED;
	if (CV_HAS_WAITERS(&pp->p_cv))
		cv_broadcast(&pp->p_cv);
	mutex_exit(sep);
}

/*
 * Add the page to the front of a linked list of pages
 * using the p_next & p_prev pointers for the list.
 * The caller is responsible for protecting the list pointers.
 */
void
page_add(page_t **ppp, page_t *pp)
{
	ASSERT(PAGE_EXCL(pp) || (PAGE_SHARED(pp) && page_iolock_assert(pp)));

	page_add_common(ppp, pp);
}



/*
 *  Common code for page_add() and mach_page_add()
 */
void
page_add_common(page_t **ppp, page_t *pp)
{
	if (*ppp == NULL) {
		pp->p_next = pp->p_prev = pp;
	} else {
		pp->p_next = *ppp;
		pp->p_prev = (*ppp)->p_prev;
		(*ppp)->p_prev = pp;
		pp->p_prev->p_next = pp;
	}
	*ppp = pp;
}


/*
 * Remove this page from a linked list of pages
 * using the p_next & p_prev pointers for the list.
 *
 * The caller is responsible for protecting the list pointers.
 */
void
page_sub(page_t **ppp, page_t *pp)
{
	ASSERT((PP_ISFREE(pp)) ? 1 :
	    (PAGE_EXCL(pp)) || (PAGE_SHARED(pp) && page_iolock_assert(pp)));

	if (*ppp == NULL || pp == NULL) {
		panic("page_sub: bad arg(s): pp %p, *ppp %p",
		    (void *)pp, (void *)(*ppp));
		/*NOTREACHED*/
	}

	page_sub_common(ppp, pp);
}


/*
 *  Common code for page_sub() and mach_page_sub()
 */
void
page_sub_common(page_t **ppp, page_t *pp)
{
	if (*ppp == pp)
		*ppp = pp->p_next;		/* go to next page */

	if (*ppp == pp)
		*ppp = NULL;			/* page list is gone */
	else {
		pp->p_prev->p_next = pp->p_next;
		pp->p_next->p_prev = pp->p_prev;
	}
	pp->p_prev = pp->p_next = pp;		/* make pp a list of one */
}


/*
 * Break page list cppp into two lists with npages in the first list.
 * The tail is returned in nppp.
 */
void
page_list_break(page_t **oppp, page_t **nppp, pgcnt_t npages)
{
	page_t *s1pp = *oppp;
	page_t *s2pp;
	page_t *e1pp, *e2pp;
	long n = 0;

	if (s1pp == NULL) {
		*nppp = NULL;
		return;
	}
	if (npages == 0) {
		*nppp = s1pp;
		*oppp = NULL;
		return;
	}
	for (n = 0, s2pp = *oppp; n < npages; n++) {
		s2pp = s2pp->p_next;
	}
	/* Fix head and tail of new lists */
	e1pp = s2pp->p_prev;
	e2pp = s1pp->p_prev;
	s1pp->p_prev = e1pp;
	e1pp->p_next = s1pp;
	s2pp->p_prev = e2pp;
	e2pp->p_next = s2pp;

	/* second list empty */
	if (s2pp == s1pp) {
		*oppp = s1pp;
		*nppp = NULL;
	} else {
		*oppp = s1pp;
		*nppp = s2pp;
	}
}

/*
 * Concatenate page list nppp onto the end of list ppp.
 */
void
page_list_concat(page_t **ppp, page_t **nppp)
{
	page_t *s1pp, *s2pp, *e1pp, *e2pp;

	if (*nppp == NULL) {
		return;
	}
	if (*ppp == NULL) {
		*ppp = *nppp;
		return;
	}
	s1pp = *ppp;
	e1pp =  s1pp->p_prev;
	s2pp = *nppp;
	e2pp = s2pp->p_prev;
	s1pp->p_prev = e2pp;
	e2pp->p_next = s1pp;
	e1pp->p_next = s2pp;
	s2pp->p_prev = e1pp;
}

/*
 * return the next page in the page list
 */
page_t *
page_list_next(page_t *pp)
{
	return (pp->p_next);
}


/*
 * Add the page to the front of the linked list of pages
 * using p_vpnext/p_vpprev pointers for the list.
 *
 * The caller is responsible for protecting the lists.
 */
void
page_vpadd(page_t **ppp, page_t *pp)
{
	if (*ppp == NULL) {
		pp->p_vpnext = pp->p_vpprev = pp;
	} else {
		pp->p_vpnext = *ppp;
		pp->p_vpprev = (*ppp)->p_vpprev;
		(*ppp)->p_vpprev = pp;
		pp->p_vpprev->p_vpnext = pp;
	}
	*ppp = pp;
}

/*
 * Remove this page from the linked list of pages
 * using p_vpnext/p_vpprev pointers for the list.
 *
 * The caller is responsible for protecting the lists.
 */
void
page_vpsub(page_t **ppp, page_t *pp)
{
	if (*ppp == NULL || pp == NULL) {
		panic("page_vpsub: bad arg(s): pp %p, *ppp %p",
		    (void *)pp, (void *)(*ppp));
		/*NOTREACHED*/
	}

	if (*ppp == pp)
		*ppp = pp->p_vpnext;		/* go to next page */

	if (*ppp == pp)
		*ppp = NULL;			/* page list is gone */
	else {
		pp->p_vpprev->p_vpnext = pp->p_vpnext;
		pp->p_vpnext->p_vpprev = pp->p_vpprev;
	}
	pp->p_vpprev = pp->p_vpnext = pp;	/* make pp a list of one */
}

/*
 * Lock a physical page into memory "long term".  Used to support "lock
 * in memory" functions.  Accepts the page to be locked, and a cow variable
 * to indicate whether a the lock will travel to the new page during
 * a potential copy-on-write.
 */
int
page_pp_lock(
	page_t *pp,			/* page to be locked */
	int cow,			/* cow lock */
	int kernel)			/* must succeed -- ignore checking */
{
	int r = 0;			/* result -- assume failure */

	ASSERT(PAGE_LOCKED(pp));

	page_struct_lock(pp);
	/*
	 * Acquire the "freemem_lock" for availrmem.
	 */
	if (cow) {
		mutex_enter(&freemem_lock);
		if ((availrmem > pages_pp_maximum) &&
		    (pp->p_cowcnt < (ushort_t)PAGE_LOCK_MAXIMUM)) {
			availrmem--;
			pages_locked++;
			mutex_exit(&freemem_lock);
			r = 1;
			if (++pp->p_cowcnt == (ushort_t)PAGE_LOCK_MAXIMUM) {
				cmn_err(CE_WARN,
				    "COW lock limit reached on pfn 0x%lx",
				    page_pptonum(pp));
			}
		} else
			mutex_exit(&freemem_lock);
	} else {
		if (pp->p_lckcnt) {
			if (pp->p_lckcnt < (ushort_t)PAGE_LOCK_MAXIMUM) {
				r = 1;
				if (++pp->p_lckcnt ==
				    (ushort_t)PAGE_LOCK_MAXIMUM) {
					cmn_err(CE_WARN, "Page lock limit "
					    "reached on pfn 0x%lx",
					    page_pptonum(pp));
				}
			}
		} else {
			if (kernel) {
				/* availrmem accounting done by caller */
				++pp->p_lckcnt;
				r = 1;
			} else {
				mutex_enter(&freemem_lock);
				if (availrmem > pages_pp_maximum) {
					availrmem--;
					pages_locked++;
					++pp->p_lckcnt;
					r = 1;
				}
				mutex_exit(&freemem_lock);
			}
		}
	}
	page_struct_unlock(pp);
	return (r);
}

/*
 * Decommit a lock on a physical page frame.  Account for cow locks if
 * appropriate.
 */
void
page_pp_unlock(
	page_t *pp,			/* page to be unlocked */
	int cow,			/* expect cow lock */
	int kernel)			/* this was a kernel lock */
{
	ASSERT(PAGE_LOCKED(pp));

	page_struct_lock(pp);
	/*
	 * Acquire the "freemem_lock" for availrmem.
	 * If cowcnt or lcknt is already 0 do nothing; i.e., we
	 * could be called to unlock even if nothing is locked. This could
	 * happen if locked file pages were truncated (removing the lock)
	 * and the file was grown again and new pages faulted in; the new
	 * pages are unlocked but the segment still thinks they're locked.
	 */
	if (cow) {
		if (pp->p_cowcnt) {
			mutex_enter(&freemem_lock);
			pp->p_cowcnt--;
			availrmem++;
			pages_locked--;
			mutex_exit(&freemem_lock);
		}
	} else {
		if (pp->p_lckcnt && --pp->p_lckcnt == 0) {
			if (!kernel) {
				mutex_enter(&freemem_lock);
				availrmem++;
				pages_locked--;
				mutex_exit(&freemem_lock);
			}
		}
	}
	page_struct_unlock(pp);
}

/*
 * This routine reserves availrmem for npages;
 * 	flags: KM_NOSLEEP or KM_SLEEP
 * 	returns 1 on success or 0 on failure
 */
int
page_resv(pgcnt_t npages, uint_t flags)
{
	mutex_enter(&freemem_lock);
	while (availrmem < tune.t_minarmem + npages) {
		if (flags & KM_NOSLEEP) {
			mutex_exit(&freemem_lock);
			return (0);
		}
		mutex_exit(&freemem_lock);
		page_needfree(npages);
		kmem_reap();
		delay(hz >> 2);
		page_needfree(-(spgcnt_t)npages);
		mutex_enter(&freemem_lock);
	}
	availrmem -= npages;
	mutex_exit(&freemem_lock);
	return (1);
}

/*
 * This routine unreserves availrmem for npages;
 */
void
page_unresv(pgcnt_t npages)
{
	mutex_enter(&freemem_lock);
	availrmem += npages;
	mutex_exit(&freemem_lock);
}

/*
 * See Statement at the beginning of segvn_lockop() regarding
 * the way we handle cowcnts and lckcnts.
 *
 * Transfer cowcnt on 'opp' to cowcnt on 'npp' if the vpage
 * that breaks COW has PROT_WRITE.
 *
 * Note that, we may also break COW in case we are softlocking
 * on read access during physio;
 * in this softlock case, the vpage may not have PROT_WRITE.
 * So, we need to transfer lckcnt on 'opp' to lckcnt on 'npp'
 * if the vpage doesn't have PROT_WRITE.
 *
 * This routine is never called if we are stealing a page
 * in anon_private.
 *
 * The caller subtracted from availrmem for read only mapping.
 * if lckcnt is 1 increment availrmem.
 */
void
page_pp_useclaim(
	page_t *opp,		/* original page frame losing lock */
	page_t *npp,		/* new page frame gaining lock */
	uint_t	write_perm) 	/* set if vpage has PROT_WRITE */
{
	int payback = 0;

	ASSERT(PAGE_LOCKED(opp));
	ASSERT(PAGE_LOCKED(npp));

	page_struct_lock(opp);

	ASSERT(npp->p_cowcnt == 0);
	ASSERT(npp->p_lckcnt == 0);

	/* Don't use claim if nothing is locked (see page_pp_unlock above) */
	if ((write_perm && opp->p_cowcnt != 0) ||
	    (!write_perm && opp->p_lckcnt != 0)) {

		if (write_perm) {
			npp->p_cowcnt++;
			ASSERT(opp->p_cowcnt != 0);
			opp->p_cowcnt--;
		} else {

			ASSERT(opp->p_lckcnt != 0);

			/*
			 * We didn't need availrmem decremented if p_lckcnt on
			 * original page is 1. Here, we are unlocking
			 * read-only copy belonging to original page and
			 * are locking a copy belonging to new page.
			 */
			if (opp->p_lckcnt == 1)
				payback = 1;

			npp->p_lckcnt++;
			opp->p_lckcnt--;
		}
	}
	if (payback) {
		mutex_enter(&freemem_lock);
		availrmem++;
		pages_useclaim--;
		mutex_exit(&freemem_lock);
	}
	page_struct_unlock(opp);
}

/*
 * Simple claim adjust functions -- used to support changes in
 * claims due to changes in access permissions.  Used by segvn_setprot().
 */
int
page_addclaim(page_t *pp)
{
	int r = 0;			/* result */

	ASSERT(PAGE_LOCKED(pp));

	page_struct_lock(pp);
	ASSERT(pp->p_lckcnt != 0);

	if (pp->p_lckcnt == 1) {
		if (pp->p_cowcnt < (ushort_t)PAGE_LOCK_MAXIMUM) {
			--pp->p_lckcnt;
			r = 1;
			if (++pp->p_cowcnt == (ushort_t)PAGE_LOCK_MAXIMUM) {
				cmn_err(CE_WARN,
				    "COW lock limit reached on pfn 0x%lx",
				    page_pptonum(pp));
			}
		}
	} else {
		mutex_enter(&freemem_lock);
		if ((availrmem > pages_pp_maximum) &&
		    (pp->p_cowcnt < (ushort_t)PAGE_LOCK_MAXIMUM)) {
			--availrmem;
			++pages_claimed;
			mutex_exit(&freemem_lock);
			--pp->p_lckcnt;
			r = 1;
			if (++pp->p_cowcnt == (ushort_t)PAGE_LOCK_MAXIMUM) {
				cmn_err(CE_WARN,
				    "COW lock limit reached on pfn 0x%lx",
				    page_pptonum(pp));
			}
		} else
			mutex_exit(&freemem_lock);
	}
	page_struct_unlock(pp);
	return (r);
}

int
page_subclaim(page_t *pp)
{
	int r = 0;

	ASSERT(PAGE_LOCKED(pp));

	page_struct_lock(pp);
	ASSERT(pp->p_cowcnt != 0);

	if (pp->p_lckcnt) {
		if (pp->p_lckcnt < (ushort_t)PAGE_LOCK_MAXIMUM) {
			r = 1;
			/*
			 * for availrmem
			 */
			mutex_enter(&freemem_lock);
			availrmem++;
			pages_claimed--;
			mutex_exit(&freemem_lock);

			pp->p_cowcnt--;

			if (++pp->p_lckcnt == (ushort_t)PAGE_LOCK_MAXIMUM) {
				cmn_err(CE_WARN,
				    "Page lock limit reached on pfn 0x%lx",
				    page_pptonum(pp));
			}
		}
	} else {
		r = 1;
		pp->p_cowcnt--;
		pp->p_lckcnt++;
	}
	page_struct_unlock(pp);
	return (r);
}

int
page_addclaim_pages(page_t  **ppa)
{

	pgcnt_t	lckpgs = 0, pg_idx;

	VM_STAT_ADD(pagecnt.pc_addclaim_pages);

	mutex_enter(&page_llock);
	for (pg_idx = 0; ppa[pg_idx] != NULL; pg_idx++) {

		ASSERT(PAGE_LOCKED(ppa[pg_idx]));
		ASSERT(ppa[pg_idx]->p_lckcnt != 0);
		if (ppa[pg_idx]->p_cowcnt == (ushort_t)PAGE_LOCK_MAXIMUM) {
			mutex_exit(&page_llock);
			return (0);
		}
		if (ppa[pg_idx]->p_lckcnt > 1)
			lckpgs++;
	}

	if (lckpgs != 0) {
		mutex_enter(&freemem_lock);
		if (availrmem >= pages_pp_maximum + lckpgs) {
			availrmem -= lckpgs;
			pages_claimed += lckpgs;
		} else {
			mutex_exit(&freemem_lock);
			mutex_exit(&page_llock);
			return (0);
		}
		mutex_exit(&freemem_lock);
	}

	for (pg_idx = 0; ppa[pg_idx] != NULL; pg_idx++) {
		ppa[pg_idx]->p_lckcnt--;
		ppa[pg_idx]->p_cowcnt++;
	}
	mutex_exit(&page_llock);
	return (1);
}

int
page_subclaim_pages(page_t  **ppa)
{
	pgcnt_t	ulckpgs = 0, pg_idx;

	VM_STAT_ADD(pagecnt.pc_subclaim_pages);

	mutex_enter(&page_llock);
	for (pg_idx = 0; ppa[pg_idx] != NULL; pg_idx++) {

		ASSERT(PAGE_LOCKED(ppa[pg_idx]));
		ASSERT(ppa[pg_idx]->p_cowcnt != 0);
		if (ppa[pg_idx]->p_lckcnt == (ushort_t)PAGE_LOCK_MAXIMUM) {
			mutex_exit(&page_llock);
			return (0);
		}
		if (ppa[pg_idx]->p_lckcnt != 0)
			ulckpgs++;
	}

	if (ulckpgs != 0) {
		mutex_enter(&freemem_lock);
		availrmem += ulckpgs;
		pages_claimed -= ulckpgs;
		mutex_exit(&freemem_lock);
	}

	for (pg_idx = 0; ppa[pg_idx] != NULL; pg_idx++) {
		ppa[pg_idx]->p_cowcnt--;
		ppa[pg_idx]->p_lckcnt++;

	}
	mutex_exit(&page_llock);
	return (1);
}

page_t *
page_numtopp(pfn_t pfnum, se_t se)
{
	page_t *pp;

retry:
	pp = page_numtopp_nolock(pfnum);
	if (pp == NULL) {
		return ((page_t *)NULL);
	}

	/*
	 * Acquire the appropriate lock on the page.
	 */
	while (!page_lock(pp, se, (kmutex_t *)NULL, P_RECLAIM)) {
		if (page_pptonum(pp) != pfnum)
			goto retry;
		continue;
	}

	if (page_pptonum(pp) != pfnum) {
		page_unlock(pp);
		goto retry;
	}

	return (pp);
}

page_t *
page_numtopp_noreclaim(pfn_t pfnum, se_t se)
{
	page_t *pp;

retry:
	pp = page_numtopp_nolock(pfnum);
	if (pp == NULL) {
		return ((page_t *)NULL);
	}

	/*
	 * Acquire the appropriate lock on the page.
	 */
	while (!page_lock(pp, se, (kmutex_t *)NULL, P_NO_RECLAIM)) {
		if (page_pptonum(pp) != pfnum)
			goto retry;
		continue;
	}

	if (page_pptonum(pp) != pfnum) {
		page_unlock(pp);
		goto retry;
	}

	return (pp);
}

/*
 * This routine is like page_numtopp, but will only return page structs
 * for pages which are ok for loading into hardware using the page struct.
 */
page_t *
page_numtopp_nowait(pfn_t pfnum, se_t se)
{
	page_t *pp;

retry:
	pp = page_numtopp_nolock(pfnum);
	if (pp == NULL) {
		return ((page_t *)NULL);
	}

	/*
	 * Try to acquire the appropriate lock on the page.
	 */
	if (PP_ISFREE(pp))
		pp = NULL;
	else {
		if (!page_trylock(pp, se))
			pp = NULL;
		else {
			if (page_pptonum(pp) != pfnum) {
				page_unlock(pp);
				goto retry;
			}
			if (PP_ISFREE(pp)) {
				page_unlock(pp);
				pp = NULL;
			}
		}
	}
	return (pp);
}

/*
 * Returns a count of dirty pages that are in the process
 * of being written out.  If 'cleanit' is set, try to push the page.
 */
pgcnt_t
page_busy(int cleanit)
{
	page_t *page0 = page_first();
	page_t *pp = page0;
	pgcnt_t nppbusy = 0;
	u_offset_t off;

	do {
		vnode_t *vp = pp->p_vnode;

		/*
		 * A page is a candidate for syncing if it is:
		 *
		 * (a)	On neither the freelist nor the cachelist
		 * (b)	Hashed onto a vnode
		 * (c)	Not a kernel page
		 * (d)	Dirty
		 * (e)	Not part of a swapfile
		 * (f)	a page which belongs to a real vnode; eg has a non-null
		 *	v_vfsp pointer.
		 * (g)	Backed by a filesystem which doesn't have a
		 *	stubbed-out sync operation
		 */
		if (!PP_ISFREE(pp) && vp != NULL && vp != &kvp &&
		    hat_ismod(pp) && !IS_SWAPVP(vp) && vp->v_vfsp != NULL &&
		    vfs_can_sync(vp->v_vfsp)) {
			nppbusy++;
			vfs_syncprogress();

			if (!cleanit)
				continue;
			if (!page_trylock(pp, SE_EXCL))
				continue;

			if (PP_ISFREE(pp) || vp == NULL || IS_SWAPVP(vp) ||
			    pp->p_lckcnt != 0 || pp->p_cowcnt != 0 ||
			    !(hat_pagesync(pp,
			    HAT_SYNC_DONTZERO | HAT_SYNC_STOPON_MOD) & P_MOD)) {
				page_unlock(pp);
				continue;
			}
			off = pp->p_offset;
			VN_HOLD(vp);
			page_unlock(pp);
			(void) VOP_PUTPAGE(vp, off, PAGESIZE,
			    B_ASYNC | B_FREE, kcred);
			VN_RELE(vp);
		}
	} while ((pp = page_next(pp)) != page0);

	return (nppbusy);
}

void page_invalidate_pages(void);

/*
 * callback handler to vm sub-system
 *
 * callers make sure no recursive entries to this func.
 */
/*ARGSUSED*/
boolean_t
callb_vm_cpr(void *arg, int code)
{
	if (code == CB_CODE_CPR_CHKPT)
		page_invalidate_pages();
	return (B_TRUE);
}

/*
 * Invalidate all pages of the system.
 * It shouldn't be called until all user page activities are all stopped.
 */
void
page_invalidate_pages()
{
	page_t *pp;
	page_t *page0;
	pgcnt_t nbusypages;
	int retry = 0;
	const int MAXRETRIES = 4;
#if defined(__sparc)
	extern struct vnode prom_ppages;
#endif /* __sparc */

top:
	/*
	 * Flush dirty pages and destory the clean ones.
	 */
	nbusypages = 0;

	pp = page0 = page_first();
	do {
		struct vnode	*vp;
		u_offset_t	offset;
		int		mod;

		/*
		 * skip the page if it has no vnode or the page associated
		 * with the kernel vnode or prom allocated kernel mem.
		 */
#if defined(__sparc)
		if ((vp = pp->p_vnode) == NULL || vp == &kvp ||
		    vp == &prom_ppages)
#else /* x86 doesn't have prom or prom_ppage */
		if ((vp = pp->p_vnode) == NULL || vp == &kvp)
#endif /* __sparc */
			continue;

		/*
		 * skip the page which is already free invalidated.
		 */
		if (PP_ISFREE(pp) && PP_ISAGED(pp))
			continue;

		/*
		 * skip pages that are already locked or can't be "exclusively"
		 * locked or are already free.  After we lock the page, check
		 * the free and age bits again to be sure it's not destroied
		 * yet.
		 * To achieve max. parallelization, we use page_trylock instead
		 * of page_lock so that we don't get block on individual pages
		 * while we have thousands of other pages to process.
		 */
		if (!page_trylock(pp, SE_EXCL)) {
			nbusypages++;
			continue;
		} else if (PP_ISFREE(pp)) {
			if (!PP_ISAGED(pp)) {
				page_destroy_free(pp);
			} else {
				page_unlock(pp);
			}
			continue;
		}
		/*
		 * Is this page involved in some I/O? shared?
		 *
		 * The page_struct_lock need not be acquired to
		 * examine these fields since the page has an
		 * "exclusive" lock.
		 */
		if (pp->p_lckcnt != 0 || pp->p_cowcnt != 0) {
			page_unlock(pp);
			continue;
		}

		if (vp->v_type == VCHR) {
			panic("vp->v_type == VCHR");
			/*NOTREACHED*/
		}

		if (!page_try_demote_pages(pp)) {
			page_unlock(pp);
			continue;
		}

		/*
		 * Check the modified bit. Leave the bits alone in hardware
		 * (they will be modified if we do the putpage).
		 */
		mod = (hat_pagesync(pp, HAT_SYNC_DONTZERO | HAT_SYNC_STOPON_MOD)
			& P_MOD);
		if (mod) {
			offset = pp->p_offset;
			/*
			 * Hold the vnode before releasing the page lock
			 * to prevent it from being freed and re-used by
			 * some other thread.
			 */
			VN_HOLD(vp);
			page_unlock(pp);
			/*
			 * No error return is checked here. Callers such as
			 * cpr deals with the dirty pages at the dump time
			 * if this putpage fails.
			 */
			(void) VOP_PUTPAGE(vp, offset, PAGESIZE, B_INVAL,
			    kcred);
			VN_RELE(vp);
		} else {
			page_destroy(pp, 0);
		}
	} while ((pp = page_next(pp)) != page0);
	if (nbusypages && retry++ < MAXRETRIES) {
		delay(1);
		goto top;
	}
}

/*
 * Replace the page "old" with the page "new" on the page hash and vnode lists
 *
 * the replacemnt must be done in place, ie the equivalent sequence:
 *
 *	vp = old->p_vnode;
 *	off = old->p_offset;
 *	page_do_hashout(old)
 *	page_do_hashin(new, vp, off)
 *
 * doesn't work, since
 *  1) if old is the only page on the vnode, the v_pages list has a window
 *     where it looks empty. This will break file system assumptions.
 * and
 *  2) pvn_vplist_dirty() can't deal with pages moving on the v_pages list.
 */
static void
page_do_relocate_hash(page_t *new, page_t *old)
{
	page_t	**hash_list;
	vnode_t	*vp = old->p_vnode;
	kmutex_t *sep;

	ASSERT(PAGE_EXCL(old));
	ASSERT(PAGE_EXCL(new));
	ASSERT(vp != NULL);
	ASSERT(MUTEX_HELD(page_vnode_mutex(vp)));
	ASSERT(MUTEX_HELD(PAGE_HASH_MUTEX(PAGE_HASH_FUNC(vp, old->p_offset))));

	/*
	 * First find old page on the page hash list
	 */
	hash_list = &page_hash[PAGE_HASH_FUNC(vp, old->p_offset)];

	for (;;) {
		if (*hash_list == old)
			break;
		if (*hash_list == NULL) {
			panic("page_do_hashout");
			/*NOTREACHED*/
		}
		hash_list = &(*hash_list)->p_hash;
	}

	/*
	 * update new and replace old with new on the page hash list
	 */
	new->p_vnode = old->p_vnode;
	new->p_offset = old->p_offset;
	new->p_hash = old->p_hash;
	*hash_list = new;

	if ((new->p_vnode->v_flag & VISSWAP) != 0)
		PP_SETSWAP(new);

	/*
	 * replace old with new on the vnode's page list
	 */
	if (old->p_vpnext == old) {
		new->p_vpnext = new;
		new->p_vpprev = new;
	} else {
		new->p_vpnext = old->p_vpnext;
		new->p_vpprev = old->p_vpprev;
		new->p_vpnext->p_vpprev = new;
		new->p_vpprev->p_vpnext = new;
	}
	if (vp->v_pages == old)
		vp->v_pages = new;

	/*
	 * clear out the old page
	 */
	old->p_hash = NULL;
	old->p_vpnext = NULL;
	old->p_vpprev = NULL;
	old->p_vnode = NULL;
	PP_CLRSWAP(old);
	old->p_offset = (u_offset_t)-1;
	page_clr_all_props(old);

	/*
	 * Wake up processes waiting for this page.  The page's
	 * identity has been changed, and is probably not the
	 * desired page any longer.
	 */
	sep = page_se_mutex(old);
	mutex_enter(sep);
	old->p_selock &= ~SE_EWANTED;
	if (CV_HAS_WAITERS(&old->p_cv))
		cv_broadcast(&old->p_cv);
	mutex_exit(sep);
}

/*
 * This function moves the identity of page "pp_old" to page "pp_new".
 * Both pages must be locked on entry.  "pp_new" is free, has no identity,
 * and need not be hashed out from anywhere.
 */
void
page_relocate_hash(page_t *pp_new, page_t *pp_old)
{
	vnode_t *vp = pp_old->p_vnode;
	u_offset_t off = pp_old->p_offset;
	kmutex_t *phm, *vphm;

	/*
	 * Rehash two pages
	 */
	ASSERT(PAGE_EXCL(pp_old));
	ASSERT(PAGE_EXCL(pp_new));
	ASSERT(vp != NULL);
	ASSERT(pp_new->p_vnode == NULL);

	/*
	 * hashout then hashin while holding the mutexes
	 */
	phm = PAGE_HASH_MUTEX(PAGE_HASH_FUNC(vp, off));
	mutex_enter(phm);
	vphm = page_vnode_mutex(vp);
	mutex_enter(vphm);

	page_do_relocate_hash(pp_new, pp_old);

	mutex_exit(vphm);
	mutex_exit(phm);

	/*
	 * The page_struct_lock need not be acquired for lckcnt and
	 * cowcnt since the page has an "exclusive" lock.
	 */
	ASSERT(pp_new->p_lckcnt == 0);
	ASSERT(pp_new->p_cowcnt == 0);
	pp_new->p_lckcnt = pp_old->p_lckcnt;
	pp_new->p_cowcnt = pp_old->p_cowcnt;
	pp_old->p_lckcnt = pp_old->p_cowcnt = 0;

	/* The following comment preserved from page_flip(). */
	/* XXX - Do we need to protect fsdata? */
	pp_new->p_fsdata = pp_old->p_fsdata;
}

/*
 * Helper routine used to lock all remaining members of a
 * large page. The caller is responsible for passing in a locked
 * pp. If pp is a large page, then it succeeds in locking all the
 * remaining constituent pages or it returns with only the
 * original page locked.
 *
 * Returns 1 on success, 0 on failure.
 *
 * If success is returned this routine gurantees p_szc for all constituent
 * pages of a large page pp belongs to can't change. To achieve this we
 * recheck szc of pp after locking all constituent pages and retry if szc
 * changed (it could only decrease). Since hat_page_demote() needs an EXCL
 * lock on one of constituent pages it can't be running after all constituent
 * pages are locked.  hat_page_demote() with a lock on a constituent page
 * outside of this large page (i.e. pp belonged to a larger large page) is
 * already done with all constituent pages of pp since the root's p_szc is
 * changed last. Thefore no need to synchronize with hat_page_demote() that
 * locked a constituent page outside of pp's current large page.
 */
#ifdef DEBUG
uint32_t gpg_trylock_mtbf = 0;
#endif

int
group_page_trylock(page_t *pp, se_t se)
{
	page_t  *tpp;
	pgcnt_t	npgs, i, j;
	uint_t pszc = pp->p_szc;

#ifdef DEBUG
	if (gpg_trylock_mtbf && !(gethrtime() % gpg_trylock_mtbf)) {
		return (0);
	}
#endif

	if (pp != PP_GROUPLEADER(pp, pszc)) {
		return (0);
	}

retry:
	ASSERT(PAGE_LOCKED_SE(pp, se));
	ASSERT(!PP_ISFREE(pp));
	if (pszc == 0) {
		return (1);
	}
	npgs = page_get_pagecnt(pszc);
	tpp = pp + 1;
	for (i = 1; i < npgs; i++, tpp++) {
		if (!page_trylock(tpp, se)) {
			tpp = pp + 1;
			for (j = 1; j < i; j++, tpp++) {
				page_unlock(tpp);
			}
			return (0);
		}
	}
	if (pp->p_szc != pszc) {
		ASSERT(pp->p_szc < pszc);
		ASSERT(pp->p_vnode != NULL && pp->p_vnode != &kvp &&
		    !IS_SWAPFSVP(pp->p_vnode));
		tpp = pp + 1;
		for (i = 1; i < npgs; i++, tpp++) {
			page_unlock(tpp);
		}
		pszc = pp->p_szc;
		goto retry;
	}
	return (1);
}

void
group_page_unlock(page_t *pp)
{
	page_t *tpp;
	pgcnt_t	npgs, i;

	ASSERT(PAGE_LOCKED(pp));
	ASSERT(!PP_ISFREE(pp));
	ASSERT(pp == PP_PAGEROOT(pp));
	npgs = page_get_pagecnt(pp->p_szc);
	for (i = 1, tpp = pp + 1; i < npgs; i++, tpp++) {
		page_unlock(tpp);
	}
}

/*
 * returns
 * 0 		: on success and *nrelocp is number of relocated PAGESIZE pages
 * ERANGE	: this is not a base page
 * EBUSY	: failure to get locks on the page/pages
 * ENOMEM	: failure to obtain replacement pages
 * EAGAIN	: OBP has not yet completed its boot-time handoff to the kernel
 *
 * Return with all constituent members of target and replacement
 * SE_EXCL locked. It is the callers responsibility to drop the
 * locks.
 */
int
do_page_relocate(
	page_t **target,
	page_t **replacement,
	int grouplock,
	spgcnt_t *nrelocp,
	lgrp_t *lgrp)
{
#ifdef DEBUG
	page_t *first_repl;
#endif /* DEBUG */
	page_t *repl;
	page_t *targ;
	page_t *pl = NULL;
	uint_t ppattr;
	pfn_t   pfn, repl_pfn;
	uint_t	szc;
	spgcnt_t npgs, i;
	int repl_contig = 0;
	uint_t flags = 0;
	spgcnt_t dofree = 0;

	*nrelocp = 0;

#if defined(__sparc)
	/*
	 * We need to wait till OBP has completed
	 * its boot-time handoff of its resources to the kernel
	 * before we allow page relocation
	 */
	if (page_relocate_ready == 0) {
		return (EAGAIN);
	}
#endif

	/*
	 * If this is not a base page,
	 * just return with 0x0 pages relocated.
	 */
	targ = *target;
	ASSERT(PAGE_EXCL(targ));
	ASSERT(!PP_ISFREE(targ));
	szc = targ->p_szc;
	ASSERT(szc < mmu_page_sizes);
	VM_STAT_ADD(vmm_vmstats.ppr_reloc[szc]);
	pfn = targ->p_pagenum;
	if (pfn != PFN_BASE(pfn, szc)) {
		VM_STAT_ADD(vmm_vmstats.ppr_relocnoroot[szc]);
		return (ERANGE);
	}

	if ((repl = *replacement) != NULL && repl->p_szc >= szc) {
		repl_pfn = repl->p_pagenum;
		if (repl_pfn != PFN_BASE(repl_pfn, szc)) {
			VM_STAT_ADD(vmm_vmstats.ppr_reloc_replnoroot[szc]);
			return (ERANGE);
		}
		repl_contig = 1;
	}

	/*
	 * We must lock all members of this large page or we cannot
	 * relocate any part of it.
	 */
	if (grouplock != 0 && !group_page_trylock(targ, SE_EXCL)) {
		VM_STAT_ADD(vmm_vmstats.ppr_relocnolock[targ->p_szc]);
		return (EBUSY);
	}

	/*
	 * reread szc it could have been decreased before
	 * group_page_trylock() was done.
	 */
	szc = targ->p_szc;
	ASSERT(szc < mmu_page_sizes);
	VM_STAT_ADD(vmm_vmstats.ppr_reloc[szc]);
	ASSERT(pfn == PFN_BASE(pfn, szc));

	npgs = page_get_pagecnt(targ->p_szc);

	if (repl == NULL) {
		dofree = npgs;		/* Size of target page in MMU pages */
		if (!page_create_wait(dofree, 0)) {
			if (grouplock != 0) {
				group_page_unlock(targ);
			}
			VM_STAT_ADD(vmm_vmstats.ppr_relocnomem[szc]);
			return (ENOMEM);
		}

		/*
		 * seg kmem pages require that the target and replacement
		 * page be the same pagesize.
		 */
		flags = (targ->p_vnode == &kvp) ? PGR_SAMESZC : 0;
		repl = page_get_replacement_page(targ, lgrp, flags);
		if (repl == NULL) {
			if (grouplock != 0) {
				group_page_unlock(targ);
			}
			page_create_putback(dofree);
			VM_STAT_ADD(vmm_vmstats.ppr_relocnomem[szc]);
			return (ENOMEM);
		}
	}
#ifdef DEBUG
	else {
		ASSERT(PAGE_LOCKED(repl));
	}
#endif /* DEBUG */

#if defined(__sparc)
	/*
	 * Let hat_page_relocate() complete the relocation if it's kernel page
	 */
	if (targ->p_vnode == &kvp) {
		*replacement = repl;
		if (hat_page_relocate(target, replacement, nrelocp) != 0) {
			if (grouplock != 0) {
				group_page_unlock(targ);
			}
			if (dofree) {
				*replacement = NULL;
				page_free_replacement_page(repl);
				page_create_putback(dofree);
			}
			VM_STAT_ADD(vmm_vmstats.ppr_krelocfail[szc]);
			return (EAGAIN);
		}
		VM_STAT_ADD(vmm_vmstats.ppr_relocok[szc]);
		return (0);
	}
#else
#if defined(lint)
	dofree = dofree;
#endif
#endif

#ifdef DEBUG
	first_repl = repl;
#endif /* DEBUG */

	for (i = 0; i < npgs; i++) {
		ASSERT(PAGE_EXCL(targ));

		(void) hat_pageunload(targ, HAT_FORCE_PGUNLOAD);

		ASSERT(hat_page_getshare(targ) == 0);
		ASSERT(!PP_ISFREE(targ));
		ASSERT(targ->p_pagenum == (pfn + i));
		ASSERT(repl_contig == 0 ||
		    repl->p_pagenum == (repl_pfn + i));

		/*
		 * Copy the page contents and attributes then
		 * relocate the page in the page hash.
		 */
		ppcopy(targ, repl);
		ppattr = hat_page_getattr(targ, (P_MOD | P_REF | P_RO));
		page_clr_all_props(repl);
		page_set_props(repl, ppattr);
		page_relocate_hash(repl, targ);

		ASSERT(hat_page_getshare(targ) == 0);
		ASSERT(hat_page_getshare(repl) == 0);
		/*
		 * Now clear the props on targ, after the
		 * page_relocate_hash(), they no longer
		 * have any meaning.
		 */
		page_clr_all_props(targ);
		ASSERT(targ->p_next == targ);
		ASSERT(targ->p_prev == targ);
		page_list_concat(&pl, &targ);

		targ++;
		if (repl_contig != 0) {
			repl++;
		} else {
			repl = repl->p_next;
		}
	}
	/* assert that we have come full circle with repl */
	ASSERT(repl_contig == 1 || first_repl == repl);

	*target = pl;
	if (*replacement == NULL) {
		ASSERT(first_repl == repl);
		*replacement = repl;
	}
	VM_STAT_ADD(vmm_vmstats.ppr_relocok[szc]);
	*nrelocp = npgs;
	return (0);
}
/*
 * On success returns 0 and *nrelocp the number of PAGESIZE pages relocated.
 */
int
page_relocate(
	page_t **target,
	page_t **replacement,
	int grouplock,
	int freetarget,
	spgcnt_t *nrelocp,
	lgrp_t *lgrp)
{
	spgcnt_t ret;

	/* do_page_relocate returns 0 on success or errno value */
	ret = do_page_relocate(target, replacement, grouplock, nrelocp, lgrp);

	if (ret != 0 || freetarget == 0) {
		return (ret);
	}
	if (*nrelocp == 1) {
		ASSERT(*target != NULL);
		page_free(*target, 1);
	} else {
		page_t *tpp = *target;
		uint_t szc = tpp->p_szc;
		pgcnt_t npgs = page_get_pagecnt(szc);
		ASSERT(npgs > 1);
		ASSERT(szc != 0);
		do {
			ASSERT(PAGE_EXCL(tpp));
			ASSERT(!hat_page_is_mapped(tpp));
			ASSERT(tpp->p_szc == szc);
			PP_SETFREE(tpp);
			PP_SETAGED(tpp);
			npgs--;
		} while ((tpp = tpp->p_next) != *target);
		ASSERT(npgs == 0);
		page_list_add_pages(*target, 0);
		npgs = page_get_pagecnt(szc);
		page_create_putback(npgs);
	}
	return (ret);
}

/*
 * it is up to the caller to deal with pcf accounting.
 */
void
page_free_replacement_page(page_t *pplist)
{
	page_t *pp;

	while (pplist != NULL) {
		/*
		 * pp_targ is a linked list.
		 */
		pp = pplist;
		if (pp->p_szc == 0) {
			page_sub(&pplist, pp);
			page_clr_all_props(pp);
			PP_SETFREE(pp);
			PP_SETAGED(pp);
			page_list_add(pp, PG_FREE_LIST | PG_LIST_TAIL);
			page_unlock(pp);
			VM_STAT_ADD(pagecnt.pc_free_replacement_page[0]);
		} else {
			spgcnt_t curnpgs = page_get_pagecnt(pp->p_szc);
			page_t *tpp;
			page_list_break(&pp, &pplist, curnpgs);
			tpp = pp;
			do {
				ASSERT(PAGE_EXCL(tpp));
				ASSERT(!hat_page_is_mapped(tpp));
				page_clr_all_props(pp);
				PP_SETFREE(tpp);
				PP_SETAGED(tpp);
			} while ((tpp = tpp->p_next) != pp);
			page_list_add_pages(pp, 0);
			VM_STAT_ADD(pagecnt.pc_free_replacement_page[1]);
		}
	}
}

/*
 * Relocate target to non-relocatable replacement page.
 */
int
page_relocate_cage(page_t **target, page_t **replacement)
{
	page_t *tpp, *rpp;
	spgcnt_t pgcnt, npgs;
	int result;

	tpp = *target;

	ASSERT(PAGE_EXCL(tpp));
	ASSERT(tpp->p_szc == 0);

	pgcnt = btop(page_get_pagesize(tpp->p_szc));

	do {
		(void) page_create_wait(pgcnt, PG_WAIT | PG_NORELOC);
		rpp = page_get_replacement_page(tpp, NULL, PGR_NORELOC);
		if (rpp == NULL) {
			page_create_putback(pgcnt);
			kcage_cageout_wakeup();
		}
	} while (rpp == NULL);

	ASSERT(PP_ISNORELOC(rpp));

	result = page_relocate(&tpp, &rpp, 0, 1, &npgs, NULL);

	if (result == 0) {
		*replacement = rpp;
		if (pgcnt != npgs)
			panic("page_relocate_cage: partial relocation");
	}

	return (result);
}

/*
 * Release the page lock on a page, place on cachelist
 * tail if no longer mapped. Caller can let us know if
 * the page is known to be clean.
 */
int
page_release(page_t *pp, int checkmod)
{
	int status;

	ASSERT(PAGE_LOCKED(pp) && !PP_ISFREE(pp) &&
		(pp->p_vnode != NULL));

	if (!hat_page_is_mapped(pp) && !IS_SWAPVP(pp->p_vnode) &&
	    ((PAGE_SHARED(pp) && page_tryupgrade(pp)) || PAGE_EXCL(pp)) &&
	    pp->p_lckcnt == 0 && pp->p_cowcnt == 0 &&
	    !hat_page_is_mapped(pp)) {

		/*
		 * If page is modified, unlock it
		 *
		 * (p_nrm & P_MOD) bit has the latest stuff because:
		 * (1) We found that this page doesn't have any mappings
		 *	_after_ holding SE_EXCL and
		 * (2) We didn't drop SE_EXCL lock after the check in (1)
		 */
		if (checkmod && hat_ismod(pp)) {
			page_unlock(pp);
			status = PGREL_MOD;
		} else {
			/*LINTED: constant in conditional context*/
			VN_DISPOSE(pp, B_FREE, 0, kcred);
			status = PGREL_CLEAN;
		}
	} else {
		page_unlock(pp);
		status = PGREL_NOTREL;
	}
	return (status);
}

int
page_try_demote_pages(page_t *pp)
{
	page_t *tpp, *rootpp = pp;
	pfn_t	pfn = page_pptonum(pp);
	spgcnt_t i, npgs;
	uint_t	szc = pp->p_szc;
	vnode_t *vp = pp->p_vnode;

	ASSERT(PAGE_EXCL(rootpp));

	VM_STAT_ADD(pagecnt.pc_try_demote_pages[0]);

	if (rootpp->p_szc == 0) {
		VM_STAT_ADD(pagecnt.pc_try_demote_pages[1]);
		return (1);
	}

	if (vp != NULL && !IS_SWAPFSVP(vp) && vp != &kvp) {
		VM_STAT_ADD(pagecnt.pc_try_demote_pages[2]);
		page_demote_vp_pages(rootpp);
		ASSERT(pp->p_szc == 0);
		return (1);
	}

	/*
	 * Adjust rootpp if  passed in is not the base
	 * constituent page.
	 */
	npgs = page_get_pagecnt(rootpp->p_szc);
	ASSERT(npgs > 1);
	if (!IS_P2ALIGNED(pfn, npgs)) {
		pfn = P2ALIGN(pfn, npgs);
		rootpp = page_numtopp_nolock(pfn);
		VM_STAT_ADD(pagecnt.pc_try_demote_pages[3]);
		ASSERT(rootpp->p_vnode != NULL);
		ASSERT(rootpp->p_szc == szc);
	}

	/*
	 * We can't demote kernel pages since we can't hat_unload()
	 * the mappings.
	 */
	if (rootpp->p_vnode == &kvp)
		return (0);

	/*
	 * Attempt to lock all constituent pages except the page passed
	 * in since it's already locked.
	 */
	for (tpp = rootpp, i = 0; i < npgs; i++, tpp++) {
		ASSERT(!PP_ISFREE(tpp));
		ASSERT(tpp->p_vnode != NULL);

		if (tpp != pp && !page_trylock(tpp, SE_EXCL))
			break;
		ASSERT(tpp->p_szc == rootpp->p_szc);
		ASSERT(page_pptonum(tpp) == page_pptonum(rootpp) + i);
		(void) hat_pageunload(tpp, HAT_FORCE_PGUNLOAD);
	}

	/*
	 * If we failed to lock them all then unlock what we have locked
	 * so far and bail.
	 */
	if (i < npgs) {
		tpp = rootpp;
		while (i-- > 0) {
			if (tpp != pp)
				page_unlock(tpp);
			tpp++;
		}
		VM_STAT_ADD(pagecnt.pc_try_demote_pages[4]);
		return (0);
	}

	/*
	 * XXX probably p_szc clearing and page unlocking can be done within
	 * one loop but since this is rare code we can play very safe.
	 */
	for (tpp = rootpp, i = 0; i < npgs; i++, tpp++) {
		ASSERT(PAGE_EXCL(tpp));
		tpp->p_szc = 0;
	}

	/*
	 * Unlock all pages except the page passed in.
	 */
	for (tpp = rootpp, i = 0; i < npgs; i++, tpp++) {
		ASSERT(!hat_page_is_mapped(tpp));
		if (tpp != pp)
			page_unlock(tpp);
	}
	VM_STAT_ADD(pagecnt.pc_try_demote_pages[5]);
	return (1);
}

/*
 * Called by page_free() and page_destroy() to demote the page size code
 * (p_szc) to 0 (since we can't just put a single PAGESIZE page with non zero
 * p_szc on free list, neither can we just clear p_szc of a single page_t
 * within a large page since it will break other code that relies on p_szc
 * being the same for all page_t's of a large page). Anonymous pages should
 * never end up here because anon_map_getpages() cannot deal with p_szc
 * changes after a single constituent page is locked.  While anonymous or
 * kernel large pages are demoted or freed the entire large page at a time
 * with all constituent pages locked EXCL for the file system pages we
 * have to be able to demote a large page (i.e. decrease all constituent pages
 * p_szc) with only just an EXCL lock on one of constituent pages. The reason
 * we can easily deal with anonymous page demotion the entire large page at a
 * time is that those operation originate at address space level and concern
 * the entire large page region with actual demotion only done when pages are
 * not shared with any other processes (therefore we can always get EXCL lock
 * on all anonymous constituent pages after clearing segment page
 * cache). However file system pages can be truncated or invalidated at a
 * PAGESIZE level from the file system side and end up in page_free() or
 * page_destroy() (we also allow only part of the large page to be SOFTLOCKed
 * and therfore pageout should be able to demote a large page by EXCL locking
 * any constituent page that is not under SOFTLOCK). In those cases we cannot
 * rely on being able to lock EXCL all constituent pages.
 *
 * To prevent szc changes on file system pages one has to lock all constituent
 * pages at least SHARED (or call page_szc_lock()). The only subsystem that
 * doesn't rely on locking all constituent pages (or using page_szc_lock()) to
 * prevent szc changes is hat layer that uses its own page level mlist
 * locks. hat assumes that szc doesn't change after mlist lock for a page is
 * taken. Therefore we need to change szc under hat level locks if we only
 * have an EXCL lock on a single constituent page and hat still references any
 * of constituent pages.  (Note we can't "ignore" hat layer by simply
 * hat_pageunload() all constituent pages without having EXCL locks on all of
 * constituent pages). We use hat_page_demote() call to safely demote szc of
 * all constituent pages under hat locks when we only have an EXCL lock on one
 * of constituent pages.
 *
 * This routine calls page_szc_lock() before calling hat_page_demote() to
 * allow segvn in one special case not to lock all constituent pages SHARED
 * before calling hat_memload_array() that relies on p_szc not changeing even
 * before hat level mlist lock is taken.  In that case segvn uses
 * page_szc_lock() to prevent hat_page_demote() changeing p_szc values.
 *
 * Anonymous or kernel page demotion still has to lock all pages exclusively
 * and do hat_pageunload() on all constituent pages before demoting the page
 * therefore there's no need for anonymous or kernel page demotion to use
 * hat_page_demote() mechanism.
 *
 * hat_page_demote() removes all large mappings that map pp and then decreases
 * p_szc starting from the last constituent page of the large page. By working
 * from the tail of a large page in pfn decreasing order allows one looking at
 * the root page to know that hat_page_demote() is done for root's szc area.
 * e.g. if a root page has szc 1 one knows it only has to lock all constituent
 * pages within szc 1 area to prevent szc changes because hat_page_demote()
 * that started on this page when it had szc > 1 is done for this szc 1 area.
 *
 * We are guranteed that all constituent pages of pp's large page belong to
 * the same vnode with the consecutive offsets increasing in the direction of
 * the pfn i.e. the identity of constituent pages can't change until their
 * p_szc is decreased. Therefore it's safe for hat_page_demote() to remove
 * large mappings to pp even though we don't lock any constituent page except
 * pp (i.e. we won't unload e.g. kernel locked page).
 */
static void
page_demote_vp_pages(page_t *pp)
{
	kmutex_t *mtx;

	ASSERT(PAGE_EXCL(pp));
	ASSERT(!PP_ISFREE(pp));
	ASSERT(pp->p_vnode != NULL);
	ASSERT(!IS_SWAPFSVP(pp->p_vnode));
	ASSERT(pp->p_vnode != &kvp);

	VM_STAT_ADD(pagecnt.pc_demote_pages[0]);

	mtx = page_szc_lock(pp);
	if (mtx != NULL) {
		hat_page_demote(pp);
		mutex_exit(mtx);
	}
	ASSERT(pp->p_szc == 0);
}

/*
 * Page retire operation.
 *
 * page_retire()
 * Attempt to retire (throw away) page pp.  We cannot do this if
 * the page is dirty; if the page is clean, we can try.  We return 0 on
 * success, -1 on failure.  This routine should be invoked by the platform's
 * memory error detection code.
 *
 * pages_retired_limit_exceeded()
 * We set a limit on the number of pages which may be retired. This
 * is set to a percentage of total physical memory. This limit is
 * enforced here.
 */

static pgcnt_t	retired_pgcnt = 0;

/*
 * routines to update the count of retired pages
 */
static void
page_retired(page_t *pp)
{
	ASSERT(pp);

	page_settoxic(pp, PAGE_IS_RETIRED);
	atomic_add_long(&retired_pgcnt, 1);
}

static void
retired_page_removed(page_t *pp)
{
	ASSERT(pp);
	ASSERT(page_isretired(pp));
	ASSERT(retired_pgcnt > 0);

	page_clrtoxic(pp);
	atomic_add_long(&retired_pgcnt, -1);
}


static int
pages_retired_limit_exceeded()
{
	pgcnt_t	retired_max;

	/*
	 * If the percentage is zero or is not set correctly,
	 * return TRUE so that pages are not retired.
	 */
	if (max_pages_retired_bps <= 0 ||
	    max_pages_retired_bps >= 10000)
		return (1);

	/*
	 * Calculate the maximum number of pages allowed to
	 * be retired as a percentage of total physical memory
	 * (Remember that we are using basis points, hence the 10000.)
	 */
	retired_max = (physmem * max_pages_retired_bps) / 10000;

	/*
	 * return 'TRUE' if we have already retired more
	 * than the legal limit
	 */
	return (retired_pgcnt >= retired_max);
}

#define	PAGE_RETIRE_SELOCK	0
#define	PAGE_RETIRE_NORECLAIM	1
#define	PAGE_RETIRE_LOCKED	2
#define	PAGE_RETIRE_COW		3
#define	PAGE_RETIRE_DIRTY	4
#define	PAGE_RETIRE_LPAGE	5
#define	PAGE_RETIRE_SUCCESS	6
#define	PAGE_RETIRE_LIMIT	7
#define	PAGE_RETIRE_NCODES	8

typedef struct page_retire_op {
	int	pr_count;
	short	pr_unlock;
	short	pr_retval;
	char	*pr_message;
} page_retire_op_t;

page_retire_op_t page_retire_ops[PAGE_RETIRE_NCODES] = {
	{	0,	0,	-1,	"cannot lock page"		},
	{	0,	0,	-1,	"cannot reclaim cached page"	},
	{	0,	1,	-1,	"page is locked"		},
	{	0,	1,	-1,	"copy-on-write page"		},
	{	0,	1,	-1,	"page is dirty"			},
	{	0,	1,	-1,	"cannot demote large page"	},
	{	0,	0,	0,	"page successfully retired"	},
	{	0,	0,	-1,	"excess pages retired already"	},
};

static int
page_retire_done(page_t *pp, int code)
{
	page_retire_op_t *prop = &page_retire_ops[code];

	prop->pr_count++;

	if (prop->pr_unlock)
		page_unlock(pp);

	if (page_retire_messages > 1) {
		printf("page_retire(%p) pfn 0x%lx %s: %s\n",
		    (void *)pp, page_pptonum(pp),
		    prop->pr_retval == -1 ? "failed" : "succeeded",
		    prop->pr_message);
	}

	return (prop->pr_retval);
}

int
page_retire(page_t *pp, uchar_t flag)
{
	uint64_t pa = ptob((uint64_t)page_pptonum(pp));

	ASSERT(flag == PAGE_IS_FAILING || flag == PAGE_IS_TOXIC);

	/*
	 * DR operations change the association between a page_t
	 * and the physical page it represents. Check if the
	 * page is still bad.
	 */
	if (!page_isfaulty(pp)) {
		page_clrtoxic(pp);
		return (page_retire_done(pp, PAGE_RETIRE_SUCCESS));
	}

	/*
	 * We set the flag here so that even if we fail due
	 * to exceeding the limit for retired pages, the
	 * page will still be checked and either cleared
	 * or retired in page_free().
	 */
	page_settoxic(pp, flag);

	if (flag == PAGE_IS_TOXIC) {
		if (page_retire_messages) {
			cmn_err(CE_NOTE, "Scheduling clearing of error on"
			    " page 0x%08x.%08x",
			    (uint32_t)(pa >> 32), (uint32_t)pa);
		}

	} else { /* PAGE_IS_FAILING */
		if (pages_retired_limit_exceeded()) {
			/*
			 * Return as we have already exceeded the
			 * maximum number of pages allowed to be
			 * retired
			 */
			return (page_retire_done(pp, PAGE_RETIRE_LIMIT));
		}

		if (page_retire_messages) {
			cmn_err(CE_NOTE, "Scheduling removal of "
			    "page 0x%08x.%08x",
			    (uint32_t)(pa >> 32), (uint32_t)pa);
		}
	}

	if (PAGE_LOCKED(pp) || !page_trylock(pp, SE_EXCL))
		return (page_retire_done(pp, PAGE_RETIRE_SELOCK));

	/*
	 * If this is a large page we first try and demote it
	 * to PAGESIZE pages and then dispose of the toxic page.
	 * On failure we will let the page free/destroy
	 * code handle it later since this is a mapped page.
	 * Note that free large pages can always be demoted.
	 *
	 */
	if (pp->p_szc != 0) {
		if (PP_ISFREE(pp))
			(void) page_demote_free_pages(pp);
		else
			(void) page_try_demote_pages(pp);

		if (pp->p_szc != 0)
			return (page_retire_done(pp, PAGE_RETIRE_LPAGE));
	}

	if (PP_ISFREE(pp)) {
		if (!page_reclaim(pp, NULL))
			return (page_retire_done(pp, PAGE_RETIRE_NORECLAIM));
		/*LINTED: constant in conditional context*/
		VN_DISPOSE(pp, pp->p_vnode ? B_INVAL : B_FREE, 0, kcred)
		return (page_retire_done(pp, PAGE_RETIRE_SUCCESS));
	}

	if (pp->p_lckcnt != 0)
		return (page_retire_done(pp, PAGE_RETIRE_LOCKED));

	if (pp->p_cowcnt != 0)
		return (page_retire_done(pp, PAGE_RETIRE_COW));

	/*
	 * Unload all translations to this page.  No new translations
	 * can be created while we hold the exclusive lock on the page.
	 */
	(void) hat_pageunload(pp, HAT_FORCE_PGUNLOAD);

	if (hat_ismod(pp))
		return (page_retire_done(pp, PAGE_RETIRE_DIRTY));

	/*LINTED: constant in conditional context*/
	VN_DISPOSE(pp, B_INVAL, 0, kcred);

	return (page_retire_done(pp, PAGE_RETIRE_SUCCESS));
}

/*
 * Mark any existing pages for migration in the given range
 */
void
page_mark_migrate(struct seg *seg, caddr_t addr, size_t len,
    struct anon_map *amp, ulong_t anon_index, vnode_t *vp,
    u_offset_t vnoff, int rflag)
{
	struct anon	*ap;
	vnode_t		*curvp;
	lgrp_t		*from;
	pgcnt_t		i;
	pgcnt_t		nlocked;
	u_offset_t	off;
	pfn_t		pfn;
	size_t		pgsz;
	size_t		segpgsz;
	pgcnt_t		pages;
	uint_t		pszc;
	page_t		**ppa;
	pgcnt_t		ppa_nentries;
	page_t		*pp;
	caddr_t		va;
	ulong_t		an_idx;
	anon_sync_obj_t	cookie;

	ASSERT(seg->s_as && AS_LOCK_HELD(seg->s_as, &seg->s_as->a_lock));

	/*
	 * Don't do anything if don't need to do lgroup optimizations
	 * on this system
	 */
	if (!lgrp_optimizations())
		return;

	/*
	 * Align address and length to (potentially large) page boundary
	 */
	segpgsz = page_get_pagesize(seg->s_szc);
	addr = (caddr_t)P2ALIGN((uintptr_t)addr, segpgsz);
	if (rflag)
		len = P2ROUNDUP(len, segpgsz);

	/*
	 * Allocate page array to accomodate largest page size
	 */
	pgsz = page_get_pagesize(page_num_pagesizes() - 1);
	ppa_nentries = btop(pgsz);
	ppa = kmem_zalloc(ppa_nentries * sizeof (page_t *), KM_SLEEP);

	/*
	 * Do one (large) page at a time
	 */
	va = addr;
	while (va < addr + len) {
		/*
		 * Lookup (root) page for vnode and offset corresponding to
		 * this virtual address
		 * Try anonmap first since there may be copy-on-write
		 * pages, but initialize vnode pointer and offset using
		 * vnode arguments just in case there isn't an amp.
		 */
		curvp = vp;
		off = vnoff + va - seg->s_base;
		if (amp) {
			ANON_LOCK_ENTER(&amp->a_rwlock, RW_READER);
			an_idx = anon_index + seg_page(seg, va);
			anon_array_enter(amp, an_idx, &cookie);
			ap = anon_get_ptr(amp->ahp, an_idx);
			if (ap)
				swap_xlate(ap, &curvp, &off);
			anon_array_exit(&cookie);
			ANON_LOCK_EXIT(&amp->a_rwlock);
		}

		pp = NULL;
		if (curvp)
			pp = page_lookup(curvp, off, SE_SHARED);

		/*
		 * If there isn't a page at this virtual address,
		 * skip to next page
		 */
		if (pp == NULL) {
			va += PAGESIZE;
			continue;
		}

		/*
		 * Figure out which lgroup this page is in for kstats
		 */
		pfn = page_pptonum(pp);
		from = lgrp_pfn_to_lgrp(pfn);

		/*
		 * Get page size, and round up and skip to next page boundary
		 * if unaligned address
		 */
		pszc = pp->p_szc;
		pgsz = page_get_pagesize(pszc);
		pages = btop(pgsz);
		if (!IS_P2ALIGNED(va, pgsz) ||
		    !IS_P2ALIGNED(pfn, pages) ||
		    pgsz > segpgsz) {
			pgsz = MIN(pgsz, segpgsz);
			page_unlock(pp);
			i = btop(P2END((uintptr_t)va, pgsz) -
			    (uintptr_t)va);
			va = (caddr_t)P2END((uintptr_t)va, pgsz);
			lgrp_stat_add(from->lgrp_id, LGRP_PMM_FAIL_PGS, i);
			continue;
		}

		/*
		 * Upgrade to exclusive lock on page
		 */
		if (!page_tryupgrade(pp)) {
			page_unlock(pp);
			va += pgsz;
			lgrp_stat_add(from->lgrp_id, LGRP_PMM_FAIL_PGS,
			    btop(pgsz));
			continue;
		}

		/*
		 * Remember pages locked exclusively and how many
		 */
		ppa[0] = pp;
		nlocked = 1;

		/*
		 * Lock constituent pages if this is large page
		 */
		if (pages > 1) {
			/*
			 * Lock all constituents except root page, since it
			 * should be locked already.
			 */
			for (i = 1; i < pages; i++) {
				pp++;
				if (!page_trylock(pp, SE_EXCL)) {
					break;
				}
				if (PP_ISFREE(pp) ||
				    pp->p_szc != pszc) {
					/*
					 * hat_page_demote() raced in with us.
					 */
					ASSERT(!IS_SWAPFSVP(curvp));
					page_unlock(pp);
					break;
				}
				ppa[nlocked] = pp;
				nlocked++;
			}
		}

		/*
		 * If all constituent pages couldn't be locked,
		 * unlock pages locked so far and skip to next page.
		 */
		if (nlocked != pages) {
			for (i = 0; i < nlocked; i++)
				page_unlock(ppa[i]);
			va += pgsz;
			lgrp_stat_add(from->lgrp_id, LGRP_PMM_FAIL_PGS,
			    btop(pgsz));
			continue;
		}

		/*
		 * hat_page_demote() can no longer happen
		 * since last cons page had the right p_szc after
		 * all cons pages were locked. all cons pages
		 * should now have the same p_szc.
		 */

		/*
		 * All constituent pages locked successfully, so mark
		 * large page for migration and unload the mappings of
		 * constituent pages, so a fault will occur on any part of the
		 * large page
		 */
		PP_SETMIGRATE(ppa[0]);
		for (i = 0; i < nlocked; i++) {
			pp = ppa[i];
			(void) hat_pageunload(pp, HAT_FORCE_PGUNLOAD);
			ASSERT(hat_page_getshare(pp) == 0);
			page_unlock(pp);
		}
		lgrp_stat_add(from->lgrp_id, LGRP_PMM_PGS, nlocked);

		va += pgsz;
	}
	kmem_free(ppa, ppa_nentries * sizeof (page_t *));
}

/*
 * Migrate any pages that have been marked for migration in the given range
 */
void
page_migrate(
	struct seg	*seg,
	caddr_t		addr,
	page_t		**ppa,
	pgcnt_t		npages)
{
	lgrp_t		*from;
	lgrp_t		*to;
	page_t		*newpp;
	page_t		*pp;
	pfn_t		pfn;
	size_t		pgsz;
	spgcnt_t	page_cnt;
	spgcnt_t	i;
	uint_t		pszc;

	ASSERT(seg->s_as && AS_LOCK_HELD(seg->s_as, &seg->s_as->a_lock));

	while (npages > 0) {
		pp = *ppa;
		pszc = pp->p_szc;
		pgsz = page_get_pagesize(pszc);
		page_cnt = btop(pgsz);

		/*
		 * Check to see whether this page is marked for migration
		 *
		 * Assume that root page of large page is marked for
		 * migration and none of the other constituent pages
		 * are marked.  This really simplifies clearing the
		 * migrate bit by not having to clear it from each
		 * constituent page.
		 *
		 * note we don't want to relocate an entire large page if
		 * someone is only using one subpage.
		 */
		if (npages < page_cnt)
			break;

		/*
		 * Is it marked for migration?
		 */
		if (!PP_ISMIGRATE(pp))
			goto next;

		/*
		 * Determine lgroups that page is being migrated between
		 */
		pfn = page_pptonum(pp);
		if (!IS_P2ALIGNED(pfn, page_cnt)) {
			break;
		}
		from = lgrp_pfn_to_lgrp(pfn);
		to = lgrp_mem_choose(seg, addr, pgsz);

		/*
		 * Check to see whether we are trying to migrate page to lgroup
		 * where it is allocated already
		 */
		if (to == from) {
			PP_CLRMIGRATE(pp);
			goto next;
		}

		/*
		 * Need to get exclusive lock's to migrate
		 */
		for (i = 0; i < page_cnt; i++) {
			ASSERT(PAGE_LOCKED(ppa[i]));
			if (page_pptonum(ppa[i]) != pfn + i ||
			    ppa[i]->p_szc != pszc) {
				break;
			}
			if (!page_tryupgrade(ppa[i])) {
				lgrp_stat_add(from->lgrp_id,
				    LGRP_PM_FAIL_LOCK_PGS,
				    page_cnt);
				break;
			}
		}
		if (i != page_cnt) {
			while (--i != -1) {
				page_downgrade(ppa[i]);
			}
			goto next;
		}

		(void) page_create_wait(page_cnt, PG_WAIT);
		newpp = page_get_replacement_page(pp, to, PGR_SAMESZC);
		if (newpp == NULL) {
			page_create_putback(page_cnt);
			for (i = 0; i < page_cnt; i++) {
				page_downgrade(ppa[i]);
			}
			lgrp_stat_add(to->lgrp_id, LGRP_PM_FAIL_ALLOC_PGS,
			    page_cnt);
			goto next;
		}
		ASSERT(newpp->p_szc == pszc);
		/*
		 * Clear migrate bit and relocate page
		 */
		PP_CLRMIGRATE(pp);
		if (page_relocate(&pp, &newpp, 0, 1, &page_cnt, to)) {
			panic("page_migrate: page_relocate failed");
		}
		ASSERT(page_cnt * PAGESIZE == pgsz);

		/*
		 * Keep stats for number of pages migrated from and to
		 * each lgroup
		 */
		lgrp_stat_add(from->lgrp_id, LGRP_PM_SRC_PGS, page_cnt);
		lgrp_stat_add(to->lgrp_id, LGRP_PM_DEST_PGS, page_cnt);
		/*
		 * update the page_t array we were passed in and
		 * unlink constituent pages of a large page.
		 */
		for (i = 0; i < page_cnt; ++i, ++pp) {
			ASSERT(PAGE_EXCL(newpp));
			ASSERT(newpp->p_szc == pszc);
			ppa[i] = newpp;
			pp = newpp;
			page_sub(&newpp, pp);
			page_downgrade(pp);
		}
		ASSERT(newpp == NULL);
next:
		addr += pgsz;
		ppa += page_cnt;
		npages -= page_cnt;
	}
}

/*
 * initialize the vnode for retired pages
 */
static void
page_retired_init(void)
{
	vn_setops(&retired_ppages, &retired_vnodeops);
}

/* ARGSUSED */
static void
retired_dispose(vnode_t *vp, page_t *pp, int flag, int dn, cred_t *cr)
{
	panic("retired_dispose invoked");
}

/* ARGSUSED */
static void
retired_inactive(vnode_t *vp, cred_t *cr)
{}

void
page_unretire_pages(void)
{
	page_t		*pp;
	kmutex_t	*vphm;
	vnode_t		*vp;
	page_t		*rpages[UNRETIRE_PAGES];
	pgcnt_t		i, npages, rmem;
	uint64_t	pa;

	rmem = 0;

	for (;;) {
		/*
		 * We do this in 2 steps:
		 *
		 * 1. We walk the retired pages list and collect a list of
		 *    pages that have the toxic field cleared.
		 *
		 * 2. We iterate through the page list and unretire each one.
		 *
		 * We have to do it in two steps on account of the mutexes that
		 * we need to acquire.
		 */

		vp = &retired_ppages;
		vphm = page_vnode_mutex(vp);
		mutex_enter(vphm);

		if ((pp = vp->v_pages) == NULL) {
			mutex_exit(vphm);
			break;
		}

		i = 0;
		do {
			ASSERT(pp != NULL);
			ASSERT(pp->p_vnode == vp);

			/*
			 * DR operations change the association between a page_t
			 * and the physical page it represents. Check if the
			 * page is still bad. If not, unretire it.
			 */
			if (!page_isfaulty(pp))
				rpages[i++] = pp;

			pp = pp->p_vpnext;
		} while ((pp != vp->v_pages) && (i < UNRETIRE_PAGES));

		mutex_exit(vphm);

		npages = i;
		for (i = 0; i < npages; i++) {
			pp = rpages[i];
			pa = ptob((uint64_t)page_pptonum(pp));

			/*
			 * Need to upgrade the shared lock to an exclusive
			 * lock in order to hash out the page.
			 *
			 * The page could have been retired but the page lock
			 * may not have been downgraded yet. If so, skip this
			 * page. page_free() will call this function after the
			 * lock is downgraded.
			 */

			if (!PAGE_SHARED(pp) || !page_tryupgrade(pp))
				continue;

			/*
			 * Both page_free() and DR call this function. They
			 * can potentially call this function at the same
			 * time and race with each other.
			 */
			if (!page_isretired(pp) || page_isfaulty(pp)) {
				page_downgrade(pp);
				continue;
			}

			cmn_err(CE_NOTE,
				"unretiring retired page 0x%08x.%08x",
				(uint32_t)(pa >> 32), (uint32_t)pa);

			/*
			 * When a page is removed from the retired pages vnode,
			 * its toxic field is also cleared. So, we do not have
			 * to do that seperately here.
			 */
			page_hashout(pp, (kmutex_t *)NULL);

			/*
			 * This is a good page. So, free it.
			 */
			pp->p_vnode = NULL;
			page_free(pp, 1);
			rmem++;
		}

		/*
		 * If the rpages array was filled up, then there could be more
		 * retired pages that are not faulty. We need to iterate
		 * again and unretire them. Otherwise, we are done.
		 */
		if (npages < UNRETIRE_PAGES)
			break;
	}

	mutex_enter(&freemem_lock);
	availrmem += rmem;
	mutex_exit(&freemem_lock);
}

ulong_t mem_waiters 	= 0;
ulong_t	max_count 	= 20;
#define	MAX_DELAY	0x1ff

/*
 * Check if enough memory is available to proceed.
 * Depending on system configuration and how much memory is
 * reserved for swap we need to check against two variables.
 * e.g. on systems with little physical swap availrmem can be
 * more reliable indicator of how much memory is available.
 * On systems with large phys swap freemem can be better indicator.
 * If freemem drops below threshold level don't return an error
 * immediately but wake up pageout to free memory and block.
 * This is done number of times. If pageout is not able to free
 * memory within certain time return an error.
 * The same applies for availrmem but kmem_reap is used to
 * free memory.
 */
int
page_mem_avail(pgcnt_t npages)
{
	ulong_t count;

#if defined(__i386)
	if (freemem > desfree + npages &&
	    availrmem > swapfs_reserve + npages &&
	    btop(vmem_size(heap_arena, VMEM_FREE)) > tune.t_minarmem +
	    npages)
		return (1);
#else
	if (freemem > desfree + npages &&
	    availrmem > swapfs_reserve + npages)
		return (1);
#endif

	count = max_count;
	atomic_add_long(&mem_waiters, 1);

	while (freemem < desfree + npages && --count) {
		cv_signal(&proc_pageout->p_cv);
		if (delay_sig(hz + (mem_waiters & MAX_DELAY))) {
			atomic_add_long(&mem_waiters, -1);
			return (0);
		}
	}
	if (count == 0) {
		atomic_add_long(&mem_waiters, -1);
		return (0);
	}

	count = max_count;
	while (availrmem < swapfs_reserve + npages && --count) {
		kmem_reap();
		if (delay_sig(hz + (mem_waiters & MAX_DELAY))) {
			atomic_add_long(&mem_waiters, -1);
			return (0);
		}
	}
	atomic_add_long(&mem_waiters, -1);
	if (count == 0)
		return (0);

#if defined(__i386)
	if (btop(vmem_size(heap_arena, VMEM_FREE)) <
	    tune.t_minarmem + npages)
		return (0);
#endif
	return (1);
}


/*
 * Search the memory segments to locate the desired page.  Within a
 * segment, pages increase linearly with one page structure per
 * physical page frame (size PAGESIZE).  The search begins
 * with the segment that was accessed last, to take advantage of locality.
 * If the hint misses, we start from the beginning of the sorted memseg list
 */


/*
 * Some data structures for pfn to pp lookup.
 */
ulong_t mhash_per_slot;
struct memseg *memseg_hash[N_MEM_SLOTS];

page_t *
page_numtopp_nolock(pfn_t pfnum)
{
	struct memseg *seg;
	page_t *pp;
	vm_cpu_data_t *vc = CPU->cpu_vm_data;

	ASSERT(vc != NULL);

	MEMSEG_STAT_INCR(nsearch);

	/* Try last winner first */
	if (((seg = vc->vc_pnum_memseg) != NULL) &&
		(pfnum >= seg->pages_base) && (pfnum < seg->pages_end)) {
		MEMSEG_STAT_INCR(nlastwon);
		pp = seg->pages + (pfnum - seg->pages_base);
		if (pp->p_pagenum == pfnum)
			return ((page_t *)pp);
	}

	/* Else Try hash */
	if (((seg = memseg_hash[MEMSEG_PFN_HASH(pfnum)]) != NULL) &&
		(pfnum >= seg->pages_base) && (pfnum < seg->pages_end)) {
		MEMSEG_STAT_INCR(nhashwon);
		vc->vc_pnum_memseg = seg;
		pp = seg->pages + (pfnum - seg->pages_base);
		if (pp->p_pagenum == pfnum)
			return ((page_t *)pp);
	}

	/* Else Brute force */
	for (seg = memsegs; seg != NULL; seg = seg->next) {
		if (pfnum >= seg->pages_base && pfnum < seg->pages_end) {
			vc->vc_pnum_memseg = seg;
			pp = seg->pages + (pfnum - seg->pages_base);
			return ((page_t *)pp);
		}
	}
	vc->vc_pnum_memseg = NULL;
	MEMSEG_STAT_INCR(nnotfound);
	return ((page_t *)NULL);

}

struct memseg *
page_numtomemseg_nolock(pfn_t pfnum)
{
	struct memseg *seg;
	page_t *pp;

	/* Try hash */
	if (((seg = memseg_hash[MEMSEG_PFN_HASH(pfnum)]) != NULL) &&
		(pfnum >= seg->pages_base) && (pfnum < seg->pages_end)) {
		pp = seg->pages + (pfnum - seg->pages_base);
		if (pp->p_pagenum == pfnum)
			return (seg);
	}

	/* Else Brute force */
	for (seg = memsegs; seg != NULL; seg = seg->next) {
		if (pfnum >= seg->pages_base && pfnum < seg->pages_end) {
			return (seg);
		}
	}
	return ((struct memseg *)NULL);
}

/*
 * Given a page and a count return the page struct that is
 * n structs away from the current one in the global page
 * list.
 *
 * This function wraps to the first page upon
 * reaching the end of the memseg list.
 */
page_t *
page_nextn(page_t *pp, ulong_t n)
{
	struct memseg *seg;
	page_t *ppn;
	vm_cpu_data_t *vc = (vm_cpu_data_t *)CPU->cpu_vm_data;

	ASSERT(vc != NULL);

	if (((seg = vc->vc_pnext_memseg) == NULL) ||
	    (seg->pages_base == seg->pages_end) ||
	    !(pp >= seg->pages && pp < seg->epages)) {

		for (seg = memsegs; seg; seg = seg->next) {
			if (pp >= seg->pages && pp < seg->epages)
				break;
		}

		if (seg == NULL) {
			/* Memory delete got in, return something valid. */
			/* TODO: fix me. */
			seg = memsegs;
			pp = seg->pages;
		}
	}

	/* check for wraparound - possible if n is large */
	while ((ppn = (pp + n)) >= seg->epages || ppn < pp) {
		n -= seg->epages - pp;
		seg = seg->next;
		if (seg == NULL)
			seg = memsegs;
		pp = seg->pages;
	}
	vc->vc_pnext_memseg = seg;
	return (ppn);
}

/*
 * Initialize for a loop using page_next_scan_large().
 */
page_t *
page_next_scan_init(void **cookie)
{
	ASSERT(cookie != NULL);
	*cookie = (void *)memsegs;
	return ((page_t *)memsegs->pages);
}

/*
 * Return the next page in a scan of page_t's, assuming we want
 * to skip over sub-pages within larger page sizes.
 *
 * The cookie is used to keep track of the current memseg.
 */
page_t *
page_next_scan_large(
	page_t		*pp,
	ulong_t		*n,
	void		**cookie)
{
	struct memseg	*seg = (struct memseg *)*cookie;
	page_t		*new_pp;
	ulong_t		cnt;
	pfn_t		pfn;


	/*
	 * get the count of page_t's to skip based on the page size
	 */
	ASSERT(pp != NULL);
	if (pp->p_szc == 0) {
		cnt = 1;
	} else {
		pfn = page_pptonum(pp);
		cnt = page_get_pagecnt(pp->p_szc);
		cnt -= pfn & (cnt - 1);
	}
	*n += cnt;
	new_pp = pp + cnt;

	/*
	 * Catch if we went past the end of the current memory segment. If so,
	 * just move to the next segment with pages.
	 */
	if (new_pp >= seg->epages) {
		do {
			seg = seg->next;
			if (seg == NULL)
				seg = memsegs;
		} while (seg->pages == seg->epages);
		new_pp = seg->pages;
		*cookie = (void *)seg;
	}

	return (new_pp);
}


/*
 * Returns next page in list. Note: this function wraps
 * to the first page in the list upon reaching the end
 * of the list. Callers should be aware of this fact.
 */

/* We should change this be a #define */

page_t *
page_next(page_t *pp)
{
	return (page_nextn(pp, 1));
}

page_t *
page_first()
{
	return ((page_t *)memsegs->pages);
}


/*
 * This routine is called at boot with the initial memory configuration
 * and when memory is added or removed.
 */
void
build_pfn_hash()
{
	pfn_t cur;
	pgcnt_t index;
	struct memseg *pseg;
	int	i;

	/*
	 * Clear memseg_hash array.
	 * Since memory add/delete is designed to operate concurrently
	 * with normal operation, the hash rebuild must be able to run
	 * concurrently with page_numtopp_nolock(). To support this
	 * functionality, assignments to memseg_hash array members must
	 * be done atomically.
	 *
	 * NOTE: bzero() does not currently guarantee this for kernel
	 * threads, and cannot be used here.
	 */
	for (i = 0; i < N_MEM_SLOTS; i++)
		memseg_hash[i] = NULL;

	hat_kpm_mseghash_clear(N_MEM_SLOTS);

	/*
	 * Physmax is the last valid pfn.
	 */
	mhash_per_slot = (physmax + 1) >> MEM_HASH_SHIFT;
	for (pseg = memsegs; pseg != NULL; pseg = pseg->next) {
		index = MEMSEG_PFN_HASH(pseg->pages_base);
		cur = pseg->pages_base;
		do {
			if (index >= N_MEM_SLOTS)
				index = MEMSEG_PFN_HASH(cur);

			if (memseg_hash[index] == NULL ||
			    memseg_hash[index]->pages_base > pseg->pages_base) {
				memseg_hash[index] = pseg;
				hat_kpm_mseghash_update(index, pseg);
			}
			cur += mhash_per_slot;
			index++;
		} while (cur < pseg->pages_end);
	}
}

/*
 * Return the pagenum for the pp
 */
pfn_t
page_pptonum(page_t *pp)
{
	return (pp->p_pagenum);
}

/*
 * interface to the referenced and modified etc bits
 * in the PSM part of the page struct
 * when no locking is desired.
 */
void
page_set_props(page_t *pp, uint_t flags)
{
	ASSERT((flags & ~(P_MOD | P_REF | P_RO)) == 0);
	pp->p_nrm |= (uchar_t)flags;
}

void
page_clr_all_props(page_t *pp)
{
	pp->p_nrm = 0;
}

/*
 * The following functions is called from free_vp_pages()
 * for an inexact estimate of a newly free'd page...
 */
ulong_t
page_share_cnt(page_t *pp)
{
	return (hat_page_getshare(pp));
}

/*
 * The following functions are used in handling memory
 * errors.
 */

int
page_istoxic(page_t *pp)
{
	return ((pp->p_toxic & PAGE_IS_TOXIC) == PAGE_IS_TOXIC);
}

int
page_isfailing(page_t *pp)
{
	return ((pp->p_toxic & PAGE_IS_FAILING) == PAGE_IS_FAILING);
}

int
page_isretired(page_t *pp)
{
	return ((pp->p_toxic & PAGE_IS_RETIRED) == PAGE_IS_RETIRED);
}

int
page_deteriorating(page_t *pp)
{
	return ((pp->p_toxic & (PAGE_IS_TOXIC | PAGE_IS_FAILING)) != 0);
}

void
page_settoxic(page_t *pp, uchar_t flag)
{
	uchar_t new_flag = 0;
	while ((new_flag & flag) != flag) {
		uchar_t old_flag = pp->p_toxic;
		new_flag = old_flag | flag;
		(void) cas8(&pp->p_toxic, old_flag, new_flag);
		new_flag = ((volatile page_t *)pp)->p_toxic;
	}
}

void
page_clrtoxic(page_t *pp)
{
	/*
	 * We don't need to worry about atomicity on the
	 * p_toxic flag here as this is only called from
	 * page_free() while holding an exclusive lock on
	 * the page
	 */
	pp->p_toxic = PAGE_IS_OK;
}

void
page_clrtoxic_flag(page_t *pp, uchar_t flag)
{
	uchar_t new_flag = ((volatile page_t *)pp)->p_toxic;
	while ((new_flag & flag) == flag) {
		uchar_t old_flag = new_flag;
		new_flag = old_flag & ~flag;
		(void) cas8(&pp->p_toxic, old_flag, new_flag);
		new_flag = ((volatile page_t *)pp)->p_toxic;
	}
}

int
page_isfaulty(page_t *pp)
{
	return ((pp->p_toxic & PAGE_IS_FAULTY) == PAGE_IS_FAULTY);
}

/*
 * The following four functions are called from /proc code
 * for the /proc/<pid>/xmap interface.
 */
int
page_isshared(page_t *pp)
{
	return (hat_page_getshare(pp) > 1);
}

int
page_isfree(page_t *pp)
{
	return (PP_ISFREE(pp));
}

int
page_isref(page_t *pp)
{
	return (hat_page_getattr(pp, P_REF));
}

int
page_ismod(page_t *pp)
{
	return (hat_page_getattr(pp, P_MOD));
}
