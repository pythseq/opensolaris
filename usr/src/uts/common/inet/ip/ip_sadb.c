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

#include <sys/types.h>
#include <sys/stream.h>
#include <sys/sunddi.h>
#include <sys/strlog.h>

#include <inet/common.h>
#include <inet/mib2.h>
#include <inet/ip.h>
#include <inet/ip6.h>
#include <inet/ipdrop.h>

#include <net/pfkeyv2.h>
#include <inet/ipsec_info.h>
#include <inet/sadb.h>
#include <inet/ipsec_impl.h>
#include <inet/ipsecesp.h>
#include <inet/ipsecah.h>
#include <sys/kstat.h>

/* stats */
static kstat_t *ipsec_ksp;
ipsec_kstats_t *ipsec_kstats;

/* The IPsec SADBs for AH and ESP */
sadbp_t ah_sadb, esp_sadb;

/* Packet dropper for IP IPsec processing failures */
extern ipdropper_t ip_dropper;

void
ipsec_kstat_init(void)
{
	ipsec_ksp = kstat_create("ip", 0, "ipsec_stat", "net",
	    KSTAT_TYPE_NAMED, sizeof (*ipsec_kstats) / sizeof (kstat_named_t),
	    KSTAT_FLAG_PERSISTENT);

	ASSERT(ipsec_ksp != NULL);

	ipsec_kstats = ipsec_ksp->ks_data;

#define	KI(x) kstat_named_init(&ipsec_kstats->x, #x, KSTAT_DATA_UINT64)
	KI(esp_stat_in_requests);
	KI(esp_stat_in_discards);
	KI(esp_stat_lookup_failure);
	KI(ah_stat_in_requests);
	KI(ah_stat_in_discards);
	KI(ah_stat_lookup_failure);
#undef KI

	kstat_install(ipsec_ksp);
}

void
ipsec_kstat_destroy(void)
{
	kstat_delete(ipsec_ksp);
}

/*
 * Returns B_TRUE if the identities in the SA match the identities
 * in the "latch" structure.
 */

static boolean_t
ipsec_match_outbound_ids(ipsec_latch_t *ipl, ipsa_t *sa)
{
	ASSERT(ipl->ipl_ids_latched == B_TRUE);
	return ipsid_equal(ipl->ipl_local_cid, sa->ipsa_src_cid) &&
	    ipsid_equal(ipl->ipl_remote_cid, sa->ipsa_dst_cid);
}

/*
 * Look up a security association based on the unique ID generated by IP and
 * transport information, such as ports and upper-layer protocol, and the
 * address(es).	 Used for uniqueness testing and outbound packets.  The
 * source address may be ignored.
 *
 * I expect an SA hash bucket, and that its per-bucket mutex is held.
 * The SA ptr I return will have its reference count incremented by one.
 */
