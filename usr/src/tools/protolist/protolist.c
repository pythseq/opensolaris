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
 * Copyright (c) 1998 - 2001 by Sun Microsystems, Inc.
 * All rights reserved.
 */

#pragma ident	"%Z%%M%	%I%	%E% SMI"

#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/mkdev.h>
#include <ftw.h>
#include <strings.h>
#include <stdlib.h>

#define	MAX_DEPTH	50

/*ARGSUSED2*/
static int
visit_dir(const char *path, const struct stat *st,
	int file_type, struct FTW *ft)
{
	char	*uid, *gid, ftype;
	char	symsrc[MAXPATHLEN];
	char	buffer[MAXPATHLEN];
	char	*abs_name;
	char	name[MAXPATHLEN];
	char	maj[10], min[10];
	char	*p;
	int	c;
	static int first_time = 1;
	int	inum;

	/*
	 * The first directory is the current directory '.',
	 * this is relevent in out protolist - I throw it out.
	 */
	if (first_time) {
		first_time = 0;
		if ((path[0] == '.') && (path[1] == '\0'))
			return (0);
	}

	abs_name = (char *)(path + 2);
	maj[0] = min[0] = symsrc[0] = '-';
	maj[1] = min[1] = symsrc[1] = '\0';

	(void) strcpy(name, abs_name);
	/*
	 * is this a relocatable object?  if so set
	 * the symsrc appropriately.
	 *
	 * All relocatable objects start with /sun or /i86
	 *
	 * eg:
	 *    /sun4d/kadb == kadb
	 *    /i86pc/kadb == kadb
	 */
#if defined(sparc)
#define	ARCH_STR "sun"
#elif defined(i386)
#define	ARCH_STR "i86"
#elif defined(__ppc)
#define	ARCH_STR "prep"
#else
#error "Unknown instruction set"
#endif
	if (strncmp(abs_name, ARCH_STR, 3) == 0) {
		if (((st->st_mode & S_IFMT) == S_IFDIR) ||
		    ((st->st_mode & S_IFMT) == S_IFLNK))
			return (0);

		(void) strcpy(buffer, abs_name);
		if (p = index(buffer, '/')) {
			(void) strcpy(symsrc, abs_name);
			*p++ = '\0';
			(void) strcpy(name, p);
		}
	}

	switch (st->st_mode & S_IFMT) {
	case S_IFCHR:
		(void) sprintf(maj, "%ld", major(st->st_rdev));
		(void) sprintf(min, "%ld", minor(st->st_rdev));
		ftype = 'c';
		break;
	case S_IFDIR:
		ftype = 'd';
		break;
	case S_IFBLK:
		(void) sprintf(maj, "%ld", major(st->st_rdev));
		(void) sprintf(min, "%ld", minor(st->st_rdev));
		ftype = 'b';
		break;
	case S_IFREG:
		ftype = 'f';
		break;
	case S_IFLNK:
		if ((c = readlink(path, symsrc, MAXPATHLEN)) == -1)
			perror("readlink");
		symsrc[c] = '\0';
		ftype = 's';
		break;
	default:
		ftype = '?';
		break;
	}

	switch (st->st_uid) {
	case 0:
		uid = "root";
		break;
	case 1:
		uid = "daemon";
		break;
	case 2:
		uid = "bin";
		break;
	case 3:
		uid = "sys";
		break;
	case 4:
		uid = "adm";
		break;
	case 5:
		uid = "uucp";
		break;
	case 9:
		uid = "nuucp";
		break;
	case 25:
		uid = "smmsp";
		break;
	case 37:
		uid = "listen";
		break;
	case 71:
		uid = "lp";
		break;
	case 60001:
	case 65534:
		uid = "nobody";
		break;
	case 60002:
		uid = "noaccess";
		break;
	default:
		uid = "NO_SUCH_UID";
		break;
	}

	switch (st->st_gid) {
	case 0:
		gid = "root";
		break;
	case 1:
		gid = "other";
		break;
	case 2:
		gid = "bin";
		break;
	case 3:
		gid = "sys";
		break;
	case 4:
		gid = "adm";
		break;
	case 5:
		gid = "uucp";
		break;
	case 6:
		gid = "mail";
		break;
	case 7:
		gid = "tty";
		break;
	case 8:
		gid = "lp";
		break;
	case 9:
		gid = "nuucp";
		break;
	case 10:
		gid = "staff";
		break;
	case 12:
		gid = "daemon";
		break;
	case 25:
		gid = "smmsp";
		break;
	case 60001:
		gid = "nobody";
		break;
	case 60002:
		gid = "noaccess";
		break;
	case 65534:
		gid = "nogroup";
		break;
	default:
		gid = "NO_SUCH_GID";
		break;
	}
	if (st->st_nlink == 1)
		inum = 0;
	else
		inum = st->st_ino;

	(void) printf("%c %-30s %-20s %4lo %-5s %-5s %6d %2ld %2s %2s\n",
		ftype, name, symsrc, st->st_mode % 010000, uid, gid,
		inum, st->st_nlink, maj, min);
	return (0);
}

int
main(int argc, char *argv[])
{

	if (argc != 2) {
		(void) fprintf(stderr, "usage: protolist <protodir>\n");
		exit(1);
	}

	if (chdir(argv[1]) < 0) {
		perror("chdir");
		exit(1);
	}

	if (nftw(".", visit_dir, MAX_DEPTH, FTW_PHYS) != 0) {
		perror("nftw");
		exit(1);
	}

	return (0);
}
