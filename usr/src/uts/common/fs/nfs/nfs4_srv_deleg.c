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
 * Copyright 2009 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#include <sys/systm.h>
#include <rpc/auth.h>
#include <rpc/clnt.h>
#include <nfs/nfs4_kprot.h>
#include <nfs/nfs4.h>
#include <nfs/lm.h>
#include <sys/cmn_err.h>
#include <sys/disp.h>
#include <sys/sdt.h>

#include <sys/pathname.h>

#include <sys/strsubr.h>
#include <sys/ddi.h>

#include <sys/vnode.h>
#include <sys/sdt.h>
#include <inet/common.h>
#include <inet/ip.h>
#include <inet/ip6.h>
#include <sys/sdt.h>

#define	MAX_READ_DELEGATIONS 5

static int rfs4_max_setup_cb_tries = 5;

#ifdef DEBUG
static int rfs4_test_cbgetattr_fail = 0;
int rfs4_cb_null;
int rfs4_cb_debug;
int rfs4_deleg_debug;
#endif

int mds_cbrecall_no_session = 0;

static void rfs4_recall_file(rfs4_file_t *, bool_t, rfs4_client_t *);
static	void		rfs4_revoke_deleg(rfs4_deleg_state_t *);
static	void		rfs41_revoke_deleg(rfs4_deleg_state_t *);
static	void		rfs4_revoke_file(rfs4_file_t *);
static	void		rfs4_cb_chflush(rfs4_cbinfo_t *);
static	CLIENT		*rfs4_cb_getch(rfs4_cbinfo_t *);
static	void		rfs4_cb_freech(rfs4_cbinfo_t *, CLIENT *, bool_t);
static rfs4_deleg_state_t *rfs4_deleg_state(struct compound_state *,
    rfs4_state_t *, open_delegation_type4, int *);

/*
 * Convert a universal address to an transport specific
 * address using inet_pton.
 */
int
uaddr2sockaddr(int af, char *ua, void *ap, in_port_t *pp)
{
	int dots = 0, i, j, len, k;
	unsigned char c;
	in_port_t port = 0;

	len = strlen(ua);

	for (i = len-1; i >= 0; i--) {

		if (ua[i] == '.')
			dots++;

		if (dots == 2) {

			ua[i] = '\0';
			/*
			 * We use k to remember were to stick '.' back, since
			 * ua was kmem_allocateded from the pool len+1.
			 */
			k = i;
			if (inet_pton(af, ua, ap) == 1) {

				c = 0;

				for (j = i+1; j < len; j++) {
					if (ua[j] == '.') {
						port = c << 8;
						c = 0;
					} else if (ua[j] >= '0' &&
					    ua[j] <= '9') {
						c *= 10;
						c += ua[j] - '0';
					} else {
						ua[k] = '.';
						return (EINVAL);
					}
				}
				port += c;


				/* reset to network order */
				if (af == AF_INET) {
					*(uint32_t *)ap =
					    htonl(*(uint32_t *)ap);
					*pp = htons(port);
				} else {
					int ix;
					uint16_t *sap;

					for (sap = ap, ix = 0; ix <
					    sizeof (struct in6_addr) /
					    sizeof (uint16_t); ix++)
						sap[ix] = htons(sap[ix]);

					*pp = htons(port);
				}

				ua[k] = '.';
				return (0);
			} else {
				ua[k] = '.';
				return (EINVAL);
			}
		}
	}

	return (EINVAL);
}

/*
 * Update the delegation policy with the
 * value of "new_policy"
 */
void
rfs4_set_deleg_policy(nfs_server_instance_t *instp,
		srv_deleg_policy_t new_policy)
{
	rw_enter(&instp->deleg_policy_lock, RW_WRITER);
	instp->deleg_policy = new_policy;
	rw_exit(&instp->deleg_policy_lock);
}

void
rfs4_hold_deleg_policy(nfs_server_instance_t *instp)
{
	rw_enter(&instp->deleg_policy_lock, RW_READER);
}

void
rfs4_rele_deleg_policy(nfs_server_instance_t *instp)
{
	rw_exit(&instp->deleg_policy_lock);
}


/*
 * This free function is to be used when the client struct is being
 * released and nothing at all is needed of the callback info any
 * longer.
 */
void
rfs4_cbinfo_free(rfs4_cbinfo_t *cbp)
{
	char *addr = cbp->cb_callback.cb_location.r_addr;
	char *netid = cbp->cb_callback.cb_location.r_netid;

	/* Free old address if any */

	if (addr)
		kmem_free(addr, strlen(addr) + 1);
	if (netid)
		kmem_free(netid, strlen(netid) + 1);

	addr = cbp->cb_newer.cb_callback.cb_location.r_addr;
	netid = cbp->cb_newer.cb_callback.cb_location.r_netid;

	if (addr)
		kmem_free(addr, strlen(addr) + 1);
	if (netid)
		kmem_free(netid, strlen(netid) + 1);

	if (cbp->cb_chc_free) {
		rfs4_cb_chflush(cbp);
	}
}

/*
 * The server uses this to check the callback path supplied by the
 * client.  The callback connection is marked "in progress" while this
 * work is going on and then eventually marked either OK or FAILED.
 * This work can be done as part of a separate thread and at the end
 * of this the thread will exit or it may be done such that the caller
 * will continue with other work.
 */
static void
rfs4_do_cb_null(rfs4_client_t *cp)
{
	struct timeval tv;
	CLIENT *ch;
	rfs4_cbstate_t newstate;
	rfs4_cbinfo_t *cbp = &cp->rc_cbinfo;

	mutex_enter(cbp->cb_lock);
	/* If another thread is doing CB_NULL RPC then return */
	if (cbp->cb_nullcaller == TRUE) {
		mutex_exit(cbp->cb_lock);
		rfs4_client_rele(cp);
		return;
	}

	/* Mark the cbinfo as having a thread in the NULL callback */
	cbp->cb_nullcaller = TRUE;

	/*
	 * Are there other threads still using the cbinfo client
	 * handles?  If so, this thread must wait before going and
	 * mucking aroiund with the callback information
	 */
	while (cbp->cb_refcnt != 0)
		cv_wait(cbp->cb_cv_nullcaller, cbp->cb_lock);

	/*
	 * This thread itself may find that new callback info has
	 * arrived and is set up to handle this case and redrive the
	 * call to the client's callback server.
	 */
retry:
	if (cbp->cb_newer.cb_new == TRUE &&
	    cbp->cb_newer.cb_confirmed == TRUE) {
		char *addr = cbp->cb_callback.cb_location.r_addr;
		char *netid = cbp->cb_callback.cb_location.r_netid;

		/*
		 * Free the old stuff if it exists; may be the first
		 * time through this path
		 */
		if (addr)
			kmem_free(addr, strlen(addr) + 1);
		if (netid)
			kmem_free(netid, strlen(netid) + 1);

		/* Move over the addr/netid */
		cbp->cb_callback.cb_location.r_addr =
		    cbp->cb_newer.cb_callback.cb_location.r_addr;
		cbp->cb_newer.cb_callback.cb_location.r_addr = NULL;
		cbp->cb_callback.cb_location.r_netid =
		    cbp->cb_newer.cb_callback.cb_location.r_netid;
		cbp->cb_newer.cb_callback.cb_location.r_netid = NULL;

		/* Get the program number */
		cbp->cb_callback.cb_program =
		    cbp->cb_newer.cb_callback.cb_program;
		cbp->cb_newer.cb_callback.cb_program = 0;

		/* Don't forget the protocol's "cb_ident" field */
		cbp->cb_ident = cbp->cb_newer.cb_ident;
		cbp->cb_newer.cb_ident = 0;

		/* no longer new */
		cbp->cb_newer.cb_new = FALSE;
		cbp->cb_newer.cb_confirmed = FALSE;

		/* get rid of the old client handles that may exist */
		rfs4_cb_chflush(cbp);

		cbp->cb_state = CB_NONE;
		cbp->cb_timefailed = 0; /* reset the clock */
		cbp->cb_notified_of_cb_path_down = TRUE;
	}

	if (cbp->cb_state != CB_NONE) {
		cv_broadcast(cbp->cb_cv);	/* let the others know */
		cbp->cb_nullcaller = FALSE;
		mutex_exit(cbp->cb_lock);
		rfs4_client_rele(cp);
		return;
	}

	/* mark rfs4_client_t as CALLBACK NULL in progress */
	cbp->cb_state = CB_INPROG;
	mutex_exit(cbp->cb_lock);

	/* get/generate a client handle */
	if ((ch = rfs4_cb_getch(cbp)) == NULL) {
		mutex_enter(cbp->cb_lock);
		cbp->cb_state = CB_BAD;
		cbp->cb_timefailed = gethrestime_sec(); /* observability */
		goto retry;
	}


	tv.tv_sec = 30;
	tv.tv_usec = 0;
	if (clnt_call(ch, CB_NULL, xdr_void, NULL, xdr_void, NULL, tv) != 0) {
		newstate = CB_BAD;
	} else {
		newstate = CB_OK;
#ifdef	DEBUG
		rfs4_cb_null++;
#endif
	}

	/* Check to see if the client has specified new callback info */
	mutex_enter(cbp->cb_lock);
	rfs4_cb_freech(cbp, ch, TRUE);
	if (cbp->cb_newer.cb_new == TRUE &&
	    cbp->cb_newer.cb_confirmed == TRUE) {
		goto retry;	/* give the CB_NULL another chance */
	}

	cbp->cb_state = newstate;
	if (cbp->cb_state == CB_BAD)
		cbp->cb_timefailed = gethrestime_sec(); /* observability */

	cv_broadcast(cbp->cb_cv);	/* start up the other threads */
	cbp->cb_nullcaller = FALSE;
	mutex_exit(cbp->cb_lock);

	rfs4_client_rele(cp);
}

/*
 * Given a client struct, inspect the callback info to see if the
 * callback path is up and available.
 *
 * If new callback path is available and no one has set it up then
 * try to set it up. If setup is not successful after 5 tries (5 secs)
 * then gives up and returns NULL.
 *
 * If callback path is being initialized, then wait for the CB_NULL RPC
 * call to occur.
 */
