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
 * Copyright 2004 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#pragma ident	"%Z%%M%	%I%	%E% SMI"

#include "sun_msgfmt.h"

static void	read_psffm(char *);
static void	sortit(char *, char *);
static wchar_t	*consume_whitespace(wchar_t *);
static char	expand_meta(wchar_t **);
static struct domain_struct	*find_domain_node(char *);
static void	insert_message(struct domain_struct *, char *, char *);
static void	output_all_mo_files(void);
static void	output_one_mo_file(struct domain_struct *);
static size_t _mbsntowcs(wchar_t **, char **, size_t *);

#ifdef DEBUG
static void	printlist(void);
#endif

static char	gcurrent_domain[TEXTDOMAINMAX+1];
static char	*gmsgid;		/* Stores msgid when read po file */
static char	*gmsgstr;		/* Stores msgstr when read po file */
static int	gmsgid_size;		/* The current size of msgid buffer */
static int	gmsgstr_size;		/* The current size of msgstr buffer */
static char	*outfile = NULL;
static int	linenum;		/* The line number in the file */
static int	msgid_linenum;		/* The last msgid token line number */
static int	msgstr_linenum;		/* The last msgstr token line number */

static int	oflag = 0;
static int	sun_p = 0;
int	verbose = 0;

static struct domain_struct	*first_domain = NULL;
static struct domain_struct	*last_used_domain = NULL;

static int	mbcurmax;

static char	**oargv;
static char	*inputdir;

extern void	check_gnu(char *, size_t);

#define	GNU_MSGFMT	"/usr/lib/gmsgfmt"
void
invoke_gnu_msgfmt(void)
{
	/*
	 * Transferring to /usr/lib/gmsgfmt
	 */
	char	*gnu_msgfmt;
#ifdef	DEBUG_MSGFMT
	gnu_msgfmt = getenv("GNU_MSGFMT");
	if (!gnu_msgfmt)
		gnu_msgfmt = GNU_MSGFMT;
#else
	gnu_msgfmt = GNU_MSGFMT;
#endif

	if (verbose) {
		diag(gettext(DIAG_INVOKING_GNU));
	}

	(void) execv(gnu_msgfmt, oargv);
	/* exec failed */
	error(gettext(ERR_EXEC_FAILED), gnu_msgfmt);
	/* NOTREACHED */
}

static void
usage(void)
{
	(void) fprintf(stderr, gettext(ERR_USAGE));
	exit(2);
}

/*
 * msgfmt - Generate binary tree for runtime gettext() using psffm: "Portable
 * Source File Format for Messages" file template. This file may have
 * previously been generated by the xgettext filter for c source files.
 */

int
main(int argc, char **argv)
{
	int	ret;
	static struct flags	flag;

	(void) setlocale(LC_ALL, "");
#if	!defined(TEXT_DOMAIN)
#define	TEXT_DOMAIN	"SYS_TEST"
#endif
	(void) textdomain(TEXT_DOMAIN);

	oargv = argv;
	ret = parse_option(&argc, &argv, &flag);
	if (ret == -1) {
		usage();
		/* NOTREACHED */
	}

	if (flag.sun_p) {
		/* never invoke gnu msgfmt */
		if (flag.gnu_p) {
			error(gettext(ERR_GNU_ON_SUN));
			/* NOTREACHED */
		}
		sun_p = flag.sun_p;
	}
	if (flag.idir) {
		inputdir = flag.idir;
	}
	if (flag.ofile) {
		oflag = 1;
		outfile = flag.ofile;
	}
	if (flag.verbose) {
		verbose = 1;
	}

	if (flag.gnu_p) {
		/* invoke /usr/lib/gmsgfmt */
		invoke_gnu_msgfmt();
		/* NOTREACHED */
	}

	/*
	 * read all portable object files specified in command arguments.
	 * Allocate initial size for msgid and msgstr. If it needs more
	 * spaces, realloc later.
	 */
	gmsgid = (char *)Xmalloc(MAX_VALUE_LEN);
	gmsgstr = (char *)Xmalloc(MAX_VALUE_LEN);

	gmsgid_size = gmsgstr_size = MAX_VALUE_LEN;
	(void) memset(gmsgid, 0, gmsgid_size);
	(void) memset(gmsgstr, 0, gmsgstr_size);

	mbcurmax = MB_CUR_MAX;

	while (argc-- > 0) {
		if (verbose) {
			diag(gettext(DIAG_START_PROC), *argv);
		}
		read_psffm(*argv++);
	}

	output_all_mo_files();

#ifdef DEBUG
	printlist();
#endif

	return (0);

} /* main */



