#
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License, Version 1.0 only
# (the "License").  You may not use this file except in compliance
# with the License.
#
# You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
# or http://www.opensolaris.org/os/licensing.
# See the License for the specific language governing permissions
# and limitations under the License.
#
# When distributing Covered Code, include this CDDL HEADER in each
# file and include the License file at usr/src/OPENSOLARIS.LICENSE.
# If applicable, add the following below this CDDL HEADER, with the
# fields enclosed by brackets "[]" replaced with your own identifying
# information: Portions Copyright [yyyy] [name of copyright owner]
#
# CDDL HEADER END
#
#
# Copyright 2004 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#
#ident	"%Z%%M%	%I%	%E% SMI"

.KEEP_STATE:

PROG = esc

include $(SRC)/cmd/fm/eversholt/Makefile.esc.com

EFTCLASS = writer
LOCALOBJS = escmain.o
OBJS = $(LOCALOBJS) $(COMMONOBJS)
SRCS = $(LOCALOBJS:.o=.c) $(COMMONSRCS)

#
# Reset STRIPFLAG to the empty string.  esc is intentionally
# installed with symbol tables to aid compiler debugging.
#
STRIPFLAG=
CPPFLAGS += -I../common
CFLAGS += -DESC $(CTF_FLAGS)
LDLIBS += -lumem

all debug: $(PROG)

install: all $(ROOTPROG)

LINTSRCS += $(LOCALOBJS:%.o=../common/%.c)

$(PROG): $(OBJS)
	$(LINK.c) -o $@ $(OBJS) $(LDLIBS)
	$(CTFMRG)
	$(POST_PROCESS)

clean:
	$(RM) $(OBJS) y.output y.tab.c y.tab.h a.out core

clobber: clean
	$(RM) $(PROG)

esclex.o: escparse.o

%.o: ../common/%.c
	$(COMPILE.c) $<
	$(CTFCONVO)
