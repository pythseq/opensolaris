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

#include <sys/param.h>
#include <sys/kmem.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/disp.h>
#include <sys/vfs.h>
#include <sys/vnode.h>
#include <sys/pathname.h>
#include <sys/cred.h>
#include <sys/mount.h>
#include <sys/cmn_err.h>
#include <sys/debug.h>
#include <sys/systm.h>
#include <rpc/types.h>
#include <rpc/xdr.h>
#include <rpc/auth.h>
#include <rpc/clnt.h>
#include <sys/ticotsord.h>
#include <sys/dirent.h>
#include <fs/fs_subr.h>
#include <rpcsvc/autofs_prot.h>
#include <sys/fs/autofs.h>
#include <sys/callb.h>
#include <sys/sysmacros.h>
#include <sys/zone.h>
#include <sys/fs/mntdata.h>

/*
 * Autofs and Zones:
 *
 * Zones are delegated the responsibility of managing their own autofs mounts
 * and maps.  Each zone runs its own copy of automountd, with its own timeouts,
 * and other logically "global" parameters.  kRPC and virtualization in the
 * loopback transport (tl) will prevent a zone from communicating with another
 * zone's automountd.
 *
 * Each zone has its own "rootfnnode" and associated tree of auto nodes.
 *
 * Each zone also has its own set of "unmounter" kernel threads; these are
 * created and run within the zone's context (ie, they are created via
 * zthread_create()).
 *
 * Cross-zone mount triggers are disallowed.  There is a check in
 * auto_trigger_mount() to this effect; EPERM is returned to indicate that the
 * mount is not owned by the caller.
 *
 * autofssys() enables a caller in the global zone to clean up in-kernel (as
 * well as regular) autofs mounts via the unmount_tree() mechanism.  This is
 * routinely done when all mounts are removed as part of zone shutdown.
 */
#define	TYPICALMAXPATHLEN	64

static kmutex_t autofs_nodeid_lock;

static int auto_perform_link(fnnode_t *, struct linka *, cred_t *);
static int auto_perform_actions(fninfo_t *, fnnode_t *,
    action_list *, cred_t *);
static int auto_getmntpnt(vnode_t *, char *, vnode_t **, cred_t *);
static int auto_lookup_request(fninfo_t *, char *, struct linka *,
    cred_t *, bool_t, bool_t *);
static int auto_mount_request(fninfo_t *, char *, action_list **,
    cred_t *, bool_t);

/*
 * Clears the MF_INPROG flag, and wakes up those threads sleeping on
 * fn_cv_mount if MF_WAITING is set.
 */
void
auto_unblock_others(
	fnnode_t *fnp,
	uint_t operation)		/* either MF_INPROG or MF_LOOKUP */
{
	ASSERT(operation & (MF_INPROG | MF_LOOKUP));
	fnp->fn_flags &= ~operation;
	if (fnp->fn_flags & MF_WAITING) {
		fnp->fn_flags &= ~MF_WAITING;
		cv_broadcast(&fnp->fn_cv_mount);
	}
}

int
auto_wait4mount(fnnode_t *fnp)
{
	int error;
	k_sigset_t smask;

	AUTOFS_DPRINT((4, "auto_wait4mount: fnp=%p\n", (void *)fnp));

	mutex_enter(&fnp->fn_lock);
	while (fnp->fn_flags & (MF_INPROG | MF_LOOKUP)) {
		/*
		 * There is a mount or a lookup in progress.
		 */
		fnp->fn_flags |= MF_WAITING;
		sigintr(&smask, 1);
		if (!cv_wait_sig(&fnp->fn_cv_mount, &fnp->fn_lock)) {
			/*
			 * Decided not to wait for operation to
			 * finish after all.
			 */
			sigunintr(&smask);
			mutex_exit(&fnp->fn_lock);
			return (EINTR);
		}
		sigunintr(&smask);
	}
	error = fnp->fn_error;

	if (error == EINTR) {
		/*
		 * The thread doing the mount got interrupted, we need to
		 * try again, by returning EAGAIN.
		 */
		error = EAGAIN;
	}
	mutex_exit(&fnp->fn_lock);

	AUTOFS_DPRINT((5, "auto_wait4mount: fnp=%p error=%d\n", (void *)fnp,
	    error));
	return (error);
}

int
auto_lookup_aux(fnnode_t *fnp, char *name, cred_t *cred)
{
	struct fninfo *fnip;
	struct linka link;
	bool_t mountreq = FALSE;
	int error = 0;

	fnip = vfstofni(fntovn(fnp)->v_vfsp);
	bzero(&link, sizeof (link));
	error = auto_lookup_request(fnip, name, &link, cred, TRUE, &mountreq);
	if (!error) {
		if (link.link != NULL) {
			/*
			 * This node should be a symlink
			 */
			error = auto_perform_link(fnp, &link, cred);
			kmem_free(link.dir, strlen(link.dir) + 1);
			kmem_free(link.link, strlen(link.link) + 1);
		} else if (mountreq) {
			/*
			 * The automount daemon is requesting a mount,
			 * implying this entry must be a wildcard match and
			 * therefore in need of verification that the entry
			 * exists on the server.
			 */
			mutex_enter(&fnp->fn_lock);
			AUTOFS_BLOCK_OTHERS(fnp, MF_INPROG);
			fnp->fn_error = 0;

			/*
			 * Unblock other lookup requests on this node,
			 * this is needed to let the lookup generated by
			 * the mount call to complete. The caveat is
			 * other lookups on this node can also get by,
			 * i.e., another lookup on this node that occurs
			 * while this lookup is attempting the mount
			 * would return a positive result no matter what.
			 * Therefore two lookups on the this node could
			 * potentially get disparate results.
			 */
			AUTOFS_UNBLOCK_OTHERS(fnp, MF_LOOKUP);
			mutex_exit(&fnp->fn_lock);
			/*
			 * auto_new_mount_thread fires up a new thread which
			 * calls automountd finishing up the work
			 */
			auto_new_mount_thread(fnp, name, cred);

			/*
			 * At this point, we are simply another thread
			 * waiting for the mount to complete
			 */
			error = auto_wait4mount(fnp);
			if (error == AUTOFS_SHUTDOWN)
				error = ENOENT;
		}
	}

	mutex_enter(&fnp->fn_lock);
	fnp->fn_error = error;

	/*
	 * Notify threads waiting for lookup/mount that
	 * it's done.
	 */
	if (mountreq) {
		AUTOFS_UNBLOCK_OTHERS(fnp, MF_INPROG);
	} else {
		AUTOFS_UNBLOCK_OTHERS(fnp, MF_LOOKUP);
	}
	mutex_exit(&fnp->fn_lock);
	return (error);
}

/*
 * Starting point for thread to handle mount requests with automountd.
 * XXX auto_mount_thread() is not suspend-safe within the scope of
 * the present model defined for cpr to suspend the system. Calls
 * made by the auto_mount_thread() that have been identified to be unsafe
 * are (1) RPC client handle setup and client calls to automountd which
 * can block deep down in the RPC library, (2) kmem_alloc() calls with the
 * KM_SLEEP flag which can block if memory is low, and (3) VFS_*(), and
 * lookuppnvp() calls which can result in over the wire calls to servers.
 * The thread should be completely reevaluated to make it suspend-safe in
 * case of future updates to the cpr model.
 */
static void
auto_mount_thread(struct autofs_callargs *argsp)
{
	struct fninfo *fnip;
	fnnode_t *fnp;
	vnode_t *vp;
	char *name;
	size_t namelen;
	cred_t *cred;
	action_list *alp = NULL;
	int error;
	callb_cpr_t cprinfo;
	kmutex_t auto_mount_thread_cpr_lock;

	mutex_init(&auto_mount_thread_cpr_lock, NULL, MUTEX_DEFAULT, NULL);
	CALLB_CPR_INIT(&cprinfo, &auto_mount_thread_cpr_lock, callb_generic_cpr,
		"auto_mount_thread");

	fnp = argsp->fnc_fnp;
	vp = fntovn(fnp);
	fnip = vfstofni(vp->v_vfsp);
	name = argsp->fnc_name;
	cred = argsp->fnc_cred;
	ASSERT(crgetzoneid(argsp->fnc_cred) == fnip->fi_zoneid);

	error = auto_mount_request(fnip, name, &alp, cred, TRUE);
	if (!error)
		error = auto_perform_actions(fnip, fnp, alp, cred);
	mutex_enter(&fnp->fn_lock);
	fnp->fn_error = error;

	/*
	 * Notify threads waiting for mount that
	 * it's done.
	 */
	AUTOFS_UNBLOCK_OTHERS(fnp, MF_INPROG);
	mutex_exit(&fnp->fn_lock);

	VN_RELE(vp);
	crfree(argsp->fnc_cred);
	namelen = strlen(argsp->fnc_name) + 1;
	kmem_free(argsp->fnc_name, namelen);
	kmem_free(argsp, sizeof (*argsp));

	mutex_enter(&auto_mount_thread_cpr_lock);
	CALLB_CPR_EXIT(&cprinfo);
	mutex_destroy(&auto_mount_thread_cpr_lock);
	zthread_exit();
	/* NOTREACHED */
}

static int autofs_thr_success = 0;

/*
 * Creates new thread which calls auto_mount_thread which does
 * the bulk of the work calling automountd, via 'auto_perform_actions'.
 */
void
auto_new_mount_thread(fnnode_t *fnp, char *name, cred_t *cred)
{
	struct autofs_callargs *argsp;

	argsp = kmem_alloc(sizeof (*argsp), KM_SLEEP);
	VN_HOLD(fntovn(fnp));
	argsp->fnc_fnp = fnp;
	argsp->fnc_name = kmem_alloc(strlen(name) + 1, KM_SLEEP);
	(void) strcpy(argsp->fnc_name, name);
	argsp->fnc_origin = curthread;
	crhold(cred);
	argsp->fnc_cred = cred;

	(void) zthread_create(NULL, 0, auto_mount_thread, argsp, 0,
	    minclsyspri);
	autofs_thr_success++;
}