static rfs4_cbinfo_t *
rfs4_cbinfo_hold(rfs4_client_t *cp)
{
	rfs4_cbinfo_t *cbp = &cp->rc_cbinfo;
	int retries = 0;

	mutex_enter(cbp->cb_lock);

	while (cbp->cb_newer.cb_new == TRUE && cbp->cb_nullcaller == FALSE) {
		/*
		 * Looks like a new callback path may be available and
		 * noone has set it up.
		 */
		mutex_exit(cbp->cb_lock);
		rfs4_dbe_hold(cp->rc_dbe);
		rfs4_do_cb_null(cp); /* caller will release client hold */

		mutex_enter(cbp->cb_lock);
		/*
		 * If callback path is no longer new, or it's being setup
		 * then stop and wait for it to be done.
		 */
		if (cbp->cb_newer.cb_new == FALSE || cbp->cb_nullcaller == TRUE)
			break;
		mutex_exit(cbp->cb_lock);

		if (++retries >= rfs4_max_setup_cb_tries)
			return (NULL);
		delay(hz);
		mutex_enter(cbp->cb_lock);
	}

	/* Is there a thread working on doing the CB_NULL RPC? */
	if (cbp->cb_nullcaller == TRUE)
		cv_wait(cbp->cb_cv, cbp->cb_lock);  /* if so, wait on it */

	/* If the callback path is not okay (up and running), just quit */
	if (cbp->cb_state != CB_OK) {
		mutex_exit(cbp->cb_lock);
		return (NULL);
	}

	/* Let someone know we are using the current callback info */
	cbp->cb_refcnt++;
	mutex_exit(cbp->cb_lock);
	return (cbp);
}

/*
 * The caller is done with the callback info.  It may be that the
 * caller's RPC failed and the NFSv4 client has actually provided new
 * callback information.  If so, let the caller know so they can
 * advantage of this and maybe retry the RPC that originally failed.
 */
static int
rfs4_cbinfo_rele(rfs4_cbinfo_t *cbp, rfs4_cbstate_t newstate)
{
	int cb_new = FALSE;

	mutex_enter(cbp->cb_lock);

	/* The caller gets a chance to mark the callback info as bad */
	if (newstate != CB_NOCHANGE)
		cbp->cb_state = newstate;
	if (newstate == CB_FAILED) {
		cbp->cb_timefailed = gethrestime_sec(); /* observability */
		cbp->cb_notified_of_cb_path_down = FALSE;
	}

	cbp->cb_refcnt--;	/* no longer using the information */

	/*
	 * A thread may be waiting on this one to finish and if so,
	 * let it know that it is okay to do the CB_NULL to the
	 * client's callback server.
	 */
	if (cbp->cb_refcnt == 0 && cbp->cb_nullcaller)
		cv_broadcast(cbp->cb_cv_nullcaller);

	/*
	 * If this is the last thread to use the callback info and
	 * there is new callback information to try and no thread is
	 * there ready to do the CB_NULL, then return true to teh
	 * caller so they can do the CB_NULL
	 */
	if (cbp->cb_refcnt == 0 &&
	    cbp->cb_nullcaller == FALSE &&
	    cbp->cb_newer.cb_new == TRUE &&
	    cbp->cb_newer.cb_confirmed == TRUE)
		cb_new = TRUE;

	mutex_exit(cbp->cb_lock);

	return (cb_new);
}

/*
 * Common v4 routine to init a callback client handle
 */

static CLIENT *
cbch_init(struct netbuf *nb, uint32_t cb_program)
{
	struct knetconfig knc;
	vnode_t *vp;
	char *devnam;
	int err = 0;
	CLIENT *ch = NULL;
	struct sockaddr *sa;

	sa = (struct sockaddr *)nb->buf;

	if (sa->sa_family == AF_INET) {
		knc.knc_semantics = NC_TPI_COTS;
		knc.knc_protofmly = "inet";
		knc.knc_proto = "tcp";
		devnam = "/dev/tcp";
	} else if (sa->sa_family == AF_INET6) {
		knc.knc_semantics = NC_TPI_COTS;
		knc.knc_protofmly = "inet6";
		knc.knc_proto = "tcp";
		devnam = "/dev/tcp6";
	} else {
		DTRACE_PROBE2(nfss__cb__debug, char *,
		    "cbch_init: unknown transport family", int, sa->sa_family);

		goto cb_init_out;
	}

	if (lookupname(devnam, UIO_SYSSPACE, FOLLOW, NULLVPP, &vp) != 0) {

		DTRACE_PROBE2(nfss__cb__debug, char *,
		    "cbch_init: lookupname failed", int, err);

		goto cb_init_out;
	}

	if (vp->v_type != VCHR) {

		DTRACE_PROBE2(nfss__cb__debug, char *,
		    "cbch_init: v_type not of type VCHR", char *, devnam);
		VN_RELE(vp);
		goto cb_init_out;
	}

	knc.knc_rdev = vp->v_rdev;

	VN_RELE(vp);

	if (err = clnt_tli_kcreate(&knc, nb, cb_program,
	    NFS_CB, 0, 0, curthread->t_cred, &ch)) {

		DTRACE_PROBE2(nfss__cb__debug, char *,
		    "cbch_init: clnt_tli_kcreate failed", int, err);
		ch = NULL;
	}

cb_init_out:
	return (ch);
}

/*
 * Given the information in the callback info struct, create a client
 * handle that can be used by the server for its callback path.
 */
static CLIENT *
rfs4_cbch_init(rfs4_cbinfo_t *cbp)
{
	int useresvport = 0;
	int af;
	int size;
	netaddr4 *naddr;
	void *addr;
	void *taddr;
	in_port_t *pp;
	struct sockaddr_in addr4;
	struct sockaddr_in6 addr6;
	struct netbuf nb;
	CLIENT *ch = NULL;

	DTRACE_PROBE2(nfss__cb__debug, char *,
	    "rfs4_cbch_init: entry cbp:", rfs4_cbinfo_t *, cbp);

	mutex_enter(cbp->cb_lock);

	naddr = (netaddr4 *)&cbp->cb_callback.cb_location;

	if (naddr->na_r_netid == NULL || naddr->na_r_addr == NULL) {
		goto ch_out;
	}

	if (strcmp(naddr->na_r_netid, "tcp") == 0) {
		af = AF_INET;
		size = sizeof (addr4);
		bzero(&addr4, size);
		addr4.sin_family = (sa_family_t)af;
		addr = &addr4.sin_addr;
		pp = &addr4.sin_port;
		taddr = &addr4;
	} else if (strcmp(naddr->na_r_netid, "tcp6") == 0) {
		af = AF_INET6;
		size = sizeof (addr6);
		bzero(&addr6, size);
		addr6.sin6_family = (sa_family_t)af;
		addr = &addr6.sin6_addr;
		pp = &addr6.sin6_port;
		taddr = &addr6;
	} else {
		DTRACE_PROBE2(nfss__cb__debug, char *,
		    "rfs4_cbch_init: bad transport", char *,
		    cbp->cb_callback.cb_location.r_netid);
		goto ch_out;
	}

	if (uaddr2sockaddr(af, naddr->na_r_addr, addr, pp)) {

		DTRACE_PROBE2(nfss__cb__debug, char *,
		    "rfs4_cbch_init: malformed universal addr: ",
		    void *, naddr->na_r_addr);

		goto ch_out;
	}


	nb.maxlen = nb.len = size;
	nb.buf = (char *)taddr;

	ch = cbch_init(&nb, cbp->cb_callback.cb_program);

	/* turn off reserved port usage */
	if (ch != NULL)
		(void) CLNT_CONTROL(ch, CLSET_BINDRESVPORT,
		    (char *)&useresvport);
ch_out:
	mutex_exit(cbp->cb_lock);
	return (ch);
}

/*
 * Iterate over the client handle cache and
 * destroy it.
 */
static void
rfs4_cb_chflush(rfs4_cbinfo_t *cbp)
{
	CLIENT *ch;

	while (cbp->cb_chc_free) {
		cbp->cb_chc_free--;
		ch = cbp->cb_chc[cbp->cb_chc_free];
		cbp->cb_chc[cbp->cb_chc_free] = NULL;
		if (ch) {
			if (ch->cl_auth)
				auth_destroy(ch->cl_auth);
			clnt_destroy(ch);
		}
	}
}

/*
 * Return a client handle, either from a the small
 * rfs4_client_t cache or one that we just created.
 */
static CLIENT *
rfs4_cb_getch(rfs4_cbinfo_t *cbp)
{
	CLIENT *cbch = NULL;
	uint32_t zilch = 0;

	mutex_enter(cbp->cb_lock);

	if (cbp->cb_chc_free) {
		cbp->cb_chc_free--;
		cbch = cbp->cb_chc[ cbp->cb_chc_free ];
		mutex_exit(cbp->cb_lock);
		(void) CLNT_CONTROL(cbch, CLSET_XID, (char *)&zilch);
		return (cbch);
	}

	mutex_exit(cbp->cb_lock);

	/* none free so make it now */
	cbch = rfs4_cbch_init(cbp);

	return (cbch);
}

/*
 * Return the client handle to the small cache or
 * destroy it.
 */
static void
rfs4_cb_freech(rfs4_cbinfo_t *cbp, CLIENT *ch, bool_t lockheld)
{
	if (lockheld == FALSE)
		mutex_enter(cbp->cb_lock);

	if (cbp->cb_chc_free < RFS4_CBCH_MAX) {
		cbp->cb_chc[ cbp->cb_chc_free++ ] = ch;
		if (lockheld == FALSE)
			mutex_exit(cbp->cb_lock);
		return;
	}
	if (lockheld == FALSE)
		mutex_exit(cbp->cb_lock);

	/*
	 * cache maxed out of free entries, obliterate
	 * this client handle, destroy it, throw it away.
	 */
	if (ch->cl_auth)
		auth_destroy(ch->cl_auth);
	clnt_destroy(ch);
}

static CLIENT *
rfs41_cb_chinit(uint32_t cbprog)
{
	CLIENT *ch;
	struct knetconfig knc;
	int err;

	/*
	 * The dest addr and parts of knc fields passed into
	 * clnt_tli_kcreate() are dummy. The connection is
	 * picked up later and RPC does not really use it to
	 * create connections for 4.1 callbacks.
	 */

	bzero(&knc, sizeof (struct knetconfig));

	/*
	 * knc_semantics is important to choose the
	 * right transport type.
	 */
	knc.knc_semantics = NC_TPI_COTS;
	knc.knc_protofmly = "inet";
	knc.knc_proto = "tcp";

	if (err = clnt_tli_kcreate(&knc, 0, cbprog, NFS_CB, 0, 0,
	    curthread->t_cred, &ch)) {
		DTRACE_PROBE2(nfss__cb__debug, char *,
		    "rfs41_cbch_init: clnt_tli_kcreate failed", int, err);
		ch = NULL;
	}
	if (ch != NULL)
		CLNT_CONTROL(ch, CLSET_CBCLIENT, NULL);
	return (ch);
}

CLIENT *
rfs41_cb_getch(mds_session_t *sp)
{
	CLIENT *cbch = NULL;
	sess_channel_t *bcp;
	sess_bcsd_t *bsdp;

	rfs4_dbe_lock(sp->sn_dbe);
	bcp = SNTOBC(sp);
	rfs4_dbe_unlock(sp->sn_dbe);

	rw_enter(&bcp->cn_lock, RW_READER);
	bsdp = CTOBSD(bcp);

	rw_enter(&bsdp->bsd_rwlock, RW_WRITER);
	if (bsdp->bsd_ch_free) {
		bsdp->bsd_ch_free--;
		cbch = bsdp->bsd_clnt[bsdp->bsd_ch_free];
	} else {
		cbch = rfs41_cb_chinit(sp->sn_bc.progno);
		CLNT_CONTROL(cbch, CLSET_TAG, (void *)sp->sn_sessid);
	}

	rw_exit(&bsdp->bsd_rwlock);
	rw_exit(&bcp->cn_lock);
	return (cbch);
}

