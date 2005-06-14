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
 * PCI Express PEC implementation:
 *	initialization
 *	Bus error interrupt handler
 */

#include <sys/types.h>
#include <sys/kmem.h>
#include <sys/spl.h>
#include <sys/sysmacros.h>
#include <sys/sunddi.h>
#include <sys/machsystm.h>	/* ldphysio() */
#include <sys/async.h>
#include <sys/ddi_impldefs.h>
#include <sys/ontrap.h>
#include <sys/membar.h>
#include "px_obj.h"

/*LINTLIBRARY*/

extern uint_t px_ranges_phi_mask;

static int    px_pec_msg_add_intr(px_t *px_p);
static void   px_pec_msg_rem_intr(px_t *px_p);

static void
px_ilu_attach(px_pec_t *pec_p)
{
	/*
	 * Register ilu error interrupt.  This will
	 * also program the correct values into the
	 * log enable and interrupt enable registers.
	 */
	px_err_add_fh(&pec_p->pec_px_p->px_fault, PX_ERR_ILU,
	    (caddr_t)pec_p->pec_px_p->px_address[PX_REG_CSR]);
}

int
px_ilu_intr(dev_info_t *dip, px_fh_t *fh_p)
{
	uint32_t offset = px_fhd_tbl[fh_p->fh_err_id].fhd_st;
	uint64_t stat = fh_p->fh_stat;

	if (stat)
		LOG(DBG_ERR_INTR, dip, "[%x]=%16llx ilu stat\n", offset, stat);
	return (stat ? DDI_INTR_CLAIMED : DDI_INTR_UNCLAIMED);
}

static void
px_tlu_attach(px_pec_t *pec_p)
{
	caddr_t	csr_base = (caddr_t)pec_p->pec_px_p->px_address[PX_REG_CSR];
	px_fault_t *px_fault_p = &pec_p->pec_px_p->px_fault;

	px_err_add_fh(px_fault_p, PX_ERR_TLU_UE, csr_base);
	px_err_add_fh(px_fault_p, PX_ERR_TLU_CE, csr_base);
	px_err_add_fh(px_fault_p, PX_ERR_TLU_OE, csr_base);
}

static void
px_lpu_attach(px_pec_t *pec_p)
{
	caddr_t csr_base = (caddr_t)pec_p->pec_px_p->px_address[PX_REG_CSR];
	px_fault_t *px_fault_p = &pec_p->pec_px_p->px_fault;

	px_err_add_fh(px_fault_p, PX_ERR_LPU_LINK, csr_base);
	px_err_add_fh(px_fault_p, PX_ERR_LPU_PHY, csr_base);
	px_err_add_fh(px_fault_p, PX_ERR_LPU_REC_PHY, csr_base);
	px_err_add_fh(px_fault_p, PX_ERR_LPU_TRNS_PHY, csr_base);
	px_err_add_fh(px_fault_p, PX_ERR_LPU_LTSSM, csr_base);
	px_err_add_fh(px_fault_p, PX_ERR_LPU_GIGABLZ, csr_base);
}

int
px_tlu_ue_intr(dev_info_t *dip, px_fh_t *fh_p)
{
	uint32_t offset = px_fhd_tbl[fh_p->fh_err_id].fhd_st;
	uint64_t stat = fh_p->fh_stat;

	if (stat)
		LOG(DBG_ERR_INTR, dip, "[%x]=%16llx tlu ue stat\n", offset,
			stat);
	return (stat ? DDI_INTR_CLAIMED : DDI_INTR_UNCLAIMED);
}

int
px_tlu_ce_intr(dev_info_t *dip, px_fh_t *fh_p)
{
	uint32_t offset = px_fhd_tbl[fh_p->fh_err_id].fhd_st;
	uint64_t stat = fh_p->fh_stat;

	if (stat)
		LOG(DBG_ERR_INTR, dip, "[%x]=%16llx tlu ce stat\n", offset,
			stat);
	return (stat ? DDI_INTR_CLAIMED : DDI_INTR_UNCLAIMED);
}

int
px_tlu_oe_intr(dev_info_t *dip, px_fh_t *fh_p)
{
	uint32_t offset = px_fhd_tbl[fh_p->fh_err_id].fhd_st;
	uint64_t stat = fh_p->fh_stat;

	if (stat)
		LOG(DBG_ERR_INTR, dip, "[%x]=%16llx tlu other stat\n", offset,
			stat);
	return (stat ? DDI_INTR_CLAIMED : DDI_INTR_UNCLAIMED);
}