int
auto_calldaemon(
	fninfo_t *fnip,
	rpcproc_t which,
	xdrproc_t xdrargs,
	void *argsp,
	xdrproc_t xdrres,
	void *resp,
	cred_t *cred,
	bool_t hard)				/* retry forever? */
{
	CLIENT *client;
	enum clnt_stat status;
	struct rpc_err rpcerr;
	struct timeval wait;
	bool_t tryagain;
	int error = 0;
	k_sigset_t smask;
	struct autofs_globals *fngp = vntofn(fnip->fi_rootvp)->fn_globals;

	AUTOFS_DPRINT((4, "auto_calldaemon\n"));

	error = clnt_tli_kcreate(&fnip->fi_knconf, &fnip->fi_addr,
	    AUTOFS_PROG, AUTOFS_VERS, 0, INT_MAX, cred, &client);

	if (error) {
		auto_log(fngp, CE_WARN, "autofs: clnt_tli_kcreate: error %d",
		    error);
		goto done;
	}

	/*
	 * Release the old authentication handle.  It was probably
	 * AUTH_UNIX.
	 */
	auth_destroy(client->cl_auth);

	/*
	 * Create a new authentication handle for AUTH_LOOPBACK.  This
	 * will allow us to correctly handle the entire groups list.
	 */
	client->cl_auth = authloopback_create();
	if (client->cl_auth == NULL) {
		clnt_destroy(client);
		error = EINTR;
		auto_log(fngp, CE_WARN,
		    "autofs: authloopback_create: error %d", error);
		goto done;
	}

	wait.tv_sec = fnip->fi_rpc_to;
	wait.tv_usec = 0;
	do {
		tryagain = FALSE;
		error = 0;

		/*
		 * Mask out all signals except SIGHUP, SIGINT, SIGQUIT
		 * and SIGTERM. (Preserving the existing masks)
		 */
		sigintr(&smask, 1);

		status = CLNT_CALL(client, which, xdrargs, argsp,
		    xdrres, resp, wait);

		/*
		 * Restore original signal mask
		 */
		sigunintr(&smask);

		switch (status) {
		case RPC_SUCCESS:
			break;

		case RPC_INTR:
			error = EINTR;
			break;

		case RPC_TIMEDOUT:
			tryagain = TRUE;
			error = ETIMEDOUT;
			break;

		case RPC_CANTCONNECT:
		case RPC_CANTCREATESTREAM:
			/*
			 * The connection could not be established
			 */
			/* fall thru */
		case RPC_XPRTFAILED:
			/*
			 * The connection could not be established or
			 * was dropped, we differentiate between the two
			 * conditions by calling CLNT_GETERR and look at
			 * rpcerror.re_errno.
			 * If rpcerr.re_errno == ECONNREFUSED, then the
			 * connection could not be established at all.
			 */
			error = ECONNREFUSED;
			if (status == RPC_XPRTFAILED) {
				CLNT_GETERR(client, &rpcerr);
				if (rpcerr.re_errno != ECONNREFUSED) {
					/*
					 * The connection was dropped, return
					 * to the caller if hard is not set.
					 * It is the responsability of the
					 * caller to retry the call if
					 * appropriate.
					 */
					error = ECONNRESET;
				}
			}
			/*
			 * We know that the current thread is doing work on
			 * behalf of its own zone, so it's ok to use
			 * curproc->p_zone.
			 */
			ASSERT(fngp->fng_zoneid == getzoneid());
			if (zone_status_get(curproc->p_zone) >=
			    ZONE_IS_SHUTTING_DOWN) {
				/*
				 * There's no point in trying to talk to
				 * automountd.  Plus, zone_shutdown() is
				 * waiting for us.
				 */
				tryagain = FALSE;
				break;
			}
			tryagain = hard;
			if (!fngp->fng_printed_not_running_msg) {
				if (tryagain) {
					fngp->fng_printed_not_running_msg = 1;
					zprintf(fngp->fng_zoneid,
					"automountd not running, retrying\n");
				}
			}
			break;

		default:
			auto_log(fngp, CE_WARN, "autofs: %s",
			    clnt_sperrno(status));
			error = ENOENT;
			break;
		}
	} while (tryagain);

	if (status == RPC_SUCCESS) {
		if (fngp->fng_printed_not_running_msg == 1) {
			fngp->fng_printed_not_running_msg = 0;
			zprintf(fngp->fng_zoneid, "automountd OK\n");
		}
	}
	auth_destroy(client->cl_auth);
	clnt_destroy(client);

done:
	ASSERT(status == RPC_SUCCESS || error != 0);

	AUTOFS_DPRINT((5, "auto_calldaemon error=%d\n", error));
	return (error);
}

static int
auto_null_request(fninfo_t *fnip, cred_t *cred, bool_t hard)
{
	int error;

	AUTOFS_DPRINT((4, "\tauto_null_request\n"));

	error = auto_calldaemon(fnip, NULLPROC, xdr_void, NULL, xdr_void, NULL,
	    cred, hard);

	AUTOFS_DPRINT((5, "\tauto_null_request: error=%d\n", error));
	return (error);
}

static int
auto_lookup_request(
	fninfo_t *fnip,
	char *key,
	struct linka *lnp,
	cred_t *cred,
	bool_t hard,
	bool_t *mountreq)
{
	int error;
	struct autofs_globals *fngp;
	struct autofs_lookupargs request;
	struct autofs_lookupres result;
	struct linka *p;

	AUTOFS_DPRINT((4, "auto_lookup_request: path=%s name=%s\n",
	    fnip->fi_path, key));

	fngp = vntofn(fnip->fi_rootvp)->fn_globals;
	request.map = fnip->fi_map;
	request.path = fnip->fi_path;

	if (fnip->fi_flags & MF_DIRECT)
		request.name = fnip->fi_key;
	else
		request.name = key;
	AUTOFS_DPRINT((4, "auto_lookup_request: using key=%s\n", request.name));

	request.subdir = fnip->fi_subdir;
	request.opts = fnip->fi_opts;
	request.isdirect = fnip->fi_flags & MF_DIRECT ? TRUE : FALSE;

	bzero(&result, sizeof (result));
	error = auto_calldaemon(fnip, AUTOFS_LOOKUP,
	    xdr_autofs_lookupargs, &request,
	    xdr_autofs_lookupres, &result,
	    cred, hard);
	if (!error) {
		fngp->fng_verbose = result.lu_verbose;
		switch (result.lu_res) {
		case AUTOFS_OK:
			switch (result.lu_type.action) {
			case AUTOFS_MOUNT_RQ:
				lnp->link = NULL;
				lnp->dir = NULL;
				*mountreq = TRUE;
				break;
			case AUTOFS_LINK_RQ:
				p =
				&result.lu_type.lookup_result_type_u.lt_linka;
				lnp->dir = kmem_alloc(strlen(p->dir) + 1,
				    KM_SLEEP);
				(void) strcpy(lnp->dir, p->dir);
				lnp->link = kmem_alloc(strlen(p->link) + 1,
				    KM_SLEEP);
				(void) strcpy(lnp->link, p->link);
				break;
			case AUTOFS_NONE:
				lnp->link = NULL;
				lnp->dir = NULL;
				break;
			default:
				auto_log(fngp, CE_WARN,
				    "auto_lookup_request: bad action type %d",
				    result.lu_res);
				error = ENOENT;
			}
			break;
		case AUTOFS_NOENT:
			error = ENOENT;
			break;
		default:
			error = ENOENT;
			auto_log(fngp, CE_WARN,
			    "auto_lookup_request: unknown result: %d",
			    result.lu_res);
			break;
		}
	}

done:
	xdr_free(xdr_autofs_lookupres, (char *)&result);

	AUTOFS_DPRINT((5, "auto_lookup_request: path=%s name=%s error=%d\n",
	    fnip->fi_path, key, error));
	return (error);
}

static int
auto_mount_request(
	fninfo_t *fnip,
	char *key,
	action_list **alpp,
	cred_t *cred,
	bool_t hard)
{
	int error;
	struct autofs_globals *fngp;
	struct autofs_lookupargs request;
	struct autofs_mountres *result;

	AUTOFS_DPRINT((4, "auto_mount_request: path=%s name=%s\n",
	    fnip->fi_path, key));

	fngp = vntofn(fnip->fi_rootvp)->fn_globals;
	request.map = fnip->fi_map;
	request.path = fnip->fi_path;

	if (fnip->fi_flags & MF_DIRECT)
		request.name = fnip->fi_key;
	else
		request.name = key;
	AUTOFS_DPRINT((4, "auto_mount_request: using key=%s\n", request.name));

	request.subdir = fnip->fi_subdir;
	request.opts = fnip->fi_opts;
	request.isdirect = fnip->fi_flags & MF_DIRECT ? TRUE : FALSE;

	*alpp = NULL;
	result = kmem_zalloc(sizeof (*result), KM_SLEEP);
	error = auto_calldaemon(fnip, AUTOFS_MOUNT,
	    xdr_autofs_lookupargs, &request,
	    xdr_autofs_mountres, result,
	    cred, hard);
	if (!error) {
		fngp->fng_verbose = result->mr_verbose;
		switch (result->mr_type.status) {
		case AUTOFS_ACTION:
			error = 0;
			/*
			 * Save the action list since it is used by
			 * the caller. We NULL the action list pointer
			 * in 'result' so that xdr_free() will not free
			 * the list.
			 */
			*alpp = result->mr_type.mount_result_type_u.list;
			result->mr_type.mount_result_type_u.list = NULL;
			break;
		case AUTOFS_DONE:
			error = result->mr_type.mount_result_type_u.error;
			break;
		default:
			error = ENOENT;
			auto_log(fngp, CE_WARN,
			    "auto_mount_request: unknown status %d",
			    result->mr_type.status);
			break;
		}
	}

	xdr_free(xdr_autofs_mountres, (char *)result);
	kmem_free(result, sizeof (*result));

	AUTOFS_DPRINT((5, "auto_mount_request: path=%s name=%s error=%d\n",
	    fnip->fi_path, key, error));
	return (error);
}


static int
auto_send_unmount_request(
	fninfo_t *fnip,
	umntrequest *ul,
	cred_t *cred,
	bool_t hard)
{
	int error;
	umntres result;

	AUTOFS_DPRINT((4, "\tauto_send_unmount_request: fstype=%s "
			" mntpnt=%s\n", ul->fstype, ul->mntpnt));

	error = auto_calldaemon(fnip, AUTOFS_UNMOUNT,
	    xdr_umntrequest, ul,
	    xdr_umntres, &result,
	    cred, hard);
	if (!error)
		error = result.status;

	AUTOFS_DPRINT((5, "\tauto_send_unmount_request: error=%d\n", error));

	return (error);
}

