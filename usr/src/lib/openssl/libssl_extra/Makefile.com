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
# usr/src/lib/openssl/libssl_extra/Makefile.com

LIBRARY= libssl_extra.a

OBJECTS=\
	ssl_algs.o \
	ssl_ciph.o \
	ssl_lib.o

include ../../Makefile.com

CPPFLAGS += -DCRYPTO_UNLIMITED
LDLIBS += $(ROOT)/$(SFWLIBDIR)/libcrypto_extra.so$(VERS)
LDLIBS += $(OPENSSL_LDFLAGS) -lcrypto -lssl -lc
DYNFLAGS += $(OPENSSL_DYNFLAGS)


LIBS =		$(DYNLIB)
SRCDIR =	$(OPENSSL_SRC)/ssl

$(LINTLIB):= 	SRCS = $(SRCDIR)/$(LINTSRC)

.KEEP_STATE:

all: $(LIBS)

include $(SRC)/lib/Makefile.targ