int
px_lpu_intr(dev_info_t *dip, px_fh_t *fh_p)
{
	uint32_t offset = px_fhd_tbl[fh_p->fh_err_id].fhd_st;
	uint64_t stat = fh_p->fh_stat;

	if (stat)
		LOG(DBG_ERR_INTR, dip, "[%x]=%16llx lpu stat\n", offset, stat);
	return (stat ? DDI_INTR_CLAIMED : DDI_INTR_UNCLAIMED);
}

int
px_pec_attach(px_t *px_p)
{
	px_pec_t *pec_p;
	int i, len;
	int nrange = px_p->px_ranges_length / sizeof (px_ranges_t);
	dev_info_t *dip = px_p->px_dip;
	px_ranges_t *rangep = px_p->px_ranges_p;
	int ret;

	/*
	 * Allocate a state structure for the PEC and cross-link it
	 * to its per px node state structure.
	 */
	pec_p = kmem_zalloc(sizeof (px_pec_t), KM_SLEEP);
	px_p->px_pec_p = pec_p;
	pec_p->pec_px_p = px_p;

	len = snprintf(pec_p->pec_nameinst_str,
		sizeof (pec_p->pec_nameinst_str),
		"%s%d", NAMEINST(dip));
	pec_p->pec_nameaddr_str = pec_p->pec_nameinst_str + ++len;
	(void) snprintf(pec_p->pec_nameaddr_str,
		sizeof (pec_p->pec_nameinst_str) - len,
		"%s@%s", NAMEADDR(dip));

	/*
	 * Add interrupt handlers to process correctable/fatal/non fatal
	 * PCIE messages.
	 */
	if ((ret = px_pec_msg_add_intr(px_p)) != DDI_SUCCESS) {
		px_pec_msg_rem_intr(px_p);
		return (ret);
	}

	/*
	 * Get this pec's mem32 and mem64 segments to determine whether
	 * a dma object originates from ths pec. i.e. dev to dev dma
	 */
	for (i = 0; i < nrange; i++, rangep++) {
		uint64_t rng_addr, rng_size, *pfnbp, *pfnlp;
		uint32_t rng_type = rangep->child_high & PCI_ADDR_MASK;

		switch (rng_type) {
			case PCI_ADDR_MEM32:
				pfnbp = &pec_p->pec_base32_pfn;
				pfnlp = &pec_p->pec_last32_pfn;
				break;

			case PCI_ADDR_MEM64:
				pfnbp = &pec_p->pec_base64_pfn;
				pfnlp = &pec_p->pec_last64_pfn;
				break;

			case PCI_ADDR_CONFIG:
			case PCI_ADDR_IO:
			default:
				continue;
		}
		rng_addr = (uint64_t)(rangep->parent_high &
					px_ranges_phi_mask) << 32;
		rng_addr |= (uint64_t)rangep->parent_low;
		rng_size = (uint64_t)rangep->size_high << 32;
		rng_size |= (uint64_t)rangep->size_low;

		*pfnbp = mmu_btop(rng_addr);
		*pfnlp = mmu_btop(rng_addr + rng_size);
	}

	/*
	 * configure ILU.
	 */
	px_ilu_attach(pec_p);

	/*
	 * configure TLU.
	 */
	px_tlu_attach(pec_p);

	/*
	 * configure LPU
	 */
	px_lpu_attach(pec_p);

	/*
	 * Register a function to disable pec error interrupts during a panic.
	 * do in px_attach. bus_func_register(BF_TYPE_ERRDIS,
	 * (busfunc_t)pec_disable_pci_errors, pec_p);
	 */

	mutex_init(&pec_p->pec_pokefault_mutex, NULL, MUTEX_DRIVER, 0);

	return (DDI_SUCCESS);
}

uint_t
pec_disable_px_errors(px_pec_t *pec_p)
{
	px_t *px_p = pec_p->pec_px_p;
	px_ib_t *ib_p = px_p->px_ib_p;

	/*
	 * Disable error interrupts via the interrupt mapping register.
	 */
	px_ib_intr_disable(ib_p, px_p->px_inos[PX_INTR_PEC], IB_INTR_NOWAIT);
	return (BF_NONE);
}

