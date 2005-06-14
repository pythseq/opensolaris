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
/*	Copyright (c) 1984, 1986, 1987, 1988, 1989 AT&T	*/
/*	  All Rights Reserved  	*/


/*
 * Copyright 1996-2003 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#pragma ident	"%Z%%M%	%I%	%E% SMI"

#include	<stdio.h>
#include 	<limits.h>
#include	<locale.h>
#include	<libintl.h>
#include	<sys/fstyp.h>
#include	<errno.h>
#include	<sys/vfstab.h>
#include	<sys/types.h>
#include	<sys/stat.h>
#include	<fcntl.h>
#include	<string.h>

#define	FSTYPE_MAX	8
#define	ARGV_MAX	1024
#define	VFS_PATH	"/usr/lib/fs"
#define	ALT_PATH	"/etc/fs"

extern char	*default_fstype();
void	stat_snap(char *, char *, char *);
char	*special = NULL;  /*  device special name  */
char	*fstype = NULL;	  /*  fstype name is filled in here  */
char	*cbasename;	  /* name of command */
char	*newargv[ARGV_MAX]; 	/* args for the fstype specific command  */
char	vfstab[] = VFSTAB;
int	newargc = 2;

/*
 * TRANSLATION_NOTE - the usage strings in the c_usgstr[] of the
 * following structures should be given a translation; the call to gettext
 * is in the usage() function. The strings are the ones containing
 * "[-F FSType]".
 */

struct commands {
	char *c_basename;
	char *c_optstr;
	char *c_usgstr[4]; /* make sure as large as largest array size */
} cmd_data[] = {
	"clri", "F:o:?V",
	{
		"[-F FSType] [-V] special inumber ...",
		NULL
	},
	"mkfs", "F:o:mb:?V",
	{
		"[-F FSType] [-V] [-m] [-o specific_options] special ",
		"[operands]", NULL
	},
	"dcopy", "F:o:?V",
	{
		"[-F FSType] [-V] special inumber ...",
		NULL
	},
	"fsdb", "F:o:z:?V",
	{
		"[-F FSType] [-V] [-o specific_options] special",
		NULL
	},
	"fssnap", "F:dio:?V",
	{
		"[-F FSType] [-V] -o special_options  /mount/point",
		"-d [-F FSType] [-V] /mount/point | dev",
		"-i [-F FSType] [-V] [-o special-options] [/mount/point | dev]",
		NULL
	},
	"labelit", "F:o:?nV",
	{
		"[-F FSType] [-V] [-o specific_options] special [operands]",
		NULL
	},
	NULL, "F:o:?V",
	{
		"[-F FSType] [-V] [-o specific_options] special [operands]",
		NULL
	}
};
struct 	commands *c_ptr;