/*
 * read_psffm - read in "psffm" format file, check syntax, printing error
 * messages as needed, output binary tree to file <domain>
 */

static void
read_psffm(char *file)
{
	int	fd;
	static char	msgfile[MAXPATHLEN];
	wchar_t	*linebufptr, *p;
	char	*bufptr = 0;
	int	quotefound;	/* double quote was seen */
	int	inmsgid = 0;	/* indicates "msgid" was seen */
	int	inmsgstr = 0;	/* indicates "msgstr" was seen */
	int	indomain = 0;	/* indicates "domain" was seen */
	wchar_t	wc;
	char	mb;
	int	n;
	char	token_found;	/* Boolean value */
	unsigned int	bufptr_index = 0; /* current index of bufptr */
	char	*mbuf, *addr;
	size_t	fsize, ln_size, ll;
	wchar_t	*linebufhead = NULL;
	struct stat64	statbuf;
	char	*filename;

	/*
	 * For each po file to be read,
	 * 1) set domain to default and
	 * 2) set linenumer to 0.
	 */
	(void) strcpy(gcurrent_domain, DEFAULT_DOMAIN);
	linenum = 0;

	if (!inputdir) {
		filename = Xstrdup(file);
	} else {
		size_t	dirlen, filelen, len;

		dirlen = strlen(inputdir);
		filelen = strlen(file);
		len = dirlen + 1 + filelen + 1;
		filename = (char *)Xmalloc(len);
		(void) memcpy(filename, inputdir, dirlen);
		*(filename + dirlen) = '/';
		(void) memcpy(filename + dirlen + 1, file, filelen);
		*(filename + dirlen + 1 + filelen) = '\0';
	}

	fd = open(filename, O_RDONLY);
	if (fd == -1) {
		error(gettext(ERR_OPEN_FAILED), filename);
		/* NOTREACHED */
	}
	if (fstat64(fd, &statbuf) == -1) {
		error(gettext(ERR_STAT_FAILED), filename);
		/* NOTREACHED */
	}
	fsize = (size_t)statbuf.st_size;
	if (fsize == 0) {
		/*
		 * The size of the specified po file is 0.
		 * In Solaris 8 and earlier, msgfmt was silent
		 * for the null po file.  So, just returns
		 * without generating an error message.
		 */
		(void) close(fd);
		free(filename);
		return;
	}
	addr = mmap(NULL, fsize, PROT_READ, MAP_SHARED, fd, 0);
	if (addr == MAP_FAILED) {
		error(gettext(ERR_MMAP_FAILED), filename);
		/* NOTREACHED */
	}
	(void) close(fd);

	if (!sun_p)
		check_gnu(addr, fsize);

	mbuf = addr;
	for (;;) {
		if (linebufhead) {
			free(linebufhead);
			linebufhead = NULL;
		}
		ln_size = _mbsntowcs(&linebufhead, &mbuf, &fsize);
		if (ln_size == (size_t)-1) {
			error(gettext(ERR_READ_FAILED), filename);
			/* NOTREACHED */
		} else if (ln_size == 0) {
			break;	/* End of File. */
		}
		linenum++;

		linebufptr = linebufhead;
		quotefound = 0;

		switch (*linebufptr) {
			case L'#':	/* comment    */
			case L'\n':	/* empty line */
				continue;
			case L'\"': /* multiple lines of msgid and msgstr */
				quotefound = 1;
				break;
		}

		/*
		 * Process MSGID Tokens.
		 */
		token_found = (wcsncmp(MSGID_TOKEN, linebufptr,
				MSGID_LEN) == 0) ? 1 : 0;

		if (token_found || (quotefound && inmsgid)) {

			if (token_found) {
				if (!CK_NXT_CH(linebufptr, MSGID_LEN+1)) {
					diag(gettext(ERR_NOSPC), linenum);
					error(gettext(ERR_EXITING));
					/* NOTREACHED */
				}
			}

			if (inmsgid && !quotefound) {
				warning(gettext(WARN_NO_MSGSTR), msgid_linenum);
				continue;
			}
			if (inmsgstr) {
				sortit(gmsgid, gmsgstr);
				(void) memset(gmsgid, 0, gmsgid_size);
				(void) memset(gmsgstr, 0, gmsgstr_size);
			}

			if (inmsgid) {
				/* multiple lines of msgid */
				/* cancel the previous null termination */
				bufptr_index--;
			} else {
				/*
				 * The first line of msgid.
				 * Save linenum of msgid to be used when
				 * printing warning or error message.
				 */
				msgid_linenum = linenum;
				p = linebufptr;
				linebufptr = consume_whitespace(
					linebufptr + MSGID_LEN);
				ln_size -= linebufptr - p;
				bufptr = gmsgid;
				bufptr_index = 0;
			}

			inmsgid = 1;
			inmsgstr = 0;
			indomain = 0;
			goto load_buffer;
		}

		/*
		 * Process MSGSTR Tokens.
		 */
		token_found = (wcsncmp(MSGSTR_TOKEN, linebufptr,
			MSGSTR_LEN) == 0) ? 1 : 0;
		if (token_found || (quotefound && inmsgstr)) {

			if (token_found) {
				if (!CK_NXT_CH(linebufptr, MSGSTR_LEN+1)) {
					diag(gettext(ERR_NOSPC), linenum);
					error(gettext(ERR_EXITING));
					/* NOTREACHED */
				}
			}


			if (inmsgstr && !quotefound) {
				warning(gettext(WARN_NO_MSGID), msgstr_linenum);
				continue;
			}
			if (inmsgstr) {
				/* multiple lines of msgstr */
				/* cancel the previous null termination */
				bufptr_index--;
			} else {
				/*
				 * The first line of msgstr.
				 * Save linenum of msgid to be used when
				 * printing warning or error message.
				 */
				msgstr_linenum = linenum;
				p = linebufptr;
				linebufptr = consume_whitespace(
					linebufptr + MSGSTR_LEN);
				ln_size -= linebufptr - p;
				bufptr = gmsgstr;
				bufptr_index = 0;
			}

			inmsgstr = 1;
			inmsgid = 0;
			indomain = 0;
			goto load_buffer;
		}

		/*
		 * Process DOMAIN Tokens.
		 * Add message id and message string to sorted list
		 * if msgstr was processed last time.
		 */
		token_found = (wcsncmp(DOMAIN_TOKEN, linebufptr,
			DOMAIN_LEN) == 0) ? 1 : 0;
		if ((token_found) || (quotefound && indomain)) {
			if (token_found) {
				if (!CK_NXT_CH(linebufptr, DOMAIN_LEN+1)) {
					diag(gettext(ERR_NOSPC), linenum);
					error(gettext(ERR_EXITING));
					/* NOTREACHED */
				}
			}


			/*
			 * process msgid and msgstr pair for previous domain
			 */
			if (inmsgstr) {
				sortit(gmsgid, gmsgstr);
			}

			/* refresh msgid and msgstr buffer */
			if (inmsgstr || inmsgid) {
				(void) memset(gmsgid, 0, gmsgid_size);
				(void) memset(gmsgstr, 0, gmsgstr_size);
			}

			if (indomain) {
				/* multiple lines of domain */
				/* cancel the previous null termination */
				bufptr_index--;
			} else {
				p = linebufptr;
				linebufptr = consume_whitespace(
					linebufptr + DOMAIN_LEN);
				(void) memset(gcurrent_domain, 0,
						sizeof (gcurrent_domain));
				ln_size -= linebufptr - p;
				bufptr = gcurrent_domain;
				bufptr_index = 0;
			}

			indomain = 1;
			inmsgid = 0;
			inmsgstr = 0;
		} /* if */

load_buffer:
		/*
		 * Now, fill up the buffer pointed by bufptr.
		 * At this point bufptr should point to one of
		 * msgid, msgptr, or current_domain.
		 * Otherwise, the entire line is ignored.
		 */

		if (!bufptr) {
			warning(gettext(WARN_SYNTAX_ERR), linenum);
			continue;
		}

		if (*linebufptr++ != L'\"') {
			warning(gettext(WARN_MISSING_QUOTE), linenum);
			--linebufptr;
		}
		quotefound = 0;

		/*
		 * If there is not enough space in the buffer,
		 * increase buffer by ln_size by realloc.
		 */
		ll = ln_size * mbcurmax;
		if (bufptr == gmsgid) {
			if (gmsgid_size < (bufptr_index + ll)) {
				gmsgid = (char *)Xrealloc(gmsgid,
					bufptr_index + ll);
				bufptr = gmsgid;
				gmsgid_size = bufptr_index + ll;
			}
		} else if (bufptr == gmsgstr) {
			if (gmsgstr_size < (bufptr_index + ll)) {
				gmsgstr = (char *)Xrealloc(gmsgstr,
					bufptr_index + ll);
				bufptr = gmsgstr;
				gmsgstr_size = bufptr_index + ll;
			}
		}

		while (wc = *linebufptr++) {
			switch (wc) {
			case L'\n':
				if (!quotefound) {
warning(gettext(WARN_MISSING_QUOTE_AT_EOL), linenum);
				}
				break;

			case L'\"':
				quotefound = 1;
				break;

			case L'\\':
				if ((mb = expand_meta(&linebufptr)) != NULL)
					bufptr[bufptr_index++] = mb;
				break;

			default:
				if ((n = wctomb(&bufptr[bufptr_index], wc)) > 0)
					bufptr_index += n;
			} /* switch */
			if (quotefound) {
				/*
				 * Check if any remaining characters
				 * after closing quote.
				 */
				linebufptr = consume_whitespace(linebufptr);
				if (*linebufptr != L'\n') {
					warning(gettext(WARN_INVALID_STRING),
						linenum);
				}
				break;
			}
		} /* while */

		bufptr[bufptr_index++] = '\0';

		(void) strcpy(msgfile, gcurrent_domain);
		(void) strcat(msgfile, ".mo");
	} /* for(;;) */

	if (inmsgstr) {
		sortit(gmsgid, gmsgstr);
	}

	if (linebufhead)
		free(linebufhead);
	if (munmap(addr, statbuf.st_size) == -1) {
		error(gettext(ERR_MUNMAP_FAILED), filename);
		/* NOTREACHED */
	}

	free(filename);
	return;

} /* read_psffm */