void
rfs41_cb_freech(mds_session_t *sp, CLIENT *ch)
{
	sess_channel_t *bcp;
	sess_bcsd_t *bsdp;

	rfs4_dbe_lock(sp->sn_dbe);
	bcp = SNTOBC(sp);
	rfs4_dbe_unlock(sp->sn_dbe);

	rw_enter(&bcp->cn_lock, RW_READER);
	bsdp = CTOBSD(bcp);

	rw_enter(&bsdp->bsd_rwlock, RW_WRITER);
	if (bsdp->bsd_ch_free < RFS4_CBCH_MAX) {
		bsdp->bsd_clnt[bsdp->bsd_ch_free++] = ch;
		rw_exit(&bsdp->bsd_rwlock);
		rw_exit(&bcp->cn_lock);
		return;
	}

	rw_exit(&bsdp->bsd_rwlock);
	rw_exit(&bcp->cn_lock);

	/*
	 * cache maxed out of free entries, obliterate
	 * this client handle, destroy it, throw it away.
	 */
	if (ch->cl_auth)
		auth_destroy(ch->cl_auth);
	clnt_destroy(ch);
}

/*
 * Iterate over the session's client handle cache and
 * destroy it.
 */
void
rfs41_cb_chflush(mds_session_t *sp)
{
	CLIENT *ch;
	sess_channel_t *bcp;
	sess_bcsd_t *bsdp;

	rfs4_dbe_lock(sp->sn_dbe);
	bcp = SNTOBC(sp);
	rfs4_dbe_unlock(sp->sn_dbe);

	rw_enter(&bcp->cn_lock, RW_READER);
	bsdp = CTOBSD(bcp);

	rw_enter(&bsdp->bsd_rwlock, RW_WRITER);

	while (bsdp->bsd_ch_free) {
		bsdp->bsd_ch_free--;
		ch = bsdp->bsd_clnt[bsdp->bsd_ch_free];
		bsdp->bsd_clnt[bsdp->bsd_ch_free] = NULL;
		if (ch) {
			if (ch->cl_auth)
				auth_destroy(ch->cl_auth);
			clnt_destroy(ch);
		}
	}

	rw_exit(&bsdp->bsd_rwlock);
}

/*
 * With the supplied callback information - initialize the client
 * callback data.  If there is a callback in progress, save the
 * callback info so that a thread can pick it up in the future.
 */
void
rfs4_client_setcb(rfs4_client_t *cp, cb_client4 *cb, uint32_t cb_ident)
{
	char *addr = NULL;
	char *netid = NULL;
	rfs4_cbinfo_t *cbp = &cp->rc_cbinfo;
	size_t len;

	/* Set the call back for the client */
	if (cb->cb_location.r_addr && cb->cb_location.r_addr[0] != '\0' &&
	    cb->cb_location.r_netid && cb->cb_location.r_netid[0] != '\0') {
		len = strlen(cb->cb_location.r_addr) + 1;
		addr = kmem_alloc(len, KM_SLEEP);
		bcopy(cb->cb_location.r_addr, addr, len);
		len = strlen(cb->cb_location.r_netid) + 1;
		netid = kmem_alloc(len, KM_SLEEP);
		bcopy(cb->cb_location.r_netid, netid, len);
	}
	/* ready to save the new information but first free old, if exists */
	mutex_enter(cbp->cb_lock);

	cbp->cb_newer.cb_callback.cb_program = cb->cb_program;

	if (cbp->cb_newer.cb_callback.cb_location.r_addr != NULL)
		kmem_free(cbp->cb_newer.cb_callback.cb_location.r_addr,
		    strlen(cbp->cb_newer.cb_callback.cb_location.r_addr) + 1);
	cbp->cb_newer.cb_callback.cb_location.r_addr = addr;

	if (cbp->cb_newer.cb_callback.cb_location.r_netid != NULL)
		kmem_free(cbp->cb_newer.cb_callback.cb_location.r_netid,
		    strlen(cbp->cb_newer.cb_callback.cb_location.r_netid) + 1);
	cbp->cb_newer.cb_callback.cb_location.r_netid = netid;

	cbp->cb_newer.cb_ident = cb_ident;

	if (addr && *addr && netid && *netid) {
		cbp->cb_newer.cb_new = TRUE;
		cbp->cb_newer.cb_confirmed = FALSE;
	} else {
		cbp->cb_newer.cb_new = FALSE;
		cbp->cb_newer.cb_confirmed = FALSE;
	}

	mutex_exit(cbp->cb_lock);
}

/*
 * The server uses this when processing SETCLIENTID_CONFIRM.  Callback
 * information may have been provided on SETCLIENTID and this call
 * marks that information as confirmed and then starts a thread to
 * test the callback path.
 */
void
rfs4_deleg_cb_check(rfs4_client_t *cp)
{
	if (cp->rc_cbinfo.cb_newer.cb_new == FALSE)
		return;

	cp->rc_cbinfo.cb_newer.cb_confirmed = TRUE;

	rfs4_dbe_hold(cp->rc_dbe); /* hold the client struct for thread */

	(void) thread_create(NULL, 0, rfs4_do_cb_null, cp, 0, &p0, TS_RUN,
	    minclsyspri);
}

static void
rfs4args_cb_recall_free(nfs_cb_argop4 *argop)
{
	CB_RECALL4args	*rec_argp;

	rec_argp = &argop->nfs_cb_argop4_u.opcbrecall;
	if (rec_argp->fh.nfs_fh4_val)
		kmem_free(rec_argp->fh.nfs_fh4_val, rec_argp->fh.nfs_fh4_len);
}

/* XXX - this only works for one entry in the referring_call_list4 - rick */
void
rfs41args_cb_sequence_free(nfs_cb_argop4 *argop)
{
	CB_SEQUENCE4args	*ap;
	referring_call_list4	*rp;
	uint_t			 len;

	ap = &argop->nfs_cb_argop4_u.opcbsequence;
	if ((rp = ap->csa_rcall_lval) != NULL) {
		if (rp->rcl_val != NULL) {
			len = rp->rcl_len;
			kmem_free(rp->rcl_val, len * sizeof (referring_call4));
			rp->rcl_val = NULL;
		}
		len = ap->csa_rcall_llen;
		kmem_free(rp, len * sizeof (referring_call_list4));
		ap->csa_rcall_lval = NULL;
	}
}

/* ARGSUSED */
static void
rfs4args_cb_getattr_free(nfs_cb_argop4 *argop)
{
	CB_GETATTR4args *argp;

	argp = &argop->nfs_cb_argop4_u.opcbgetattr;
	if (argp->fh.nfs_fh4_val)
		kmem_free(argp->fh.nfs_fh4_val, argp->fh.nfs_fh4_len);
}

void
rfs4freeargres(CB_COMPOUND4args *args, CB_COMPOUND4res *resp)
{
	int i, arglen;
	nfs_cb_argop4 *argop;

	/*
	 * First free any special args alloc'd for specific ops.
	 */
	arglen = args->array_len;
	argop = args->array;
	for (i = 0; i < arglen; i++, argop++) {

		switch (argop->argop) {
		case OP_CB_SEQUENCE:
			rfs41args_cb_sequence_free(argop);
			break;

		case OP_CB_RECALL:
			rfs4args_cb_recall_free(argop);
			break;

		case OP_CB_GETATTR:
			rfs4args_cb_getattr_free(argop);
			break;

		case OP_CB_LAYOUTRECALL:
			break;

		default:
			return;
		}
	}

	if (args->tag.utf8string_len > 0)
		UTF8STRING_FREE(args->tag)

	kmem_free(args->array, arglen * sizeof (nfs_cb_argop4));
	if (resp)
		(void) xdr_free(xdr_CB_COMPOUND4res, (caddr_t)resp);
}

slotid4
svc_slot_maxslot(mds_session_t *sp)
{
	slotid4		 ms;
	sess_channel_t	*bcp;
	sess_bcsd_t	*bsdp;

	rfs4_dbe_lock(sp->sn_dbe);
	bcp = SNTOBC(sp);
	rfs4_dbe_unlock(sp->sn_dbe);

	rw_enter(&bcp->cn_lock, RW_READER);
	if ((bsdp = CTOBSD(bcp)) == NULL)
		cmn_err(CE_PANIC, "svc_slot_maxslot: BC Specific Data Not Set");

	rw_enter(&bsdp->bsd_rwlock, RW_READER);
	slot_table_query(bsdp->bsd_stok, SLT_MAXSLOT, &ms);
	rw_exit(&bsdp->bsd_rwlock);

	rw_exit(&bcp->cn_lock);
	return (ms);
}

/*
 * Server-side slot allocations from BC's slot table.
 */
slot_ent_t *
svc_slot_alloc(mds_session_t *sp)
{
	slot_ent_t	*p;
	sess_channel_t	*bcp;
	sess_bcsd_t	*bsdp;

	rfs4_dbe_lock(sp->sn_dbe);
	bcp = SNTOBC(sp);
	rfs4_dbe_unlock(sp->sn_dbe);

	rw_enter(&bcp->cn_lock, RW_READER);
	if ((bsdp = CTOBSD(bcp)) == NULL)
		cmn_err(CE_PANIC, "svc_slot_alloc: BC Specific Data Not Set");

	rw_enter(&bsdp->bsd_rwlock, RW_READER);
	(void) slot_alloc(bsdp->bsd_stok, SLT_SLEEP, &p);
	rw_exit(&bsdp->bsd_rwlock);

	rw_exit(&bcp->cn_lock);
	return (p);
}

/*
 * Server-side slot allocations from BC's slot table.
 */
void
svc_slot_free(mds_session_t *sp, slot_ent_t *p)
{
	sess_channel_t	*bcp;
	sess_bcsd_t	*bsdp;

	ASSERT(sp != NULL);
	ASSERT(p != NULL);
	rfs4_dbe_lock(sp->sn_dbe);
	bcp = SNTOBC(sp);
	rfs4_dbe_unlock(sp->sn_dbe);

	rw_enter(&bcp->cn_lock, RW_READER);
	if ((bsdp = CTOBSD(bcp)) == NULL)
		cmn_err(CE_PANIC, "svc_slot_free: BC Specific Data Not Set");

	rw_enter(&bsdp->bsd_rwlock, RW_READER);
	slot_free(bsdp->bsd_stok, p);
	rw_exit(&bsdp->bsd_rwlock);

	rw_exit(&bcp->cn_lock);
}

void
svc_slot_cb_seqid(CB_COMPOUND4res *resp, slot_ent_t *p)
{
	CB_SEQUENCE4res	*rp;

	if (resp == NULL || resp->array == NULL)
		return;

	ASSERT(resp->array->resop == OP_CB_SEQUENCE);
	rp = &resp->array->nfs_cb_resop4_u.opcbsequence;
	if (rp->csr_status == NFS4_OK) {
		slot_incr_seq(p, 1);
	}
}

