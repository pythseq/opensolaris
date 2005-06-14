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

/*
 * Mdb kernel support module.  This module is loaded automatically when the
 * kvm target is initialized.  Any global functions declared here are exported
 * for the resolution of symbols in subsequently loaded modules.
 *
 * WARNING: Do not assume that static variables in mdb_ks will be initialized
 * to zero.
 */


#include <mdb/mdb_target.h>
#include <mdb/mdb_param.h>
#include <mdb/mdb_modapi.h>
#include <mdb/mdb_ks.h>

#include <sys/types.h>
#include <sys/procfs.h>
#include <sys/proc.h>
#include <sys/dnlc.h>
#include <sys/autoconf.h>
#include <sys/machelf.h>
#include <sys/modctl.h>
#include <sys/hwconf.h>
#include <sys/kobj.h>
#include <sys/fs/autofs.h>
#include <sys/ddi_impldefs.h>
#include <sys/refstr_impl.h>
#include <errno.h>

#include <vm/seg_vn.h>
#include <vm/page.h>

#define	MDB_PATH_NELEM	256			/* Maximum path components */

typedef struct mdb_path {
	size_t mdp_nelem;			/* Number of components */
	uint_t mdp_complete;			/* Path completely resolved? */
	uintptr_t mdp_vnode[MDB_PATH_NELEM];	/* Array of vnode_t addresses */
	char *mdp_name[MDB_PATH_NELEM];		/* Array of name components */
} mdb_path_t;

static int mdb_autonode2path(uintptr_t, mdb_path_t *);
static int mdb_sprintpath(char *, size_t, mdb_path_t *);

/*
 * Kernel parameters from <sys/param.h> which we keep in-core:
 */
unsigned long _mdb_ks_pagesize;
unsigned int _mdb_ks_pageshift;
unsigned long _mdb_ks_pageoffset;
unsigned long long _mdb_ks_pagemask;
unsigned long _mdb_ks_mmu_pagesize;
unsigned int _mdb_ks_mmu_pageshift;
unsigned long _mdb_ks_mmu_pageoffset;
unsigned long _mdb_ks_mmu_pagemask;
uintptr_t _mdb_ks_kernelbase;
uintptr_t _mdb_ks_userlimit;
uintptr_t _mdb_ks_userlimit32;
uintptr_t _mdb_ks_argsbase;
unsigned long _mdb_ks_msg_bsize;
unsigned long _mdb_ks_defaultstksz;
int _mdb_ks_ncpu;

/*
 * In-core copy of DNLC information:
 */
#define	MDB_DNLC_HSIZE	1024
#define	MDB_DNLC_HASH(vp)	(((uintptr_t)(vp) >> 3) & (MDB_DNLC_HSIZE - 1))
#define	MDB_DNLC_NCACHE_SZ(ncp) (sizeof (ncache_t) + (ncp)->namlen)
#define	MDB_DNLC_MAX_RETRY 4


static ncache_t **dnlc_hash;	/* mdbs hash array of dnlc entries */

/*
 * This will be the location of the vnodeops pointer for "autofs_vnodeops"
 * The pointer still needs to be read with mdb_vread() to get the location
 * of the vnodeops structure for autofs.
 */
static struct vnodeops *autofs_vnops_ptr;

/*
 * STREAMS queue registrations:
 */
typedef struct mdb_qinfo {
	const mdb_qops_t *qi_ops;	/* Address of ops vector */
	uintptr_t qi_addr;		/* Address of qinit structure (key) */
	struct mdb_qinfo *qi_next;	/* Next qinfo in list */
} mdb_qinfo_t;

static mdb_qinfo_t *qi_head;		/* Head of qinfo chain */

/*
 * Device naming callback structure:
 */
typedef struct nm_query {
	const char *nm_name;		/* Device driver name [in/out] */
	major_t nm_major;		/* Device major number [in/out] */
	ushort_t nm_found;		/* Did we find a match? [out] */
} nm_query_t;

/*
 * Address-to-modctl callback structure:
 */
typedef struct a2m_query {
	uintptr_t a2m_addr;		/* Virtual address [in] */
	uintptr_t a2m_where;		/* Modctl address [out] */
} a2m_query_t;

/*
 * Segment-to-mdb_map callback structure:
 */
typedef struct {
	struct seg_ops *asm_segvn_ops;	/* Address of segvn ops [in] */
	void (*asm_callback)(const struct mdb_map *, void *); /* Callb [in] */
	void *asm_cbdata;		/* Callback data [in] */
} asmap_arg_t;

static void
dnlc_free(void)
{
	ncache_t *ncp, *next;
	int i;

	if (dnlc_hash == NULL) {
		return;
	}

	/*
	 * Free up current dnlc entries
	 */
	for (i = 0; i < MDB_DNLC_HSIZE; i++) {
		for (ncp = dnlc_hash[i]; ncp; ncp = next) {
			next = ncp->hash_next;
			mdb_free(ncp, MDB_DNLC_NCACHE_SZ(ncp));
		}
	}
	mdb_free(dnlc_hash, MDB_DNLC_HSIZE * sizeof (ncache_t *));
	dnlc_hash = NULL;
}

char bad_dnlc[] = "inconsistent dnlc chain: %d, ncache va: %p"
	" - continuing with the rest\n";