/*
 * Skip leading white spaces and tabs.
 */
static wchar_t *
consume_whitespace(wchar_t *buf)
{
	wchar_t	*bufptr = buf;
	wchar_t	c;

	/*
	 * Skip leading white spaces.
	 */
	while ((c = *bufptr) != L'\0') {
		if (c == L' ' || c == L'\t') {
			bufptr++;
			continue;
		}
		break;
	}
	return (bufptr);
} /* consume_white_space */


/*
 * handle escape sequences.
 */
static char
expand_meta(wchar_t **buf)
{
	wchar_t	wc = **buf;
	char	n;

	switch (wc) {
	case L'"':
		(*buf)++;
		return ('\"');
	case L'\\':
		(*buf)++;
		return ('\\');
	case L'b':
		(*buf)++;
		return ('\b');
	case L'f':
		(*buf)++;
		return ('\f');
	case L'n':
		(*buf)++;
		return ('\n');
	case L'r':
		(*buf)++;
		return ('\r');
	case L't':
		(*buf)++;
		return ('\t');
	case L'v':
		(*buf)++;
		return ('\v');
	case L'a':
		(*buf)++;
		return ('\a');
	case L'\'':
		(*buf)++;
		return ('\'');
	case L'?':
		(*buf)++;
		return ('\?');
	case L'0':
	case L'1':
	case L'2':
	case L'3':
	case L'4':
	case L'5':
	case L'6':
	case L'7':
		/*
		 * This case handles \ddd where ddd is octal number.
		 * There could be one, two, or three octal numbers.
		 */
		(*buf)++;
		n = (char)(wc - L'0');
		wc = **buf;
		if (wc >= L'0' && wc <= L'7') {
			(*buf)++;
			n = 8*n + (char)(wc - L'0');
			wc = **buf;
			if (wc >= L'0' && wc <= L'7') {
				(*buf)++;
				n = 8*n + (char)(wc - L'0');
			}
		}
		return (n);
	default:
		return (NULL);
	}
} /* expand_meta */