/*
 * General callback routine for the server to the client.
 */
static enum clnt_stat
rfs4_do_callback(rfs4_client_t *cp, CB_COMPOUND4args *args,
    CB_COMPOUND4res *res, struct timeval timeout)
{
	rfs4_cbinfo_t *cbp;
	CLIENT *ch;
	/* start with this in case cb_getch() fails */
	enum clnt_stat	stat = RPC_FAILED;

	res->tag.utf8string_val = NULL;
	res->array = NULL;

retry:
	cbp = rfs4_cbinfo_hold(cp);
	if (cbp == NULL)
		return (stat);

	/* get a client handle */
	if ((ch = rfs4_cb_getch(cbp)) != NULL) {
		/*
		 * reset the cb_ident since it may have changed in
		 * rfs4_cbinfo_hold()
		 */
		args->callback_ident = cbp->cb_ident;

		stat = clnt_call(ch, CB_COMPOUND, xdr_CB_COMPOUND4args_srv,
		    (caddr_t)args, xdr_CB_COMPOUND4res,
		    (caddr_t)res, timeout);

		/* free client handle */
		rfs4_cb_freech(cbp, ch, FALSE);
	}

	/*
	 * If the rele says that there may be new callback info then
	 * retry this sequence and it may succeed as a result of the
	 * new callback path
	 */
	if (rfs4_cbinfo_rele(cbp,
	    (stat == RPC_SUCCESS ? CB_NOCHANGE : CB_FAILED)) == TRUE)
		goto retry;

	return (stat);
}

/*
 * Used by the NFSv4 server to get attributes for a file while
 * handling the case where a file has been write delegated.  For the
 * time being, VOP_GETATTR() is called and CB_GETATTR processing is
 * not undertaken.  This call site is maintained in case the server is
 * updated in the future to handle write delegation space guarantees.
 */
nfsstat4
rfs4_vop_getattr(vnode_t *vp, vattr_t *vap, int flag, cred_t *cr)
{

	int error;

	error = VOP_GETATTR(vp, vap, flag, cr, NULL);
	return (puterrno4(error));
}

/*
 * This is used everywhere in the v2/v3 server to allow the
 * integration of all NFS versions and the support of delegation.  For
 * now, just call the VOP_GETATTR().  If the NFSv4 server is enhanced
 * in the future to provide space guarantees for write delegations
 * then this call site should be expanded to interact with the client.
 */
int
rfs4_delegated_getattr(vnode_t *vp, vattr_t *vap, int flag, cred_t *cr)
{
	return (VOP_GETATTR(vp, vap, flag, cr, NULL));
}

/*
 * Place the actual cb_recall otw call to client.
 */
void
rfs4_do_cb_recall(rfs4_deleg_state_t *dsp, bool_t trunc)
{
	CB_COMPOUND4args	cb4_args;
	CB_COMPOUND4res		cb4_res;
	CB_RECALL4args		*rec_argp;
	CB_RECALL4res		*rec_resp;
	nfs_cb_argop4		*argop;
	int			numops;
	int			argoplist_size;
	struct timeval		timeout;
	nfs_fh4			*fhp;
	enum clnt_stat		call_stat;

	/*
	 * set up the compound args
	 */
	numops = 1;	/* CB_RECALL only */

	argoplist_size = numops * sizeof (nfs_cb_argop4);
	argop = kmem_zalloc(argoplist_size, KM_SLEEP);
	argop->argop = OP_CB_RECALL;
	rec_argp = &argop->nfs_cb_argop4_u.opcbrecall;

	(void) str_to_utf8("cb_recall", &cb4_args.tag);
	cb4_args.minorversion = CB4_MINOR_v0;
	/* cb4_args.callback_ident is set in rfs4_do_callback() */
	cb4_args.array_len = numops;
	cb4_args.array = argop;

	/*
	 * fill in the args struct
	 */
	bcopy(&dsp->rds_delegid.stateid, &rec_argp->stateid, sizeof (stateid4));
	rec_argp->truncate = trunc;

	fhp = &dsp->rds_finfo->rf_filehandle;
	rec_argp->fh.nfs_fh4_val = kmem_alloc(sizeof (char) *
	    fhp->nfs_fh4_len, KM_SLEEP);
	nfs_fh4_copy(fhp, &rec_argp->fh);

	/* Keep track of when we did this for observability */
	dsp->rds_time_recalled = gethrestime_sec();

	/*
	 * Set up the timeout for the callback and make the actual call.
	 * Timeout will be 80% of the lease period for this server.
	 */
	timeout.tv_sec = (dbe_to_instp(dsp->rds_dbe)->lease_period * 80) / 100;
	timeout.tv_usec = 0;

	DTRACE_NFSV4_3(cb__recall__start, rfs4_client_t *, dsp->rds_client,
	    rfs4_deleg_state_t *, dsp, CB_RECALL4args *, rec_argp);

	call_stat = rfs4_do_callback(dsp->rds_client, &cb4_args, &cb4_res,
	    timeout);

	rec_resp = (cb4_res.array_len == 0) ? NULL :
	    &cb4_res.array[0].nfs_cb_resop4_u.opcbrecall;

	DTRACE_NFSV4_3(cb__recall__done, rfs4_client_t *, dsp->rds_client,
	    rfs4_deleg_state_t *, dsp, CB_RECALL4res *, rec_resp);

	if (call_stat != RPC_SUCCESS || cb4_res.status != NFS4_OK) {
		rfs4_return_deleg(dsp, TRUE);
	}

	rfs4freeargres(&cb4_args, &cb4_res);
}

bool_t
rfs41_file_still_delegated(rfs4_deleg_state_t *dsp)
{
	rfs4_file_t	*fp;

	ASSERT(dsp != NULL);
	ASSERT(dsp->rds_finfo != NULL);
	fp = dsp->rds_finfo;

	/* do we have a delegation on this file? */
	rfs4_dbe_lock(fp->rf_dbe);
	if (fp->rf_dinfo->rd_dtype == OPEN_DELEGATE_NONE) {	/* check type */
		rfs4_dbe_unlock(fp->rf_dbe);
		return (FALSE);
	}

	/* check deleg cnt */
	if (list_next(&fp->rf_delegstatelist, dsp) == NULL) {
		rfs4_dbe_unlock(fp->rf_dbe);
		return (FALSE);
	}
	rfs4_dbe_unlock(fp->rf_dbe);
	return (TRUE);
}

void
rfs41_cb_seq_rcl_args(CB_SEQUENCE4args *ap, rfs4_deleg_state_t *dsp)
{
	referring_call_list4	*rp;
	referring_call4		*rcp;

	ASSERT(ap != NULL);

	/* construct one entry in referring_call_list4 */
	ap->csa_rcall_llen = 1;
	rp = (referring_call_list4 *)kmem_zalloc(sizeof (referring_call_list4),
	    KM_SLEEP);
	ap->csa_rcall_lval = rp;

	/* construct one referring_call4 entry in list above */
	rp->rcl_len = 1;
	rcp = (referring_call4 *)kmem_zalloc(sizeof (referring_call4),
	    KM_SLEEP);
	rp->rcl_val = rcp;

	/* set the necessary arg fields */
	bcopy(&dsp->rds_rs.sessid, &rp->rcl_sessionid, sizeof (sessionid4));
	rcp->rc_sequenceid = dsp->rds_rs.seqid;
	rcp->rc_slotid = dsp->rds_rs.slotno;
}

void
rfs41_cb_path_down(mds_session_t *sp, uint32_t sonly)
{
	uint32_t	cp_flag = SEQ4_STATUS_CB_PATH_DOWN;
	uint32_t	sn_flag = SEQ4_STATUS_CB_PATH_DOWN_SESSION;
	uint32_t	idx = log2(sn_flag);

	ASSERT(sp != NULL);
	ASSERT(sp->sn_clnt != NULL);

	/* NB - refcnt for both these bits == active cb connections */

	/* session */
	sp->sn_seq4[idx].ba_sonly = sonly;
	rfs41_seq4_rele(&sp->sn_seq4, sn_flag);

	/* clientid */
	rfs41_seq4_rele(&sp->sn_clnt->rc_seq4, cp_flag);
}

/*
 * Place the actual cb_recall otw call to client. (using slot_XXX api)
 */
