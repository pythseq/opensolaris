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

#ifndef	_QCN_H
#define	_QCN_H

#pragma ident	"%Z%%M%	%I%	%E% SMI"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * sun4v Console driver
 */

#include <sys/types.h>
#include <sys/stream.h>
#include <sys/tty.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>

#define	RINGBITS	8		/* # of bits in ring ptrs */
#define	RINGSIZE	(1<<RINGBITS)	/* size of ring */
#define	RINGMASK	(RINGSIZE-1)

#define	RING_INIT(qsp)	((qsp)->qcn_rput = (qsp)->qcn_rget = 0)
#define	RING_CNT(qsp)	(((qsp)->qcn_rput - (qsp)->qcn_rget) & RINGMASK)
#define	RING_POK(qsp, n) ((int)RING_CNT(qsp) < (int)(RINGSIZE-(n)))
#define	RING_PUT(qsp, c) \
	((qsp)->qcn_ring[(qsp)->qcn_rput++ & RINGMASK] =  (uchar_t)(c))
#define	RING_GET(qsp)	((qsp)->qcn_ring[(qsp)->qcn_rget++ & RINGMASK])

/*
 * qcn driver's soft state structure
 */
typedef struct qcn {
	/* mutexes */
	kmutex_t qcn_hi_lock;		/* protects qcn_t (soft state)	*/
	kmutex_t qcn_softlock;	/* protects input handler	*/
	kmutex_t qcn_lock;	/* protects output queue	*/

	/* stream queues */
	queue_t *qcn_writeq;		/* stream write queue		*/
	queue_t	*qcn_readq;		/* stream read queue		*/

	/* dev info */
	dev_info_t	*qcn_dip;	/* dev_info			*/

	/* for handling IOCTL messages */
	bufcall_id_t	qcn_wbufcid;	/* for console ioctl	*/
	tty_common_t	qcn_tty;	/* for console ioctl	*/

	/* for console output timeout */
	time_t qcn_sc_active;		/* last time (sec) SC was active */
	uint_t	qcn_polling;
	uchar_t	qcn_rget;
	uchar_t	qcn_rput;
	int	qcn_soft_pend;
	ddi_softint_handle_t qcn_softint_hdl;
	ushort_t	qcn_ring[RINGSIZE];
	ushort_t	qcn_hangup;
	ddi_intr_handle_t *qcn_htable;	/* For array of interrupts */
	int	qcn_intr_type;	/* What type of interrupt */
	int	qcn_intr_cnt;	/* # of intrs count returned */
	size_t	qcn_intr_size;	/* Size of intr array */
	uint_t	qcn_intr_pri;	/* Interrupt priority   */
	ddi_iblock_cookie_t qcn_soft_pri;
	uint_t	qcn_rbuf_overflow;
} qcn_t;

/* Constants used by promif routines */
#define	QCN_CLNT_STR	"CON_CLNT"
#define	QCN_OBP_STR	"CON_OBP"

/* alternate break sequence */
extern void (*abort_seq_handler)();

extern struct mod_ops mod_driverops;

#define	QCN_TXINT_ENABLE	0x1
#define	QCN_RXINT_ENABLE	0x2

#ifdef __cplusplus
}
#endif

#endif	/* _QCN_H */
