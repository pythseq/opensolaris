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

/*
 * Solaris DDI STREAMS utility routines (PSARC/2003/648).
 *
 * Please see the appropriate section 9F manpage for documentation.
 */

#include <sys/types.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/stream.h>
#include <sys/stropts.h>
#include <sys/strsun.h>
#include <sys/cmn_err.h>

void
merror(queue_t *wq, mblk_t *mp, int error)
{
	if ((mp = mexchange(wq, mp, 1, M_ERROR, -1)) == NULL)
		return;

	*mp->b_rptr = (uchar_t)error;
	qreply(wq, mp);
}

void
mioc2ack(mblk_t *mp, mblk_t *dp, size_t count, int rval)
{
	struct iocblk *iocp = (struct iocblk *)mp->b_rptr;
	mblk_t *odp = mp->b_cont;  	/* allows freemsg() to be a tail call */

	DB_TYPE(mp) = M_IOCACK;
	iocp->ioc_count = count;
	iocp->ioc_error = 0;
	iocp->ioc_rval = rval;

	mp->b_cont = dp;
	if (dp != NULL)
		dp->b_wptr = dp->b_rptr + count;
	freemsg(odp);
}

void
miocack(queue_t *wq, mblk_t *mp, int count, int rval)
{
	struct iocblk *iocp = (struct iocblk *)mp->b_rptr;

	DB_TYPE(mp) = M_IOCACK;
	iocp->ioc_count = count;
	iocp->ioc_error = 0;
	iocp->ioc_rval = rval;
	qreply(wq, mp);
}

void
miocnak(queue_t *wq, mblk_t *mp, int count, int error)
{
	struct iocblk *iocp = (struct iocblk *)mp->b_rptr;

	DB_TYPE(mp) = M_IOCNAK;
	iocp->ioc_count = count;
	iocp->ioc_error = error;
	qreply(wq, mp);
}

mblk_t *
mexchange(queue_t *wq, mblk_t *mp, size_t size, uchar_t type, int32_t primtype)
{
	if (mp == NULL || MBLKSIZE(mp) < size || DB_REF(mp) > 1) {
		freemsg(mp);
		if ((mp = allocb(size, BPRI_LO)) == NULL) {
			if (wq != NULL) {
				if ((mp = allocb(1, BPRI_HI)) != NULL)
					merror(wq, mp, ENOSR);
			}
			return (NULL);
		}
	}

	DB_TYPE(mp) = type;
	mp->b_rptr = DB_BASE(mp);
	mp->b_wptr = mp->b_rptr + size;
	if (primtype >= 0)
		*(int32_t *)mp->b_rptr = primtype;

	return (mp);
}

size_t
msgsize(mblk_t *mp)
{
	size_t	n = 0;

	for (; mp != NULL; mp = mp->b_cont)
		n += MBLKL(mp);

	return (n);
}

void
mcopymsg(mblk_t *mp, void *bufp)
{
	caddr_t	dest = bufp;
	mblk_t	*bp;
	size_t	n;

	for (bp = mp; bp != NULL; bp = bp->b_cont) {
		n = MBLKL(bp);
		bcopy(bp->b_rptr, dest, n);
		dest += n;
	}

	freemsg(mp);
}

void
mcopyin(mblk_t *mp, void *private, size_t size, void *useraddr)
{
	struct copyreq *cp = (struct copyreq *)mp->b_rptr;

	if (useraddr != NULL) {
		cp->cq_addr = (caddr_t)useraddr;
	} else {
		ASSERT(DB_TYPE(mp) == M_IOCTL);
		ASSERT(mp->b_cont != NULL);
		ASSERT(((struct iocblk *)mp->b_rptr)->ioc_count == TRANSPARENT);
		cp->cq_addr = (caddr_t)*(uintptr_t *)mp->b_cont->b_rptr;
	}

	cp->cq_flag = 0;
	cp->cq_size = size;
	cp->cq_private = (mblk_t *)private;

	DB_TYPE(mp) = M_COPYIN;
	mp->b_wptr = mp->b_rptr + sizeof (struct copyreq);

	if (mp->b_cont != NULL) {
		freemsg(mp->b_cont);
		mp->b_cont = NULL;
	}
}

void
mcopyout(mblk_t *mp, void *private, size_t size, void *useraddr, mblk_t *dp)
{
	struct copyreq *cp = (struct copyreq *)mp->b_rptr;

	if (useraddr != NULL)
		cp->cq_addr = (caddr_t)useraddr;
	else {
		ASSERT(DB_TYPE(mp) == M_IOCTL);
		ASSERT(mp->b_cont != NULL);
		ASSERT(((struct iocblk *)mp->b_rptr)->ioc_count == TRANSPARENT);
		cp->cq_addr = (caddr_t)*(uintptr_t *)mp->b_cont->b_rptr;
	}

	cp->cq_flag = 0;
	cp->cq_size = size;
	cp->cq_private = (mblk_t *)private;

	DB_TYPE(mp) = M_COPYOUT;
	mp->b_wptr = mp->b_rptr + sizeof (struct copyreq);

	if (dp != NULL) {
		if (mp->b_cont != NULL)
			freemsg(mp->b_cont);
		mp->b_cont = dp;
		mp->b_cont->b_wptr = mp->b_cont->b_rptr + size;
	}
}

int
miocpullup(mblk_t *iocmp, size_t size)
{
	struct iocblk	*iocp = (struct iocblk *)iocmp->b_rptr;
	mblk_t		*datamp = iocmp->b_cont;
	mblk_t		*newdatamp;

	/*
	 * We'd like to be sure that DB_TYPE(iocmp) == M_IOCTL, but some
	 * nitwit routines like ttycommon_ioctl() always reset the type of
	 * legitimate M_IOCTL messages to M_IOCACK as a "courtesy" to the
	 * caller, even when the routine does not understand the M_IOCTL.
	 * The ttycommon_ioctl() routine does us the additional favor of
	 * clearing ioc_count, so we cannot rely on it having a correct
	 * size either (blissfully, ttycommon_ioctl() does not screw with
	 * TRANSPARENT messages, so we can still sanity check for that).
	 */
	ASSERT(MBLKL(iocmp) == sizeof (struct iocblk));
	if (MBLKL(iocmp) != sizeof (struct iocblk)) {
		cmn_err(CE_WARN, "miocpullup: passed mblk_t %p is not an ioctl"
		    " mblk_t", (void *)iocmp);
		return (EINVAL);
	}

	if (iocp->ioc_count == TRANSPARENT)
		return (EINVAL);

	if (size == 0)
		return (0);

	if (datamp == NULL)
		return (EINVAL);

	if (MBLKL(datamp) >= size)
		return (0);

	newdatamp = msgpullup(datamp, size);
	if (newdatamp == NULL) {
		if (msgdsize(datamp) < size)
			return (EINVAL);
		return (ENOMEM);
	}

	iocmp->b_cont = newdatamp;
	freemsg(datamp);
	return (0);
}