static int
dnlc_load(void)
{
	int i; /* hash index */
	int retry_cnt = 0;
	int skip_bad_chains = 0;
	int nc_hashsz; /* kernel hash array size */
	uintptr_t nc_hash_addr; /* kernel va of ncache hash array */
	uintptr_t head; /* kernel va of head of hash chain */

	/*
	 * If we've already cached the DNLC and we're looking at a dump,
	 * our cache is good forever, so don't bother re-loading.
	 */
	if (dnlc_hash && mdb_prop_postmortem) {
		return (0);
	}

	/*
	 * For a core dump, retries wont help.
	 * Just print and skip any bad chains.
	 */
	if (mdb_prop_postmortem) {
		skip_bad_chains = 1;
	}
retry:
	if (retry_cnt++ >= MDB_DNLC_MAX_RETRY) {
		/*
		 * Give up retrying the rapidly changing dnlc.
		 * Just print and skip any bad chains
		 */
		skip_bad_chains = 1;
	}

	dnlc_free(); /* Free up the mdb hashed dnlc - if any */

	/*
	 * Although nc_hashsz and the location of nc_hash doesn't currently
	 * change, it may do in the future with a more dynamic dnlc.
	 * So always read these values afresh.
	 */
	if (mdb_readvar(&nc_hashsz, "nc_hashsz") == -1) {
		mdb_warn("failed to read nc_hashsz");
		return (-1);
	}
	if (mdb_readvar(&nc_hash_addr, "nc_hash") == -1) {
		mdb_warn("failed to read nc_hash");
		return (-1);
	}

	/*
	 * Allocate the mdb dnlc hash array
	 */
	dnlc_hash = mdb_zalloc(MDB_DNLC_HSIZE * sizeof (ncache_t *), UM_SLEEP);

	/* for each kernel hash chain */
	for (i = 0, head = nc_hash_addr; i < nc_hashsz;
	    i++, head += sizeof (nc_hash_t)) {
		nc_hash_t nch; /* kernel hash chain header */
		ncache_t *ncp; /* name cache pointer */
		int hash; /* mdb hash value */
		uintptr_t nc_va; /* kernel va of next ncache */
		uintptr_t ncprev_va; /* kernel va of previous ncache */
		int khash; /* kernel dnlc hash value */
		uchar_t namelen; /* name length */
		ncache_t nc; /* name cache entry */
		int nc_size; /* size of a name cache entry */

		/*
		 * We read each element of the nc_hash array individually
		 * just before we process the entries in its chain. This is
		 * because the chain can change so rapidly on a running system.
		 */
		if (mdb_vread(&nch, sizeof (nc_hash_t), head) == -1) {
			mdb_warn("failed to read nc_hash chain header %d", i);
			dnlc_free();
			return (-1);
		}

		ncprev_va = head;
		nc_va = (uintptr_t)(nch.hash_next);
		/* for each entry in the chain */
		while (nc_va != head) {
			/*
			 * The size of the ncache entries varies
			 * because the name is appended to the structure.
			 * So we read in the structure then re-read
			 * for the structure plus name.
			 */
			if (mdb_vread(&nc, sizeof (ncache_t), nc_va) == -1) {
				if (skip_bad_chains) {
					mdb_warn(bad_dnlc, i, nc_va);
					break;
				}
				goto retry;
			}
			nc_size = MDB_DNLC_NCACHE_SZ(&nc);
			ncp = mdb_alloc(nc_size, UM_SLEEP);
			if (mdb_vread(ncp, nc_size - 1, nc_va) == -1) {
				mdb_free(ncp, nc_size);
				if (skip_bad_chains) {
					mdb_warn(bad_dnlc, i, nc_va);
					break;
				}
				goto retry;
			}

			/*
			 * Check for chain consistency
			 */
			if ((uintptr_t)ncp->hash_prev != ncprev_va) {
				mdb_free(ncp, nc_size);
				if (skip_bad_chains) {
					mdb_warn(bad_dnlc, i, nc_va);
					break;
				}
				goto retry;
			}
			/*
			 * Terminate the new name with a null.
			 * Note, we allowed space for this null when
			 * allocating space for the entry.
			 */
			ncp->name[ncp->namlen] = '\0';

			/*
			 * Validate new entry by re-hashing using the
			 * kernel dnlc hash function and comparing the hash
			 */
			DNLCHASH(ncp->name, ncp->dp, khash, namelen);
			if ((namelen != ncp->namlen) ||
			    (khash != ncp->hash)) {
				mdb_free(ncp, nc_size);
				if (skip_bad_chains) {
					mdb_warn(bad_dnlc, i, nc_va);
					break;
				}
				goto retry;
			}

			/*
			 * Finally put the validated entry into the mdb
			 * hash chains. Reuse the kernel next hash field
			 * for the mdb hash chain pointer.
			 */
			hash = MDB_DNLC_HASH(ncp->vp);
			ncprev_va = nc_va;
			nc_va = (uintptr_t)(ncp->hash_next);
			ncp->hash_next = dnlc_hash[hash];
			dnlc_hash[hash] = ncp;
		}
	}
	return (0);
}

/*ARGSUSED*/
int
dnlcdump(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv)
{
	ncache_t *ent;
	int i;

	if ((flags & DCMD_ADDRSPEC) || argc != 0)
		return (DCMD_USAGE);

	if (dnlc_load() == -1)
		return (DCMD_ERR);

	mdb_printf("%<u>%-?s %-?s %-32s%</u>\n", "VP", "DVP", "NAME");

	for (i = 0; i < MDB_DNLC_HSIZE; i++) {
		for (ent = dnlc_hash[i]; ent != NULL; ent = ent->hash_next) {
			mdb_printf("%0?p %0?p %s\n",
			    ent->vp, ent->dp, ent->name);
		}
	}

	return (DCMD_OK);
}