/*
 * Finds the head of the current domain linked list and
 * call insert_message() to insert msgid and msgstr pair
 * to the linked list.
 */
static void
sortit(char *msgid, char *msgstr)
{
	struct domain_struct	*dom;

#ifdef DEBUG
	(void) fprintf(stderr,
		"==> sortit(), domain=<%s> msgid=<%s> msgstr=<%s>\n",
		gcurrent_domain, msgid, msgstr);
#endif

	/*
	 * If "-o filename" is specified, then all "domain" directive
	 * are ignored and, all messages will be stored in domain
	 * whose name is filename.
	 */
	if (oflag) {
		dom = find_domain_node(outfile);
	} else {
		dom = find_domain_node(gcurrent_domain);
	}

	insert_message(dom, msgid, msgstr);
}

/*
 * This routine inserts message in the current domain message list.
 * It is inserted in ascending order.
 */
static void
insert_message(struct domain_struct *dom,
	char *msgid, char *msgstr)
{
	struct msg_chain	*p1;
	struct msg_chain	*node, *prev_node;
	int			b;

	/*
	 * Find the optimal starting search position.
	 * The starting search position is either the first node
	 * or the current_elem of domain.
	 * The current_elem is the pointer to the node which
	 * is most recently accessed in domain.
	 */
	if (dom->current_elem != NULL) {
		b = strcmp(msgid, dom->current_elem->msgid);
		if (b == 0) {
			if (verbose)
				warning(gettext(WARN_DUP_MSG),
					msgid, msgid_linenum);
			return;
		} else if (b > 0) { /* to implement descending order */
			p1 = dom->first_elem;
		} else {
			p1 = dom->current_elem;
		}
	} else {
		p1 = dom->first_elem;
	}

	/*
	 * search msgid insert position in the list
	 * Search starts from the node pointed by p1.
	 */
	prev_node = NULL;
	while (p1) {
		b = strcmp(msgid, p1->msgid);
		if (b == 0) {
			if (verbose)
				warning(gettext(WARN_DUP_MSG),
					msgid, msgid_linenum);
			return;
		} else if (b < 0) {  /* to implement descending order */
			/* move to the next node */
			prev_node = p1;
			p1 = p1->next;
		} else {
			/* insert a new msg node */
			node = (struct msg_chain *)
				Xmalloc(sizeof (struct msg_chain));
			node->next = p1;
			node->msgid  = Xstrdup(msgid);
			node->msgstr = Xstrdup(msgstr);

			if (prev_node) {
				prev_node->next = node;
			} else {
				dom->first_elem = node;
			}
			dom->current_elem = node;
			return;
		}
	} /* while */

	/*
	 * msgid is smaller than any of msgid in the list or
	 * list is empty.
	 * Therefore, append it.
	 */
	node = (struct msg_chain *)
		Xmalloc(sizeof (struct msg_chain));
	node->next = NULL;
	node->msgid  = Xstrdup(msgid);
	node->msgstr = Xstrdup(msgstr);

	if (prev_node) {
		prev_node->next = node;
	} else {
		dom->first_elem = node;
	}
	dom->current_elem = node;

	return;

} /* insert_message */


