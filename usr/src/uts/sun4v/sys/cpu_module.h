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

#ifndef _SYS_CPU_MODULE_H
#define	_SYS_CPU_MODULE_H

#pragma ident	"%Z%%M%	%I%	%E% SMI"

#include <sys/pte.h>
#include <sys/async.h>
#include <sys/x_call.h>
#include <sys/conf.h>
#include <sys/obpdefs.h>

#ifdef	__cplusplus
extern "C" {
#endif


#ifdef _KERNEL

/*
 * The are functions that are expected of the cpu modules.
 */

extern struct module_ops *moduleops;

struct kdi;

/*
 * module initialization
 */
void	cpu_setup(void);

/*
 * set CPU implementation details
 *
 * mmu_init_mmu_page_sizes changes the mmu_page_sizes variable from
 *	The default 4 page sizes to 6 page sizes for Panther-only domains,
 *	and is called from fillsysinfo.c:check_cpus_set at early bootup time.
 */
struct cpu_node;
void	cpu_fiximp(struct cpu_node *cpunode);
#pragma weak mmu_init_mmu_page_sizes
int	mmu_init_mmu_page_sizes(int cinfo);

/*
 * virtual demap flushes (tlbs & virtual tag caches)
 */
void	vtag_flushpage(caddr_t addr, uint_t ctx);
void	vtag_flushctx(uint_t ctx);
void	vtag_flushall(void);
void	vtag_flushpage_tl1(uint64_t addr, uint64_t ctx);
void	vtag_flush_pgcnt_tl1(uint64_t addr, uint64_t ctx_pgcnt);
void	vtag_flushctx_tl1(uint64_t ctx, uint64_t dummy);
void	vtag_flushall_tl1(uint64_t dummy1, uint64_t dummy2);
void	vtag_unmap_perm_tl1(uint64_t addr, uint64_t ctx);

/*
 * virtual alias flushes (virtual address caches)
 */
void	vac_flushpage(pfn_t pf, int color);
void	vac_flushpage_tl1(uint64_t pf, uint64_t color);
void	vac_flushcolor(int color, pfn_t pf);
void	vac_flushcolor_tl1(uint64_t color, uint64_t dummy);

/*
 * Calculate, set optimal dtlb pagesize, for ISM and mpss, to support
 * cpus with non-fully-associative dtlbs.
 */
extern uchar_t *ctx_pgsz_array;

/*
 * flush instruction cache if needed
 */
void	flush_instr_mem(caddr_t addr, size_t len);

/*
 * Cpu-specific error and ecache handling routines
 */
#pragma weak itlb_parity_trap
void itlb_parity_trap(void);

#pragma weak dtlb_parity_trap
void dtlb_parity_trap(void);

/*
 * this symbol appears as a second label for vtag_flushall
 * only for cpus that implement DEMAP_ALL_TYPE
 */
#pragma	weak demap_all

/*
 * change cpu speed
 */
void	cpu_change_speed(uint64_t divisor, uint64_t arg2);

/*
 * flush routine
 */
#pragma weak dtrace_flush_sec
void	dtrace_flush_sec(uintptr_t);

/*
 * Cpu private initialize/uninitialize, including ecache scrubber.
 */
void	cpu_init_private(struct cpu *);
void	cpu_uninit_private(struct cpu *);

#pragma weak cpu_mp_init
void    cpu_mp_init(void);

#pragma weak cpu_feature_init
void    cpu_feature_init(void);

#pragma weak cpu_error_init
void	cpu_error_init(int);

extern int kzero(void *addr, size_t count);
extern void uzero(void *addr, size_t count);
extern void bzero(void *addr, size_t count);

#endif /* _KERNEL */

#ifdef	__cplusplus
}
#endif

#endif /* _SYS_CPU_MODULE_H */