/*ARGSUSED*/
void
mds_do_cb_recall(rfs4_deleg_state_t *dsp, bool_t trunc)
{
	CB_COMPOUND4args	cb4_args;
	CB_COMPOUND4res		cb4_res;
	CB_SEQUENCE4args	*cbsap;
	CB_RECALL4args		*cbrap;
	mds_session_t		*sp;
	slot_ent_t		*p;
	nfs_cb_argop4		*argops;
	int			numops;
	int			argoplist_size;
	struct timeval		timeout;
	nfs_fh4			*fhp;
	enum clnt_stat		call_stat = RPC_FAILED;
	int			zilch = 0;
	CLIENT			*ch;
	int			rcl = 0;	/* referring call list */
	int			retried = 0;
	uint32_t		sc = 0;		/* session ctxt */

	/*
	 * get the session id
	 */
	sp = mds_findsession_by_clid(dbe_to_instp(dsp->rds_dbe),
	    dsp->rds_client->rc_clientid);
	if (sp == NULL) {
		/*
		 * this shouldn't ever happen.  if it does, just
		 * increment a counter for now and return.
		 */
		mds_cbrecall_no_session++;
		return;
	}

	/*
	 * set up the compound args
	 */
	numops = 2;	/* CB_SEQUENCE + CB_RECALL */
	argoplist_size = numops * sizeof (nfs_cb_argop4);
	argops = kmem_zalloc(argoplist_size, KM_SLEEP);

	argops[0].argop = OP_CB_SEQUENCE;
	cbsap = &argops[0].nfs_cb_argop4_u.opcbsequence;

	argops[1].argop = OP_CB_RECALL;
	cbrap = &argops[1].nfs_cb_argop4_u.opcbrecall;

	(void) str_to_utf8("mds_cb_recall", &cb4_args.tag);
	cb4_args.minorversion = CB4_MINOR_v1;

	cb4_args.callback_ident = sp->sn_bc.progno;
	cb4_args.array_len = numops;
	cb4_args.array = argops;

	cb4_res.tag.utf8string_val = NULL;
	cb4_res.array = NULL;

	/*
	 * CB_SEQUENCE
	 */
	bcopy(sp->sn_sessid, cbsap->csa_sessionid, sizeof (sessionid4));
	p = svc_slot_alloc(sp);
	mutex_enter(&p->se_lock);
	cbsap->csa_slotid = p->se_sltno;
	cbsap->csa_sequenceid = p->se_seqid;
	cbsap->csa_highest_slotid = svc_slot_maxslot(sp);
	cbsap->csa_cachethis = FALSE;

	/*
	 * Section 2.10.5.3 (draft 23)
	 *
	 *		case description		refcnt
	 *	----------------------------------	------
	 * 1) rs state gets created (deleg granted) 	1
	 *    slot is reused				0
	 *
	 * 2) rs state gets created (deleg granted)	1
	 *    cb_seq, cb_recall				2
	 *    <-- client replies to cb_recall		1
	 *    eventually, slot is reused		0
	 *
	 * 3) rs state gets created (deleg granted)	1
	 *    cb_seq, cb_recall				2
	 *    eventually, slot is reused		1
	 *    <-- client replies to cb_recall		0
	 *
	 * Cases 2 & 3 are covered here; case 1 covered as
	 * part of a new request to op_sequence.
	 */
	if (dsp->rds_rs.refcnt == 0) {
		cbsap->csa_rcall_llen = 0;
		cbsap->csa_rcall_lval = NULL;
	} else {
		rfs41_deleg_rs_hold(dsp);
		rcl = 1;
		rfs41_cb_seq_rcl_args(cbsap, dsp);
	}
	mutex_exit(&p->se_lock);

	/*
	 * CB_RECALL
	 */
	bcopy(&dsp->rds_delegid.stateid, &cbrap->stateid, sizeof (stateid4));
	cbrap->truncate = trunc;
	fhp = &dsp->rds_finfo->rf_filehandle;
	cbrap->fh.nfs_fh4_val = kmem_alloc(sizeof (char) *
	    fhp->nfs_fh4_len, KM_SLEEP);
	nfs_fh4_copy(fhp, &cbrap->fh);

	/*
	 * Set up the timeout for the callback and make the actual call.
	 * Timeout will be 80% of the lease period for this server.
	 */
	dsp->rds_time_recalled = gethrestime_sec();	/* observability */
	timeout.tv_sec = (rfs4_lease_time * 80) / 100;
	timeout.tv_usec = 0;

retry:
	ch = rfs41_cb_getch(sp);
	(void) CLNT_CONTROL(ch, CLSET_XID, (char *)&zilch);
	call_stat = clnt_call(ch, CB_COMPOUND,
	    xdr_CB_COMPOUND4args_srv, (caddr_t)&cb4_args,
	    xdr_CB_COMPOUND4res, (caddr_t)&cb4_res, timeout);
	rfs41_cb_freech(sp, ch);

	/*
	 * If the back channel is down, then mark session(s) appropriately
	 * (SEQ4_STATUS_CB_PATH_DOWN). On NFS4ERR_DELAY, retry the callback
	 * after a lease period; if that _still_ results in an error, revoke
	 * the delegation and assert SEQ4_STATUS_RECALLABLE_STATE_REVOKED
	 * section 10.4.5 (draft-23). As per Section 8.3 (d23), it's up to
	 * the client to figure out 'which' stateid got revoked.
	 */
	if (call_stat != RPC_SUCCESS) {
		if (!retried)
			delay(SEC_TO_TICK(rfs4_lease_time));

		if (rfs41_file_still_delegated(dsp)) {
			if (!retried) {
				retried = 1;
				goto retry;
			}

			/*
			 * We want to make sure that the delegation is
			 * still valid lest we assert a SEQ4 flag that
			 * will never be turned off.
			 */
			rfs41_revoke_deleg(dsp);
		}
		sc = (call_stat == RPC_CANTSEND || call_stat == RPC_CANTRECV);
		rfs41_cb_path_down(sp, sc);
		goto done;

	} else if (cb4_res.status != NFS4_OK) {
		switch (cb4_res.status) {
		case NFS4ERR_BADHANDLE:
		case NFS4ERR_BADXDR:
		case NFS4ERR_OP_NOT_IN_SESSION:
		case NFS4ERR_REQ_TOO_BIG:
		case NFS4ERR_TOO_MANY_OPS:
			/* What do we do when it's our own fault ? */
			break;

		/* XXX - rick: NFS4ERR_BAD_STATEID should also retry */
		/* case NFS4ERR_BAD_STATEID: */
		case NFS4ERR_DELAY:
			if (!retried)
				delay(SEC_TO_TICK(rfs4_lease_time));

			if (!rfs41_file_still_delegated(dsp))
				break;

			if (!retried) {
				retried = 1;
				goto retry;
			}
			/* FALLTHROUGH */

		case NFS4ERR_BAD_STATEID:	/* XXX see above */
		default:
			if (rfs41_file_still_delegated(dsp))
				rfs41_revoke_deleg(dsp);
			break;
		}
	}
	svc_slot_cb_seqid(&cb4_res, p);
done:
	if (rcl)
		rfs41_deleg_rs_rele(dsp);
	svc_slot_free(sp, p);

	rfs4freeargres(&cb4_args, &cb4_res);
	rfs41_session_rele(sp);
}

struct recall_arg {
	rfs4_deleg_state_t *dsp;
	void (*recall)(rfs4_deleg_state_t *, bool_t trunc);
	bool_t trunc;
};

static void
do_recall(struct recall_arg *arg)
{
	rfs4_deleg_state_t *dsp = arg->dsp;
	rfs4_file_t *fp = dsp->rds_finfo;
	callb_cpr_t cpr_info;
	kmutex_t cpr_lock;

	mutex_init(&cpr_lock, NULL, MUTEX_DEFAULT, NULL);
	CALLB_CPR_INIT(&cpr_info, &cpr_lock, callb_generic_cpr, "nfsv4Recall");

	/*
	 * It is possible that before this thread starts
	 * the client has send us a return_delegation, and
	 * if that is the case we do not need to send the
	 * recall callback.
	 */
	if (dsp->rds_dtype != OPEN_DELEGATE_NONE) {
		DTRACE_PROBE3(nfss__i__recall,
		    struct recall_arg *, arg,
		    struct rfs4_deleg_state_t *, dsp,
		    struct rfs4_file_t *, fp);

		if (arg->recall)
			(void) (*arg->recall)(dsp, arg->trunc);
	}

	mutex_enter(fp->rf_dinfo->rd_recall_lock);
	/*
	 * Recall count may go negative if the parent thread that is
	 * creating the individual callback threads does not modify
	 * the recall_count field before the callback thread actually
	 * gets a response from the CB_RECALL
	 */
	fp->rf_dinfo->rd_recall_count--;
	if (fp->rf_dinfo->rd_recall_count == 0)
		cv_signal(fp->rf_dinfo->rd_recall_cv);
	mutex_exit(fp->rf_dinfo->rd_recall_lock);

	mutex_enter(&cpr_lock);
	CALLB_CPR_EXIT(&cpr_info);
	mutex_destroy(&cpr_lock);

	rfs4_deleg_state_rele(dsp); /* release the hold for this thread */

	kmem_free(arg, sizeof (struct recall_arg));
}

struct master_recall_args {
    rfs4_file_t *fp;
    void (*recall)(rfs4_deleg_state_t *, bool_t);
    bool_t trunc;
};

static void
do_recall_file(struct master_recall_args *map)
{
	rfs4_file_t *fp = map->fp;
	rfs4_deleg_state_t *dsp;
	struct recall_arg *arg;
	callb_cpr_t cpr_info;
	kmutex_t cpr_lock;
	int32_t recall_count;
	nfs_server_instance_t *instp;

	rfs4_dbe_lock(fp->rf_dbe);

	/* Recall already in progress ? */
	mutex_enter(fp->rf_dinfo->rd_recall_lock);
	if (fp->rf_dinfo->rd_recall_count != 0) {
		mutex_exit(fp->rf_dinfo->rd_recall_lock);
		rfs4_dbe_rele_nolock(fp->rf_dbe);
		rfs4_dbe_unlock(fp->rf_dbe);
		kmem_free(map, sizeof (struct master_recall_args));
		return;
	}

	mutex_exit(fp->rf_dinfo->rd_recall_lock);

	instp = dbe_to_instp(fp->rf_dbe);

	mutex_init(&cpr_lock, NULL, MUTEX_DEFAULT, NULL);
	CALLB_CPR_INIT(&cpr_info, &cpr_lock, callb_generic_cpr,	"v4RecallFile");

	/*
	 * iterate over the file delegation list and
	 * recall..
	 */
	recall_count = 0;
	for (dsp = list_head(&fp->rf_delegstatelist); dsp != NULL;
	    dsp = list_next(&fp->rf_delegstatelist, dsp)) {

		rfs4_dbe_lock(dsp->rds_dbe);
		/*
		 * if this delegation state
		 * is being reaped skip it
		 */
		if (rfs4_dbe_is_invalid(dsp->rds_dbe)) {
			rfs4_dbe_unlock(dsp->rds_dbe);
			continue;
		}

		/* hold for receiving thread */
		rfs4_dbe_hold(dsp->rds_dbe);
		rfs4_dbe_unlock(dsp->rds_dbe);

		arg = kmem_alloc(sizeof (struct recall_arg), KM_SLEEP);
		arg->recall = instp->deleg_cbrecall;
		arg->trunc = map->trunc;
		arg->dsp = dsp;

		recall_count++;

		(void) thread_create(NULL, 0, do_recall, arg, 0, &p0, TS_RUN,
		    minclsyspri);
	}

	rfs4_dbe_unlock(fp->rf_dbe);

	mutex_enter(fp->rf_dinfo->rd_recall_lock);
	/*
	 * Recall count may go negative if the parent thread that is
	 * creating the individual callback threads does not modify
	 * the recall_count field before the callback thread actually
	 * gets a response from the CB_RECALL
	 */
	fp->rf_dinfo->rd_recall_count += recall_count;
	while (fp->rf_dinfo->rd_recall_count)
		cv_wait(fp->rf_dinfo->rd_recall_cv,
		    fp->rf_dinfo->rd_recall_lock);

	mutex_exit(fp->rf_dinfo->rd_recall_lock);

	DTRACE_PROBE1(nfss__i__recall_done, rfs4_file_t *, fp);
	rfs4_file_rele(fp);
	kmem_free(map, sizeof (struct master_recall_args));
	mutex_enter(&cpr_lock);
	CALLB_CPR_EXIT(&cpr_info);
	mutex_destroy(&cpr_lock);
}

static void
rfs4_recall_file(rfs4_file_t *fp, bool_t trunc, rfs4_client_t *cp)
{
	struct master_recall_args *args;

	rfs4_dbe_lock(fp->rf_dbe);
	if (fp->rf_dinfo->rd_dtype == OPEN_DELEGATE_NONE) {
		rfs4_dbe_unlock(fp->rf_dbe);
		return;
	}
	rfs4_dbe_hold(fp->rf_dbe);	/* hold for new thread */

	/*
	 * Mark the time we started the recall processing.
	 * If it has been previously recalled, do not reset the
	 * timer since this is used for the revocation decision.
	 */
	if (fp->rf_dinfo->rd_time_recalled == 0)
		fp->rf_dinfo->rd_time_recalled = gethrestime_sec();
	fp->rf_dinfo->rd_ever_recalled = TRUE; /* used for policy decision */
	/* Client causing recall not always available */
	if (cp)
		fp->rf_dinfo->rd_conflicted_client = cp->rc_clientid;

	rfs4_dbe_unlock(fp->rf_dbe);

	args = kmem_alloc(sizeof (struct master_recall_args), KM_SLEEP);
	args->fp = fp;
	args->recall = NULL;
	args->trunc = trunc;

	(void) thread_create(NULL, 0, do_recall_file, args, 0, &p0, TS_RUN,
	    minclsyspri);
}

