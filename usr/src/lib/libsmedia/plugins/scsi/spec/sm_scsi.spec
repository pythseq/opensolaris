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
# lib/libmedia/plugins/scsi/spec/media.spec

#include <sys/smedia.h>

function	_m_device_type
declaration 	int32_t _m_device_type(ushort_t ctype, ushort_t mtype)
version		SUNWprivate_1.1
errno		EIO
end		

function	_m_version_no
declaration 	int32_t _m_version_no(void)
version		SUNWprivate_1.1
errno		EIO
end		

function	_m_get_media_info
declaration 	int32_t _m_get_media_info(void *handle, void *ip)
version		SUNWprivate_1.1
errno		EIO
end		

function	_m_get_device_info
declaration 	int32_t _m_get_device_info(void *handle, void *ip)
version		SUNWprivate_1.1
errno		EIO
end		

function	_m_free_device_info
declaration 	int32_t _m_free_device_info(void *handle, void *ip)
version		SUNWprivate_1.1
errno		EIO
end		

function	_m_raw_write
declaration 	int32_t _m_raw_write(void *handle, void *ip)
version		SUNWprivate_1.1
errno		EIO
end		

function	_m_raw_read
declaration 	int32_t _m_raw_read(void *handle, void *ip)
version		SUNWprivate_1.1
errno		EIO
end		

function	_m_media_format
declaration 	int32_t _m_media_format(void *handle, void *ip)
version		SUNWprivate_1.1
errno		EIO
end		

function	_m_set_media_status
declaration 	int32_t _m_set_media_status(void *handle, void *ip)
version		SUNWprivate_1.1
errno		EIO
end		

function	_m_get_media_status
declaration 	int32_t _m_get_media_status(void *handle, void *ip)
version		SUNWprivate_1.1
errno		EIO
end		

function	_m_check_format_status
declaration 	int32_t _m_check_format_status(void *handle, void *ip)
version		SUNWprivate_1.1
errno		EIO
end		

function	_m_reassign_block
declaration 	int32_t _m_reassign_block(void *handle, void *ip)
version		SUNWprivate_1.1
errno		EIO
end		

function	_m_eject
declaration 	int32_t _m_eject(void *handle, void *ip)
version		SUNWprivate_1.1
errno		EIO
end		

function	_m_uscsi_cmd
declaration 	int32_t _m_uscsi_cmd(void *handle, void *ip)
version		SUNWprivate_1.1
errno		EIO
end		
