#
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License (the "License").
# You may not use this file except in compliance with the License.
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
# Copyright 2007 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#
# ident	"%Z%%M%	%I%	%E% SMI"
#

include	../Makefile.lib

HDRS=		sasl.h		saslplug.h	saslutil.h	prop.h
HDRDIR=		include
ROOTHDRDIR=	$(ROOT)/usr/include/sasl

SUBDIRS =	$(MACH)
$(BUILD64)SUBDIRS += $(MACH64)

all :=		TARGET= all
clean :=	TARGET= clean
clobber :=	TARGET= clobber
install :=	TARGET= install
lint :=		TARGET= lint

LIBRARY=        libsasl.a
POFILE=         $(LIBRARY:.a=.po)
MSGFILES=	lib/canonusr.c lib/checkpw.c lib/client.c lib/common.c \
			lib/external.c lib/server.c lib/seterror.c \
			plugin/plugin_common.c \
			$(SRC)/lib/sasl_plugins/cram/cram.c \
			$(SRC)/lib/sasl_plugins/digestmd5/digestmd5.c \
			$(SRC)/lib/sasl_plugins/gssapi/gssapi.c \
			$(SRC)/lib/sasl_plugins/plain/plain.c

.KEEP_STATE:

all clean clobber install lint: $(SUBDIRS)

all install: THIRDPARTYLICENSE

install_h:	$(ROOTHDRS)

check:	$(CHECKHDRS)

$(POFILE): $(MSGFILES)
	$(BUILDPO.msgfiles)

_msg: $(MSGDOMAINPOFILE)

include $(SRC)/Makefile.msg.targ

$(SUBDIRS): FRC
	@cd $@; pwd; $(MAKE) $(TARGET)

FRC:

THIRDPARTYLICENSE: LICENSE.txt
	$(SED) -n '/Carnegie Mellon/,$$p' LICENSE.txt > $@

CLOBBERFILES += THIRDPARTYLICENSE

# EXPORT DELETE START
# CRYPT DELETE START
# Special target to clean up the source tree for export distribution
# Warning: This target changes the source tree
EXPORT_SRC:
	$(RM) Makefile+ \
		lib/client.c+ \
		lib/server.c+ \
		lib/common.c+ \
		lib/saslint.h+ \
		include/plugin_common.h+

	$(SED) -e "/EXPORT DELETE START/,/EXPORT DELETE END/d" \
		< lib/client.c > lib/client.c+
	$(MV) lib/client.c+ lib/client.c

	$(SED) -e "/EXPORT DELETE START/,/EXPORT DELETE END/d" \
		< lib/server.c > lib/server.c+
	$(MV) lib/server.c+ lib/server.c

	$(SED) -e "/EXPORT DELETE START/,/EXPORT DELETE END/d" \
		< lib/common.c > lib/common.c+
	$(MV) lib/common.c+ lib/common.c

	$(SED) -e "/EXPORT DELETE START/,/EXPORT DELETE END/d" \
		< lib/saslint.h > lib/saslint.h+
	$(MV) lib/saslint.h+ lib/saslint.h

	$(SED) -e "/EXPORT DELETE START/,/EXPORT DELETE END/d" \
		< include/plugin_common.h > include/plugin_common.h+
	$(MV) include/plugin_common.h+ include/plugin_common.h

	$(SED) -e "/^# EXPORT DELETE START/,/^# EXPORT DELETE END/d" \
		< Makefile > Makefile+
	$(MV) Makefile+ Makefile

	$(CHMOD) 444 Makefile \
		lib/client.c \
		lib/server.c \
		lib/common.c \
		lib/saslint.h \
		include/plugin_common.h

CRYPT_SRC:
	$(RM) Makefile+ lib/common.c+

	$(SED) -e "/CRYPT DELETE START/,/CRYPT DELETE END/d" \
		< lib/common.c | $(SED) -e "/EXPORT DELETE/d" \
		> lib/common.c+
	$(MV) lib/common.c+ lib/common.c

	$(SED) -e "/CRYPT DELETE START/,/CRYPT DELETE END/d" \
		< lib/client.c | $(SED) -e "/EXPORT DELETE/d" \
		> lib/client.c+
	$(MV) lib/client.c+ lib/client.c

	$(SED) -e "/CRYPT DELETE START/,/CRYPT DELETE END/d" \
		< lib/server.c | $(SED) -e "/EXPORT DELETE/d" \
		> lib/server.c+
	$(MV) lib/server.c+ lib/server.c

	$(SED) -e "/CRYPT DELETE START/,/CRYPT DELETE END/d" \
		< lib/saslint.h | $(SED) -e "/EXPORT DELETE/d" \
		> lib/saslint.h+
	$(MV) lib/saslint.h+ lib/saslint.h

	$(SED) -e "/CRYPT DELETE START/,/CRYPT DELETE END/d" \
		< include/plugin_common.h | $(SED) -e "/EXPORT DELETE/d" \
		> include/plugin_common.h+
	$(MV) include/plugin_common.h+ include/plugin_common.h

	$(SED) -e "/^# CRYPT DELETE START/,/^# CRYPT DELETE END/d" \
		< Makefile | $(SED) -e "/^# EXPORT DELETE/d" > Makefile+
	$(MV) Makefile+ Makefile

	$(CHMOD) 444 Makefile \
		lib/client.c \
		lib/server.c \
		lib/common.c \
		lib/saslint.h \
		include/plugin_common.h

# CRYPT DELETE END
# EXPORT DELETE END

include ../Makefile.targ

.PARALLEL: $(SUBDIRS)