void
rfs4_recall_deleg(rfs4_file_t *fp, bool_t trunc, rfs4_client_t *cp)
{
	time_t elapsed1, elapsed2;
	time_t lease;

	lease = dbe_to_instp(fp->rf_dbe)->lease_period;

	if (fp->rf_dinfo->rd_time_recalled != 0) {
		elapsed1 = gethrestime_sec() - fp->rf_dinfo->rd_time_recalled;
		elapsed2 = gethrestime_sec() - fp->rf_dinfo->rd_time_lastwrite;

		/* First check to see if a revocation should occur */
		if (elapsed1 > lease && elapsed2 > lease) {
			rfs4_revoke_file(fp);
			return;
		}
		/*
		 * Next check to see if a recall should be done again
		 * so quickly.
		 */
		if (elapsed1 <= ((lease * 20) / 100))
			return;
	}
	rfs4_recall_file(fp, trunc, cp);
}

/*
 * rfs4_check_recall is called from rfs4_do_open to determine if the current
 * open conflicts with the delegation.
 * Return true if we need recall otherwise false.
 * Assumes entry locks for sp and sp->rs_finfo are held.
 */
bool_t
rfs4_check_recall(rfs4_state_t *sp, uint32_t access)
{
	open_delegation_type4 dtype = sp->rs_finfo->rf_dinfo->rd_dtype;

	switch (dtype) {
	case OPEN_DELEGATE_NONE:
		/* Not currently delegated so there is nothing to do */
		return (FALSE);
	case OPEN_DELEGATE_READ:
		/*
		 * If the access is only asking for READ then there is
		 * no conflict and nothing to do.  If it is asking
		 * for write, then there will be conflict and the read
		 * delegation should be recalled.
		 */
		if (access == OPEN4_SHARE_ACCESS_READ)
			return (FALSE);
		else
			return (TRUE);
	case OPEN_DELEGATE_WRITE:
		/* Check to see if this client has the delegation */
		return (rfs4_is_deleg(sp));
	}

	return (FALSE);
}

/*
 * Return the "best" allowable delegation available given the current
 * delegation type and the desired access and deny modes on the file.
 * At the point that this routine is called we know that the access and
 * deny modes are consistent with the file modes.
 */
static open_delegation_type4
rfs4_check_delegation(rfs4_state_t *sp, rfs4_file_t *fp)
{
	open_delegation_type4 dtype = fp->rf_dinfo->rd_dtype;
	uint32_t access = sp->rs_share_access;
	uint32_t deny = sp->rs_share_deny;
	int readcnt = 0;
	int writecnt = 0;

	switch (dtype) {
	case OPEN_DELEGATE_NONE:
		/*
		 * Determine if more than just this OPEN have the file
		 * open and if so, no delegation may be provided to
		 * the client.
		 */
		if (access & OPEN4_SHARE_ACCESS_WRITE)
			writecnt++;
		if (access & OPEN4_SHARE_ACCESS_READ)
			readcnt++;

		if (fp->rf_access_read > readcnt ||
		    fp->rf_access_write > writecnt)
			return (OPEN_DELEGATE_NONE);

		/*
		 * If the client is going to write, or if the client
		 * has exclusive access, return a write delegation.
		 */
		if ((access & OPEN4_SHARE_ACCESS_WRITE) ||
		    (deny & (OPEN4_SHARE_DENY_READ | OPEN4_SHARE_DENY_WRITE)))
			return (OPEN_DELEGATE_WRITE);
		/*
		 * If we don't want to write or we've haven't denied read
		 * access to others, return a read delegation.
		 */
		if ((access & ~OPEN4_SHARE_ACCESS_WRITE) ||
		    (deny & ~OPEN4_SHARE_DENY_READ))
			return (OPEN_DELEGATE_READ);

		/* Shouldn't get here */
		return (OPEN_DELEGATE_NONE);

	case OPEN_DELEGATE_READ:
		/*
		 * If the file is delegated for read but we wan't to
		 * write or deny others to read then we can't delegate
		 * the file. We shouldn't get here since the delegation should
		 * have been recalled already.
		 */
		if ((access & OPEN4_SHARE_ACCESS_WRITE) ||
		    (deny & OPEN4_SHARE_DENY_READ))
			return (OPEN_DELEGATE_NONE);
		return (OPEN_DELEGATE_READ);

	case OPEN_DELEGATE_WRITE:
		return (OPEN_DELEGATE_WRITE);
	}

	/* Shouldn't get here */
	return (OPEN_DELEGATE_NONE);
}

/*
 * Given the desired delegation type and the "history" of the file
 * determine the actual delegation type to return.
 */
static open_delegation_type4
rfs4_delegation_policy(nfs_server_instance_t *instp,
	open_delegation_type4 dtype,
	rfs4_dinfo_t *dinfo, clientid4 cid)
{
	time_t elapsed;

	if (instp->deleg_policy != SRV_NORMAL_DELEGATE)
		return (OPEN_DELEGATE_NONE);

	/*
	 * Has this file/delegation ever been recalled?  If not then
	 * no further checks for a delegation race need to be done.
	 * However if a recall has occurred, then check to see if a
	 * client has caused its own delegation recall to occur.  If
	 * not, then has a delegation for this file been returned
	 * recently?  If so, then do not assign a new delegation to
	 * avoid a "delegation race" between the original client and
	 * the new/conflicting client.
	 */
	if (dinfo->rd_ever_recalled == TRUE) {
		if (dinfo->rd_conflicted_client != cid) {
			elapsed = gethrestime_sec() - dinfo->rd_time_returned;
			if (elapsed < instp->lease_period)
				return (OPEN_DELEGATE_NONE);
		}
	}

	/* Limit the number of read grants */
	if (dtype == OPEN_DELEGATE_READ &&
	    dinfo->rd_rdgrants > MAX_READ_DELEGATIONS)
		return (OPEN_DELEGATE_NONE);

	/*
	 * Should consider limiting total number of read/write
	 * delegations the server will permit.
	 */

	return (dtype);
}

/*
 * Try and grant a delegation for an open give the state. The routine
 * returns the delegation type granted. This could be OPEN_DELEGATE_NONE.
 *
 * The state and associate file entry must be locked
 */
rfs4_deleg_state_t *
rfs4_grant_delegation(struct compound_state *cs,
		delegreq_t dreq, rfs4_state_t *sp, int *recall)
{
	rfs4_file_t *fp = sp->rs_finfo;
	open_delegation_type4 dtype;
	int no_delegation;

	ASSERT(rfs4_dbe_islocked(sp->rs_dbe));
	ASSERT(rfs4_dbe_islocked(fp->rf_dbe));

	/* Is the server even providing delegations? */
	if (cs->instp->deleg_policy == SRV_NEVER_DELEGATE || dreq == DELEG_NONE)
		return (NULL);

	/* Check to see if delegations have been temporarily disabled */
	mutex_enter(&cs->instp->deleg_lock);
	no_delegation = cs->instp->deleg_disabled;
	mutex_exit(&cs->instp->deleg_lock);

	if (no_delegation)
		return (NULL);

	/* Don't grant a delegation if a deletion is impending. */
	if (fp->rf_dinfo->rd_hold_grant > 0) {
		return (NULL);
	}

	/*
	 * Don't grant a delegation if there are any lock manager
	 * (NFSv2/v3) locks for the file.  This is a bit of a hack (e.g.,
	 * if there are only read locks we should be able to grant a
	 * read-only delegation), but it's good enough for now.
	 *
	 * MT safety: the lock manager checks for conflicting delegations
	 * before processing a lock request.  That check will block until
	 * we are done here.  So if the lock manager acquires a lock after
	 * we decide to grant the delegation, the delegation will get
	 * immediately recalled (if there's a conflict), so we're safe.
	 */
	if (lm_vp_active(fp->rf_vp)) {
		return (NULL);
	}

	/*
	 * Based on the type of delegation request passed in, take the
	 * appropriate action (DELEG_NONE is handled above)
	 */
	switch (dreq) {

	case DELEG_READ:
	case DELEG_WRITE:
		/*
		 * The server "must" grant the delegation in this case.
		 * Client is using open previous
		 */
		dtype = (open_delegation_type4)dreq;
		*recall = 1;
		break;
	case DELEG_ANY:
		/*
		 * If a valid callback path does not exist, no delegation may
		 * be granted.
		 */
		if ((*cs->instp->deleg_cbcheck)(sp) != CB_OK)
			return (NULL);

		/*
		 * If the original operation which caused time_rm_delayed
		 * to be set hasn't been retried and completed for one
		 * full lease period, clear it and allow delegations to
		 * get granted again.
		 */
		if (fp->rf_dinfo->rd_time_rm_delayed > 0 &&
		    gethrestime_sec() >
		    fp->rf_dinfo->rd_time_rm_delayed + cs->instp->lease_period)
			fp->rf_dinfo->rd_time_rm_delayed = 0;

		/*
		 * If we are waiting for a delegation to be returned then
		 * don't delegate this file. We do this for correctness as
		 * well as if the file is being recalled we would likely
		 * recall this file again.
		 */

		if (fp->rf_dinfo->rd_time_recalled != 0 ||
		    fp->rf_dinfo->rd_time_rm_delayed != 0)
			return (NULL);

		/* Get the "best" delegation candidate */
		dtype = rfs4_check_delegation(sp, fp);

		if (dtype == OPEN_DELEGATE_NONE)
			return (NULL);

		/*
		 * Based on policy and the history of the file get the
		 * actual delegation.
		 */
		dtype = rfs4_delegation_policy(cs->instp, dtype, fp->rf_dinfo,
		    sp->rs_owner->ro_client->rc_clientid);

		if (dtype == OPEN_DELEGATE_NONE)
			return (NULL);
		break;
	default:
		return (NULL);
	}

	/* set the delegation for the state */
	return (rfs4_deleg_state(cs, sp, dtype, recall));
}

