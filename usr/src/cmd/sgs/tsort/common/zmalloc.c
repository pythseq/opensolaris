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


#pragma ident	"%Z%%M%	%I%	%E% SMI"

/*
 *	malloc(3C) with error checking
 */

#include <stdio.h>
#include "errmsg.h"
#ifdef __STDC__
#include <stdlib.h>
#else
extern char *malloc();
#endif

char *
zmalloc(severity, n)
int	severity;
unsigned	n;
{
	char	*p;

	if ((p = (char *) malloc(n)) == NULL)
		_errmsg("UXzmalloc1", severity,
			"Cannot allocate a block of %d bytes.",
			n);
	return (p);
}