/*
 * This routine will find head of the linked list for the given
 * domain_name. This looks up cache entry first and if cache misses,
 * scans the list.
 * If not found, then create a new node.
 */
static struct domain_struct *
find_domain_node(char *domain_name)
{
	struct domain_struct	*p1;
	struct domain_struct	*node;
	struct domain_struct	*prev_node;
	int			b;


	/* for perfomance, check cache 'last_used_domain' */
	if (last_used_domain) {
		b = strcmp(domain_name, last_used_domain->domain);
		if (b == 0) {
			return (last_used_domain);
		} else if (b < 0) {
			p1 = first_domain;
		} else {
			p1 = last_used_domain;
		}
	} else {
		p1 = first_domain;
	}

	prev_node = NULL;
	while (p1) {
		b = strcmp(domain_name, p1->domain);
		if (b == 0) {
			/* node found */
			last_used_domain = p1;
			return (p1);
		} else if (b > 0) {
			/* move to the next node */
			prev_node = p1;
			p1 = p1->next;
		} else {
			/* insert a new domain node */
			node = (struct domain_struct *)
				Xmalloc(sizeof (struct domain_struct));
			node->next = p1;
			node->domain = Xstrdup(domain_name);
			node->first_elem = NULL;
			node->current_elem = NULL;
			if (prev_node) {
				/* insert the node in the middle */
				prev_node->next = node;
			} else {
				/* node inserted is the smallest */
				first_domain = node;
			}
			last_used_domain = node;
			return (node);
		}
	} /* while */

	/*
	 * domain_name is larger than any of domain name in the list or
	 * list is empty.
	 */
	node = (struct domain_struct *)
		Xmalloc(sizeof (struct domain_struct));
	node->next = NULL;
	node->domain = Xstrdup(domain_name);
	node->first_elem = NULL;
	node->current_elem = NULL;
	if (prev_node) {
		/* domain list is not empty */
		prev_node->next = node;
	} else {
		/* domain list is empty */
		first_domain = node;
	}
	last_used_domain = node;

	return (node);

} /* find_domain_node */