static int
mdb_sprintpath(char *buf, size_t len, mdb_path_t *path)
{
	char *s = buf;
	int i;

	if (len < sizeof ("/..."))
		return (-1);

	if (!path->mdp_complete) {
		(void) strcpy(s, "??");
		s += 2;

		if (path->mdp_nelem == 0)
			return (-1);
	}

	if (path->mdp_nelem == 0) {
		(void) strcpy(s, "/");
		return (0);
	}

	for (i = path->mdp_nelem - 1; i >= 0; i--) {
		/*
		 * Number of bytes left is the distance from where we
		 * are to the end, minus 2 for '/' and '\0'
		 */
		ssize_t left = (ssize_t)(&buf[len] - s) - 2;

		if (left <= 0)
			break;

		*s++ = '/';
		(void) strncpy(s, path->mdp_name[i], left);
		s[left - 1] = '\0';
		s += strlen(s);

		if (left < strlen(path->mdp_name[i]))
			break;
	}

	if (i >= 0)
		(void) strcpy(&buf[len - 4], "...");

	return (0);
}

static int
mdb_autonode2path(uintptr_t addr, mdb_path_t *path)
{
	fninfo_t fni;
	fnnode_t fn;

	vnode_t vn;
	vfs_t vfs;
	struct vnodeops *autofs_vnops = NULL;

	/*
	 * "autofs_vnops_ptr" is the address of the pointer to the vnodeops
	 * structure for autofs.  We want to read it each time we access
	 * it since autofs could (in theory) be unloaded and reloaded.
	 */
	if (mdb_vread(&autofs_vnops, sizeof (autofs_vnops),
	    (uintptr_t)autofs_vnops_ptr) == -1)
		return (-1);

	if (mdb_vread(&vn, sizeof (vn), addr) == -1)
		return (-1);

	if (autofs_vnops == NULL || vn.v_op != autofs_vnops)
		return (-1);

	addr = (uintptr_t)vn.v_data;

	if (mdb_vread(&vfs, sizeof (vfs), (uintptr_t)vn.v_vfsp) == -1 ||
	    mdb_vread(&fni, sizeof (fni), (uintptr_t)vfs.vfs_data) == -1 ||
	    mdb_vread(&vn, sizeof (vn), (uintptr_t)fni.fi_rootvp) == -1)
		return (-1);

	for (;;) {
		size_t elem = path->mdp_nelem++;
		char elemstr[MAXNAMELEN];
		char *c, *p;

		if (elem == MDB_PATH_NELEM) {
			path->mdp_nelem--;
			return (-1);
		}

		if (mdb_vread(&fn, sizeof (fn), addr) != sizeof (fn)) {
			path->mdp_nelem--;
			return (-1);
		}

		if (mdb_readstr(elemstr, sizeof (elemstr),
		    (uintptr_t)fn.fn_name) <= 0) {
			(void) strcpy(elemstr, "?");
		}

		c = mdb_alloc(strlen(elemstr) + 1, UM_SLEEP | UM_GC);
		(void) strcpy(c, elemstr);

		path->mdp_vnode[elem] = (uintptr_t)fn.fn_vnode;

		if (addr == (uintptr_t)fn.fn_parent) {
			path->mdp_name[elem] = &c[1];
			path->mdp_complete = TRUE;
			break;
		}

		if ((p = strrchr(c, '/')) != NULL)
			path->mdp_name[elem] = p + 1;
		else
			path->mdp_name[elem] = c;

		addr = (uintptr_t)fn.fn_parent;
	}

	return (0);
}

int
mdb_vnode2path(uintptr_t addr, char *buf, size_t buflen)
{
	uintptr_t rootdir;
	ncache_t *ent;
	vnode_t vp;
	mdb_path_t path;

	/*
	 * Check to see if we have a cached value for this vnode
	 */
	if (mdb_vread(&vp, sizeof (vp), addr) != -1 &&
	    vp.v_path != NULL &&
	    mdb_readstr(buf, buflen, (uintptr_t)vp.v_path) != -1)
		return (0);

	if (dnlc_load() == -1)
		return (-1);

	if (mdb_readvar(&rootdir, "rootdir") == -1) {
		mdb_warn("failed to read 'rootdir'");
		return (-1);
	}

	bzero(&path, sizeof (mdb_path_t));
again:
	if ((addr == NULL) && (path.mdp_nelem == 0)) {
		/*
		 * 0 elems && complete tells sprintpath to just print "/"
		 */
		path.mdp_complete = TRUE;
		goto out;
	}

	if (addr == rootdir) {
		path.mdp_complete = TRUE;
		goto out;
	}

	for (ent = dnlc_hash[MDB_DNLC_HASH(addr)]; ent; ent = ent->hash_next) {
		if ((uintptr_t)ent->vp == addr) {
			if (strcmp(ent->name, "..") == 0 ||
			    strcmp(ent->name, ".") == 0)
				continue;

			path.mdp_vnode[path.mdp_nelem] = (uintptr_t)ent->vp;
			path.mdp_name[path.mdp_nelem] = ent->name;
			path.mdp_nelem++;

			if (path.mdp_nelem == MDB_PATH_NELEM) {
				path.mdp_nelem--;
				mdb_warn("path exceeded maximum expected "
				    "elements\n");
				return (-1);
			}

			addr = (uintptr_t)ent->dp;
			goto again;
		}
	}

	(void) mdb_autonode2path(addr, &path);

out:
	return (mdb_sprintpath(buf, buflen, &path));
}


