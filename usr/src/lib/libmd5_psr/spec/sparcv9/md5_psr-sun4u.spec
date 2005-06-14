#
# Copyright 2005 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
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
#pragma ident	"%Z%%M%	%I%	%E% SMI"
#
# lib/libmd5_psr/spec/sparcv9/md5_psr-sun4u.spec
#

function	MD5Init extends libmd5/spec/md5.spec
arch		sparcv9
version		SUNWprivate_1.1
end

function	MD5Update extends libmd5/spec/md5.spec
arch		sparcv9
version		SUNWprivate_1.1
end

function	MD5Final extends libmd5/spec/md5.spec
arch		sparcv9
version		SUNWprivate_1.1
end

function	md5_calc extends libmd5/spec/md5.spec
arch		sparcv9
version		SUNWprivate_1.1
end