void
rfs4_set_deleg_response(rfs4_deleg_state_t *dsp, open_delegation4 *dp,
    nfsace4 *ace,  int recall)
{
	open_write_delegation4 *wp;
	open_read_delegation4 *rp;
	nfs_space_limit4 *spl;
	nfsace4 nace;

	/*
	 * We need to allocate a new copy of the who string.
	 * this string will be freed by the rfs4_op_open dis_resfree
	 * routine. We need to do this allocation since replays will
	 * be allocated and rfs4_compound can't tell the difference from
	 * a replay and an inital open. N.B. if an ace is passed in, it
	 * the caller's responsibility to free it.
	 */

	if (ace == NULL) {
		/*
		 * Default is to deny all access, the client will have
		 * to contact the server.  XXX Do we want to actually
		 * set a deny for every one, or do we simply want to
		 * construct an entity that will match no one?
		 */
		nace.type = ACE4_ACCESS_DENIED_ACE_TYPE;
		nace.flag = 0;
		nace.access_mask = ACE4_VALID_MASK_BITS;
		(void) str_to_utf8(ACE4_WHO_EVERYONE, &nace.who);
	} else {
		nace.type = ace->type;
		nace.flag = ace->flag;
		nace.access_mask = ace->access_mask;
		(void) utf8_copy(&ace->who, &nace.who);
	}

	dp->delegation_type = dsp->rds_dtype;

	switch (dsp->rds_dtype) {
	case OPEN_DELEGATE_NONE:
		break;
	case OPEN_DELEGATE_READ:
		rp = &dp->open_delegation4_u.read;
		rp->stateid = dsp->rds_delegid.stateid;
		rp->recall = (bool_t)recall;
		rp->permissions = nace;
		break;
	case OPEN_DELEGATE_WRITE:
		wp = &dp->open_delegation4_u.write;
		wp->stateid = dsp->rds_delegid.stateid;
		wp->recall = (bool_t)recall;
		spl = &wp->space_limit;
		spl->limitby = NFS_LIMIT_SIZE;
		spl->nfs_space_limit4_u.filesize = 0;
		wp->permissions = nace;
		break;
	}
}

/*
 * Check if the file is delegated via the provided file struct.
 * Return TRUE if it is delegated.  This is intended for use by
 * the v4 server.  The v2/v3 server code should use rfs4_check_delegated().
 *
 * Note that if the file is found to have a delegation, it is
 * recalled, unless the clientid of the caller matches the clientid of the
 * delegation. If the caller has specified, there is a slight delay
 * inserted in the hopes that the delegation will be returned quickly.
 */
bool_t
rfs4_check_delegated_byfp(nfs_server_instance_t *instp,
    int mode, rfs4_file_t *fp, bool_t trunc, bool_t do_delay,
    bool_t is_rm, clientid4 *cp)
{
	rfs4_deleg_state_t *dsp;

	/* Is delegation enabled? */
	if (instp->deleg_policy == SRV_NEVER_DELEGATE)
		return (FALSE);

	/* do we have a delegation on this file? */
	rfs4_dbe_lock(fp->rf_dbe);
	if (fp->rf_dinfo->rd_dtype == OPEN_DELEGATE_NONE) {
		if (is_rm)
			fp->rf_dinfo->rd_hold_grant++;
		rfs4_dbe_unlock(fp->rf_dbe);
		return (FALSE);
	}
	/*
	 * do we have a write delegation on this file or are we
	 * requesting write access to a file with any type of existing
	 * delegation?
	 */
	if (mode == FWRITE || fp->rf_dinfo->rd_dtype == OPEN_DELEGATE_WRITE) {
		if (cp != NULL) {
			dsp = list_head(&fp->rf_delegstatelist);
			if (dsp == NULL) {
				rfs4_dbe_unlock(fp->rf_dbe);
				return (FALSE);
			}
			/*
			 * Does the requestor already own the delegation?
			 */
			if (dsp->rds_client->rc_clientid == *(cp)) {
				rfs4_dbe_unlock(fp->rf_dbe);
				return (FALSE);
			}
		}

		rfs4_dbe_unlock(fp->rf_dbe);
		rfs4_recall_deleg(fp, trunc, NULL);

		if (!do_delay) {
			rfs4_dbe_lock(fp->rf_dbe);
			fp->rf_dinfo->rd_time_rm_delayed = gethrestime_sec();
			rfs4_dbe_unlock(fp->rf_dbe);
			return (TRUE);
		}

		delay(NFS4_DELEGATION_CONFLICT_DELAY);

		rfs4_dbe_lock(fp->rf_dbe);
		if (fp->rf_dinfo->rd_dtype != OPEN_DELEGATE_NONE) {
			fp->rf_dinfo->rd_time_rm_delayed = gethrestime_sec();
			rfs4_dbe_unlock(fp->rf_dbe);
			return (TRUE);
		}
	}
	if (is_rm)
		fp->rf_dinfo->rd_hold_grant++;
	rfs4_dbe_unlock(fp->rf_dbe);
	return (FALSE);
}


/*
 * Check if the file is delegated in the case of a v2 or v3 access.
 * Return TRUE if it is delegated which in turn means that v2 should
 * drop the request and in the case of v3 JUKEBOX should be returned.
 */
bool_t
rfs4_check_delegated(int mode, vnode_t *vp, bool_t trunc, bool_t do_delay,
    bool_t is_rm, void *vcp)
{
	int    delegd = 0;
	bool_t create = FALSE;
	rfs4_file_t *fp;
	clientid4 *cp = (clientid4 *)vcp;
	nfs_server_instance_t *nsip;

	/* iterrate through the instances */
	rw_enter(&nsi_lock, RW_READER);
	for (nsip = list_head(&nsi_head);
	    nsip != NULL && !delegd;
	    nsip = list_next(&nsi_head, &nsip->nsi_list)) {

		mutex_enter(&nsip->state_lock);

		if ((nsip->inst_flags & NFS_INST_STORE_INIT) &&
		    (nsip->deleg_policy != SRV_NEVER_DELEGATE)) {

			fp = rfs4_findfile(nsip, vp, NULL, &create);
			if (fp != NULL) {
				if (rfs4_check_delegated_byfp(nsip, mode, fp,
				    trunc, do_delay, is_rm, cp))
					delegd++;
				rfs4_file_rele(fp);
			}
		}
		mutex_exit(&nsip->state_lock);
	}
	rw_exit(&nsi_lock);
	return (delegd ? TRUE : FALSE);
}

/*
 * Release a hold on the hold_grant counter which
 * prevents delegation from being granted while a remove
 * or a rename is in progress.
 */
void
rfs4_clear_dont_grant(nfs_server_instance_t *instp,
		rfs4_file_t *fp)
{
	if (instp->deleg_policy == SRV_NEVER_DELEGATE)
		return;
	rfs4_dbe_lock(fp->rf_dbe);
	ASSERT(fp->rf_dinfo->rd_hold_grant > 0);
	fp->rf_dinfo->rd_hold_grant--;
	fp->rf_dinfo->rd_time_rm_delayed = 0;
	rfs4_dbe_unlock(fp->rf_dbe);
}

/*
 * State support for delegation.
 * Set the state delegation type for this state;
 * This routine is called from open via rfs4_grant_delegation and the entry
 * locks on sp and sp->rs_finfo are assumed.
 */
static rfs4_deleg_state_t *
rfs4_deleg_state(struct compound_state *cs,
		rfs4_state_t *sp, open_delegation_type4 dtype, int *recall)
{
	rfs4_file_t *fp = sp->rs_finfo;
	bool_t create = TRUE;
	rfs4_deleg_state_t *dsp;
	vnode_t *vp;
	int open_prev = *recall;
	int ret;
	int fflags = 0;

	ASSERT(rfs4_dbe_islocked(sp->rs_dbe));
	ASSERT(rfs4_dbe_islocked(fp->rf_dbe));

	/* Shouldn't happen */
	if (fp->rf_dinfo->rd_recall_count != 0 ||
	    (fp->rf_dinfo->rd_dtype == OPEN_DELEGATE_READ &&
	    dtype != OPEN_DELEGATE_READ)) {
		return (NULL);
	}

	/* Unlock to avoid deadlock */
	rfs4_dbe_unlock(fp->rf_dbe);
	rfs4_dbe_unlock(sp->rs_dbe);

	dsp = rfs4_finddeleg(cs, sp, &create);

	rfs4_dbe_lock(sp->rs_dbe);
	rfs4_dbe_lock(fp->rf_dbe);

	if (dsp == NULL)
		return (NULL);

	/*
	 * It is possible that since we dropped the lock
	 * in order to call finddeleg, the rfs4_file_t
	 * was marked such that we should not grant a
	 * delegation, if so bail out.
	 */
	if (fp->rf_dinfo->rd_hold_grant > 0) {
		rfs4_deleg_state_rele(dsp);
		return (NULL);
	}

	if (create == FALSE) {
		if (sp->rs_owner->ro_client == dsp->rds_client &&
		    dsp->rds_dtype == dtype) {
			return (dsp);
		} else {
			rfs4_deleg_state_rele(dsp);
			return (NULL);
		}
	}

	/*
	 * Check that this file has not been delegated to another
	 * client
	 */
	if (fp->rf_dinfo->rd_recall_count != 0 ||
	    fp->rf_dinfo->rd_dtype == OPEN_DELEGATE_WRITE ||
	    (fp->rf_dinfo->rd_dtype == OPEN_DELEGATE_READ &&
	    dtype != OPEN_DELEGATE_READ)) {
		rfs4_deleg_state_rele(dsp);
		return (NULL);
	}

	vp = fp->rf_vp;
	/* vnevent_support returns 0 if file system supports vnevents */
	if (vnevent_support(vp, NULL)) {
		rfs4_deleg_state_rele(dsp);
		return (NULL);
	}

	/* Calculate the fflags for this OPEN. */
	if (sp->rs_share_access & OPEN4_SHARE_ACCESS_READ)
		fflags |= FREAD;
	if (sp->rs_share_access & OPEN4_SHARE_ACCESS_WRITE)
		fflags |= FWRITE;

	*recall = 0;
	/*
	 * Before granting a delegation we need to know if anyone else has
	 * opened the file in a conflicting mode.  However, first we need to
	 * know how we opened the file to check the counts properly.
	 */
	if (dtype == OPEN_DELEGATE_READ) {
		if (((fflags & FWRITE) && vn_has_other_opens(vp, V_WRITE)) ||
		    (((fflags & FWRITE) == 0) && vn_is_opened(vp, V_WRITE)) ||
		    vn_is_mapped(vp, V_WRITE)) {
			if (open_prev) {
				*recall = 1;
			} else {
				rfs4_deleg_state_rele(dsp);
				return (NULL);
			}
		}
		ret = fem_install(vp, cs->instp->deleg_rdops, (void *)fp,
		    OPARGUNIQ, rfs4_mon_hold, rfs4_mon_rele);
		if (((fflags & FWRITE) && vn_has_other_opens(vp, V_WRITE)) ||
		    (((fflags & FWRITE) == 0) && vn_is_opened(vp, V_WRITE)) ||
		    vn_is_mapped(vp, V_WRITE)) {
			if (open_prev) {
				*recall = 1;
			} else {
				(void) fem_uninstall(vp, cs->instp->deleg_rdops,
				    (void *)fp);
				rfs4_deleg_state_rele(dsp);
				return (NULL);
			}
		}
		/*
		 * Because a client can hold onto a delegation after the
		 * file has been closed, we need to keep track of the
		 * access to this file.  Otherwise the CIFS server would
		 * not know about the client accessing the file and could
		 * inappropriately grant an OPLOCK.
		 * fem_install() returns EBUSY when asked to install a
		 * OPARGUNIQ monitor more than once.  Therefore, check the
		 * return code because we only want this done once.
		 */
		if (ret == 0)
			vn_open_upgrade(vp, FREAD);
	} else { /* WRITE */
		if (((fflags & FWRITE) && vn_has_other_opens(vp, V_WRITE)) ||
		    (((fflags & FWRITE) == 0) && vn_is_opened(vp, V_WRITE)) ||
		    ((fflags & FREAD) && vn_has_other_opens(vp, V_READ)) ||
		    (((fflags & FREAD) == 0) && vn_is_opened(vp, V_READ)) ||
		    vn_is_mapped(vp, V_RDORWR)) {
			if (open_prev) {
				*recall = 1;
			} else {
				rfs4_deleg_state_rele(dsp);
				return (NULL);
			}
		}
		ret = fem_install(vp, cs->instp->deleg_wrops, (void *)fp,
		    OPARGUNIQ, rfs4_mon_hold, rfs4_mon_rele);
		if (((fflags & FWRITE) && vn_has_other_opens(vp, V_WRITE)) ||
		    (((fflags & FWRITE) == 0) && vn_is_opened(vp, V_WRITE)) ||
		    ((fflags & FREAD) && vn_has_other_opens(vp, V_READ)) ||
		    (((fflags & FREAD) == 0) && vn_is_opened(vp, V_READ)) ||
		    vn_is_mapped(vp, V_RDORWR)) {
			if (open_prev) {
				*recall = 1;
			} else {
				(void) fem_uninstall(vp, cs->instp->deleg_wrops,
				    (void *)fp);
				rfs4_deleg_state_rele(dsp);
				return (NULL);
			}
		}
		/*
		 * Because a client can hold onto a delegation after the
		 * file has been closed, we need to keep track of the
		 * access to this file.  Otherwise the CIFS server would
		 * not know about the client accessing the file and could
		 * inappropriately grant an OPLOCK.
		 * fem_install() returns EBUSY when asked to install a
		 * OPUNIQ monitor more than once.  Therefore, check the
		 * return code because we only want this done once.
		 */
		if (ret == 0)
			vn_open_upgrade(vp, FREAD|FWRITE);
	}

	/*
	 * Place on delegation list for file
	 */
	ASSERT(!list_link_active(&dsp->rds_node));
	list_insert_tail(&fp->rf_delegstatelist, dsp);

	dsp->rds_dtype = fp->rf_dinfo->rd_dtype = dtype;

	/* Update delegation stats for this file */
	fp->rf_dinfo->rd_time_lastgrant = gethrestime_sec();

	/* reset since this is a new delegation */
	fp->rf_dinfo->rd_conflicted_client = 0;
	fp->rf_dinfo->rd_ever_recalled = FALSE;

	if (dtype == OPEN_DELEGATE_READ)
		fp->rf_dinfo->rd_rdgrants++;
	else
		fp->rf_dinfo->rd_wrgrants++;

	return (dsp);
}

