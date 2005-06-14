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
/*	Copyright (c) 1988 AT&T	*/
/*	  All Rights Reserved  	*/


/*
 * Copyright (c) 1998 by Sun Microsystems, Inc.
 * All rights reserved.
 */

#pragma ident	"%Z%%M%	%I%	%E% SMI" 	/* SVr4.0 1.5	*/

#pragma weak	elf_next = _elf_next


#include "syn.h"
#include "libelf.h"
#include "decl.h"


Elf_Cmd
elf_next(Elf * elf)
{
	Elf	*parent;

	if (elf == 0)
		return (ELF_C_NULL);
	ELFRLOCK(elf)
	if ((parent = elf->ed_parent) == 0) {
		ELFUNLOCK(elf);
		return (ELF_C_NULL);
	}
	ELFWLOCK(parent)
	if (elf->ed_siboff >= parent->ed_fsz) {
		ELFUNLOCK(parent)
		ELFUNLOCK(elf);
		return (ELF_C_NULL);
	}

	parent->ed_nextoff = elf->ed_siboff;
	ELFUNLOCK(parent)
	ELFUNLOCK(elf);
	return (ELF_C_READ);
}