uintptr_t
mdb_pid2proc(pid_t pid, proc_t *proc)
{
	int pid_hashsz, hash;
	uintptr_t paddr, pidhash, procdir;
	struct pid pidp;

	if (mdb_readvar(&pidhash, "pidhash") == -1)
		return (NULL);

	if (mdb_readvar(&pid_hashsz, "pid_hashsz") == -1)
		return (NULL);

	if (mdb_readvar(&procdir, "procdir") == -1)
		return (NULL);

	hash = pid & (pid_hashsz - 1);

	if (mdb_vread(&paddr, sizeof (paddr),
	    pidhash + (hash * sizeof (paddr))) == -1)
		return (NULL);

	while (paddr != 0) {
		if (mdb_vread(&pidp, sizeof (pidp), paddr) == -1)
			return (NULL);

		if (pidp.pid_id == pid) {
			uintptr_t procp;

			if (mdb_vread(&procp, sizeof (procp), procdir +
			    (pidp.pid_prslot * sizeof (procp))) == -1)
				return (NULL);

			if (proc != NULL)
				(void) mdb_vread(proc, sizeof (proc_t), procp);

			return (procp);
		}
		paddr = (uintptr_t)pidp.pid_link;
	}
	return (NULL);
}

uintptr_t
mdb_vnode2page(uintptr_t vp, uintptr_t offset)
{
	long page_hashsz, ndx;
	uintptr_t page_hash, pp;

	if (mdb_readvar(&page_hashsz, "page_hashsz") == -1 ||
	    mdb_readvar(&page_hash, "page_hash") == -1)
		return (NULL);

	ndx = PAGE_HASH_FUNC(vp, offset);
	page_hash += ndx * sizeof (uintptr_t);

	mdb_vread(&pp, sizeof (pp), page_hash);

	while (pp != NULL) {
		page_t page;

		mdb_vread(&page, sizeof (page), pp);

		if ((uintptr_t)page.p_vnode == vp &&
		    (uintptr_t)page.p_offset == offset)
			return (pp);

		pp = (uintptr_t)page.p_hash;
	}

	return (NULL);
}

char
mdb_vtype2chr(vtype_t type, mode_t mode)
{
	static const char vttab[] = {
		' ',	/* VNON */
		' ',	/* VREG */
		'/',	/* VDIR */
		' ',	/* VBLK */
		' ',	/* VCHR */
		'@',	/* VLNK */
		'|',	/* VFIFO */
		'>',	/* VDOOR */
		' ',	/* VPROC */
		'=',	/* VSOCK */
		' ',	/* VBAD */
	};

	if (type < 0 || type >= sizeof (vttab) / sizeof (vttab[0]))
		return ('?');

	if (type == VREG && (mode & 0111) != 0)
		return ('*');

	return (vttab[type]);
}

static int
a2m_walk_modctl(uintptr_t addr, const struct modctl *m, a2m_query_t *a2m)
{
	struct module mod;

	if (m->mod_mp == NULL)
		return (0);

	if (mdb_vread(&mod, sizeof (mod), (uintptr_t)m->mod_mp) == -1) {
		mdb_warn("couldn't read modctl %p's module", addr);
		return (0);
	}

	if (a2m->a2m_addr >= (uintptr_t)mod.text &&
	    a2m->a2m_addr < (uintptr_t)mod.text + mod.text_size)
		goto found;

	if (a2m->a2m_addr >= (uintptr_t)mod.data &&
	    a2m->a2m_addr < (uintptr_t)mod.data + mod.data_size)
		goto found;

	return (0);

found:
	a2m->a2m_where = addr;
	return (-1);
}

uintptr_t
mdb_addr2modctl(uintptr_t addr)
{
	a2m_query_t a2m;

	a2m.a2m_addr = addr;
	a2m.a2m_where = NULL;

	(void) mdb_walk("modctl", (mdb_walk_cb_t)a2m_walk_modctl, &a2m);
	return (a2m.a2m_where);
}

static mdb_qinfo_t *
qi_lookup(uintptr_t qinit_addr)
{
	mdb_qinfo_t *qip;

	for (qip = qi_head; qip != NULL; qip = qip->qi_next) {
		if (qip->qi_addr == qinit_addr)
			return (qip);
	}

	return (NULL);
}

void
mdb_qops_install(const mdb_qops_t *qops, uintptr_t qinit_addr)
{
	mdb_qinfo_t *qip = qi_lookup(qinit_addr);

	if (qip != NULL) {
		qip->qi_ops = qops;
		return;
	}

	qip = mdb_alloc(sizeof (mdb_qinfo_t), UM_SLEEP);

	qip->qi_ops = qops;
	qip->qi_addr = qinit_addr;
	qip->qi_next = qi_head;

	qi_head = qip;
}

void
mdb_qops_remove(const mdb_qops_t *qops, uintptr_t qinit_addr)
{
	mdb_qinfo_t *qip, *p = NULL;

	for (qip = qi_head; qip != NULL; p = qip, qip = qip->qi_next) {
		if (qip->qi_addr == qinit_addr && qip->qi_ops == qops) {
			if (qi_head == qip)
				qi_head = qip->qi_next;
			else
				p->qi_next = qip->qi_next;
			mdb_free(qip, sizeof (mdb_qinfo_t));
			return;
		}
	}
}

