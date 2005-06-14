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

#include	<string.h>
#include	"machdep.h"
#include	"reloc.h"
#include	"_librtld.h"
#include	"_elf.h"


/*
 * Undo relocations that have been applied to a memory image.  Basically this
 * involves copying the original files relocation offset into the new image
 * being created.
 */
void
undo_reloc(void *vrel, unsigned char *oaddr, unsigned char *iaddr,
    Reloc *reloc)
{
#if 0		/* XX64 -- fix me */
	Rel		*rel = vrel;
	/* LINTED */
	unsigned long	*_oaddr = (unsigned long *)oaddr;
	/* LINTED */
	unsigned long	*_iaddr = (unsigned long *)iaddr;

	switch (ELF_R_TYPE(rel->r_info)) {
	case R_386_NONE:
		break;

	case R_386_COPY:
		(void) memset((void *)oaddr, 0, (size_t)reloc->r_size);
		break;

	case R_386_JMP_SLOT:
		if (_iaddr)
			*_oaddr = *_iaddr + reloc->r_value;
		else
			*_oaddr = reloc->r_value;
		break;

	default:
		if (_iaddr)
			*_oaddr = *_iaddr;
		else
			*_oaddr = 0;
		break;
	}
#endif	/* XX64 -- fix me */
}


/*
 * Copy a relocation record and increment its value.  The record must reflect
 * the new address to which this image is fixed.  Note that .got entries
 * associated with .plt's must be fixed to the new base address.
 */
void
inc_reloc(void *vnrel, void *vorel, Reloc *reloc, unsigned char *oaddr,
    unsigned char *iaddr)
{
#if 0		/* XX64 -- fix me */
	Rel		*nrel = vnrel;
	Rel		*orel = vorel;
	/* LINTED */
	unsigned long	*_oaddr = (unsigned long *)oaddr;
	/* LINTED */
	unsigned long	*_iaddr = (unsigned long *)iaddr;

	if (ELF_R_TYPE(nrel->r_info) == R_386_JMP_SLOT) {
		if (_iaddr)
			*_oaddr = *_iaddr + reloc->r_value;
		else
			*_oaddr = reloc->r_value;
	}

	*nrel = *orel;
	nrel->r_offset += reloc->r_value;
#endif	/* XX64 -- fix me */
}


/*
 * Clear a relocation record.  The relocation has been applied to the image and
 * thus the relocation must not occur again.
 */
void
clear_reloc(void * vrel)
{
#if 0		/* XX64 -- fix me */
	Rel *	rel = vrel;

	rel->r_offset = 0;
	rel->r_info = ELF_R_INFO(0, R_386_NONE);
#endif	/* XX64 -- fix me */
}


/*
 * Apply a relocation to an image being built from an input file.  Use the
 * runtime linkers routines to do the necessary magic.
 */
void
apply_reloc(void *vrel, Reloc *reloc, const char *name,
    unsigned char *oaddr, Rt_map *lmp)
{
#if 0		/* XX64 -- fix me */
	Rel	*rel = vrel;
	Byte	type = ELF_R_TYPE(rel->r_info);
	Word	value = reloc->r_value;

	if (type == R_386_JMP_SLOT) {
		uintptr_t	addr, vaddr;

		if (FLAGS(lmp) & FLG_RT_FIXED)
			vaddr = 0;
		else
			vaddr = ADDR(lmp);
		addr = (uintptr_t)oaddr - rel->r_offset;
		/* LINTED */
		elf_plt_write((uintptr_t)addr, vaddr, rel,
		    (uintptr_t)value, reloc->r_pltndx);
	} else if (type == R_386_COPY) {
		(void) memcpy((void *)oaddr, (void *)value,
		    (size_t)reloc->r_size);

	} else {
		(void) do_reloc(type, oaddr, &value, reloc->r_name, name);
	}
#endif	/* XX64 -- fix me */
}