ipsa_t *
ipsec_getassocbyconn(isaf_t *bucket, ipsec_out_t *io, uint32_t *src,
    uint32_t *dst, sa_family_t af, uint8_t protocol)
{
	ipsa_t *retval, *candidate;
	ipsec_action_t *candact;
	boolean_t need_unique;
	uint64_t unique_id;
	uint32_t old_flags, excludeflags;
	ipsec_policy_t *pp = io->ipsec_out_policy;
	ipsec_action_t *actlist = io->ipsec_out_act;
	ipsec_action_t *act;
	ipsec_latch_t *ipl = io->ipsec_out_latch;
	ipsa_ref_t *ipr = NULL;

	ASSERT(MUTEX_HELD(&bucket->isaf_lock));

	/*
	 * Fast path: do we have a latch structure, is it for this bucket,
	 * and does the generation number match?  If so, refhold and return.
	 */

	if (ipl != NULL) {
		ASSERT((protocol == IPPROTO_AH) || (protocol == IPPROTO_ESP));
		ipr = &ipl->ipl_ref[protocol - IPPROTO_ESP];

		retval = ipr->ipsr_sa;

		/*
		 * NOTE: The isaf_gen check (incremented upon
		 * sadb_unlinkassoc()) protects against retval being a freed
		 * SA.  (We're exploiting short-circuit evaluation.)
		 */
		if ((bucket == ipr->ipsr_bucket) &&
		    (bucket->isaf_gen == ipr->ipsr_gen) &&
		    (retval->ipsa_state != IPSA_STATE_DEAD) &&
		    !(retval->ipsa_flags & IPSA_F_CINVALID)) {
			IPSA_REFHOLD(retval);
			return (retval);
		}
	}

	ASSERT((pp != NULL) || (actlist != NULL));
	if (actlist == NULL)
		actlist = pp->ipsp_act;
	ASSERT(actlist != NULL);

	need_unique = actlist->ipa_want_unique;
	unique_id = SA_FORM_UNIQUE_ID(io);

	/*
	 * Precompute mask for SA flags comparison: If we need a
	 * unique SA and an SA has already been used, or if the SA has
	 * a unique value which doesn't match, we aren't interested in
	 * the SA..
	 */

	excludeflags = IPSA_F_UNIQUE;
	if (need_unique)
		excludeflags |= IPSA_F_USED;

	/*
	 * Walk the hash bucket, matching on:
	 *
	 * - unique_id
	 * - destination
	 * - source
	 * - algorithms
	 * - <MORE TBD>
	 *
	 * Make sure that wildcard sources are inserted at the end of the hash
	 * bucket.
	 *
	 * DEFINITIONS:	A _shared_ SA is one with unique_id == 0 and USED.
	 *		An _unused_ SA is one with unique_id == 0 and not USED.
	 *		A _unique_ SA is one with unique_id != 0 and USED.
	 *		An SA with unique_id != 0 and not USED never happens.
	 */

	candidate = NULL;

	for (retval = bucket->isaf_ipsa; retval != NULL;
	    retval = retval->ipsa_next) {
		ASSERT((candidate == NULL) ||
		    MUTEX_HELD(&candidate->ipsa_lock));

		/*
		 * Q: Should I lock this SA?
		 * A: For now, yes.  I change and use too many fields in here
		 *    (e.g. unique_id) that I may be racing with other threads.
		 *    Also, the refcnt needs to be bumped up.
		 */

		mutex_enter(&retval->ipsa_lock);

		/* My apologies for the use of goto instead of continue. */
		if (!IPSA_ARE_ADDR_EQUAL(dst, retval->ipsa_dstaddr, af))
			goto next_ipsa;	/* Destination mismatch. */
		if (!IPSA_ARE_ADDR_EQUAL(src, retval->ipsa_srcaddr, af) &&
		    !IPSA_IS_ADDR_UNSPEC(retval->ipsa_srcaddr, af))
			goto next_ipsa;	/* Specific source and not matched. */

		/*
		 * XXX should be able to use cached/latched action
		 * to dodge this loop
		 */
		for (act = actlist; act != NULL; act = act->ipa_next) {
			ipsec_act_t *ap = &act->ipa_act;
			if (ap->ipa_type != IPSEC_POLICY_APPLY)
				continue;

			/*
			 * XXX ugly.  should be better way to do this test
			 */
			if (protocol == IPPROTO_AH) {
				if (!(ap->ipa_apply.ipp_use_ah))
					continue;
				if (ap->ipa_apply.ipp_auth_alg !=
				    retval->ipsa_auth_alg)
					continue;
				if (ap->ipa_apply.ipp_ah_minbits >
					retval->ipsa_authkeybits)
					continue;
			} else {
				if (!(ap->ipa_apply.ipp_use_esp))
					continue;

				if ((ap->ipa_apply.ipp_encr_alg !=
				    retval->ipsa_encr_alg))
					continue;

				if (ap->ipa_apply.ipp_espe_minbits >
				    retval->ipsa_encrkeybits)
					continue;

				if (ap->ipa_apply.ipp_esp_auth_alg != 0) {
					if (ap->ipa_apply.ipp_esp_auth_alg !=
					    retval->ipsa_auth_alg)
						continue;
					if (ap->ipa_apply.ipp_espa_minbits >
					    retval->ipsa_authkeybits)
						continue;
				}
			}

			/*
			 * Check key mgmt proto, cookie
			 */
			if ((ap->ipa_apply.ipp_km_proto != 0) &&
			    (retval->ipsa_kmp != 0) &&
			    (ap->ipa_apply.ipp_km_proto != retval->ipsa_kmp))
				continue;

			if ((ap->ipa_apply.ipp_km_cookie != 0) &&
			    (retval->ipsa_kmc != 0) &&
			    (ap->ipa_apply.ipp_km_cookie != retval->ipsa_kmc))
				continue;

			break;
		}
		if (act == NULL)
			goto next_ipsa;	/* nothing matched */

		/*
		 * Do identities match?
		 */
		if (ipl && ipl->ipl_ids_latched &&
		    !ipsec_match_outbound_ids(ipl, retval))
			goto next_ipsa;

		/*
		 * At this point, we know that we have at least a match on:
		 *
		 * - dest
		 * - source (if source is specified, i.e. non-zeroes)
		 * - auth alg (if auth alg is specified, i.e. non-zero)
		 * - encrypt. alg (if encrypt. alg is specified, i.e. non-zero)
		 * and we know that the SA keylengths are appropriate.
		 *
		 * (Keep in mind known-src SAs are hit before zero-src SAs,
		 * thanks to sadb_insertassoc().)
		 * If we need a unique asssociation, optimally we have
		 * ipsa_unique_id == unique_id, otherwise NOT USED
		 * is held in reserve (stored in candidate).
		 *
		 * For those stored in candidate, take best-match (i.e. given
		 * a choice, candidate should have non-zero ipsa_src).
		 */

		/*
		 * If SA has a unique value which matches, we're all set...
		 * "key management knows best"
		 */
		if ((retval->ipsa_flags & IPSA_F_UNIQUE) &&
		    ((unique_id & retval->ipsa_unique_mask) ==
			retval->ipsa_unique_id))
			break;

		/*
		 * If we need a unique SA and this SA has already been used,
		 * or if the SA has a unique value which doesn't match,
		 * this isn't for us.
		 */

		if (retval->ipsa_flags & excludeflags)
			goto next_ipsa;


		/*
		 * I found a candidate..
		 */
		if (candidate == NULL) {
			/*
			 * and didn't already have one..
			 */
			candidate = retval;
			candact = act;
			continue;
		} else {
			/*
			 * If candidate's source address is zero and
			 * the current match (i.e. retval) address is
			 * not zero, we have a better candidate..
			 */
			if (IPSA_IS_ADDR_UNSPEC(candidate->ipsa_srcaddr, af) &&
			    !IPSA_IS_ADDR_UNSPEC(retval->ipsa_srcaddr, af)) {
				mutex_exit(&candidate->ipsa_lock);
				candidate = retval;
				candact = act;
				continue;
			}
		}
next_ipsa:
		mutex_exit(&retval->ipsa_lock);
	}
	ASSERT((retval == NULL) || MUTEX_HELD(&retval->ipsa_lock));
	ASSERT((candidate == NULL) || MUTEX_HELD(&candidate->ipsa_lock));
	ASSERT((retval == NULL) || (act != NULL));
	ASSERT((candidate == NULL) || (candact != NULL));

	/* Let caller react to a lookup failure when it gets NULL. */
	if (retval == NULL && candidate == NULL)
		return (NULL);

	if (retval == NULL) {
		ASSERT(MUTEX_HELD(&candidate->ipsa_lock));
		retval = candidate;
		act = candact;
	} else if (candidate != NULL) {
		mutex_exit(&candidate->ipsa_lock);
	}
	ASSERT(MUTEX_HELD(&retval->ipsa_lock));
	ASSERT(act != NULL);

	/*
	 * Even though I hold the mutex, since the reference counter is an
	 * atomic operation, I really have to use the IPSA_REFHOLD macro.
	 */
	IPSA_REFHOLD(retval);

	/*
	 * This association is no longer unused.
	 */
	old_flags = retval->ipsa_flags;
	retval->ipsa_flags |= IPSA_F_USED;

	/*
	 * Cache a reference to this SA for the fast path.
	 */
	if (ipr != NULL) {
		ipr->ipsr_bucket = bucket;
		ipr->ipsr_gen = bucket->isaf_gen;
		ipr->ipsr_sa = retval;
		/* I'm now caching, so the cache-invalid flag goes away! */
		retval->ipsa_flags &= ~IPSA_F_CINVALID;
	}
	/*
	 * Latch various things while we're here..
	 */
	if (ipl != NULL) {
		if (!ipl->ipl_ids_latched) {
			ipsec_latch_ids(ipl,
			    retval->ipsa_src_cid, retval->ipsa_dst_cid);
		}
		if (!ipl->ipl_out_action_latched) {
			IPACT_REFHOLD(act);
			ipl->ipl_out_action = act;
			ipl->ipl_out_action_latched = B_TRUE;
		}
	}

	/*
	 * Set the uniqueness only first time.
	 */
	if (need_unique && !(old_flags & IPSA_F_USED)) {
		if (retval->ipsa_unique_id == 0) {
			ASSERT((retval->ipsa_flags & IPSA_F_UNIQUE) == 0);
			/*
			 * From now on, only this src, dst[ports, addr],
			 * proto, should use it.
			 */
			retval->ipsa_flags |= IPSA_F_UNIQUE;
			retval->ipsa_unique_id = unique_id;
			retval->ipsa_unique_mask = SA_UNIQUE_MASK(
			    io->ipsec_out_src_port, io->ipsec_out_dst_port,
			    protocol);
		}

		/*
		 * Set the source address and adjust the hash
		 * buckets only if src_addr is zero.
		 */
		if (IPSA_IS_ADDR_UNSPEC(retval->ipsa_srcaddr, af)) {
			/*
			 * sadb_unlinkassoc() will decrement the refcnt.  Bump
			 * up when we have the lock so that we don't have to
			 * acquire locks when we come back from
			 * sadb_insertassoc().
			 *
			 * We don't need to bump the bucket's gen since
			 * we aren't moving to a new bucket.
			 */
			IPSA_REFHOLD(retval);
			IPSA_COPY_ADDR(retval->ipsa_srcaddr, src, af);
			mutex_exit(&retval->ipsa_lock);
			sadb_unlinkassoc(retval);
			/*
			 * Since the bucket lock is held, we know
			 * sadb_insertassoc() will succeed.
			 */
#ifdef DEBUG
			if (sadb_insertassoc(retval, bucket) != 0) {
				cmn_err(CE_PANIC,
				    "sadb_insertassoc() failed in "
				    "ipsec_getassocbyconn().\n");
			}
#else	/* non-DEBUG */
			(void) sadb_insertassoc(retval, bucket);
#endif	/* DEBUG */
			return (retval);
		}
	}
	mutex_exit(&retval->ipsa_lock);

	return (retval);
}