/*
 * State routine for the server when a delegation is returned.
 */
void
rfs4_return_deleg(rfs4_deleg_state_t *dsp, bool_t revoked)
{
	rfs4_file_t *fp = dsp->rds_finfo;
	open_delegation_type4 dtypewas;
	nfs_server_instance_t *instp;

	rfs4_dbe_lock(fp->rf_dbe);

	/* nothing to do if no longer on list */
	if (!list_link_active(&dsp->rds_node)) {
		rfs4_dbe_unlock(fp->rf_dbe);
		return;
	}

	/* Remove state from recall list */
	list_remove(&fp->rf_delegstatelist, dsp);

	instp = dbe_to_instp(fp->rf_dbe);
	if (instp->inst_flags & NFS_INST_v41) {
		mds_session_t	*sp;
		slotid4		 slot;
		slot_ent_t	*slp;
		extern void rfs41_rs_erase(void *);

		if (dsp->rds_rs.refcnt > 0) {
			/*
			 * refcnt > 0, so this means we still have an active
			 * hold on deleg_state. If (for some reason) we don't
			 * find the sp, the worse that'll happen is that we'll
			 * leak some state (ie. won't be able to clean up the
			 * hold). But nothing to get too excited about.
			 */
			slot = dsp->rds_rs.slotno;
			sp = mds_findsession_by_id(instp, dsp->rds_rs.sessid);
			if (sp != NULL) {
				rfs4_dbe_lock(sp->sn_dbe);
				ASSERT(sp->sn_replay != NULL);
				slp = slrc_slot_get(sp->sn_replay, slot);
				if (slp->se_p == dsp) {
					rfs41_rs_erase(dsp);
					slp->se_p = NULL;
				}
				rfs4_dbe_unlock(sp->sn_dbe);
				rfs41_session_rele(sp);
			}
		}
	}

	/*
	 * If no more delegations then remove the FEM
	 * monitors
	 */
	if (list_is_empty(&fp->rf_delegstatelist)) {
		dtypewas = fp->rf_dinfo->rd_dtype;
		fp->rf_dinfo->rd_dtype = OPEN_DELEGATE_NONE;
		rfs4_dbe_cv_broadcast(fp->rf_dbe);

		/* if file system was unshared, the vp will be NULL */
		if (fp->rf_vp != NULL) {
			/*
			 * Once a delegation is no longer held by any client,
			 * the monitor is uninstalled.  At this point, the
			 * client must send OPEN otw, so we don't need the
			 * reference on the vnode anymore.  The open
			 * downgrade removes the reference put on earlier.
			 */
			if (dtypewas == OPEN_DELEGATE_READ) {
				(void) fem_uninstall(fp->rf_vp,
				    instp->deleg_rdops, (void *)fp);
				vn_open_downgrade(fp->rf_vp, FREAD);
			} else if (dtypewas == OPEN_DELEGATE_WRITE) {
				(void) fem_uninstall(fp->rf_vp,
				    instp->deleg_wrops, (void *)fp);
				vn_open_downgrade(fp->rf_vp, FREAD|FWRITE);
			}
		}
	}

	switch (dsp->rds_dtype) {
	case OPEN_DELEGATE_READ:
		fp->rf_dinfo->rd_rdgrants--;
		break;
	case OPEN_DELEGATE_WRITE:
		fp->rf_dinfo->rd_wrgrants--;
		break;
	default:
		break;
	}

	/* used in the policy decision */
	fp->rf_dinfo->rd_time_returned = gethrestime_sec();

	/*
	 * reset the time_recalled field so future delegations are not
	 * accidentally revoked
	 */
	if ((fp->rf_dinfo->rd_rdgrants + fp->rf_dinfo->rd_wrgrants) == 0)
		fp->rf_dinfo->rd_time_recalled = 0;

	rfs4_dbe_unlock(fp->rf_dbe);

	rfs4_dbe_lock(dsp->rds_dbe);

	dsp->rds_dtype = OPEN_DELEGATE_NONE;

	if (revoked == TRUE)
		dsp->rds_time_revoked = gethrestime_sec();

	rfs4_dbe_invalidate(dsp->rds_dbe);

	rfs4_dbe_unlock(dsp->rds_dbe);

	if (revoked == TRUE) {
		rfs4_dbe_lock(dsp->rds_client->rc_dbe);
		dsp->rds_client->rc_deleg_revoked++;	/* observability */
		rfs4_dbe_unlock(dsp->rds_client->rc_dbe);
	}
}

static void
rfs4_revoke_deleg(rfs4_deleg_state_t *dsp)
{
	rfs4_return_deleg(dsp, TRUE);
}

static void
rfs41_revoke_deleg(rfs4_deleg_state_t *dsp)
{
	cmn_err(CE_NOTE, "rfs41_revoke_deleg: delegation revoked");
	rfs41_seq4_hold(&dsp->rds_client->rc_seq4,
	    SEQ4_STATUS_RECALLABLE_STATE_REVOKED);
	rfs4_revoke_deleg(dsp);
}

static void
rfs4_revoke_file(rfs4_file_t *fp)
{
	rfs4_deleg_state_t *dsp;

	/*
	 * The lock for rfs4_file_t must be held when traversing the
	 * delegation list but that lock needs to be released to call
	 * rfs4_revoke_deleg().
	 *
	 * The called function rfs4_revoke_deleg removes the entry
	 * from the fp delegation list, so the while loop will keep
	 * looping until the list is empty.
	 */
	rfs4_dbe_lock(fp->rf_dbe);
	while (dsp = list_head(&fp->rf_delegstatelist)) {
		rfs4_dbe_hold(dsp->rds_dbe);
		rfs4_dbe_unlock(fp->rf_dbe);
		rfs4_revoke_deleg(dsp);
		rfs4_deleg_state_rele(dsp);
		rfs4_dbe_lock(fp->rf_dbe);
	}
	rfs4_dbe_unlock(fp->rf_dbe);
}

/*
 * A delegation is assumed to be present on the file associated with
 * "sp".  Check to see if the delegation matches is associated with
 * the same client as referenced by "sp".  If it is not, TRUE is
 * returned.  If the delegation DOES match the client (or no
 * delegation is present), return FALSE.
 * Assume the state entry and file entry are locked.
 *
 * This routine only checks the delegations of the calling server instance.
 * Since this is only called from rfs4_check_recall(), which is only called
 * by rfs4_do_open() and mds_do_open(), they only need to check if they own
 * this delegation.  All other conflict detection will be done by the monitor
 * on OPEN.
 */
bool_t
rfs4_is_deleg(rfs4_state_t *sp)
{
	rfs4_deleg_state_t *dsp;
	rfs4_file_t *fp = sp->rs_finfo;
	rfs4_client_t *cp = sp->rs_owner->ro_client;

	ASSERT(rfs4_dbe_islocked(fp->rf_dbe));
	for (dsp = list_head(&fp->rf_delegstatelist); dsp != NULL;
	    dsp = list_next(&fp->rf_delegstatelist, dsp)) {
		if (cp != dsp->rds_client)
			return (TRUE);
	}

	return (FALSE);
}

void
rfs4_disable_delegation(nfs_server_instance_t *instp)
{
	mutex_enter(&instp->deleg_lock);
	instp->deleg_disabled++;
	mutex_exit(&instp->deleg_lock);
}

void
rfs4_enable_delegation(nfs_server_instance_t *instp)
{
	mutex_enter(&instp->deleg_lock);
	ASSERT(instp->deleg_disabled > 0);
	instp->deleg_disabled--;
	mutex_exit(&instp->deleg_lock);
}

void
rfs4_mon_hold(void *arg)
{
	rfs4_file_t *fp = arg;

	rfs4_dbe_hold(fp->rf_dbe);
}

void
rfs4_mon_rele(void *arg)
{
	rfs4_file_t *fp = arg;

	rfs4_dbe_rele_nolock(fp->rf_dbe);
}
