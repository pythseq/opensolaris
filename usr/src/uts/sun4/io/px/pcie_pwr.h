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

#ifndef _SYS_PCIE_PWR_H
#define	_SYS_PCIE_PWR_H

#pragma ident	"%Z%%M%	%I%	%E% SMI"

#ifdef	__cplusplus
extern "C" {
#endif

#include "px_ioapi.h"	/* for msiq */

/*
 * device port types coarsely defined for PM purpose
 */
typedef enum pcie_devport {
	_PCIE_END, _PCIE_ROOTCMPLX, _SWITCH_UP,
	_SWITCH_DOWN, _BRIDGE_UP, _BRIDGE_DOWN, _PCI_PCI
} pcie_devport_t;

/* index of counters for each level */
#define	PCIE_D3_INDEX		PM_LEVEL_D3
#define	PCIE_D2_INDEX 		PM_LEVEL_D2
#define	PCIE_D1_INDEX		PM_LEVEL_D1
#define	PCIE_D0_INDEX		PM_LEVEL_D0
#define	PCIE_UNKNOWN_INDEX	PM_LEVEL_D0 + 1
#define	PCIE_MAX_PWR_LEVELS 	5

/*
 * PCIe nexus power management data structure
 */
typedef struct pcie_pwr {
	/*
	 * general data structure fields
	 */
	kmutex_t	pwr_lock;	/* to protect the counters and  */
					/* power level change		*/

	/* capability, dev type */
	int		pwr_pmcaps;	/* pm capability */
	ddi_acc_handle_t pwr_conf_hdl;	/* for config access */
	uint8_t		pwr_pmcsr_offset; /* PMCSR offset */
	pcie_devport_t	pwr_devport;	/* def. extended to include pci */
	int		pwr_link_lvl;	/* link level. Currently not used */
	int		pwr_func_lvl;	/* function power level */
	int		pwr_flags;	/* flags */
	int		pwr_hold;	/* for temporarily keeping busy */
	/*
	 * counters to keep track of child's power level.
	 * D3,D2,D1,D0 and unknown respectively.
	 */
	int		pwr_counters[PCIE_MAX_PWR_LEVELS];

	/* fields used by pcie driver */
	kmutex_t	pwr_intr_lock;	/* used in the interrupt */
	kcondvar_t	pwr_cv;		/* used in interrupt and timeout */
	msiqid_t	pwr_msiq_id;	/* EQ id for PME ack messages	*/
	uint32_t	pwr_pmetoack_ignored; /* count of PME_To_ACKs ignored */
} pcie_pwr_t;

typedef struct pcie_pwr_child {
	/*
	 * Per child dip counters decsribing
	 * a child's components
	 */
	int	pwr_child_counters[PCIE_MAX_PWR_LEVELS];
} pcie_pwr_child_t;

typedef struct pcie_pm {
	pcie_pwr_t	*pcie_pwr_p;	/* nexus PM info */
	pcie_pwr_child_t *pcie_par_pminfo; /* PM info created by the parent */
} pcie_pm_t;

#define	PCIE_PMINFO(dip)	\
	((pcie_pm_t *)(DEVI(dip)->devi_nex_pm))

#define	PCIE_NEXUS_PMINFO(dip)	\
	(PCIE_PMINFO(dip)->pcie_pwr_p)

#define	PCIE_PAR_PMINFO(dip)	\
	(PCIE_PMINFO(dip)->pcie_par_pminfo)

#define	PCIE_CHILD_COUNTERS(cdip)	\
	(PCIE_PAR_PMINFO(cdip)->pwr_child_counters)

#define	PCIE_SET_PMINFO(dip, pminfo_p)	\
	(DEVI(dip)->devi_nex_pm = (pminfo_p))

#define	PCIE_RESET_PMINFO(dip)	\
	(DEVI(dip)->devi_nex_pm = NULL)

#define	PCIE_IS_COMPS_COUNTED(cdip)	\
	(PCIE_PMINFO(cdip) && PCIE_PAR_PMINFO(cdip))

/*
 * pmcap field: device power management capability.
 * First 4 bits must indicate support for D3, D2, D1 and D0
 * respectively. Their bit position matches with their index
 * into the counters array.
 */
#define	PCIE_SUPPORTS_D3	0x01 /* Supports D1 */
#define	PCIE_SUPPORTS_D2	0x02 /* Supports D2 */
#define	PCIE_SUPPORTS_D1	0x04 /* Supports D2 */
#define	PCIE_SUPPORTS_D0	0x08 /* Supports D2 */
#define	PCIE_L2_CAP		0x10 /* if with Vaux, optional */
#define	PCIE_L0s_L1_CAP		0x20 /* ASPM, L0s must, L1 optional */

#define	PCIE_DEFAULT_LEVEL_SUPPORTED	(PCIE_SUPPORTS_D3 | PCIE_SUPPORTS_D0)

#define	PCIE_LEVEL_SUPPORTED(pmcaps, level)	\
	((pmcaps) & (1 << (level)))

#define	PCIE_SUPPORTS_DEVICE_PM(dip)	\
	(ddi_prop_exists(DDI_DEV_T_ANY, (dip),	\
	    DDI_PROP_DONTPASS | DDI_PROP_NOTPROM, "pm-components") == 1)
/*
 * flags field
 */
#define	PCIE_ASPM_ENABLED		0x01
#define	PCIE_SLOT_LOADED		0x02
#define	PCIE_PMETOACK_RECVD		0x04
#define	PCIE_PME_TURNOFF_PENDING	0x08
#define	PCIE_PM_BUSY			0x10

#define	PM_LEVEL_L3	0
#define	PM_LEVEL_L2	1
#define	PM_LEVEL_L1	2
#define	PM_LEVEL_L0	3

/* ioctl definitions for ppm drivers */
#define	PPMREQ			(('P' << 24) | ('M' << 16))
#define	PPMREQ_MASK		0xffff
#define	PPMREQ_PRE_PWR_OFF	(PPMREQ | 1)
#define	PPMREQ_PRE_PWR_ON	(PPMREQ | 2)
#define	PPMREQ_POST_PWR_ON	(PPMREQ | 3)

/* settle time in microseconds before PCI operation */
#define	PCI_CLK_SETTLE_TIME	10000

/*
 * Interface with other parts of the driver(s) code
 */
extern int pwr_common_setup(dev_info_t *dip);
extern void pwr_common_teardown(dev_info_t *dip);
extern int pcie_bus_power(dev_info_t *dip, void *impl_arg, pm_bus_power_op_t op,
    void *arg, void *result);
extern int pcie_power(dev_info_t *dip, int component, int level);

extern int pcie_pm_add_child(dev_info_t *dip, dev_info_t *cdip);
extern int pcie_pm_remove_child(dev_info_t *dip, dev_info_t *cdip);
extern int pcie_pwr_suspend(dev_info_t *dip);
extern int pcie_pwr_resume(dev_info_t *dip);
extern int pcie_pm_hold(dev_info_t *dip);
extern void pcie_pm_release(dev_info_t *dip);
extern void pcie_pmetoack_timeout(void *arg);
extern uint_t pcie_pwr_intr(caddr_t arg);

#ifdef	__cplusplus
}
#endif

#endif	/* _SYS_PCIE_PWR_H */