void
px_pec_detach(px_t *px_p)
{
	dev_info_t *dip = px_p->px_dip;
	px_pec_t *pec_p = px_p->px_pec_p;
	px_ib_t *ib_p = px_p->px_ib_p;
	devino_t ino = px_p->px_inos[PX_INTR_PEC];

	/*
	 * Free the pokefault mutex.
	 */
	DBG(DBG_DETACH, dip, "px_pec_detach:\n");
	mutex_destroy(&pec_p->pec_pokefault_mutex);

	/*
	 * Remove the pci error interrupt handler.
	 */
	px_ib_intr_disable(ib_p, ino, IB_INTR_WAIT);
	ddi_remove_intr(dip, 0, NULL);

	/*
	 * Remove the error disable function.
	 */
	bus_func_unregister(BF_TYPE_ERRDIS,
	    (busfunc_t)pec_disable_px_errors, pec_p);

	/*
	 * Remove interrupt handlers to process correctable/fatal/non fatal
	 * PCIE messages.
	 */
	px_pec_msg_rem_intr(px_p);

	/*
	 * Free the pec state structure.
	 */
	kmem_free(pec_p, sizeof (px_pec_t));
	px_p->px_pec_p = NULL;
}

/*
 * pec_msg_add_intr:
 *
 * Add interrupt handlers to process correctable/fatal/non fatal
 * PCIE messages.
 */
static int
px_pec_msg_add_intr(px_t *px_p)
{
	dev_info_t		*dip = px_p->px_dip;
	px_pec_t		*pec_p = px_p->px_pec_p;
	ddi_intr_handle_impl_t	hdl;
	int			ret = DDI_SUCCESS;

	DBG(DBG_MSG, px_p->px_dip, "px_pec_msg_add_intr\n");

	/* Initilize handle */
	hdl.ih_ver = DDI_INTR_VERSION;
	hdl.ih_state = DDI_IHDL_STATE_ALLOC;
	hdl.ih_dip = dip;
	hdl.ih_inum = 0;
	hdl.ih_pri = PX_ERR_PIL;

	/* Add correctable error message handler */
	hdl.ih_cb_func = (ddi_intr_handler_t *)px_pec_corr_msg_intr;
	hdl.ih_cb_arg1 = px_p;
	hdl.ih_cb_arg2 = NULL;

	if ((ret = px_add_msiq_intr(dip, dip, &hdl,
	    MSG_REC, (msgcode_t)PCIE_CORR_MSG,
	    &pec_p->pec_corr_msg_msiq_id)) != DDI_SUCCESS) {
		DBG(DBG_MSG, px_p->px_dip,
		    "PCIE_CORR_MSG registration failed\n");
		return (DDI_FAILURE);
	}

	px_lib_msg_setmsiq(dip, PCIE_CORR_MSG, pec_p->pec_corr_msg_msiq_id);
	px_lib_msg_setvalid(dip, PCIE_CORR_MSG, PCIE_MSG_VALID);

	/* Add non-fatal error message handler */
	hdl.ih_cb_func = (ddi_intr_handler_t *)px_pec_non_fatal_msg_intr;
	hdl.ih_cb_arg1 = px_p;
	hdl.ih_cb_arg2 = NULL;

	if ((ret = px_add_msiq_intr(dip, dip, &hdl,
	    MSG_REC, (msgcode_t)PCIE_NONFATAL_MSG,
	    &pec_p->pec_non_fatal_msg_msiq_id)) != DDI_SUCCESS) {
		DBG(DBG_MSG, px_p->px_dip,
		    "PCIE_NONFATAL_MSG registration failed\n");
		return (DDI_FAILURE);
	}

	px_lib_msg_setmsiq(dip, PCIE_NONFATAL_MSG,
	    pec_p->pec_non_fatal_msg_msiq_id);
	px_lib_msg_setvalid(dip, PCIE_NONFATAL_MSG, PCIE_MSG_VALID);

	/* Add fatal error message handler */
	hdl.ih_cb_func = (ddi_intr_handler_t *)px_pec_fatal_msg_intr;
	hdl.ih_cb_arg1 = px_p;
	hdl.ih_cb_arg2 = NULL;

	if ((ret = px_add_msiq_intr(dip, dip, &hdl,
	    MSG_REC, (msgcode_t)PCIE_FATAL_MSG,
	    &pec_p->pec_fatal_msg_msiq_id)) != DDI_SUCCESS) {
		DBG(DBG_MSG, px_p->px_dip,
		    "PCIE_FATAL_MSG registration failed\n");
		return (DDI_FAILURE);
	}

	px_lib_msg_setmsiq(dip, PCIE_FATAL_MSG, pec_p->pec_fatal_msg_msiq_id);
	px_lib_msg_setvalid(dip, PCIE_FATAL_MSG, PCIE_MSG_VALID);

	return (ret);
}