/*
 * Look up a security association based on the security parameters index (SPI)
 * and address(es).  This is used for inbound packets and general SA lookups
 * (even in outbound SA tables).  The source address may be ignored.  Return
 * NULL if no association is available.	 If an SA is found, return it, with
 * its refcnt incremented.  The caller must REFRELE after using the SA.
 * The hash bucket must be locked down before calling.
 */
ipsa_t *
ipsec_getassocbyspi(isaf_t *bucket, uint32_t spi, uint32_t *src, uint32_t *dst,
    sa_family_t af)
{
	ipsa_t *retval;

	ASSERT(MUTEX_HELD(&bucket->isaf_lock));

	/*
	 * Walk the hash bucket, matching exactly on SPI, then destination,
	 * then source.
	 *
	 * Per-SA locking doesn't need to happen, because I'm only matching
	 * on addresses.  Addresses are only changed during insertion/deletion
	 * from the hash bucket.  Since the hash bucket lock is held, we don't
	 * need to worry about addresses changing.
	 */

	for (retval = bucket->isaf_ipsa; retval != NULL;
	    retval = retval->ipsa_next) {
		if (retval->ipsa_spi != spi)
			continue;
		if (!IPSA_ARE_ADDR_EQUAL(dst, retval->ipsa_dstaddr, af))
			continue;

		/*
		 * Assume that wildcard source addresses are inserted at the
		 * end of the hash bucket.  (See sadb_insertassoc().)
		 * The following check for source addresses is a weak form
		 * of access control/source identity verification.  If an
		 * SA has a source address, I only match an all-zeroes
		 * source address, or that particular one.  If the SA has
		 * an all-zeroes source, then I match regardless.
		 *
		 * There is a weakness here in that a packet with all-zeroes
		 * for an address will match regardless of the source address
		 * stored in the packet.
		 */
		if (IPSA_ARE_ADDR_EQUAL(src, retval->ipsa_srcaddr, af) ||
		    IPSA_IS_ADDR_UNSPEC(retval->ipsa_srcaddr, af) ||
		    IPSA_IS_ADDR_UNSPEC(src, af))
			break;
	}

	if (retval != NULL) {
		/*
		 * Just refhold the return value.  The caller will then
		 * make the appropriate calls to set the USED flag.
		 */
		IPSA_REFHOLD(retval);
	}

	return (retval);
}