char *
mdb_qname(const queue_t *q, char *buf, size_t nbytes)
{
	struct module_info mi;
	struct qinit qi;

	if (mdb_vread(&qi, sizeof (qi), (uintptr_t)q->q_qinfo) == -1) {
		mdb_warn("failed to read qinit at %p", q->q_qinfo);
		goto err;
	}

	if (mdb_vread(&mi, sizeof (mi), (uintptr_t)qi.qi_minfo) == -1) {
		mdb_warn("failed to read module_info at %p", qi.qi_minfo);
		goto err;
	}

	if (mdb_readstr(buf, nbytes, (uintptr_t)mi.mi_idname) <= 0) {
		mdb_warn("failed to read mi_idname at %p", mi.mi_idname);
		goto err;
	}

	return (buf);

err:
	(void) mdb_snprintf(buf, nbytes, "???");
	return (buf);
}

void
mdb_qinfo(const queue_t *q, char *buf, size_t nbytes)
{
	mdb_qinfo_t *qip = qi_lookup((uintptr_t)q->q_qinfo);
	buf[0] = '\0';

	if (qip != NULL)
		qip->qi_ops->q_info(q, buf, nbytes);
}

uintptr_t
mdb_qrnext(const queue_t *q)
{
	mdb_qinfo_t *qip = qi_lookup((uintptr_t)q->q_qinfo);

	if (qip != NULL)
		return (qip->qi_ops->q_rnext(q));

	return (NULL);
}

uintptr_t
mdb_qwnext(const queue_t *q)
{
	mdb_qinfo_t *qip = qi_lookup((uintptr_t)q->q_qinfo);

	if (qip != NULL)
		return (qip->qi_ops->q_wnext(q));

	return (NULL);
}

uintptr_t
mdb_qrnext_default(const queue_t *q)
{
	return ((uintptr_t)q->q_next);
}

uintptr_t
mdb_qwnext_default(const queue_t *q)
{
	return ((uintptr_t)q->q_next);
}

/*
 * The following three routines borrowed from modsubr.c
 */
static int
nm_hash(const char *name)
{
	char c;
	int hash = 0;

	for (c = *name++; c; c = *name++)
		hash ^= c;

	return (hash & MOD_BIND_HASHMASK);
}

static uintptr_t
find_mbind(const char *name, uintptr_t *hashtab)
{
	int hashndx;
	uintptr_t mb;
	struct bind mb_local;
	char node_name[MODMAXNAMELEN + 1];


	hashndx = nm_hash(name);
	mb = hashtab[hashndx];
	while (mb) {
		if (mdb_vread(&mb_local, sizeof (mb_local), mb) == -1) {
			mdb_warn("failed to read struct bind at %p", mb);
			return (NULL);
		}
		if (mdb_readstr(node_name, sizeof (node_name),
		    (uintptr_t)mb_local.b_name) == -1) {
			mdb_warn("failed to read node name string at %p",
				mb_local.b_name);
			return (NULL);
		}

		if (strcmp(name, node_name) == 0)
			break;

		mb = (uintptr_t)mb_local.b_next;
	}
	return (mb);
}

int
mdb_name_to_major(const char *name, major_t *major)
{
	uintptr_t	mbind;
	uintptr_t	mb_hashtab[MOD_BIND_HASHSIZE];
	struct bind 	mbind_local;


	if (mdb_readsym(mb_hashtab, sizeof (mb_hashtab), "mb_hashtab") == -1) {
		mdb_warn("failed to read symbol 'mb_hashtab'");
		return (-1);
	}

	if ((mbind = find_mbind(name, mb_hashtab)) != NULL) {
		if (mdb_vread(&mbind_local, sizeof (mbind_local), mbind) ==
		    -1) {
			mdb_warn("failed to read mbind struct at %p", mbind);
			return (-1);
		}

		*major = (major_t)mbind_local.b_num;
		return (0);
	}
	return (-1);
}

const char *
mdb_major_to_name(major_t major)
{
	static char name[MODMAXNAMELEN + 1];

	uintptr_t devnamesp;
	struct devnames dn;
	uint_t devcnt;

	if (mdb_readvar(&devcnt, "devcnt") == -1 || major >= devcnt ||
	    mdb_readvar(&devnamesp, "devnamesp") == -1)
		return (NULL);

	if (mdb_vread(&dn, sizeof (struct devnames), devnamesp +
	    major * sizeof (struct devnames)) != sizeof (struct devnames))
		return (NULL);

	if (mdb_readstr(name, MODMAXNAMELEN + 1, (uintptr_t)dn.dn_name) == -1)
		return (NULL);

	return ((const char *)name);
}

/*
 * Return the name of the driver attached to the dip in drivername.
 */
int
mdb_devinfo2driver(uintptr_t dip_addr, char *drivername, size_t namebufsize)
{
	struct dev_info	devinfo;
	char bind_name[MODMAXNAMELEN + 1];
	major_t	major;
	const char *namestr;


	if (mdb_vread(&devinfo, sizeof (devinfo), dip_addr) == -1) {
		mdb_warn("failed to read devinfo at %p", dip_addr);
		return (-1);
	}

	if (mdb_readstr(bind_name, sizeof (bind_name),
	    (uintptr_t)devinfo.devi_binding_name) == -1) {
		mdb_warn("failed to read binding name at %p",
		    devinfo.devi_binding_name);
		return (-1);
	}

	/*
	 * Many->one relation: various names to one major number
	 */
	if (mdb_name_to_major(bind_name, &major) == -1) {
		mdb_warn("failed to translate bind name to major number\n");
		return (-1);
	}

	/*
	 * One->one relation: one major number corresponds to one driver
	 */
	if ((namestr = mdb_major_to_name(major)) == NULL) {
		(void) strncpy(drivername, "???", namebufsize);
		return (-1);
	}

	(void) strncpy(drivername, namestr, namebufsize);
	return (0);
}