/*
 * binary_compute() is used for pre-computing a binary search.
 */
static int
binary_compute(int i, int j, int *more, int *less)
{
	int	k;

	if (i > j) {
		return (LEAFINDICATOR);
	}
	k = (i + j) / 2;

	less[k] = binary_compute(i, k - 1, more, less);
	more[k] = binary_compute(k + 1, j, more, less);

	return (k);

} /* binary_compute */


/*
 * Write all domain data to file.
 * Each domain will create one file.
 */
static void
output_all_mo_files(void)
{
	struct domain_struct 	*p;

	p = first_domain;
	while (p) {
		/*
		 * generate message object file only if there is
		 * at least one element.
		 */
		if (p->first_elem) {
			output_one_mo_file(p);
		}
		p = p->next;
	}
	return;

} /* output_all_mo_files */


/*
 * Write one domain data list to file.
 */
static void
output_one_mo_file(struct domain_struct *dom)
{
	FILE	*fp;
	struct msg_chain	*p;
	int	message_count;
	int	string_count_msgid;
	int	string_count_msg;
	int	msgid_index = 0;
	int	msgstr_index = 0;
	int	*less, *more;
	int	i;
	char	fname [TEXTDOMAINMAX+1];

	if (!dom || !dom->first_elem)
		return;

	/*
	 * If -o flag is specified, then file name is used as domain name.
	 * If not, ".mo" is appended to the domain name.
	 */
	(void) strcpy(fname, dom->domain);
	if (!oflag) {
		(void) strcat(fname, ".mo");
	}
	fp = fopen(fname, "w");
	if (fp == NULL) {
		error(gettext(ERR_OPEN_FAILED), fname);
		/* NOTREACHED */
	}

	/* compute offsets and counts */
	message_count = 0;
	p = dom->first_elem;
	while (p) {
		p->msgid_offset = msgid_index;
		p->msgstr_offset = msgstr_index;
		msgid_index += strlen(p->msgid) + 1;
		msgstr_index += strlen(p->msgstr) + 1;
		message_count++;
		p = p->next;
	}

	/*
	 * Fill up less and more entries to be used for binary search.
	 */
	string_count_msgid = msgid_index;
	string_count_msg = msgstr_index;
	less = (int *)Xcalloc(message_count, sizeof (int));
	more = (int *)Xcalloc(message_count, sizeof (int));

	(void) binary_compute(0, message_count - 1, more, less);

#ifdef DEBUG
	{
		int i;
		for (i = 0; i < message_count; i++) {
			(void) fprintf(stderr,
				"  less[%2d]=%2d, more[%2d]=%2d\n",
				i, less[i], i, more[i]);
		}
	}
#endif

	/*
	 * write out the message object file.
	 * The middle one is the first message to check by gettext().
	 */
	i = (message_count - 1) / 2;
	(void) fwrite(&i, sizeof (int), 1, fp);
	(void) fwrite(&message_count, sizeof (int), 1, fp);
	(void) fwrite(&string_count_msgid, sizeof (int), 1, fp);
	(void) fwrite(&string_count_msg, sizeof (int), 1, fp);
	i = MSG_STRUCT_SIZE * message_count;
	(void) fwrite(&i, sizeof (int), 1, fp);

	/* march through linked list and write out all nodes. */
	i = 0;
	p = dom->first_elem;
	while (p) {	/* put out message struct */
		(void) fwrite(&less[i], sizeof (int), 1, fp);
		(void) fwrite(&more[i], sizeof (int), 1, fp);
		(void) fwrite(&p->msgid_offset, sizeof (int), 1, fp);
		(void) fwrite(&p->msgstr_offset, sizeof (int), 1, fp);
		i++;
		p = p->next;
	}

	/* put out message id strings */
	p = dom->first_elem;
	while (p) {
		(void) fwrite(p->msgid, strlen(p->msgid)+1, 1, fp);
		p = p->next;
	}

	/* put out message strings */
	p = dom->first_elem;
	while (p) {
		(void) fwrite(p->msgstr, strlen(p->msgstr)+1, 1, fp);
		p = p->next;
	}

	(void) fclose(fp);
	free(less);
	free(more);

	return;

} /* output_one_mo_file */