static int
auto_perform_link(fnnode_t *fnp, struct linka *linkp, cred_t *cred)
{
	vnode_t *vp;
	size_t len;
	char *tmp;

	AUTOFS_DPRINT((3, "auto_perform_link: fnp=%p dir=%s link=%s\n",
	    (void *)fnp, linkp->dir, linkp->link));

	len = strlen(linkp->link) + 1;		/* include '\0' */
	tmp = kmem_zalloc(len, KM_SLEEP);
	(void) kcopy(linkp->link, tmp, len);
	mutex_enter(&fnp->fn_lock);
	fnp->fn_symlink = tmp;
	fnp->fn_symlinklen = (uint_t)len;
	fnp->fn_flags |= MF_THISUID_MATCH_RQD;
	crhold(cred);
	fnp->fn_cred = cred;
	mutex_exit(&fnp->fn_lock);

	vp = fntovn(fnp);
	vp->v_type = VLNK;

	return (0);
}

static boolean_t
auto_invalid_action(fninfo_t *dfnip, fnnode_t *dfnp, action_list *p)
{
	struct mounta *m;
	struct autofs_args *argsp;
	vnode_t *dvp;
	char buff[AUTOFS_MAXPATHLEN];
	size_t len;
	struct autofs_globals *fngp;

	fngp = dfnp->fn_globals;
	dvp = fntovn(dfnp);
	/*
	 * Before we go any further, this better be a mount request.
	 */
	if (p->action.action != AUTOFS_MOUNT_RQ)
		return (B_TRUE);
	m = &p->action.action_list_entry_u.mounta;
	/*
	 * Make sure we aren't geting passed NULL values or a "dir" that
	 * isn't "." and doesn't begin with "./".
	 *
	 * We also only want to perform autofs mounts, so make sure
	 * no-one is trying to trick us into doing anything else.
	 */
	if (m->spec == NULL || m->dir == NULL || m->dir[0] != '.' ||
	    (m->dir[1] != '/' && m->dir[1] != '\0') ||
	    m->fstype == NULL || strcmp(m->fstype, "autofs") != 0 ||
	    m->dataptr == NULL || m->datalen != sizeof (struct autofs_args) ||
	    m->optptr == NULL)
		return (B_TRUE);
	/*
	 * We also don't like ".."s in the pathname.  Symlinks are
	 * handled by the fact that we'll use NOFOLLOW when we do
	 * lookup()s.
	 */
	if (strstr(m->dir, "/../") != NULL ||
	    (len = strlen(m->dir)) > sizeof ("/..") - 1 &&
	    m->dir[len] == '.' && m->dir[len - 1] == '.' &&
	    m->dir[len - 2] == '/')
		return (B_TRUE);
	argsp = (struct autofs_args *)m->dataptr;
	/*
	 * We don't want NULL values here either.
	 */
	if (argsp->addr.buf == NULL || argsp->path == NULL ||
	    argsp->opts == NULL || argsp->map == NULL || argsp->subdir == NULL)
		return (B_TRUE);
	/*
	 * We know what the claimed pathname *should* look like:
	 *
	 * If the parent (dfnp) is a mount point (VROOT), then
	 * the path should be (dfnip->fi_path + m->dir).
	 *
	 * Else, we know we're only two levels deep, so we use
	 * (dfnip->fi_path + dfnp->fn_name + m->dir).
	 *
	 * Furthermore, "." only makes sense if dfnp is a
	 * trigger node.
	 *
	 * At this point it seems like the passed-in path is
	 * redundant.
	 */
	if (dvp->v_flag & VROOT) {
		if (m->dir[1] == '\0' && !(dfnp->fn_flags & MF_TRIGGER))
			return (B_TRUE);
		(void) snprintf(buff, sizeof (buff), "%s%s",
		    dfnip->fi_path, m->dir + 1);
	} else {
		(void) snprintf(buff, sizeof (buff), "%s/%s%s",
		    dfnip->fi_path, dfnp->fn_name, m->dir + 1);
	}
	if (strcmp(argsp->path, buff) != 0) {
		auto_log(fngp, CE_WARN, "autofs: expected path of '%s', "
		    "got '%s' instead.", buff, argsp->path);
		return (B_TRUE);
	}
	return (B_FALSE); /* looks OK */
}