/*
 * Find the name of the driver attached to this dip (if any), given:
 * - the address of a dip (in core)
 * - the NAME of the global pointer to the driver's i_ddi_soft_state struct
 * - pointer to a pointer to receive the address
 */
int
mdb_devinfo2statep(uintptr_t dip_addr, char *soft_statep_name,
    uintptr_t *statep)
{
	struct dev_info	dev_info;


	if (mdb_vread(&dev_info, sizeof (dev_info), dip_addr) == -1) {
		mdb_warn("failed to read devinfo at %p", dip_addr);
		return (-1);
	}

	return (mdb_get_soft_state_byname(soft_statep_name,
	    dev_info.devi_instance, statep, NULL, 0));
}

/*
 * Returns a pointer to the top of the soft state struct for the instance
 * specified (in state_addr), given the address of the global soft state
 * pointer and size of the struct.  Also fills in the buffer pointed to by
 * state_buf_p (if non-NULL) with the contents of the state struct.
 */
int
mdb_get_soft_state_byaddr(uintptr_t ssaddr, uint_t instance,
    uintptr_t *state_addr, void *state_buf_p, size_t sizeof_state)
{
	struct i_ddi_soft_state ss;
	void *statep;


	if (mdb_vread(&ss, sizeof (ss), ssaddr) == -1)
		return (-1);

	if (instance >= ss.n_items)
		return (-1);

	if (mdb_vread(&statep, sizeof (statep), (uintptr_t)ss.array +
	    (sizeof (statep) * instance)) == -1)
		return (-1);

	if (state_addr != NULL)
		*state_addr = (uintptr_t)statep;

	if (statep == NULL) {
		errno = ENOENT;
		return (-1);
	}

	if (state_buf_p != NULL) {

		/* Read the state struct into the buffer in local space. */
		if (mdb_vread(state_buf_p, sizeof_state,
		    (uintptr_t)statep) == -1)
			return (-1);
	}

	return (0);
}


/*
 * Returns a pointer to the top of the soft state struct for the instance
 * specified (in state_addr), given the name of the global soft state pointer
 * and size of the struct.  Also fills in the buffer pointed to by
 * state_buf_p (if non-NULL) with the contents of the state struct.
 */
int
mdb_get_soft_state_byname(char *softstatep_name, uint_t instance,
    uintptr_t *state_addr, void *state_buf_p, size_t sizeof_state)
{
	uintptr_t ssaddr;

	if (mdb_readvar((void *)&ssaddr, softstatep_name) == -1)
		return (-1);

	return (mdb_get_soft_state_byaddr(ssaddr, instance, state_addr,
	    state_buf_p, sizeof_state));
}

static const mdb_dcmd_t dcmds[] = {
	{ "dnlc", NULL, "print DNLC contents", dnlcdump },
	{ NULL }
};

static const mdb_modinfo_t modinfo = { MDB_API_VERSION, dcmds };

/*ARGSUSED*/
static void
update_vars(void *arg)
{
	GElf_Sym sym;

	if (mdb_lookup_by_name("auto_vnodeops", &sym) == 0)
		autofs_vnops_ptr = (struct vnodeops *)(uintptr_t)sym.st_value;
	else
		autofs_vnops_ptr = NULL;

	(void) mdb_readvar(&_mdb_ks_pagesize, "_pagesize");
	(void) mdb_readvar(&_mdb_ks_pageshift, "_pageshift");
	(void) mdb_readvar(&_mdb_ks_pageoffset, "_pageoffset");
	(void) mdb_readvar(&_mdb_ks_pagemask, "_pagemask");
	(void) mdb_readvar(&_mdb_ks_mmu_pagesize, "_mmu_pagesize");
	(void) mdb_readvar(&_mdb_ks_mmu_pageshift, "_mmu_pageshift");
	(void) mdb_readvar(&_mdb_ks_mmu_pageoffset, "_mmu_pageoffset");
	(void) mdb_readvar(&_mdb_ks_mmu_pagemask, "_mmu_pagemask");
	(void) mdb_readvar(&_mdb_ks_kernelbase, "_kernelbase");

	(void) mdb_readvar(&_mdb_ks_userlimit, "_userlimit");
	(void) mdb_readvar(&_mdb_ks_userlimit32, "_userlimit32");
	(void) mdb_readvar(&_mdb_ks_argsbase, "_argsbase");
	(void) mdb_readvar(&_mdb_ks_msg_bsize, "_msg_bsize");
	(void) mdb_readvar(&_mdb_ks_defaultstksz, "_defaultstksz");
	(void) mdb_readvar(&_mdb_ks_ncpu, "_ncpu");
}

const mdb_modinfo_t *
_mdb_init(void)
{
	/*
	 * When used with mdb, mdb_ks is a separate dmod.  With kmdb, however,
	 * mdb_ks is compiled into the debugger module.  kmdb cannot
	 * automatically modunload itself when it exits.  If it restarts after
	 * debugger fault, static variables may not be initialized to zero.
	 * They must be manually reinitialized here.
	 */
	dnlc_hash = NULL;
	qi_head = NULL;

	mdb_callback_add(MDB_CALLBACK_STCHG, update_vars, NULL);

	update_vars(NULL);

	return (&modinfo);
}