boolean_t
ipsec_outbound_sa(mblk_t *mp, uint_t proto)
{
	mblk_t *data_mp;
	ipsec_out_t *io;
	ipaddr_t dst;
	uint32_t *dst_ptr, *src_ptr;
	isaf_t *bucket;
	ipsa_t *assoc;
	ip6_pkt_t ipp;
	in6_addr_t dst6;
	ipsa_t **sa;
	sadbp_t *sadbp;
	sa_family_t af;

	data_mp = mp->b_cont;
	io = (ipsec_out_t *)mp->b_rptr;

	if (proto == IPPROTO_ESP) {
		sa = &io->ipsec_out_esp_sa;
		sadbp = &esp_sadb;
	} else {
		ASSERT(proto == IPPROTO_AH);
		sa = &io->ipsec_out_ah_sa;
		sadbp = &ah_sadb;
	}

	ASSERT(*sa == NULL);

	if (io->ipsec_out_v4) {
		ipha_t *ipha = (ipha_t *)data_mp->b_rptr;

		ASSERT(IPH_HDR_VERSION(ipha) == IPV4_VERSION);
		dst = ip_get_dst(ipha);
		af = AF_INET;

		/*
		 * NOTE:Getting the outbound association is considerably
		 *	painful.  ipsec_getassocbyconn() will require more
		 *	parameters as policy implementations mature.
		 */
		bucket = &sadbp->s_v4.sdb_of[OUTBOUND_HASH_V4(dst)];
		src_ptr = (uint32_t *)&ipha->ipha_src;
		dst_ptr = (uint32_t *)&dst;
	} else {
		ip6_t *ip6h = (ip6_t *)data_mp->b_rptr;

		ASSERT(IPH_HDR_VERSION(ip6h) == IPV6_VERSION);
		dst6 = ip_get_dst_v6(ip6h, NULL);
		af = AF_INET6;

		bzero(&ipp, sizeof (ipp));

		/* Same NOTE: applies here! */
		bucket = &sadbp->s_v6.sdb_of[OUTBOUND_HASH_V6(dst6)];
		src_ptr = (uint32_t *)&ip6h->ip6_src;
		dst_ptr = (uint32_t *)&dst6;
	}

	mutex_enter(&bucket->isaf_lock);
	assoc = ipsec_getassocbyconn(bucket, io, src_ptr, dst_ptr, af, proto);
	mutex_exit(&bucket->isaf_lock);

	if (assoc == NULL)
		return (B_FALSE);

	if (assoc->ipsa_state == IPSA_STATE_DEAD) {
		IPSA_REFRELE(assoc);
		return (B_FALSE);
	}

	ASSERT(assoc->ipsa_state != IPSA_STATE_LARVAL);

	*sa = assoc;
	return (B_TRUE);
}