static int
auto_perform_actions(
	fninfo_t *dfnip,
	fnnode_t *dfnp,
	action_list *alp,
	cred_t *cred)	/* Credentials of the caller */
{
	action_list *p;
	struct mounta *m, margs;
	struct autofs_args *argsp;
	int error, success = 0;
	vnode_t *mvp, *dvp, *newvp;
	fnnode_t *newfnp, *mfnp;
	int auto_mount = 0;
	int save_triggers = 0;		/* set when we need to save at least */
					/* one trigger node */
	int update_times = 0;
	char *mntpnt;
	char buff[AUTOFS_MAXPATHLEN];
	timestruc_t now;
	struct autofs_globals *fngp;
	cred_t *zcred;	/* kcred-like credentials limited by our zone */

	AUTOFS_DPRINT((4, "auto_perform_actions: alp=%p\n", (void *)alp));

	fngp = dfnp->fn_globals;
	dvp = fntovn(dfnp);

	/*
	 * As automountd running in a zone may be compromised, and this may be
	 * an attack, we can't trust everything passed in by automountd, and we
	 * need to do argument verification.  We'll issue a warning and drop
	 * the request if it doesn't seem right.
	 */
	for (p = alp; p != NULL; p = p->next) {
		if (auto_invalid_action(dfnip, dfnp, p)) {
			/*
			 * This warning should be sent to the global zone,
			 * since presumably the zone administrator is the same
			 * as the attacker.
			 */
			cmn_err(CE_WARN, "autofs: invalid action list received "
			    "by automountd in zone %s.",
			    curproc->p_zone->zone_name);
			/*
			 * This conversation is over.
			 */
			xdr_free(xdr_action_list, (char *)alp);
			return (EINVAL);
		}
	}

	zcred = zone_get_kcred(getzoneid());
	ASSERT(zcred != NULL);

	if (vn_mountedvfs(dvp) != NULL) {
		/*
		 * The daemon successfully mounted a filesystem
		 * on the AUTOFS root node.
		 */
		mutex_enter(&dfnp->fn_lock);
		dfnp->fn_flags |= MF_MOUNTPOINT;
		ASSERT(dfnp->fn_dirents == NULL);
		mutex_exit(&dfnp->fn_lock);
		success++;
	} else {
		/*
		 * Clear MF_MOUNTPOINT.
		 */
		mutex_enter(&dfnp->fn_lock);
		if (dfnp->fn_flags & MF_MOUNTPOINT) {
			AUTOFS_DPRINT((10, "autofs: clearing mountpoint "
			    "flag on %s.", dfnp->fn_name));
			ASSERT(dfnp->fn_dirents == NULL);
			ASSERT(dfnp->fn_trigger == NULL);
		}
		dfnp->fn_flags &= ~MF_MOUNTPOINT;
		mutex_exit(&dfnp->fn_lock);
	}

	for (p = alp; p != NULL; p = p->next) {
		vfs_t *vfsp;	/* dummy argument */
		vfs_t *mvfsp;

		auto_mount = 0;

		m = &p->action.action_list_entry_u.mounta;
		argsp = (struct autofs_args *)m->dataptr;
		/*
		 * use the parent directory's timeout since it's the
		 * one specified/inherited by automount.
		 */
		argsp->mount_to = dfnip->fi_mount_to;
		/*
		 * The mountpoint is relative, and it is guaranteed to
		 * begin with "."
		 *
		 */
		ASSERT(m->dir[0] == '.');
		if (m->dir[0] == '.' && m->dir[1] == '\0') {
			/*
			 * mounting on the trigger node
			 */
			mvp = dvp;
			VN_HOLD(mvp);
			goto mount;
		}
		/*
		 * ignore "./" in front of mountpoint
		 */
		ASSERT(m->dir[1] == '/');
		mntpnt = m->dir + 2;

		AUTOFS_DPRINT((10, "\tdfnip->fi_path=%s\n", dfnip->fi_path));
		AUTOFS_DPRINT((10, "\tdfnip->fi_flags=%x\n", dfnip->fi_flags));
		AUTOFS_DPRINT((10, "\tmntpnt=%s\n", mntpnt));

		if (dfnip->fi_flags & MF_DIRECT) {
			AUTOFS_DPRINT((10, "\tDIRECT\n"));
			(void) sprintf(buff, "%s/%s", dfnip->fi_path, mntpnt);
		} else {
			AUTOFS_DPRINT((10, "\tINDIRECT\n"));
			(void) sprintf(buff, "%s/%s/%s", dfnip->fi_path,
			    dfnp->fn_name, mntpnt);
		}

		if (vn_mountedvfs(dvp) == NULL) {
			/*
			 * Daemon didn't mount anything on the root
			 * We have to create the mountpoint if it doesn't
			 * exist already
			 *
			 * We use the caller's credentials in case a UID-match
			 * is required (MF_THISUID_MATCH_RQD).
			 */
			rw_enter(&dfnp->fn_rwlock, RW_WRITER);
			error = auto_search(dfnp, mntpnt, &mfnp, cred);
			if (error == 0) {
				/*
				 * AUTOFS mountpoint exists
				 */
				if (vn_mountedvfs(fntovn(mfnp)) != NULL) {
					cmn_err(CE_PANIC,
					    "auto_perform_actions: "
					    "mfnp=%p covered", (void *)mfnp);
				}
			} else {
				/*
				 * Create AUTOFS mountpoint
				 */
				ASSERT((dfnp->fn_flags & MF_MOUNTPOINT) == 0);
				error = auto_enter(dfnp, mntpnt, &mfnp, cred);
				ASSERT(mfnp->fn_linkcnt == 1);
				mfnp->fn_linkcnt++;
			}
			if (!error)
				update_times = 1;
			rw_exit(&dfnp->fn_rwlock);
			ASSERT(error != EEXIST);
			if (!error) {
				/*
				 * mfnp is already held.
				 */
				mvp = fntovn(mfnp);
			} else {
				auto_log(fngp, CE_WARN, "autofs: mount of %s "
				    "failed - can't create mountpoint.", buff);
				continue;
			}
		} else {
			/*
			 * Find mountpoint in VFS mounted here. If not found,
			 * fail the submount, though the overall mount has
			 * succeeded since the root is mounted.
			 */
			if (error = auto_getmntpnt(dvp, mntpnt, &mvp, kcred)) {
				auto_log(fngp, CE_WARN, "autofs: mount of %s "
				    "failed - mountpoint doesn't exist.", buff);
				continue;
			}
			if (mvp->v_type == VLNK) {
				auto_log(fngp, CE_WARN, "autofs: %s symbolic "
				    "link: not a valid mountpoint "
				    "- mount failed", buff);
				VN_RELE(mvp);
				error = ENOENT;
				continue;
			}
		}
mount:
		m->flags |= MS_SYSSPACE | MS_OPTIONSTR;
		/*
		 * Copy mounta struct here so we can substitute a buffer
		 * that is large enough to hold the returned option string,
		 * if that string is longer that the input option string.
		 * This can happen if there are default options enabled
		 * that were not in the input option string.
		 */
		bcopy(m, &margs, sizeof (*m));
		margs.optptr = kmem_alloc(MAX_MNTOPT_STR, KM_SLEEP);
		margs.optlen = MAX_MNTOPT_STR;
		(void) strcpy(margs.optptr, m->optptr);
		margs.dir = argsp->path;
		/*
		 * We use the zone's kcred because we don't want the zone to be
		 * able to thus do something it wouldn't normally be able to.
		 */
		error = domount(NULL, &margs, mvp, zcred, &vfsp);
		kmem_free(margs.optptr, MAX_MNTOPT_STR);
		if (error != 0) {
			auto_log(fngp, CE_WARN,
			    "autofs: domount of %s failed error=%d",
			    buff, error);
			VN_RELE(mvp);
			continue;
		}
		VFS_RELE(vfsp);

		/*
		 * If mountpoint is an AUTOFS node, then I'm going to
		 * flag it that the Filesystem mounted on top was mounted
		 * in the kernel so that the unmount can be done inside the
		 * kernel as well.
		 * I don't care to flag non-AUTOFS mountpoints when an AUTOFS
		 * in-kernel mount was done on top, because the unmount
		 * routine already knows that such case was done in the kernel.
		 */
		if (vfs_matchops(dvp->v_vfsp, vfs_getops(mvp->v_vfsp))) {
			mfnp = vntofn(mvp);
			mutex_enter(&mfnp->fn_lock);
			mfnp->fn_flags |= MF_IK_MOUNT;
			mutex_exit(&mfnp->fn_lock);
		}

		(void) vn_vfswlock_wait(mvp);
		mvfsp = vn_mountedvfs(mvp);
		if (mvfsp != NULL) {
			vfs_lock_wait(mvfsp);
			vn_vfsunlock(mvp);
			error = VFS_ROOT(mvfsp, &newvp);
			vfs_unlock(mvfsp);
			if (error) {
				/*
				 * We've dropped the locks, so let's get
				 * the mounted vfs again in case it changed.
				 */
				(void) vn_vfswlock_wait(mvp);
				mvfsp = vn_mountedvfs(mvp);
				if (mvfsp != NULL) {
					error = dounmount(mvfsp, 0, CRED());
					if (error) {
						cmn_err(CE_WARN,
						    "autofs: could not "
						    "unmount vfs=%p",
						(void *)mvfsp);
					}
				} else
					vn_vfsunlock(mvp);
				VN_RELE(mvp);
				continue;
			}
		} else {
			vn_vfsunlock(mvp);
			VN_RELE(mvp);
			continue;
		}

		auto_mount = vfs_matchops(dvp->v_vfsp,
						vfs_getops(newvp->v_vfsp));
		newfnp = vntofn(newvp);
		newfnp->fn_parent = dfnp;

		/*
		 * At this time we want to save the AUTOFS filesystem as
		 * a trigger node. (We only do this if the mount occured
		 * on a node different from the root.
		 * We look at the trigger nodes during
		 * the automatic unmounting to make sure we remove them
		 * as a unit and remount them as a unit if the filesystem
		 * mounted at the root could not be unmounted.
		 */
		if (auto_mount && (error == 0) && (mvp != dvp)) {
			save_triggers++;
			/*
			 * Add AUTOFS mount to hierarchy
			 */
			newfnp->fn_flags |= MF_TRIGGER;
			rw_enter(&newfnp->fn_rwlock, RW_WRITER);
			newfnp->fn_next = dfnp->fn_trigger;
			rw_exit(&newfnp->fn_rwlock);
			rw_enter(&dfnp->fn_rwlock, RW_WRITER);
			dfnp->fn_trigger = newfnp;
			rw_exit(&dfnp->fn_rwlock);
			/*
			 * Don't VN_RELE(newvp) here since dfnp now holds
			 * reference to it as its trigger node.
			 */
			AUTOFS_DPRINT((10, "\tadding trigger %s to %s\n",
			    newfnp->fn_name, dfnp->fn_name));
			AUTOFS_DPRINT((10, "\tfirst trigger is %s\n",
			    dfnp->fn_trigger->fn_name));
			if (newfnp->fn_next != NULL)
				AUTOFS_DPRINT((10, "\tnext trigger is %s\n",
				    newfnp->fn_next->fn_name));
			else
				AUTOFS_DPRINT((10, "\tno next trigger\n"));
		} else
			VN_RELE(newvp);

		if (!error)
			success++;

		if (update_times) {
			gethrestime(&now);
			dfnp->fn_atime = dfnp->fn_mtime = now;
		}

		VN_RELE(mvp);
	}

	if (save_triggers) {
		/*
		 * Make sure the parent can't be freed while it has triggers.
		 */
		VN_HOLD(dvp);
	}

	crfree(zcred);

done:
	/*
	 * Return failure if daemon didn't mount anything, and all
	 * kernel mounts attempted failed.
	 */
	error = success ? 0 : ENOENT;

	if (alp != NULL) {
		if ((error == 0) && save_triggers) {
			/*
			 * Save action_list information, so that we can use it
			 * when it comes time to remount the trigger nodes
			 * The action list is freed when the directory node
			 * containing the reference to it is unmounted in
			 * unmount_tree().
			 */
			mutex_enter(&dfnp->fn_lock);
			ASSERT(dfnp->fn_alp == NULL);
			dfnp->fn_alp = alp;
			mutex_exit(&dfnp->fn_lock);
		} else {
			/*
			 * free the action list now,
			 */
			xdr_free(xdr_action_list, (char *)alp);
		}
	}

	AUTOFS_DPRINT((5, "auto_perform_actions: error=%d\n", error));
	return (error);
}

fnnode_t *
auto_makefnnode(
	vtype_t type,
	vfs_t *vfsp,
	char *name,
	cred_t *cred,
	struct autofs_globals *fngp)
{
	fnnode_t *fnp;
	vnode_t *vp;
	char *tmpname;
	timestruc_t now;
	/*
	 * autofs uses odd inode numbers
	 * automountd uses even inode numbers
	 *
	 * To preserve the age-old semantics that inum+devid is unique across
	 * the system, this variable must be global across zones.
	 */
	static ino_t nodeid = 3;

	fnp = kmem_zalloc(sizeof (*fnp), KM_SLEEP);
	fnp->fn_vnode = vn_alloc(KM_SLEEP);

	vp = fntovn(fnp);
	tmpname = kmem_alloc(strlen(name) + 1, KM_SLEEP);
	(void) strcpy(tmpname, name);
	fnp->fn_name = &tmpname[0];
	fnp->fn_namelen = (int)strlen(tmpname) + 1;	/* include '\0' */
	fnp->fn_uid = crgetuid(cred);
	fnp->fn_gid = crgetgid(cred);
	/*
	 * ".." is added in auto_enter and auto_mount.
	 * "." is added in auto_mkdir and auto_mount.
	 */
	/*
	 * Note that fn_size and fn_linkcnt are already 0 since
	 * we used kmem_zalloc to allocated fnp
	 */
	fnp->fn_mode = AUTOFS_MODE;
	gethrestime(&now);
	fnp->fn_atime = fnp->fn_mtime = fnp->fn_ctime = now;
	fnp->fn_ref_time = now.tv_sec;
	mutex_enter(&autofs_nodeid_lock);
	fnp->fn_nodeid = nodeid;
	nodeid += 2;
	fnp->fn_globals = fngp;
	fngp->fng_fnnode_count++;
	mutex_exit(&autofs_nodeid_lock);
	vn_setops(vp, auto_vnodeops);
	vp->v_type = type;
	vp->v_data = (void *)fnp;
	vp->v_vfsp = vfsp;
	mutex_init(&fnp->fn_lock, NULL, MUTEX_DEFAULT, NULL);
	rw_init(&fnp->fn_rwlock, NULL, RW_DEFAULT, NULL);
	cv_init(&fnp->fn_cv_mount, NULL, CV_DEFAULT, NULL);
	vn_exists(vp);
	return (fnp);
}


void
auto_freefnnode(fnnode_t *fnp)
{
	vnode_t *vp = fntovn(fnp);

	AUTOFS_DPRINT((4, "auto_freefnnode: fnp=%p\n", (void *)fnp));

	ASSERT(fnp->fn_linkcnt == 0);
	ASSERT(vp->v_count == 0);
	ASSERT(fnp->fn_dirents == NULL);
	ASSERT(fnp->fn_parent == NULL);

	vn_invalid(vp);
	kmem_free(fnp->fn_name, fnp->fn_namelen);
	if (fnp->fn_symlink) {
		ASSERT(fnp->fn_flags & MF_THISUID_MATCH_RQD);
		kmem_free(fnp->fn_symlink, fnp->fn_symlinklen);
	}
	if (fnp->fn_cred)
		crfree(fnp->fn_cred);
	mutex_destroy(&fnp->fn_lock);
	rw_destroy(&fnp->fn_rwlock);
	cv_destroy(&fnp->fn_cv_mount);
	vn_free(vp);

	mutex_enter(&autofs_nodeid_lock);
	fnp->fn_globals->fng_fnnode_count--;
	mutex_exit(&autofs_nodeid_lock);
	kmem_free(fnp, sizeof (*fnp));
}

