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

#ifndef _SYS_PX_PCI_H
#define	_SYS_PX_PCI_H

#pragma ident	"%Z%%M%	%I%	%E% SMI"

#ifdef	__cplusplus
extern "C" {
#endif

/*
 * Intel specific register offsets with bit definitions.
 */
#define	PXB_PX_CAPABILITY_ID	0x44
#define	PXB_BRIDGE_CONF		0x40

/*
 * Generic - PCI Express Capability List Structure
 * XXX - Should be moved to a more PCI generic location
 */
#define	PX_CAP_REG	0x2

/*
 * Generic - PCI Express Capabilities Register
 * XXX - Should be moved to a more PCI generic location
 */
#define	PX_CAP_REG_DEV_TYPE_PCIE_DEV	0x0000	/* PCI-E Endpont Device */
#define	PX_CAP_REG_DEV_TYPE_PCI_DEV	0x0010	/* Leg PCI Endpont Device */
#define	PX_CAP_REG_DEV_TYPE_ROOT	0x0040	/* Root Port of Root Complex */
#define	PX_CAP_REG_DEV_TYPE_UP		0x0050	/* Upstream Port of Switch */
#define	PX_CAP_REG_DEV_TYPE_DOWN	0x0060	/* Downstream Port of Switch */
#define	PX_CAP_REG_DEV_TYPE_PCIE2PCI	0x0070	/* PCI-E to PCI Bridge */
#define	PX_CAP_REG_DEV_TYPE_PCI2PCIE	0x0080	/* PCI to PCI-E Bridge */
#define	PX_CAP_REG_DEV_TYPE_MASK	0x00F0	/* Device/Port Type Mask */

/*
 * PCI/PCI-E Configuration register specific values.
 */


#define	PX_PMODE	0x4000		/* PCI/PCIX Mode */
#define	PX_PFREQ_66	0x200		/* PCI clock frequency */
#define	PX_PFREQ_100	0x400
#define	PX_PFREQ_133	0x600
#define	PX_PMRE		0x80		/* Peer memory read enable */

/*
 * Downstream delayed transaction resource partitioning.
 */
#define	PX_ODTP		0x40		/* Max. of two entries PX and PCI */

/*
 * Maximum upstream delayed transaction.
 */
#define	PX_MDT_44	0x00
#define	PX_MDT_11	0x01
#define	PX_MDT_22	0x10


#define	NUM_LOGICAL_SLOTS	32
#define	PXB_RANGE_LEN		2
#define	PXB_32BIT_IO		1
#define	PXB_32bit_MEM		1
#define	PXB_MEMGRAIN		0x100000
#define	PXB_IOGRAIN		0x1000

#define	PXB_16bit_IOADDR(addr) ((uint16_t)(((uint8_t)(addr) & 0xF0) << 8))
#define	PXB_LADDR(lo, hi) (((uint16_t)(hi) << 16) | (uint16_t)(lo))
#define	PXB_32bit_MEMADDR(addr) (PXB_LADDR(0, ((uint16_t)(addr) & 0xFFF0)))

typedef struct  slot_table {
	uchar_t		bus_id[128];
	uchar_t		slot_name[32];
	uint8_t		device_no;
	uint8_t		phys_slot_num;
} slot_table_t;

/*
 * The following typedef is used to represent an entry in the "ranges"
 * property of a device node.
 */
typedef struct {
	uint32_t	child_high;
	uint32_t	child_mid;
	uint32_t	child_low;
	uint32_t	parent_high;
	uint32_t	parent_mid;
	uint32_t	parent_low;
	uint32_t	size_high;
	uint32_t	size_low;
} pxb_ranges_t;

typedef struct {
	dev_info_t		*pxb_dip;

	ddi_acc_handle_t	pxb_config_handle;

	/* Bridge or Switch, upstream or downstream */
	int			pxb_port_type;

	/* Interrupt */
	ddi_intr_handle_t	*pxb_htable;		/* Intr Handlers */
	int			pxb_htable_size;	/* htable size */
	int			pxb_intr_count;		/* Num of Intr */
	uint_t			pxb_intr_priority;	/* Intr Priority */
	int			pxb_intr_type;		/* (MSI | FIXED) */

	/*
	 * HP support
	 */
	boolean_t		pxb_hotplug_capable;

	kmutex_t		pxb_mutex;
	uint_t			pxb_soft_state;

	/* Initialization flags */
	int			pxb_init_flags;

	/* FMA */
	int pxb_fm_cap;
	ddi_iblock_cookie_t pxb_fm_ibc;

} pxb_devstate_t;

/*
 * soft state pointer and structure template:
 */
extern void *pxb_state;

/* pxb soft states */
#define	PXB_SOFT_STATE_CLOSED		0x00
#define	PXB_SOFT_STATE_OPEN		0x01
#define	PXB_SOFT_STATE_OPEN_EXCL	0x02

/* pxb init flags */
#define	PXB_INIT_MUTEX			0x01
#define	PXB_INIT_CONFIG_HANDLE		0x02
#define	PXB_INIT_HTABLE			0x04
#define	PXB_INIT_ALLOC			0x08
#define	PXB_INIT_HANDLER		0x10
#define	PXB_INIT_ENABLE			0x20
#define	PXB_INIT_BLOCK			0x40
#define	PXB_INIT_FM			0x80

#ifdef	__cplusplus
}
#endif

#endif	/* _SYS_PX_PCI_H */
