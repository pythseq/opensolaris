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
# lib/libplot/vt0/spec/vt0.spec

function	arc	extends libplot/plot/spec/plot.spec
version		SUNW_1.1
end		

function	box	extends libplot/plot/spec/plot.spec
version		SUNW_1.1
end		

function	circle	extends libplot/plot/spec/plot.spec
version		SUNW_1.1
end		

function	closepl	extends libplot/plot/spec/plot.spec
version		SUNW_1.1
end		

function	closevt	extends libplot/plot/spec/plot.spec
version		SUNW_1.1
end		

function	cont	extends libplot/plot/spec/plot.spec
version		SUNW_1.1
end		

function	dot	extends libplot/plot/spec/plot.spec
version		SUNW_1.1
end		

function	erase	extends libplot/plot/spec/plot.spec
version		SUNW_1.1
end		

function	frame
include		<plot.h>
declaration void frame(int n)
version		SUNW_1.1
end		

function	label	extends libplot/plot/spec/plot.spec
version		SUNW_1.1
end		

function	line	extends libplot/plot/spec/plot.spec
version		SUNW_1.1
end		

function	linemod	extends libplot/plot/spec/plot.spec
version		SUNW_1.1
end		

function	move	extends libplot/plot/spec/plot.spec
version		SUNW_1.1
end		

function	openpl	extends libplot/plot/spec/plot.spec
version		SUNW_1.1
end		

function	openvt	extends libplot/plot/spec/plot.spec
version		SUNW_1.1
end		

function	point	extends libplot/plot/spec/plot.spec
version		SUNW_1.1
end		

function	space	extends libplot/plot/spec/plot.spec
version		SUNW_1.1
end		

function	xsc
version		SUNWprivate_1.1
end		

function	ysc
version		SUNWprivate_1.1
end		

function	obotx
version		SUNWprivate_1.1
end		

function	oboty
version		SUNWprivate_1.1
end		

function	scalex
version		SUNWprivate_1.1
end		

function	scaley
version		SUNWprivate_1.1
end		

function	vti
version		SUNWprivate_1.1
end		

function	xnow
version		SUNWprivate_1.1
end		

function	ynow
version		SUNWprivate_1.1
end		

function	botx
version		SUNWprivate_1.1
end		

function	boty
version		SUNWprivate_1.1
end		

function	deltx
version		SUNWprivate_1.1
end		

function	delty
version		SUNWprivate_1.1
end		

function	_lib_version
version		SUNWprivate_1.1
end		