void
auto_disconnect(
	fnnode_t *dfnp,
	fnnode_t *fnp)
{
	fnnode_t *tmp, **fnpp;
	vnode_t *vp = fntovn(fnp);
	timestruc_t now;

	AUTOFS_DPRINT((4,
	    "auto_disconnect: dfnp=%p fnp=%p linkcnt=%d\n v_count=%d",
	    (void *)dfnp, (void *)fnp, fnp->fn_linkcnt, vp->v_count));

	ASSERT(RW_WRITE_HELD(&dfnp->fn_rwlock));
	ASSERT(fnp->fn_linkcnt == 1);

	if (vn_mountedvfs(vp) != NULL) {
		cmn_err(CE_PANIC, "auto_disconnect: vp %p mounted on",
		    (void *)vp);
	}

	/*
	 * Decrement by 1 because we're removing the entry in dfnp.
	 */
	fnp->fn_linkcnt--;
	fnp->fn_size--;

	/*
	 * only changed while holding parent's (dfnp) rw_lock
	 */
	fnp->fn_parent = NULL;

	fnpp = &dfnp->fn_dirents;
	for (;;) {
		tmp = *fnpp;
		if (tmp == NULL) {
			cmn_err(CE_PANIC,
			    "auto_disconnect: %p not in %p dirent list",
			    (void *)fnp, (void *)dfnp);
		}
		if (tmp == fnp) {
			*fnpp = tmp->fn_next; 	/* remove it from the list */
			ASSERT(vp->v_count == 0);
			/* child had a pointer to parent ".." */
			dfnp->fn_linkcnt--;
			dfnp->fn_size--;
			break;
		}
		fnpp = &tmp->fn_next;
	}

	mutex_enter(&fnp->fn_lock);
	gethrestime(&now);
	fnp->fn_atime = fnp->fn_mtime = now;
	mutex_exit(&fnp->fn_lock);

	AUTOFS_DPRINT((5, "auto_disconnect: done\n"));
}

int
auto_enter(fnnode_t *dfnp, char *name, fnnode_t **fnpp, cred_t *cred)
{
	struct fnnode *cfnp, **spp;
	vnode_t *dvp = fntovn(dfnp);
	ushort_t offset = 0;
	ushort_t diff;

	AUTOFS_DPRINT((4, "auto_enter: dfnp=%p, name=%s ", (void *)dfnp, name));

	ASSERT(RW_WRITE_HELD(&dfnp->fn_rwlock));

	cfnp = dfnp->fn_dirents;
	if (cfnp == NULL) {
		/*
		 * offset = 0 for '.' and offset = 1 for '..'
		 */
		spp = &dfnp->fn_dirents;
		offset = 2;
	}

	for (; cfnp; cfnp = cfnp->fn_next) {
		if (strcmp(cfnp->fn_name, name) == 0) {
			mutex_enter(&cfnp->fn_lock);
			if (cfnp->fn_flags & MF_THISUID_MATCH_RQD) {
				/*
				 * "thisuser" kind of node, need to
				 * match CREDs as well
				 */
				mutex_exit(&cfnp->fn_lock);
				if (crcmp(cfnp->fn_cred, cred) == 0)
					return (EEXIST);
			} else {
				mutex_exit(&cfnp->fn_lock);
				return (EEXIST);
			}
		}

		if (cfnp->fn_next != NULL) {
			diff = (ushort_t)
			    (cfnp->fn_next->fn_offset - cfnp->fn_offset);
			ASSERT(diff != 0);
			if (diff > 1 && offset == 0) {
				offset = (ushort_t)cfnp->fn_offset + 1;
				spp = &cfnp->fn_next;
			}
		} else if (offset == 0) {
			offset = (ushort_t)cfnp->fn_offset + 1;
			spp = &cfnp->fn_next;
		}
	}

	*fnpp = auto_makefnnode(VDIR, dvp->v_vfsp, name, cred,
	    dfnp->fn_globals);
	if (*fnpp == NULL)
		return (ENOMEM);

	/*
	 * I don't hold the mutex on fnpp because I created it, and
	 * I'm already holding the writers lock for it's parent
	 * directory, therefore nobody can reference it without me first
	 * releasing the writers lock.
	 */
	(*fnpp)->fn_offset = offset;
	(*fnpp)->fn_next = *spp;
	*spp = *fnpp;
	(*fnpp)->fn_parent = dfnp;
	(*fnpp)->fn_linkcnt++;	/* parent now holds reference to entry */
	(*fnpp)->fn_size++;

	/*
	 * dfnp->fn_linkcnt and dfnp->fn_size protected by dfnp->rw_lock
	 */
	dfnp->fn_linkcnt++;	/* child now holds reference to parent '..' */
	dfnp->fn_size++;

	dfnp->fn_ref_time = gethrestime_sec();

	AUTOFS_DPRINT((5, "*fnpp=%p\n", (void *)*fnpp));
	return (0);
}

int
auto_search(fnnode_t *dfnp, char *name, fnnode_t **fnpp, cred_t *cred)
{
	vnode_t *dvp;
	fnnode_t *p;
	int error = ENOENT, match = 0;

	AUTOFS_DPRINT((4, "auto_search: dfnp=%p, name=%s...\n",
	    (void *)dfnp, name));

	dvp = fntovn(dfnp);
	if (dvp->v_type != VDIR) {
		cmn_err(CE_PANIC, "auto_search: dvp=%p not a directory",
		    (void *)dvp);
	}

	ASSERT(RW_LOCK_HELD(&dfnp->fn_rwlock));
	for (p = dfnp->fn_dirents; p != NULL; p = p->fn_next) {
		if (strcmp(p->fn_name, name) == 0) {
			mutex_enter(&p->fn_lock);
			if (p->fn_flags & MF_THISUID_MATCH_RQD) {
				/*
				 * "thisuser" kind of node
				 * Need to match CREDs as well
				 */
				mutex_exit(&p->fn_lock);
				match = crcmp(p->fn_cred, cred) == 0;
			} else {
				/*
				 * No need to check CRED
				 */
				mutex_exit(&p->fn_lock);
				match = 1;
			}
		}
		if (match) {
			error = 0;
			if (fnpp) {
				*fnpp = p;
				VN_HOLD(fntovn(*fnpp));
			}
			break;
		}
	}

	AUTOFS_DPRINT((5, "auto_search: error=%d\n", error));
	return (error);
}

/*
 * If dvp is mounted on, get path's vnode in the mounted on
 * filesystem.  Path is relative to dvp, ie "./path".
 * If successful, *mvp points to a the held mountpoint vnode.
 */
/* ARGSUSED */
static int
auto_getmntpnt(
	vnode_t *dvp,
	char *path,
	vnode_t **mvpp,		/* vnode for mountpoint */
	cred_t *cred)
{
	int error = 0;
	vnode_t *newvp;
	char namebuf[TYPICALMAXPATHLEN];
	struct pathname lookpn;
	vfs_t *vfsp;

	AUTOFS_DPRINT((4, "auto_getmntpnt: path=%s\n", path));

	if (error = vn_vfswlock_wait(dvp))
		return (error);

	/*
	 * Now that we have the vfswlock, check to see if dvp
	 * is still mounted on.  If not, then just bail out as
	 * there is no need to remount the triggers since the
	 * higher level mount point has gotten unmounted.
	 */
	vfsp = vn_mountedvfs(dvp);
	if (vfsp == NULL) {
		vn_vfsunlock(dvp);
		error = EBUSY;
		goto done;
	}
	/*
	 * Since mounted on, lookup "path" in the new filesystem,
	 * it is important that we do the filesystem jump here to
	 * avoid lookuppn() calling auto_lookup on dvp and deadlock.
	 */
	vfs_lock_wait(vfsp);
	vn_vfsunlock(dvp);
	error = VFS_ROOT(vfsp, &newvp);
	vfs_unlock(vfsp);
	if (error)
		goto done;

	/*
	 * We do a VN_HOLD on newvp just in case the first call to
	 * lookuppnvp() fails with ENAMETOOLONG.  We should still have a
	 * reference to this vnode for the second call to lookuppnvp().
	 */
	VN_HOLD(newvp);

	/*
	 * Now create the pathname struct so we can make use of lookuppnvp,
	 * and pn_getcomponent.
	 * This code is similar to lookupname() in fs/lookup.c.
	 */
	error = pn_get_buf(path, UIO_SYSSPACE, &lookpn,
		namebuf, sizeof (namebuf));
	if (error == 0) {
		error = lookuppnvp(&lookpn, NULL, NO_FOLLOW, NULLVPP,
		    mvpp, rootdir, newvp, cred);
	} else
		VN_RELE(newvp);
	if (error == ENAMETOOLONG) {
		/*
		 * This thread used a pathname > TYPICALMAXPATHLEN bytes long.
		 * newvp is VN_RELE'd by this call to lookuppnvp.
		 *
		 * Using 'rootdir' in a zone's context is OK here: we already
		 * ascertained that there are no '..'s in the path, and we're
		 * not following symlinks.
		 */
		if ((error = pn_get(path, UIO_SYSSPACE, &lookpn)) == 0) {
			error = lookuppnvp(&lookpn, NULL, NO_FOLLOW, NULLVPP,
			    mvpp, rootdir, newvp, cred);
			pn_free(&lookpn);
		} else
			VN_RELE(newvp);
	} else {
		/*
		 * Need to release newvp here since we held it.
		 */
		VN_RELE(newvp);
	}

done:
	AUTOFS_DPRINT((5, "auto_getmntpnt: path=%s *mvpp=%p error=%d\n",
	    path, (void *)*mvpp, error));
	return (error);
}

#define	DEEPER(x) (((x)->fn_dirents != NULL) || \
			(vn_mountedvfs(fntovn((x)))) != NULL)

/*
 * The caller, should have already VN_RELE'd its reference to the
 * root vnode of this filesystem.
 */
static int
auto_inkernel_unmount(vfs_t *vfsp)
{
	vnode_t *cvp = vfsp->vfs_vnodecovered;
	int error;

	AUTOFS_DPRINT((4,
	    "auto_inkernel_unmount: devid=%lx mntpnt(%p) count %u\n",
	    vfsp->vfs_dev, (void *)cvp, cvp->v_count));

	ASSERT(vn_vfswlock_held(cvp));

	/*
	 * Perform the unmount
	 * The mountpoint has already been locked by the caller.
	 */
	error = dounmount(vfsp, 0, kcred);

	AUTOFS_DPRINT((5, "auto_inkernel_unmount: exit count %u\n",
	    cvp->v_count));
	return (error);
}

/*
 * unmounts trigger nodes in the kernel.
 */