/*
 * Inbound IPsec SA selection.
 */

ah_t *
ipsec_inbound_ah_sa(mblk_t *mp)
{
	mblk_t *ipsec_in;
	ipha_t *ipha;
	ipsa_t 	*assoc;
	ah_t *ah;
	isaf_t *hptr;
	ipsec_in_t *ii;
	boolean_t isv6;
	ip6_t *ip6h;
	int ah_offset;
	uint32_t *src_ptr, *dst_ptr;
	int pullup_len;
	sa_family_t af;

	IP_AH_BUMP_STAT(in_requests);

	ASSERT(mp->b_datap->db_type == M_CTL);

	ipsec_in = mp;
	ii = (ipsec_in_t *)ipsec_in->b_rptr;
	mp = mp->b_cont;

	ASSERT(mp->b_datap->db_type == M_DATA);

	isv6 = !ii->ipsec_in_v4;
	if (isv6) {
		ip6h = (ip6_t *)mp->b_rptr;
		ah_offset = ipsec_ah_get_hdr_size_v6(mp, B_TRUE);
	} else {
		ipha = (ipha_t *)mp->b_rptr;
		ASSERT(ipha->ipha_protocol == IPPROTO_AH);
		ah_offset = ipha->ipha_version_and_hdr_length -
		    (uint8_t)((IP_VERSION << 4));
		ah_offset <<= 2;
	}

	/*
	 * We assume that the IP header is pulled up until
	 * the options. We need to see whether we have the
	 * AH header in the same mblk or not.
	 */
	pullup_len = ah_offset + sizeof (ah_t);
	if (mp->b_rptr + pullup_len > mp->b_wptr) {
		if (!pullupmsg(mp, pullup_len)) {
			ipsecah_rl_strlog(0,  SL_WARN | SL_ERROR,
			    "ipsec_inbound_ah_sa: Small AH header\n");
			IP_AH_BUMP_STAT(in_discards);
			ip_drop_packet(ipsec_in, B_TRUE, NULL, NULL,
			    &ipdrops_ah_bad_length, &ip_dropper);
			return (NULL);
		}
		if (isv6)
			ip6h = (ip6_t *)mp->b_rptr;
		else
			ipha = (ipha_t *)mp->b_rptr;
	}

	ah = (ah_t *)(mp->b_rptr + ah_offset);

	if (isv6) {
		src_ptr = (uint32_t *)&ip6h->ip6_src;
		dst_ptr = (uint32_t *)&ip6h->ip6_dst;
		hptr = ah_sadb.s_v6.sdb_if;
		af = AF_INET6;
	} else {
		src_ptr = (uint32_t *)&ipha->ipha_src;
		dst_ptr = (uint32_t *)&ipha->ipha_dst;
		hptr = ah_sadb.s_v4.sdb_if;
		af = AF_INET;
	}

	hptr += INBOUND_HASH(ah->ah_spi);
	mutex_enter(&hptr->isaf_lock);
	assoc = ipsec_getassocbyspi(hptr, ah->ah_spi, src_ptr, dst_ptr, af);
	mutex_exit(&hptr->isaf_lock);

	if (assoc == NULL || assoc->ipsa_state == IPSA_STATE_DEAD) {
		IP_AH_BUMP_STAT(lookup_failure);
		IP_AH_BUMP_STAT(in_discards);
		ipsecah_in_assocfailure(ipsec_in, 0,
		    SL_ERROR | SL_CONSOLE | SL_WARN,
		    "ipsec_inbound_ah_sa: No association found for "
		    "spi 0x%x, dst addr %s\n",
		    ah->ah_spi, dst_ptr, af);
		if (assoc != NULL) {
			IPSA_REFRELE(assoc);
		}
		return (NULL);
	}

	if (assoc->ipsa_state == IPSA_STATE_LARVAL) {
		/* Not fully baked; swap the packet under a rock until then */
		sadb_set_lpkt(assoc, ipsec_in);
		IPSA_REFRELE(assoc);
		return (NULL);
	}

	/*
	 * Save a reference to the association so that it can
	 * be retrieved after execution. We free any AH SA reference
	 * already there (innermost SA "wins". The reference to
	 * the SA will also be used later when doing the policy checks.
	 */
	if (ii->ipsec_in_ah_sa != NULL) {
		IPSA_REFRELE(ii->ipsec_in_ah_sa);
	}
	ii->ipsec_in_ah_sa = assoc;

	return (ah);
}

