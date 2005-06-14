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
# ident	"%Z%%M%	%I%	%E% SMI"
#
# lib/libproc/Makefile.com

LIBRARY = libproc.a
VERS = .1

CMNOBJS =	\
	P32ton.o	\
	Pcontrol.o	\
	Pcore.o		\
	Pexecname.o	\
	Pgcore.o	\
	Pidle.o		\
	Pisprocdir.o	\
	Plwpregs.o	\
	Pservice.o	\
	Psymtab.o	\
	Pscantext.o	\
	Pstack.o	\
	Psyscall.o	\
	Putil.o		\
	pr_door.o	\
	pr_exit.o	\
	pr_fcntl.o	\
	pr_getitimer.o	\
	pr_getrctl.o	\
	pr_getrlimit.o	\
	pr_getsockname.o \
	pr_ioctl.o	\
	pr_lseek.o	\
	pr_memcntl.o	\
	pr_meminfo.o	\
	pr_mmap.o	\
	pr_open.o	\
	pr_pbind.o	\
	pr_rename.o	\
	pr_sigaction.o	\
	pr_stat.o	\
	pr_statvfs.o	\
	pr_tasksys.o	\
	pr_waitid.o	\
	proc_get_info.o	\
	proc_names.o	\
	proc_arg.o	\
	proc_set.o	\
	proc_stdio.o

ISAOBJS =	\
	Pisadep.o

OBJECTS = $(CMNOBJS) $(ISAOBJS)

# include library definitions
include ../../Makefile.lib
include ../../Makefile.rootfs

SRCS =		$(CMNOBJS:%.o=../common/%.c) $(ISAOBJS:%.o=%.c)

LIBS =		$(DYNLIB) $(LINTLIB)
LDLIBS +=	-lrtld_db -lelf -lctf -lc
$(LINTLIB) :=	SRCS = $(SRCDIR)/$(LINTSRC)

SRCDIR =	../common
MAPDIR =	../spec/$(TRANSMACH)
SPECMAPFILE =	$(MAPDIR)/mapfile

CFLAGS +=	$(CCVERBOSE)
CPPFLAGS +=	-I$(SRCDIR)

.KEEP_STATE:

all: $(LIBS)

lint: lintcheck

# include library targets
include ../../Makefile.targ

objs/%.o pics/%.o: %.c
	$(COMPILE.c) -o $@ $<
	$(POST_PROCESS_O)