static void
unmount_triggers(fnnode_t *fnp, action_list **alp)
{
	fnnode_t *tp, *next;
	int error = 0;
	vfs_t *vfsp;
	vnode_t *tvp;

	AUTOFS_DPRINT((4, "unmount_triggers: fnp=%p\n", (void *)fnp));
	ASSERT(RW_WRITE_HELD(&fnp->fn_rwlock));

	*alp = fnp->fn_alp;
	next = fnp->fn_trigger;
	while ((tp = next) != NULL) {
		tvp = fntovn(tp);
		ASSERT(tvp->v_count >= 2);
		next = tp->fn_next;
		/*
		 * drop writer's lock since the unmount will end up
		 * disconnecting this node from fnp and needs to acquire
		 * the writer's lock again.
		 * next has at least a reference count >= 2 since it's
		 * a trigger node, therefore can not be accidentally freed
		 * by a VN_RELE
		 */
		rw_exit(&fnp->fn_rwlock);

		vfsp = tvp->v_vfsp;

		/*
		 * Its parent was holding a reference to it, since this
		 * is a trigger vnode.
		 */
		VN_RELE(tvp);
		if (error = auto_inkernel_unmount(vfsp)) {
			cmn_err(CE_PANIC, "unmount_triggers: "
			    "unmount of vp=%p failed error=%d",
			    (void *)tvp, error);
		}
		/*
		 * reacquire writer's lock
		 */
		rw_enter(&fnp->fn_rwlock, RW_WRITER);
	}

	/*
	 * We were holding a reference to our parent.  Drop that.
	 */
	VN_RELE(fntovn(fnp));
	fnp->fn_trigger = NULL;
	fnp->fn_alp = NULL;

	AUTOFS_DPRINT((5, "unmount_triggers: finished\n"));
}

/*
 * This routine locks the mountpoint of every trigger node if they're
 * not busy, or returns EBUSY if any node is busy. If a trigger node should
 * be unmounted first, then it sets nfnp to point to it, otherwise nfnp
 * points to NULL.
 */
static int
triggers_busy(fnnode_t *fnp, fnnode_t **nfnp)
{
	int error = 0, done;
	int lck_error = 0;
	fnnode_t *tp, *t1p;
	vfs_t *vfsp;

	ASSERT(RW_WRITE_HELD(&fnp->fn_rwlock));

	*nfnp = NULL;
	for (tp = fnp->fn_trigger; tp != NULL; tp = tp->fn_next) {
		AUTOFS_DPRINT((10, "\ttrigger: %s\n", tp->fn_name));
		vfsp = fntovn(tp)->v_vfsp;
		error = 0;
		/*
		 * The vn_vfsunlock will be done in auto_inkernel_unmount.
		 */
		lck_error = vn_vfswlock(vfsp->vfs_vnodecovered);
		if (lck_error == 0) {
			mutex_enter(&tp->fn_lock);
			ASSERT((tp->fn_flags & MF_LOOKUP) == 0);
			if (tp->fn_flags & MF_INPROG) {
				/*
				 * a mount is in progress
				 */
				error = EBUSY;
			}
			mutex_exit(&tp->fn_lock);
		}
		if (lck_error || error || DEEPER(tp) ||
		    ((fntovn(tp))->v_count) > 2) {
			/*
			 * couldn't lock it because it's busy,
			 * It is mounted on or has dirents?
			 * If reference count is greater than two, then
			 * somebody else is holding a reference to this vnode.
			 * One reference is for the mountpoint, and the second
			 * is for the trigger node.
			 */
			AUTOFS_DPRINT((10, "\ttrigger busy\n"));
			if ((lck_error == 0) && (error == 0)) {
				*nfnp = tp;
				/*
				 * The matching VN_RELE is done in
				 * unmount_tree().
				 */
				VN_HOLD(fntovn(*nfnp));
			}
			/*
			 * Unlock previously locked mountpoints
			 */
			for (done = 0, t1p = fnp->fn_trigger; !done;
			    t1p = t1p->fn_next) {
				/*
				 * Unlock all nodes previously
				 * locked. All nodes up to 'tp'
				 * were successfully locked. If 'lck_err' is
				 * set, then 'tp' was not locked, and thus
				 * should not be unlocked. If
				 * 'lck_err' is not set, then 'tp' was
				 * successfully locked, and it should
				 * be unlocked.
				 */
				if (t1p != tp || !lck_error) {
					vfsp = fntovn(t1p)->v_vfsp;
					vn_vfsunlock(vfsp->vfs_vnodecovered);
				}
				done = (t1p == tp);
			}
			error = EBUSY;
			break;
		}
	}

	AUTOFS_DPRINT((4, "triggers_busy: error=%d\n", error));
	return (error);
}

/*
 * Unlock previously locked trigger nodes.
 */
static int
triggers_unlock(fnnode_t *fnp)
{
	fnnode_t *tp;
	vfs_t *vfsp;

	ASSERT(RW_WRITE_HELD(&fnp->fn_rwlock));

	for (tp = fnp->fn_trigger; tp != NULL; tp = tp->fn_next) {
		AUTOFS_DPRINT((10, "\tunlock trigger: %s\n", tp->fn_name));
		vfsp = fntovn(tp)->v_vfsp;
		vn_vfsunlock(vfsp->vfs_vnodecovered);
	}

	return (0);
}

/*
 * It is the caller's responsibility to grab the VVFSLOCK.
 * Releases the VVFSLOCK upon return.
 */
static int
unmount_node(vnode_t *cvp, int force)
{
	int error = 0;
	fnnode_t *cfnp;
	vfs_t *vfsp;
	umntrequest ul;
	fninfo_t *fnip;

	AUTOFS_DPRINT((4, "\tunmount_node cvp=%p\n", (void *)cvp));

	ASSERT(vn_vfswlock_held(cvp));
	cfnp = vntofn(cvp);
	vfsp = vn_mountedvfs(cvp);

	if (force || cfnp->fn_flags & MF_IK_MOUNT) {
		/*
		 * Mount was performed in the kernel, so
		 * do an in-kernel unmount. auto_inkernel_unmount()
		 * will vn_vfsunlock(cvp).
		 */
		error = auto_inkernel_unmount(vfsp);
	} else {
		zone_t *zone = NULL;
		refstr_t *mntpt, *resource;
		size_t mntoptslen;

		/*
		 * Get the mnttab information of the node
		 * and ask the daemon to unmount it.
		 */
		bzero(&ul, sizeof (ul));
		mntfs_getmntopts(vfsp, &ul.mntopts, &mntoptslen);
		if (ul.mntopts == NULL) {
			auto_log(cfnp->fn_globals, CE_WARN, "unmount_node: "
			    "no memory");
			vn_vfsunlock(cvp);
			error = ENOMEM;
			goto done;
		}
		if (mntoptslen > AUTOFS_MAXOPTSLEN)
			ul.mntopts[AUTOFS_MAXOPTSLEN - 1] = '\0';

		mntpt = vfs_getmntpoint(vfsp);
		ul.mntpnt = (char *)refstr_value(mntpt);
		resource = vfs_getresource(vfsp);
		ul.mntresource = (char *)refstr_value(resource);

		fnip = vfstofni(cvp->v_vfsp);
		ul.isdirect = fnip->fi_flags & MF_DIRECT ? TRUE : FALSE;

		/*
		 * Since a zone'd automountd's view of the autofs mount points
		 * differs from those in the kernel, we need to make sure we
		 * give it consistent mount points.
		 */
		ASSERT(fnip->fi_zoneid == getzoneid());
		zone = curproc->p_zone;

		if (fnip->fi_zoneid != GLOBAL_ZONEID) {
			if (ZONE_PATH_VISIBLE(ul.mntpnt, zone)) {
				ul.mntpnt =
				    ZONE_PATH_TRANSLATE(ul.mntpnt, zone);
			}
			if (ZONE_PATH_VISIBLE(ul.mntresource, zone)) {
				ul.mntresource =
				    ZONE_PATH_TRANSLATE(ul.mntresource, zone);
			}
		}
		ul.fstype = vfssw[vfsp->vfs_fstype].vsw_name;
		vn_vfsunlock(cvp);

		error = auto_send_unmount_request(fnip, &ul, CRED(), FALSE);
		kmem_free(ul.mntopts, mntoptslen);
		refstr_rele(mntpt);
		refstr_rele(resource);
	}

done:
	AUTOFS_DPRINT((5, "\tunmount_node cvp=%p error=%d\n", (void *)cvp,
	    error));
	return (error);
}

/*
 * vp is the "root" of the AUTOFS filesystem.
 * return EBUSY if any thread is holding a reference to this vnode
 * other than us.
 */
static int
check_auto_node(vnode_t *vp)
{
	fnnode_t *fnp;
	int error = 0;
	/*
	 * number of references to expect for
	 * a non-busy vnode.
	 */
	uint_t count;

	AUTOFS_DPRINT((4, "\tcheck_auto_node vp=%p ", (void *)vp));
	fnp = vntofn(vp);
	ASSERT(fnp->fn_flags & MF_INPROG);
	ASSERT((fnp->fn_flags & MF_LOOKUP) == 0);

	count = 1;		/* we are holding a reference to vp */
	if (fnp->fn_flags & MF_TRIGGER) {
		/*
		 * parent holds a pointer to us (trigger)
		 */
		count++;
	}
	if (fnp->fn_trigger != NULL) {
		/*
		 * The trigger nodes have a hold on us.
		 */
		count++;
	}
	mutex_enter(&vp->v_lock);
	if (vp->v_flag & VROOT)
		count++;
	ASSERT(vp->v_count > 0);
	AUTOFS_DPRINT((10, "\tcount=%u ", vp->v_count));
	if (vp->v_count > count)
		error = EBUSY;
	mutex_exit(&vp->v_lock);

	AUTOFS_DPRINT((5, "\tcheck_auto_node error=%d ", error));
	return (error);
}

/*
 * rootvp is the root of the AUTOFS filesystem.
 * If rootvp is busy (v_count > 1) returns EBUSY.
 * else removes every vnode under this tree.
 * ASSUMPTION: Assumes that the only node which can be busy is
 * the root vnode. This filesystem better be two levels deep only,
 * the root and its immediate subdirs.
 * The daemon will "AUTOFS direct-mount" only one level below the root.
 */
