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
# ident	"%Z%%M%	%I%	%E% SMI"
#
# Copyright 2004 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#
# cmd/sgs/Makefile.com

.KEEP_STATE:

include		$(SRC)/cmd/sgs/Makefile.var

SRCBASE=	../../../..

i386_ARCH=	$(VAR_I386_ARCH)
sparc_ARCH=	sparc

ARCH=		$($(MACH)_ARCH)

ROOTCCSBIN64=		$(ROOTCCSBIN)/$(MACH64)
ROOTCCSBINPROG64=	$(PROG:%=$(ROOTCCSBIN64)/%)

# Establish any global flags.

# Setting DEBUG = -DDEBUG (or "make DEBUG=-DDEBUG ...")
# enables ASSERT() checking in the library
# This is automatically enabled for DEBUG builds, not for non debug builds.
DEBUG=
$(NOT_RELEASE_BUILD)DEBUG = -DDEBUG

CFLAGS +=	$(CCVERBOSE) $(DEBUG) $(XFFLAG)
CFLAGS64 +=	$(CCVERBOSE) $(DEBUG) $(XFFLAG)

# Reassign CPPFLAGS so that local search paths are used before any parent
# $ROOT paths.
CPPFLAGS=	-I. -I../common -I../../include -I../../include/$(MACH) \
		$(VAR_CPPFLAGS) $(CPPFLAGS.master)

# PICS64 is unique to our environment
$(PICS64) :=	sparc_CFLAGS += -xregs=no%appl -K pic
$(PICS64) :=	sparcv9_CFLAGS += -xregs=no%appl -K pic
$(PICS64) :=	CPPFLAGS += -DPIC -D_REENTRANT

LDZIGNORE=	-zignore
LDFLAGS +=	$(LDZIGNORE)
DYNFLAGS +=	$(LDZIGNORE)

# Establish the local tools, proto and package area.

SGSHOME=	$(SRC)/cmd/sgs
SGSPROTO=	$(SGSHOME)/proto/$(MACH)
SGSTOOLS=	$(SGSHOME)/tools
SGSMSGID=	$(SGSHOME)/messages
SGSMSGDIR=	$(SGSHOME)/messages/$(MACH)
SGSONLD=	$(ROOT)/opt/SUNWonld
SGSRPATH=	/usr/lib
SGSRPATH64=	$(SGSRPATH)/$(MACH64)

#
# Macros to be used to include link against libconv and include
# vernote.o
#
VERSREF=	-ulink_ver_string
CONVLIBDIR=	-L$(SGSHOME)/libconv/$(MACH)
CONVLIBDIR64=	-L$(SGSHOME)/libconv/$(MACH64)

ELFLIBDIR=	-L$(SGSHOME)/libelf/$(MACH)
ELFLIBDIR64=	-L$(SGSHOME)/libelf/$(MACH64)

LDDBGLIBDIR=	-L$(SGSHOME)/liblddbg/$(MACH)
LDDBGLIBDIR64=	-L$(SGSHOME)/liblddbg/$(MACH64)



# The cmd/Makefile.com and lib/Makefile.com define TEXT_DOMAIN.  We don't need
# this definition as the sgs utilities obtain their domain via sgsmsg(1l).

DTEXTDOM=


# Define any generic sgsmsg(1l) flags.  The default message generation system
# is to use gettext(3i), add the -C flag to switch to catgets(3c).

SGSMSG=		$(SGSTOOLS)/$(MACH)/sgsmsg
CHKMSG=		$(SGSTOOLS)/chkmsg.sh

SGSMSGVFLAG =
SGSMSGFLAGS =	$(SGSMSGVFLAG) -i $(SGSMSGID)/sgs.ident
CHKMSGFLAGS=	$(SGSMSGTARG:%=-m %) $(SGSMSGCHK:%=-m %)


# Native targets should use the minimum of ld(1) flags to allow building on
# previous releases.  We use mapfiles to scope, but don't bother versioning.

native:=	DYNFLAGS = $(MAPOPTS) -R$(SGSPROTO) -L$(SGSPROTO) $(ZNOVERSION)

USE_PROTO=	-Yl,$(SGSPROTO)

.KEEP_STATE_FILE: .make.state.$(MACH)

#
# lint-related stuff
#

DASHES=		"------------------------------------------------------------"

LIBNAME32 =	$(LIBNAME:%=%32)
LIBNAME64 =	$(LIBNAME:%=%64)
LIBNAMES =	$(LIBNAME32) $(LIBNAME64)

SGSLINTOUT =	lint.out
LINTOUT1 =	lint.out.1
LINTOUT32 =	lint.out.32
LINTOUT64 =	lint.out.64
LINTOUTS =	$(SGSLINTOUT) $(LINTOUT1) $(LINTOUT32) $(LINTOUT64)

LINTLIBSRC =	$(LINTLIB:%.ln=%)
LINTLIB32 =	$(LINTLIB:%.ln=%32.ln)
LINTLIB64 =	$(LINTLIB:%.ln=%64.ln)
LINTLIBS =	$(LINTLIB32) $(LINTLIB64)

LINTFLAGS =	-m -errtags=yes -erroff=E_SUPPRESSION_DIRECTIVE_UNUSED
LINTFLAGS64 =	-m -errtags=yes -erroff=E_SUPPRESSION_DIRECTIVE_UNUSED \
		-errchk=longptr64 $(VAR_LINTFLAGS64)

#
# These libraries have two resulting lint libraries.
# If a dependency is declared using these variables,
# the substitution for the 32/64 versions at lint time
# will happen automatically (see Makefile.targ).
#
LDDBG_LIB=	-llddbg
LDDBG_LIB32=	-llddbg32
LDDBG_LIB64=	-llddbg64

LD_LIB=		-lld
LD_LIB32=	-lld32
LD_LIB64=	-lld64