void
_mdb_fini(void)
{
	dnlc_free();
	while (qi_head != NULL) {
		mdb_qinfo_t *qip = qi_head;
		qi_head = qip->qi_next;
		mdb_free(qip, sizeof (mdb_qinfo_t));
	}
}

/*
 * Interface between MDB kproc target and mdb_ks.  The kproc target relies
 * on looking up and invoking these functions in mdb_ks so that dependencies
 * on the current kernel implementation are isolated in mdb_ks.
 */

/*
 * Given the address of a proc_t, return the p.p_as pointer; return NULL
 * if we were unable to read a proc structure from the given address.
 */
uintptr_t
mdb_kproc_as(uintptr_t proc_addr)
{
	proc_t p;

	if (mdb_vread(&p, sizeof (p), proc_addr) == sizeof (p))
		return ((uintptr_t)p.p_as);

	return (NULL);
}

/*
 * Given the address of a proc_t, return the p.p_model value; return
 * PR_MODEL_UNKNOWN if we were unable to read a proc structure or if
 * the model value does not match one of the two known values.
 */
uint_t
mdb_kproc_model(uintptr_t proc_addr)
{
	proc_t p;

	if (mdb_vread(&p, sizeof (p), proc_addr) == sizeof (p)) {
		switch (p.p_model) {
		case DATAMODEL_ILP32:
			return (PR_MODEL_ILP32);
		case DATAMODEL_LP64:
			return (PR_MODEL_LP64);
		}
	}

	return (PR_MODEL_UNKNOWN);
}

/*
 * Callback function for walking process's segment list.  For each segment,
 * we fill in an mdb_map_t describing its properties, and then invoke
 * the callback function provided by the kproc target.
 */
static int
asmap_step(uintptr_t addr, const struct seg *seg, asmap_arg_t *asmp)
{
	struct segvn_data svd;
	mdb_map_t map;

	if (seg->s_ops == asmp->asm_segvn_ops && mdb_vread(&svd,
	    sizeof (svd), (uintptr_t)seg->s_data) == sizeof (svd)) {

		if (svd.vp != NULL) {
			if (mdb_vnode2path((uintptr_t)svd.vp, map.map_name,
				    MDB_TGT_MAPSZ) != 0) {
				(void) mdb_snprintf(map.map_name,
				    MDB_TGT_MAPSZ, "[ vnode %p ]", svd.vp);
			}
		} else
			(void) strcpy(map.map_name, "[ anon ]");

	} else {
		(void) mdb_snprintf(map.map_name, MDB_TGT_MAPSZ,
		    "[ seg %p ]", addr);
	}

	map.map_base = (uintptr_t)seg->s_base;
	map.map_size = seg->s_size;
	map.map_flags = 0;

	asmp->asm_callback((const struct mdb_map *)&map, asmp->asm_cbdata);
	return (WALK_NEXT);
}

/*
 * Given a process address space, walk its segment list using the seg walker,
 * convert the segment data to an mdb_map_t, and pass this information
 * back to the kproc target via the given callback function.
 */
int
mdb_kproc_asiter(uintptr_t as,
    void (*func)(const struct mdb_map *, void *), void *p)
{
	asmap_arg_t arg;
	GElf_Sym sym;

	arg.asm_segvn_ops = NULL;
	arg.asm_callback = func;
	arg.asm_cbdata = p;

	if (mdb_lookup_by_name("segvn_ops", &sym) == 0)
		arg.asm_segvn_ops = (struct seg_ops *)(uintptr_t)sym.st_value;

	return (mdb_pwalk("seg", (mdb_walk_cb_t)asmap_step, &arg, as));
}

/*
 * Copy the auxv array from the given process's u-area into the provided
 * buffer.  If the buffer is NULL, only return the size of the auxv array
 * so the caller knows how much space will be required.
 */
int
mdb_kproc_auxv(uintptr_t proc, auxv_t *auxv)
{
	if (auxv != NULL) {
		proc_t p;

		if (mdb_vread(&p, sizeof (p), proc) != sizeof (p))
			return (-1);

		bcopy(p.p_user.u_auxv, auxv,
		    sizeof (auxv_t) * __KERN_NAUXV_IMPL);
	}

	return (__KERN_NAUXV_IMPL);
}

/*
 * Given a process address, return the PID.
 */
pid_t
mdb_kproc_pid(uintptr_t proc_addr)
{
	struct pid pid;
	proc_t p;

	if (mdb_vread(&p, sizeof (p), proc_addr) == sizeof (p) &&
	    mdb_vread(&pid, sizeof (pid), (uintptr_t)p.p_pidp) == sizeof (pid))
		return (pid.pid_id);

	return (-1);
}

/*
 * Interface between the MDB kvm target and mdb_ks.  The kvm target relies
 * on looking up and invoking these functions in mdb_ks so that dependencies
 * on the current kernel implementation are isolated in mdb_ks.
 */

/*
 * Determine whether or not the thread that panicked the given kernel was a
 * kernel thread (panic_thread->t_procp == &p0).
 */