static int
unmount_autofs(vnode_t *rootvp)
{
	fnnode_t *fnp, *rootfnp, *nfnp;
	int error;

	AUTOFS_DPRINT((4, "\tunmount_autofs rootvp=%p ", (void *)rootvp));

	error = check_auto_node(rootvp);
	if (error == 0) {
		/*
		 * Remove all its immediate subdirectories.
		 */
		rootfnp = vntofn(rootvp);
		rw_enter(&rootfnp->fn_rwlock, RW_WRITER);
		nfnp = NULL;	/* lint clean */
		for (fnp = rootfnp->fn_dirents; fnp != NULL; fnp = nfnp) {
			ASSERT(fntovn(fnp)->v_count == 0);
			ASSERT(fnp->fn_dirents == NULL);
			ASSERT(fnp->fn_linkcnt == 2);
			fnp->fn_linkcnt--;
			auto_disconnect(rootfnp, fnp);
			nfnp = fnp->fn_next;
			auto_freefnnode(fnp);
		}
		rw_exit(&rootfnp->fn_rwlock);
	}
	AUTOFS_DPRINT((5, "\tunmount_autofs error=%d ", error));
	return (error);
}

/*
 * max number of unmount threads running
 */
static int autofs_unmount_threads = 5;

/*
 * XXX unmount_tree() is not suspend-safe within the scope of
 * the present model defined for cpr to suspend the system. Calls made
 * by the unmount_tree() that have been identified to be unsafe are
 * (1) RPC client handle setup and client calls to automountd which can
 * block deep down in the RPC library, (2) kmem_alloc() calls with the
 * KM_SLEEP flag which can block if memory is low, and (3) VFS_*() and
 * VOP_*() calls which can result in over the wire calls to servers.
 * The thread should be completely reevaluated to make it suspend-safe in
 * case of future updates to the cpr model.
 */
void
unmount_tree(struct autofs_globals *fngp, int force)
{
	vnode_t *vp, *newvp;
	vfs_t *vfsp;
	fnnode_t *fnp, *nfnp, *pfnp;
	action_list *alp;
	int error, ilocked_it = 0;
	fninfo_t *fnip;
	time_t ref_time;
	int autofs_busy_root, unmount_as_unit, unmount_done = 0;
	timestruc_t now;

	callb_cpr_t cprinfo;
	kmutex_t unmount_tree_cpr_lock;

	mutex_init(&unmount_tree_cpr_lock, NULL, MUTEX_DEFAULT, NULL);
	CALLB_CPR_INIT(&cprinfo, &unmount_tree_cpr_lock, callb_generic_cpr,
		"unmount_tree");

	/*
	 * Got to release lock before attempting unmount in case
	 * it hangs.
	 */
	rw_enter(&fngp->fng_rootfnnodep->fn_rwlock, RW_READER);
	if ((fnp = fngp->fng_rootfnnodep->fn_dirents) == NULL) {
		ASSERT(fngp->fng_fnnode_count == 1);
		/*
		 * no autofs mounted, done.
		 */
		rw_exit(&fngp->fng_rootfnnodep->fn_rwlock);
		goto done;
	}
	VN_HOLD(fntovn(fnp));
	rw_exit(&fngp->fng_rootfnnodep->fn_rwlock);

	vp = fntovn(fnp);
	fnip = vfstofni(vp->v_vfsp);
	/*
	 * autofssys() will be calling in from the global zone and doing
	 * work on the behalf of the given zone, hence we can't always assert
	 * that we have the right credentials, nor that the caller is always in
	 * the correct zone.
	 *
	 * We do, however, know that if this is a "forced unmount" operation
	 * (which autofssys() does), then we won't go down to the krpc layers,
	 * so we don't need to fudge with the credentials.
	 */
	ASSERT(force || fnip->fi_zoneid == getzoneid());
	if (!force && auto_null_request(fnip, kcred, FALSE) != 0) {
		/*
		 * automountd not running in this zone,
		 * don't attempt unmounting this round.
		 */
		VN_RELE(vp);
		goto done;
	}
	/* reference time for this unmount round */
	ref_time = gethrestime_sec();
	/*
	 * If this an autofssys() call, we need to make sure we don't skip
	 * nodes because we think we saw them recently.
	 */
	mutex_enter(&fnp->fn_lock);
	if (force && fnp->fn_unmount_ref_time >= ref_time)
		ref_time = fnp->fn_unmount_ref_time + 1;
	mutex_exit(&fnp->fn_lock);

	AUTOFS_DPRINT((4, "unmount_tree (ID=%ld)\n", ref_time));
top:
	AUTOFS_DPRINT((10, "unmount_tree: %s\n", fnp->fn_name));
	ASSERT(fnp);
	vp = fntovn(fnp);
	if (vp->v_type == VLNK) {
		/*
		 * can't unmount symbolic links
		 */
		goto next;
	}
	fnip = vfstofni(vp->v_vfsp);
	ASSERT(vp->v_count > 0);
	error = 0;
	autofs_busy_root = unmount_as_unit = 0;
	alp = NULL;

	ilocked_it = 0;
	mutex_enter(&fnp->fn_lock);
	if (fnp->fn_flags & (MF_INPROG | MF_LOOKUP)) {
		/*
		 * Either a mount, lookup or another unmount of this
		 * subtree is in progress, don't attempt to unmount at
		 * this time.
		 */
		mutex_exit(&fnp->fn_lock);
		error = EBUSY;
		goto next;
	}
	if (fnp->fn_unmount_ref_time >= ref_time) {
		/*
		 * Already been here, try next node.
		 */
		mutex_exit(&fnp->fn_lock);
		error = EBUSY;
		goto next;
	}
	fnp->fn_unmount_ref_time = ref_time;

	/*
	 * If forced operation ignore timeout values
	 */
	if (!force && fnp->fn_ref_time + fnip->fi_mount_to >
	    gethrestime_sec()) {
		/*
		 * Node has been referenced recently, try the
		 * unmount of its children if any.
		 */
		mutex_exit(&fnp->fn_lock);
		AUTOFS_DPRINT((10, "fn_ref_time within range\n"));
		rw_enter(&fnp->fn_rwlock, RW_READER);
		if (fnp->fn_dirents) {
			/*
			 * Has subdirectory, attempt their
			 * unmount first
			 */
			nfnp = fnp->fn_dirents;
			VN_HOLD(fntovn(nfnp));
			rw_exit(&fnp->fn_rwlock);

			VN_RELE(vp);
			fnp = nfnp;
			goto top;
		}
		rw_exit(&fnp->fn_rwlock);
		/*
		 * No children, try next node.
		 */
		error = EBUSY;
		goto next;
	}

	AUTOFS_BLOCK_OTHERS(fnp, MF_INPROG);
	fnp->fn_error = 0;
	mutex_exit(&fnp->fn_lock);
	ilocked_it = 1;

	rw_enter(&fnp->fn_rwlock, RW_WRITER);
	if (fnp->fn_trigger != NULL) {
		unmount_as_unit = 1;
		if ((vn_mountedvfs(vp) == NULL) && (check_auto_node(vp))) {
			/*
			 * AUTOFS mountpoint is busy, there's
			 * no point trying to unmount. Fall through
			 * to attempt to unmount subtrees rooted
			 * at a possible trigger node, but remember
			 * not to unmount this tree.
			 */
			autofs_busy_root = 1;
		}

		if (triggers_busy(fnp, &nfnp)) {
			rw_exit(&fnp->fn_rwlock);
			if (nfnp == NULL) {
				error = EBUSY;
				goto next;
			}
			/*
			 * nfnp is busy, try to unmount it first
			 */
			mutex_enter(&fnp->fn_lock);
			AUTOFS_UNBLOCK_OTHERS(fnp, MF_INPROG);
			mutex_exit(&fnp->fn_lock);
			VN_RELE(vp);
			ASSERT(fntovn(nfnp)->v_count > 1);
			fnp = nfnp;
			goto top;
		}

		/*
		 * At this point, we know all trigger nodes are locked,
		 * and they're not busy or mounted on.
		 */

		if (autofs_busy_root) {
			/*
			 * Got to unlock the the trigger nodes since
			 * I'm not really going to unmount the filesystem.
			 */
			(void) triggers_unlock(fnp);
		} else {
			/*
			 * Attempt to unmount all the trigger nodes,
			 * save the action_list in case we need to
			 * remount them later. The action_list will be XDR
			 * freed later if there was no need to remount the
			 * trigger nodes.
			 */
			unmount_triggers(fnp, &alp);
		}
	}
	rw_exit(&fnp->fn_rwlock);

	if (autofs_busy_root)
		goto next;

	(void) vn_vfswlock_wait(vp);

	vfsp = vn_mountedvfs(vp);
	if (vfsp != NULL) {
		/*
		 * Node is mounted on.
		 */
		AUTOFS_DPRINT((10, "\tNode is mounted on\n"));

		/*
		 * Deal with /xfn/host/jurassic alikes here...
		 */
		if (vfs_matchops(vfsp, vfs_getops(vp->v_vfsp))) {
			/*
			 * If the filesystem mounted here is AUTOFS, and it
			 * is busy, try to unmount the tree rooted on it
			 * first. We know this call to VFS_ROOT is safe to
			 * call while holding VVFSLOCK, since it resolves
			 * to a call to auto_root().
			 */
			AUTOFS_DPRINT((10, "\t\tAUTOFS mounted here\n"));
			vfs_lock_wait(vfsp);
			if (VFS_ROOT(vfsp, &newvp)) {
				cmn_err(CE_PANIC,
				    "unmount_tree: VFS_ROOT(vfs=%p) failed",
				    (void *)vfsp);
			}
			vfs_unlock(vfsp);
			nfnp = vntofn(newvp);
			if (DEEPER(nfnp)) {
				vn_vfsunlock(vp);
				mutex_enter(&fnp->fn_lock);
				AUTOFS_UNBLOCK_OTHERS(fnp, MF_INPROG);
				mutex_exit(&fnp->fn_lock);
				VN_RELE(vp);
				fnp = nfnp;
				goto top;
			}
			/*
			 * Fall through to unmount this filesystem
			 */
			VN_RELE(newvp);
		}

		/*
		 * vn_vfsunlock(vp) is done inside unmount_node()
		 */
		error = unmount_node(vp, force);
		if (error == ECONNRESET) {
			AUTOFS_DPRINT((10, "\tConnection dropped\n"));
			if (vn_mountedvfs(vp) == NULL) {
				/*
				 * The filesystem was unmounted before the
				 * daemon died. Unfortunately we can not
				 * determine whether all the cleanup work was
				 * successfully finished (i.e. update mnttab,
				 * or notify NFS server of the unmount).
				 * We should not retry the operation since the
				 * filesystem has already been unmounted, and
				 * may have already been removed from mnttab,
				 * in such case the devid/rdevid we send to
				 * the daemon will not be matched. So we have
				 * to be contempt with the partial unmount.
				 * Since the mountpoint is no longer covered, we
				 * clear the error condition.
				 */
				error = 0;
				auto_log(fngp, CE_WARN,
				    "unmount_tree: automountd connection "
				    "dropped");
				if (fnip->fi_flags & MF_DIRECT) {
					auto_log(fngp, CE_WARN, "unmount_tree: "
					    "%s successfully unmounted - "
					    "do not remount triggers",
					    fnip->fi_path);
				} else {
					auto_log(fngp, CE_WARN, "unmount_tree: "
					    "%s/%s successfully unmounted - "
					    "do not remount triggers",
					    fnip->fi_path, fnp->fn_name);
				}
			}
		}
	} else {
		vn_vfsunlock(vp);
		AUTOFS_DPRINT((10, "\tNode is AUTOFS\n"));
		if (unmount_as_unit) {
			AUTOFS_DPRINT((10, "\tunmount as unit\n"));
			error = unmount_autofs(vp);
		} else {
			AUTOFS_DPRINT((10, "\tunmount one at a time\n"));
			rw_enter(&fnp->fn_rwlock, RW_READER);
			if (fnp->fn_dirents != NULL) {
				/*
				 * Has subdirectory, attempt their
				 * unmount first
				 */
				nfnp = fnp->fn_dirents;
				VN_HOLD(fntovn(nfnp));
				rw_exit(&fnp->fn_rwlock);

				mutex_enter(&fnp->fn_lock);
				AUTOFS_UNBLOCK_OTHERS(fnp, MF_INPROG);
				mutex_exit(&fnp->fn_lock);
				VN_RELE(vp);
				fnp = nfnp;
				goto top;
			}
			rw_exit(&fnp->fn_rwlock);
			goto next;
		}
	}

	if (error) {
		AUTOFS_DPRINT((10, "\tUnmount failed\n"));
		if (alp != NULL) {
			/*
			 * Unmount failed, got to remount triggers.
			 */
			ASSERT((fnp->fn_flags & MF_THISUID_MATCH_RQD) == 0);
			error = auto_perform_actions(fnip, fnp, alp, CRED());
			if (error) {
				auto_log(fngp, CE_WARN, "autofs: can't remount "
				    "triggers fnp=%p error=%d", (void *)fnp,
				    error);
				error = 0;
				/*
				 * The action list should have been
				 * xdr_free'd by auto_perform_actions
				 * since an error occured
				 */
				alp = NULL;
			}
		}
	} else {
		/*
		 * The unmount succeeded, which will cause this node to
		 * be removed from its parent if its an indirect mount,
		 * therefore update the parent's atime and mtime now.
		 * I don't update them in auto_disconnect() because I
		 * don't want atime and mtime changing every time a
		 * lookup goes to the daemon and creates a new node.
		 */
		unmount_done = 1;
		if ((fnip->fi_flags & MF_DIRECT) == 0) {
			gethrestime(&now);
			if (fnp->fn_parent == fngp->fng_rootfnnodep)
				fnp->fn_atime = fnp->fn_mtime = now;
			else
				fnp->fn_parent->fn_atime =
					fnp->fn_parent->fn_mtime = now;
		}

		/*
		 * Free the action list here
		 */
		if (alp != NULL) {
			xdr_free(xdr_action_list, (char *)alp);
			alp = NULL;
		}
	}

	fnp->fn_ref_time = gethrestime_sec();

next:
	/*
	 * Obtain parent's readers lock before grabbing
	 * reference to next sibling.
	 * XXX Note that nodes in the top level list (mounted
	 * in user space not by the daemon in the kernel) parent is itself,
	 * therefore grabbing the lock makes no sense, but doesn't
	 * hurt either.
	 */
	pfnp = fnp->fn_parent;
	ASSERT(pfnp != NULL);
	rw_enter(&pfnp->fn_rwlock, RW_READER);
	if ((nfnp = fnp->fn_next) != NULL)
		VN_HOLD(fntovn(nfnp));
	rw_exit(&pfnp->fn_rwlock);

	if (ilocked_it) {
		mutex_enter(&fnp->fn_lock);
		if (unmount_done) {
			/*
			 * Other threads may be waiting for this unmount to
			 * finish. We must let it know that in order to
			 * proceed, it must trigger the mount itself.
			 */
			fnp->fn_flags &= ~MF_IK_MOUNT;
			if (fnp->fn_flags & MF_WAITING)
				fnp->fn_error = EAGAIN;
			unmount_done = 0;
		}
		AUTOFS_UNBLOCK_OTHERS(fnp, MF_INPROG);
		mutex_exit(&fnp->fn_lock);
		ilocked_it = 0;
	}

	if (nfnp != NULL) {
		VN_RELE(vp);
		fnp = nfnp;
		/*
		 * Unmount next element
		 */
		goto top;
	}

	/*
	 * We don't want to unmount rootfnnodep, so the check is made here
	 */
	ASSERT(pfnp != fnp);
	if (pfnp != fngp->fng_rootfnnodep) {
		/*
		 * Now attempt to unmount my parent
		 */
		VN_HOLD(fntovn(pfnp));
		VN_RELE(vp);
		fnp = pfnp;

		goto top;
	}

	VN_RELE(vp);

	/*
	 * At this point we've walked the entire tree and attempted to unmount
	 * as much as we can one level at a time.
	 */
done:
	mutex_enter(&unmount_tree_cpr_lock);
	CALLB_CPR_EXIT(&cprinfo);
	mutex_destroy(&unmount_tree_cpr_lock);
}