/*
 * read one line from *mbuf,
 * skip preceding whitespaces,
 * convert the line to wide characters,
 * place the wide characters into *bufhead, and
 * return the number of wide characters placed.
 *
 * INPUT:
 *		**bufhead - address of a variable that is the pointer
 *			to wchar_t.
 *			The variable should been initialized to NULL.
 *		**mbuf - address of a variable that is the pointer
 *			to char.
 *			The pointer should point to the memory mmapped to
 *			the file to input.
 *		**fsize - address of a size_t variable that contains
 *			the size of unread bytes in the file to input.
 * OUTPUT:
 *		return - the number of wide characters placed.
 *		**bufhead - _mbsntowcs allocates the buffer to store
 *			one line in wchar_t from *mbuf and sets the address
 *			to *bufhead.
 *		**mbuf - _mbsntowcs reads one line from *mbuf and sets *mbuf
 *			to the beginning of the next line.
 *		**fsize - *fsize will be set to the size of the unread
 *			bytes in the file.
 */
static size_t
_mbsntowcs(wchar_t **bufhead, char **mbuf, size_t *fsize)
{
	wchar_t	*tp, *th;
	wchar_t	wc;
	size_t	tbufsize = LINE_SIZE;
	size_t	ttbufsize, nc;
	char	*pc = *mbuf;
	int	nb;

	if (*fsize == 0) {
		/* eof */
		return (0);
	}

	th = (wchar_t *)Xmalloc(sizeof (wchar_t) * tbufsize);
	nc = tbufsize;

	/* skip preceding whitespaces */
	while ((*pc != '\0')) {
		if ((*pc == ' ') || (*pc == '\t')) {
			pc++;
			(*fsize)--;
		} else {
			break;
		}
	}

	tp = th;
	while (*fsize > 0) {
		nb = mbtowc(&wc, pc, mbcurmax);
		if (nb == -1) {
			return ((size_t)-1);
		}

		if (*pc == '\n') {
			/* found eol */
			if (nc <= 1) {
				/*
				 * not enough buffer
				 * at least 2 more bytes are required for
				 * L'\n' and L'\0'
				 */
				ttbufsize = tbufsize + 2;
				th = (wchar_t *)Xrealloc(th,
					sizeof (wchar_t) * ttbufsize);
				tp = th + tbufsize - nc;
				tbufsize = ttbufsize;
			}
			*tp++ = L'\n';
			*tp++ = L'\0';
			pc += nb;
			*fsize -= nb;
			*mbuf = pc;
			*bufhead = th;
			return ((size_t)(tp - th));
		}
		if (nc == 0) {
			ttbufsize = tbufsize + LINE_SIZE;
			th = (wchar_t *)Xrealloc(th,
				sizeof (wchar_t) * ttbufsize);
			tp = th + tbufsize;
			nc = LINE_SIZE;
			tbufsize = ttbufsize;
		}
		*tp++ = wc;
		nc--;
		pc += nb;
		*fsize -= nb;
	}	/* while */

	/*
	 * At this point, the input file has been consumed,
	 * but there is no ending '\n'; we add it to
	 * the output file.
	 */
	if (nc <= 1) {
		/*
		 * not enough buffer
		 * at least 2 more bytes are required for
		 * L'\n' and L'\0'
		 */
		ttbufsize = tbufsize + 2;
		th = (wchar_t *)Xrealloc(th,
			sizeof (wchar_t) * ttbufsize);
		tp = th + tbufsize - nc;
		tbufsize = ttbufsize;
	}
	*tp++ = L'\n';
	*tp++ = L'\0';
	*mbuf = pc;
	*bufhead = th;
	return ((size_t)(tp - th));
}


/*
 * This is debug function. Not compiled in the final executable.
 */
#ifdef DEBUG
static void
printlist(void)
{
	struct domain_struct	*p;
	struct msg_chain	*m;

	(void) fprintf(stderr, "\n=== Printing contents of all domains ===\n");
	p = first_domain;
	while (p) {
		(void) fprintf(stderr, "domain name = <%s>\n", p->domain);
		m = p->first_elem;
		while (m) {
			(void) fprintf(stderr, "   msgid=<%s>, msgstr=<%s>\n",
					m->msgid, m->msgstr);
			m = m->next;
		}
		p = p->next;
	}
} /* printlist */
#endif