/*
 * px_pec_msg_rem_intr:
 *
 * Remove interrupt handlers to process correctable/fatal/non fatal
 * PCIE messages. For now, all these PCIe messages are mapped to
 * same MSIQ.
 */
static void
px_pec_msg_rem_intr(px_t *px_p)
{
	dev_info_t		*dip = px_p->px_dip;
	px_pec_t		*pec_p = px_p->px_pec_p;
	ddi_intr_handle_impl_t	hdl;

	DBG(DBG_MSG, px_p->px_dip, "px_pec_msg_rem_intr: dip 0x%p\n", dip);

	/* Initilize handle */
	hdl.ih_ver = DDI_INTR_VERSION;
	hdl.ih_state = DDI_IHDL_STATE_ALLOC;
	hdl.ih_dip = dip;
	hdl.ih_inum = 0;

	if (pec_p->pec_corr_msg_msiq_id >= 0) {
		px_lib_msg_setvalid(dip, PCIE_CORR_MSG, PCIE_MSG_INVALID);

		(void) px_rem_msiq_intr(dip, dip, &hdl, MSG_REC,
		    PCIE_CORR_MSG, pec_p->pec_corr_msg_msiq_id);
		pec_p->pec_corr_msg_msiq_id = -1;
	}

	if (pec_p->pec_non_fatal_msg_msiq_id >= 0) {
		px_lib_msg_setvalid(dip, PCIE_NONFATAL_MSG,
		    PCIE_MSG_INVALID);

		(void) px_rem_msiq_intr(dip, dip, &hdl, MSG_REC,
		    PCIE_NONFATAL_MSG, pec_p->pec_non_fatal_msg_msiq_id);

		pec_p->pec_non_fatal_msg_msiq_id = -1;
	}

	if (pec_p->pec_fatal_msg_msiq_id >= 0) {
		px_lib_msg_setvalid(dip, PCIE_FATAL_MSG, PCIE_MSG_INVALID);

		(void) px_rem_msiq_intr(dip, dip, &hdl, MSG_REC,
		    PCIE_FATAL_MSG, pec_p->pec_fatal_msg_msiq_id);

		pec_p->pec_fatal_msg_msiq_id = -1;
	}
}

/*ARGSUSED*/
uint_t
px_pec_corr_msg_intr(caddr_t arg)
{
	px_t		*px_p = (px_t *)arg;
	uint64_t	rid = px_p->px_pec_p->pec_msiq_rec_p->msiq_rec_rid;

	DBG(DBG_MSG_INTR, px_p->px_dip,
	    "px_pec_corr_msg_intr: requester id 0x%x\n", rid);

	px_p->px_pec_p->pec_msiq_rec_p = NULL;

	return (DDI_INTR_CLAIMED);
}

/*ARGSUSED*/
uint_t
px_pec_non_fatal_msg_intr(caddr_t arg)
{
	px_t		*px_p = (px_t *)arg;
	uint64_t	rid = px_p->px_pec_p->pec_msiq_rec_p->msiq_rec_rid;

	DBG(DBG_MSG_INTR, px_p->px_dip,
	    "px_pec_non_fatal_msg_intr: requester id 0x%x\n", rid);

	px_p->px_pec_p->pec_msiq_rec_p = NULL;

	return (DDI_INTR_CLAIMED);
}

/*ARGSUSED*/
uint_t
px_pec_fatal_msg_intr(caddr_t arg)
{
	px_t		*px_p = (px_t *)arg;
	uint64_t	rid = px_p->px_pec_p->pec_msiq_rec_p->msiq_rec_rid;

	DBG(DBG_MSG_INTR, px_p->px_dip,
	    "px_pec_fatal_msg_intr: requester id 0x%x\n", rid);

	px_p->px_pec_p->pec_msiq_rec_p = NULL;

	return (DDI_INTR_CLAIMED);
}