main(argc, argv)
int	argc;
char	*argv[];
{
	register char *ptr;
	char	full_path[PATH_MAX];
	char	*vfs_path = VFS_PATH;
	char	*alt_path = ALT_PATH;
	int	i;
	int	verbose = 0;		/* set if -V is specified */
	int	F_flg = 0;
	int	mflag = 0;
	char	*oopts = NULL;
	int	iflag = 0;
	int	usgflag = 0;
	int	arg;			/* argument from getopt() */
	extern	char *optarg;		/* getopt specific */
	extern	int optind;
	extern	int opterr;

	(void) setlocale(LC_ALL, "");

#if !defined(TEXT_DOMAIN)	/* Should be defined by cc -D */
#define	TEXT_DOMAIN "SYS_TEST"	/* Use this only if it weren't */
#endif

	(void) textdomain(TEXT_DOMAIN);

	cbasename = ptr = argv[0];
	while (*ptr) {
		if (*ptr++ == '/')
			cbasename = ptr;
	}


	if (argc == 1) {
		for (c_ptr = cmd_data; ((c_ptr->c_basename != NULL) &&
		    (strcmp(c_ptr->c_basename, cbasename) != 0)); c_ptr++)
		;
		usage(cbasename, c_ptr->c_usgstr);
		exit(2);
	}

	for (c_ptr = cmd_data; ((c_ptr->c_basename != NULL) &&
	    (strcmp(c_ptr->c_basename, cbasename) != 0));  c_ptr++)
		;
	while ((arg = getopt(argc, argv, c_ptr->c_optstr)) != -1) {
			switch (arg) {
			case 'V':	/* echo complete command line */
				verbose = 1;
				break;
			case 'F':	/* FSType specified */
				F_flg++;
				fstype = optarg;
				break;
			case 'o':	/* FSType specific arguments */
				newargv[newargc++] = "-o";
				newargv[newargc++] = optarg;
				oopts = optarg;
				break;
			case '?':	/* print usage message */
				newargv[newargc++] = "-?";
				usgflag = 1;
				break;
			case 'm':	/* FSType specific arguments */
				mflag = 1;
				newargv[newargc] = (char *)malloc(3);
				sprintf(newargv[newargc++], "-%c", arg);
				if (optarg)
					newargv[newargc++] = optarg;
				break;
			case 'i': /* fssnap only */
				iflag = 1;
				/*FALLTHROUGH*/
			default:
				newargv[newargc] = (char *)malloc(3);
				sprintf(newargv[newargc++], "-%c", arg);
				if (optarg)
					newargv[newargc++] = optarg;
				break;
			}
			optarg = NULL;
	}
	if (F_flg > 1) {
		fprintf(stderr, gettext("%s: more than one FSType specified\n"),
			cbasename);
		usage(cbasename, c_ptr->c_usgstr);
		exit(2);
	}
	if (fstype != NULL) {
		if (strlen(fstype) > FSTYPE_MAX) {
			fprintf(stderr,
			    gettext("%s: FSType %s exceeds %d characters\n"),
			    cbasename, fstype, FSTYPE_MAX);
			exit(2);
		}
	}

	/*  perform a lookup if fstype is not specified  */
	special = argv[optind];
	optind++;

	/* handle -i (fssnap command only) */
	if (iflag) {
		int diff = argc - optind;
		/*
		 * There is no reason to ever call a file system specific
		 * version since its all in kstats.
		 */
		if (diff > 0) /* gave more than one mountpoint or device */
			usage(cbasename, c_ptr->c_usgstr);
		stat_snap(cbasename, diff == 0 ? argv[argc-1] : NULL, oopts);
		exit(0);
	}

	if ((special == NULL) && (!usgflag)) {
		fprintf(stderr, gettext("%s: special not specified\n"),
		    cbasename);
		usage(cbasename, c_ptr->c_usgstr);
		exit(2);
	}
	if ((fstype == NULL) && (usgflag))
		usage(cbasename, c_ptr->c_usgstr);
	if (fstype == NULL)
		lookup();
	if (fstype == NULL) {
		fprintf(stderr, gettext("%s: FSType cannot be identified\n"),
			cbasename);
		usage(cbasename, c_ptr->c_usgstr);
		exit(2);
	}
	newargv[newargc++] = special;
	for (; optind < argc; optind++)
		newargv[newargc++] = argv[optind];

	/*  build the full pathname of the fstype dependent command  */
	sprintf(full_path, "%s/%s/%s", vfs_path, fstype, cbasename);

	newargv[1] = cbasename;

	if (verbose) {
		printf("%s -F %s ", cbasename, fstype);
		for (i = 2; newargv[i]; i++)
			printf("%s ", newargv[i]);
		printf("\n");
		exit(0);
	}

	/*
	 *  Execute the FSType specific command.
	 */
	execv(full_path, &newargv[1]);
	if ((errno == ENOENT) || (errno == EACCES)) {
		/*  build the alternate pathname */
		sprintf(full_path, "%s/%s/%s", alt_path, fstype, cbasename);
		if (verbose) {
			printf("%s -F %s ", cbasename, fstype);
			for (i = 2; newargv[i]; i++)
				printf("%s ", newargv[i]);
			printf("\n");
			exit(0);
		}
		execv(full_path, &newargv[1]);
	}
	if (errno == ENOEXEC) {
		newargv[0] = "sh";
		newargv[1] = full_path;
		execv("/sbin/sh", &newargv[0]);
	}
	if (errno != ENOENT) {
		perror(cbasename);
		fprintf(stderr, gettext("%s: cannot execute %s\n"), cbasename,
		    full_path);
		exit(2);
	}

	if (sysfs(GETFSIND, fstype) == (-1)) {
		fprintf(stderr,
		    gettext("%s: FSType %s not installed in the kernel\n"),
		    cbasename, fstype);
		exit(2);
	}
	fprintf(stderr,
	    gettext("%s: Operation not applicable for FSType %s \n"),
	    cbasename, fstype);
	exit(2);
}

usage(cmd, usg)
char *cmd;
char **usg;
{
	int i;
	fprintf(stderr, gettext("Usage:\n"));
	for (i = 0; usg[i] != NULL; i++)
		fprintf(stderr, "%s %s\n", gettext(cmd), gettext(usg[i]));
	exit(2);
}


/*
 *  This looks up the /etc/vfstab entry given the device 'special'.
 *  It is called when the fstype is not specified on the command line.
 *
 *  The following global variables are used:
 *	special, fstype
 */

lookup()
{
	FILE	*fd;
	int	ret;
	struct vfstab	vget, vref;

	if ((fd = fopen(vfstab, "r")) == NULL) {
		fprintf(stderr, gettext("%s: cannot open vfstab\n"), cbasename);
		exit(1);
	}
	vfsnull(&vref);
	vref.vfs_special = special;
	ret = getvfsany(fd, &vget, &vref);
	if (ret == -1) {
		rewind(fd);
		vfsnull(&vref);
		vref.vfs_fsckdev = special;
		ret = getvfsany(fd, &vget, &vref);
	}
	fclose(fd);

	switch (ret) {
	case -1:
		fstype = default_fstype(special);
		break;
	case 0:
		fstype = vget.vfs_fstype;
		break;
	case VFS_TOOLONG:
		fprintf(stderr,
		    gettext("%s: line in vfstab exceeds %d characters\n"),
		    cbasename, VFS_LINE_MAX-2);
		exit(1);
		break;
	case VFS_TOOFEW:
		fprintf(stderr,
		    gettext("%s: line in vfstab has too few entries\n"),
		    cbasename);
		exit(1);
		break;
	}
}

void
stat_snap(cmd, mountpoint, opts)
char *cmd;
char *mountpoint;
char *opts;
{
	int fd; /* check mount point if given */
	int en;
	char *errstr;

	if (mountpoint) {
		if ((fd = open(mountpoint, O_RDONLY)) < 0) {
			en = errno;
			errstr = strerror(errno);
			if (errstr == NULL)
				errstr = gettext("Unknown error");

			fprintf(stderr, gettext("%s: %s: error %d: %s\n"),
				cmd, mountpoint, en, errstr);

			exit(2);
		}
		close(fd);
	}
	fssnap_show_status(mountpoint, opts, 1, (opts ? 0 : 1));
}
