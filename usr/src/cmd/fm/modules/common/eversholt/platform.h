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
 *
 * platform -- platform-specific access to configuration database
 */

#ifndef	_EFT_PLATFORM_H
#define	_EFT_PLATFORM_H

#pragma ident	"%Z%%M%	%I%	%E% SMI"

#include <libnvpair.h>

#ifdef	__cplusplus
extern "C" {
#endif

#include "config.h"

void platform_init(void);
void platform_fini(void);
void platform_run_poller(const char *poller);
void platform_set_payloadnvp(nvlist_t *nvlp);
void platform_units_translate(int, struct config *, nvlist_t **, nvlist_t **,
    nvlist_t **, char *);

struct cfgdata *platform_config_snapshot(void);
struct node *platform_getpath(nvlist_t *nvl);

char **platform_get_eft_files(void);
void platform_free_eft_files(char **);

int platform_call(struct node *np, struct lut **globals, struct config *croot,
    struct arrow *arrowp, struct evalue *valuep);
int platform_payloadprop(struct node *np, struct evalue *valuep);

#ifdef	__cplusplus
}
#endif

#endif	/* _EFT_PLATFORM_H */