static void
unmount_zone_tree(struct autofs_globals *fngp)
{
	unmount_tree(fngp, 0);
	mutex_enter(&fngp->fng_unmount_threads_lock);
	fngp->fng_unmount_threads--;
	mutex_exit(&fngp->fng_unmount_threads_lock);

	AUTOFS_DPRINT((5, "unmount_tree done. Thread exiting.\n"));

	zthread_exit();
	/* NOTREACHED */
}

static int autofs_unmount_thread_timer = 120;	/* in seconds */

void
auto_do_unmount(struct autofs_globals *fngp)
{
	callb_cpr_t cprinfo;
	clock_t timeleft;
	zone_t *zone = curproc->p_zone;

	CALLB_CPR_INIT(&cprinfo, &fngp->fng_unmount_threads_lock,
		callb_generic_cpr, "auto_do_unmount");

	for (;;) {	/* forever */
		mutex_enter(&fngp->fng_unmount_threads_lock);
		CALLB_CPR_SAFE_BEGIN(&cprinfo);
newthread:
		mutex_exit(&fngp->fng_unmount_threads_lock);
		timeleft = zone_status_timedwait(zone, lbolt +
		    autofs_unmount_thread_timer * hz, ZONE_IS_SHUTTING_DOWN);
		mutex_enter(&fngp->fng_unmount_threads_lock);

		if (timeleft != -1) {	/* didn't time out */
			ASSERT(zone_status_get(zone) >= ZONE_IS_SHUTTING_DOWN);
			/*
			 * zone is exiting... don't create any new threads.
			 * fng_unmount_threads_lock is released implicitly by
			 * the below.
			 */
			CALLB_CPR_SAFE_END(&cprinfo,
				&fngp->fng_unmount_threads_lock);
			CALLB_CPR_EXIT(&cprinfo);
			zthread_exit();
			/* NOTREACHED */
		}
		if (fngp->fng_unmount_threads < autofs_unmount_threads) {
			fngp->fng_unmount_threads++;
			CALLB_CPR_SAFE_END(&cprinfo,
				&fngp->fng_unmount_threads_lock);
			mutex_exit(&fngp->fng_unmount_threads_lock);

			(void) zthread_create(NULL, 0, unmount_zone_tree, fngp,
			    0, minclsyspri);
		} else
			goto newthread;
	}
	/* NOTREACHED */
}

/*
 * Is nobrowse specified in option string?
 * opts should be a null ('\0') terminated string.
 * Returns non-zero if nobrowse has been specified.
 */
int
auto_nobrowse_option(char *opts)
{
	char *buf;
	char *p;
	char *t;
	int nobrowse = 0;
	int last_opt = 0;
	size_t len;

	len = strlen(opts) + 1;
	p = buf = kmem_alloc(len, KM_SLEEP);
	(void) strcpy(buf, opts);
	do {
		if (t = strchr(p, ','))
			*t++ = '\0';
		else
			last_opt++;
		if (strcmp(p, MNTOPT_NOBROWSE) == 0)
			nobrowse = 1;
		else if (strcmp(p, MNTOPT_BROWSE) == 0)
			nobrowse = 0;
		p = t;
	} while (!last_opt);
	kmem_free(buf, len);

	return (nobrowse);
}

/*
 * used to log warnings only if automountd is running
 * with verbose mode set
 */
void
auto_log(struct autofs_globals *fngp, int level, const char *fmt, ...)
{
	va_list args;

	if (fngp->fng_verbose > 0) {
		va_start(args, fmt);
		vzcmn_err(fngp->fng_zoneid, level, fmt, args);
		va_end(args);
	}
}

#ifdef DEBUG
static int autofs_debug = 0;

/*
 * Utilities used by both client and server
 * Standard levels:
 * 0) no debugging
 * 1) hard failures
 * 2) soft failures
 * 3) current test software
 * 4) main procedure entry points
 * 5) main procedure exit points
 * 6) utility procedure entry points
 * 7) utility procedure exit points
 * 8) obscure procedure entry points
 * 9) obscure procedure exit points
 * 10) random stuff
 * 11) all <= 1
 * 12) all <= 2
 * 13) all <= 3
 * ...
 */
/* PRINTFLIKE2 */
void
auto_dprint(int level, const char *fmt, ...)
{
	va_list args;

	if (autofs_debug == level ||
	    (autofs_debug > 10 && (autofs_debug - 10) >= level)) {
		va_start(args, fmt);
		(void) vprintf(fmt, args);
		va_end(args);
	}
}
#endif /* DEBUG */