esph_t *
ipsec_inbound_esp_sa(mblk_t *ipsec_in_mp)
{
	mblk_t *data_mp, *placeholder;
	uint32_t *src_ptr, *dst_ptr;
	ipsec_in_t *ii;
	ipha_t *ipha;
	ip6_t *ip6h;
	esph_t *esph;
	ipsa_t *ipsa;
	isaf_t *bucket;
	uint_t preamble;
	sa_family_t af;
	boolean_t isv6;

	IP_ESP_BUMP_STAT(in_requests);
	ASSERT(ipsec_in_mp->b_datap->db_type == M_CTL);

	/* We have IPSEC_IN already! */
	ii = (ipsec_in_t *)ipsec_in_mp->b_rptr;
	data_mp = ipsec_in_mp->b_cont;

	ASSERT(ii->ipsec_in_type == IPSEC_IN);

	isv6 = !ii->ipsec_in_v4;
	if (isv6) {
		ip6h = (ip6_t *)data_mp->b_rptr;
	} else {
		ipha = (ipha_t *)data_mp->b_rptr;
	}

	/*
	 * Put all data into one mblk if it's not there already.
	 * XXX This is probably bad long-term.  Figure out better ways of doing
	 * this.  Much of the inbound path depends on all of the data being
	 * in one mblk.
	 *
	 * XXX Jumbogram issues will have to be dealt with here.
	 * If the plen is 0, we'll have to scan for a HBH header with the
	 * actual packet length.
	 */
	if (data_mp->b_datap->db_ref > 1 ||
	    (data_mp->b_wptr - data_mp->b_rptr) <
	    (isv6 ? (ntohs(ip6h->ip6_plen) + sizeof (ip6_t))
		: ntohs(ipha->ipha_length))) {
		placeholder = msgpullup(data_mp, -1);
		if (placeholder == NULL) {
			IP_ESP_BUMP_STAT(in_discards);
			/*
			 * TODO: Extract inbound interface from the IPSEC_IN
			 * message's ii->ipsec_in_rill_index.
			 */
			ip_drop_packet(ipsec_in_mp, B_TRUE, NULL, NULL,
			    &ipdrops_esp_nomem, &ip_dropper);
			return (NULL);
		} else {
			/* Reset packet with new pulled up mblk. */
			freemsg(data_mp);
			data_mp = placeholder;
			ipsec_in_mp->b_cont = data_mp;
		}
	}

	/*
	 * Find the ESP header, point the address pointers at the appropriate
	 * IPv4/IPv6 places.
	 */
	if (isv6) {
		ip6h = (ip6_t *)data_mp->b_rptr;
		src_ptr = (uint32_t *)&ip6h->ip6_src;
		dst_ptr = (uint32_t *)&ip6h->ip6_dst;
		if (ip6h->ip6_nxt != IPPROTO_ESP) {
			/* There are options that need to be processed. */
			preamble = ip_hdr_length_v6(data_mp, ip6h);
		} else {
			preamble = sizeof (ip6_t);
		}

		bucket = esp_sadb.s_v6.sdb_if;
		af = AF_INET6;
	} else {
		ipha = (ipha_t *)data_mp->b_rptr;
		src_ptr = (uint32_t *)&ipha->ipha_src;
		dst_ptr = (uint32_t *)&ipha->ipha_dst;
		preamble = IPH_HDR_LENGTH(ipha);

		bucket = esp_sadb.s_v4.sdb_if;
		af = AF_INET;
	}

	esph = (esph_t *)(data_mp->b_rptr + preamble);

	/* Since hash is common on inbound (SPI value), hash here. */
	bucket += INBOUND_HASH(esph->esph_spi);
	mutex_enter(&bucket->isaf_lock);
	ipsa = ipsec_getassocbyspi(bucket, esph->esph_spi, src_ptr, dst_ptr,
	    af);
	mutex_exit(&bucket->isaf_lock);

	if (ipsa == NULL || ipsa->ipsa_state == IPSA_STATE_DEAD) {
		/*  This is a loggable error!  AUDIT ME! */
		IP_ESP_BUMP_STAT(lookup_failure);
		IP_ESP_BUMP_STAT(in_discards);
		ipsecesp_in_assocfailure(ipsec_in_mp, 0,
		    SL_ERROR | SL_CONSOLE | SL_WARN,
		    "ipsec_inbound_esp_sa: No association found for "
		    "spi 0x%x, dst addr %s\n",
		    esph->esph_spi, dst_ptr, af);
		if (ipsa != NULL) {
			IPSA_REFRELE(ipsa);
		}
		return (NULL);
	}

	if (ipsa->ipsa_state == IPSA_STATE_LARVAL) {
		/* Not fully baked; swap the packet under a rock until then */
		sadb_set_lpkt(ipsa, ipsec_in_mp);
		IPSA_REFRELE(ipsa);
		return (NULL);
	}

	/*
	 * Save a reference to the association so that it can
	 * be retrieved after execution. We free any AH SA reference
	 * already there (innermost SA "wins". The reference to
	 * the SA will also be used later when doing the policy checks.
	 */
	if (ii->ipsec_in_esp_sa != NULL) {
		IPSA_REFRELE(ii->ipsec_in_esp_sa);
	}
	ii->ipsec_in_esp_sa = ipsa;

	return (esph);
}