void
mdb_dump_print_content(dumphdr_t *dh, pid_t content)
{
	GElf_Sym sym;
	uintptr_t pt;
	uintptr_t procp;
	int expcont = 0;
	int actcont;

	(void) mdb_readvar(&expcont, "dump_conflags");
	actcont = dh->dump_flags & DF_CONTENT;

	if (actcont == DF_ALL) {
		mdb_printf("dump content: all kernel and user pages\n");
		return;
	} else if (actcont == DF_CURPROC) {
		mdb_printf("dump content: kernel pages and pages from "
		    "PID %d", content);
		return;
	}

	mdb_printf("dump content: kernel pages only\n");
	if (!(expcont & DF_CURPROC))
		return;

	if (mdb_readvar(&pt, "panic_thread") != sizeof (pt) || pt == NULL)
		goto kthreadpanic_err;

	if (mdb_vread(&procp, sizeof (procp), pt + OFFSETOF(kthread_t,
	    t_procp)) == -1 || procp == NULL)
		goto kthreadpanic_err;

	if (mdb_lookup_by_name("p0", &sym) != 0)
		goto kthreadpanic_err;

	if (procp == (uintptr_t)sym.st_value) {
		mdb_printf("  (curproc requested, but a kernel thread "
		    "panicked)\n");
	} else {
		mdb_printf("  (curproc requested, but the process that "
		    "panicked could not be dumped)\n");
	}

	return;

kthreadpanic_err:
	mdb_printf("  (curproc requested, but the process that panicked could "
	    "not be found)\n");
}

/*
 * Determine the process that was saved in a `curproc' dump.  This process will
 * be recorded as the first element in dump_pids[].
 */
int
mdb_dump_find_curproc(void)
{
	uintptr_t pidp;
	pid_t pid = -1;

	if (mdb_readvar(&pidp, "dump_pids") == sizeof (pidp) &&
	    mdb_vread(&pid, sizeof (pid), pidp) == sizeof (pid) &&
	    pid > 0)
		return (pid);
	else
		return (-1);
}


/*
 * Following three funcs extracted from sunddi.c
 */

/*
 * Return core address of root node of devinfo tree
 */
static uintptr_t
mdb_ddi_root_node(void)
{
	uintptr_t	top_devinfo_addr;

	/* return (top_devinfo);   */
	if (mdb_readvar(&top_devinfo_addr, "top_devinfo") == -1) {
		mdb_warn("failed to read top_devinfo");
		return (NULL);
	}
	return (top_devinfo_addr);
}

/*
 * Return the name of the devinfo node pointed at by 'dip_addr' in the buffer
 * pointed at by 'name.'
 *
 * - dip_addr is a pointer to a dev_info struct in core.
 */
static char *
mdb_ddi_deviname(uintptr_t dip_addr, char *name, size_t name_size)
{
	uintptr_t addrname;
	ssize_t	length;
	char *local_namep = name;
	size_t local_name_size = name_size;
	struct dev_info	local_dip;


	if (dip_addr == mdb_ddi_root_node()) {
		if (name_size < 1) {
			mdb_warn("failed to get node name: buf too small\n");
			return (NULL);
		}

		*name = '\0';
		return (name);
	}

	if (name_size < 2) {
		mdb_warn("failed to get node name: buf too small\n");
		return (NULL);
	}

	local_namep = name;
	*local_namep++ = '/';
	*local_namep = '\0';
	local_name_size--;

	if (mdb_vread(&local_dip, sizeof (struct dev_info), dip_addr) == -1) {
		mdb_warn("failed to read devinfo struct");
	}

	length = mdb_readstr(local_namep, local_name_size,
	    (uintptr_t)local_dip.devi_node_name);
	if (length == -1) {
		mdb_warn("failed to read node name");
		return (NULL);
	}
	local_namep += length;
	local_name_size -= length;
	addrname = (uintptr_t)local_dip.devi_addr;

	if (addrname != NULL) {

		if (local_name_size < 2) {
			mdb_warn("not enough room for node address string");
			return (name);
		}
		*local_namep++ = '@';
		*local_namep = '\0';
		local_name_size--;

		length = mdb_readstr(local_namep, local_name_size, addrname);
		if (length == -1) {
			mdb_warn("failed to read name");
			return (NULL);
		}
	}

	return (name);
}

/*
 * Generate the full path under the /devices dir to the device entry.
 *
 * dip is a pointer to a devinfo struct in core (not in local memory).
 */
char *
mdb_ddi_pathname(uintptr_t dip_addr, char *path, size_t pathlen)
{
	struct dev_info local_dip;
	uintptr_t	parent_dip;
	char		*bp;
	size_t		buf_left;


	if (dip_addr == mdb_ddi_root_node()) {
		*path = '\0';
		return (path);
	}


	if (mdb_vread(&local_dip, sizeof (struct dev_info), dip_addr) == -1) {
		mdb_warn("failed to read devinfo struct");
	}

	parent_dip = (uintptr_t)local_dip.devi_parent;
	(void) mdb_ddi_pathname(parent_dip, path, pathlen);

	bp = path + strlen(path);
	buf_left = pathlen - strlen(path);
	(void) mdb_ddi_deviname(dip_addr, bp, buf_left);
	return (path);
}


/*
 * Read in the string value of a refstr, which is appended to the end of
 * the structure.
 */
ssize_t
mdb_read_refstr(uintptr_t refstr_addr, char *str, size_t nbytes)
{
	struct refstr *r = (struct refstr *)refstr_addr;

	return (mdb_readstr(str, nbytes, (uintptr_t)r->rs_string));
}
