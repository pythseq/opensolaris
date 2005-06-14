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

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <kstat.h>
#include <sys/processor.h>	/* for processorid_t */

#include <libintl.h>
#include <locale.h>

#ifndef	TEXT_DOMAIN
#define	TEXT_DOMAIN	"SYS_TEST"
#endif

#define	NCPUSTATES	6	/* on-line, off-line, no-intr, faulted, */
				/* spare, power-off */

/*
 * Possible states that a cpu may be in, and their corresponding
 * localized versions.
 */
static struct {
	const char *state;	/* State returned in kstat. */
	const char *lstate;	/* Localized version of the state. */
} cpu_states[NCPUSTATES];

static const char cmdname[] = "psrinfo";

#define	CORES_PER_CHIP_MAX 256	/* ABEN (Arbitrarily big-enough number) */
static int chip_count = 0;
static int max_chip_id;
static struct chip {
	int visible;
	int online;
	int core_count;
	char impl[128];
	char brand[128];
	processorid_t cores[CORES_PER_CHIP_MAX];
} *chips;

static void cpu_info(kstat_ctl_t *kc, kstat_t *ksp, int verbosity,
	int phys_view, int visible);

static void
usage(char *msg)
{
	if (msg != NULL)
		(void) fprintf(stderr, "%s: %s\n", cmdname, msg);
	(void) fprintf(stderr,
	    gettext("usage: \n\t%s [-v] [-p] [processor_id ...]\n"
	    "\t%s -s [-p] processor_id\n"), cmdname, cmdname);
	exit(2);
}

int
main(int argc, char *argv[])
{
	kstat_ctl_t *kc;
	kstat_t	*ksp;
	int c;
	processorid_t cpu;
	int verbosity = 1;	/* 0 = silent, 1 = normal, 3 = verbose */
	int phys_view = 0;
	int errors = 0;
	char *errptr;

	(void) setlocale(LC_ALL, "");
	(void) textdomain(TEXT_DOMAIN);

	while ((c = getopt(argc, argv, "psv")) != EOF) {
		switch (c) {
		case 'v':
			verbosity |= 2;
			break;
		case 's':
			verbosity &= ~1;
			break;
		case 'p':
			phys_view = 1;
			break;
		default:
			usage(NULL);
		}
	}

	argc -= optind;
	argv += optind;

	if (verbosity == 2)
		usage(gettext("options -s and -v are mutually exclusive"));

	if (verbosity == 0 && argc != 1)
		usage(gettext("must specify exactly one processor if -s used"));

	if ((kc = kstat_open()) == NULL) {
		(void) fprintf(stderr, gettext("%s: kstat_open() failed: %s\n"),
		    cmdname, strerror(errno));
		exit(1);
	}

	/*
	 * Build localized cpu state table.
	 */
	cpu_states[0].state = PS_ONLINE;
	cpu_states[0].lstate = gettext(PS_ONLINE);
	cpu_states[1].state = PS_POWEROFF;
	cpu_states[1].lstate = gettext(PS_POWEROFF);
	cpu_states[2].state = PS_NOINTR;
	cpu_states[2].lstate = gettext(PS_NOINTR);
	cpu_states[3].state = PS_FAULTED;
	cpu_states[3].lstate = gettext(PS_FAULTED);
	cpu_states[4].state = PS_SPARE;
	cpu_states[4].lstate = gettext(PS_SPARE);
	cpu_states[5].state = PS_OFFLINE;
	cpu_states[5].lstate = gettext(PS_OFFLINE);

	if (phys_view) {
		/*
		 * Note that we assume that MAX_CHIPID is <= MAX_CPUID.
		 * If this becomes untrue, a new sysconf() would be warranted.
		 */
		max_chip_id = sysconf(_SC_CPUID_MAX);
		chips = calloc(max_chip_id + 1, sizeof (struct chip));
		if (chips == NULL) {
			perror("calloc");
			exit(1);
		}
	}

	/*
	 * In the physical view, we want to display all the core info or
	 * none, even when the user specifies a range of CPUIDs.  So for
	 * "psrinfo -pv <range>" or "psrinfo -ps <range>", we inventory
	 * every cpu_info kstat, and *then* we go through the user
	 * specified processors merely flipping on their "visible"
	 * flags.
	 */

	if (argc == 0 || (phys_view && (verbosity != 1))) {
		/*
		 * No processors specified.  Report on all of them.
		 * Or do a complete inventory in preparation for a
		 * specified list of physical processors.  See note
		 * immediately above.
		 */
		processorid_t maxcpu = -1;

		for (ksp = kc->kc_chain; ksp != NULL; ksp = ksp->ks_next)
			if (strcmp(ksp->ks_module, "cpu_info") == 0 &&
			    ksp->ks_instance > maxcpu)
				maxcpu = ksp->ks_instance;

		for (cpu = 0; cpu <= maxcpu; cpu++)
			if (ksp = kstat_lookup(kc, "cpu_info", cpu, NULL))
				cpu_info(kc, ksp, verbosity, phys_view,
				    argc == 0);
	}

	if (argc != 0) {
		/*
		 * Report on specified processors.
		 */
		for (; argc > 0; argv++, argc--) {
			if (strchr(*argv, '-') == NULL) {
				/* individual processor id */
				char cpubuf[20];

				(void) sprintf(cpubuf, "cpu_info%.10s", *argv);
				if (ksp = kstat_lookup(kc, "cpu_info", -1,
				    cpubuf))
					cpu_info(kc, ksp, verbosity,
					    phys_view, 1);
				else {
					(void) fprintf(stderr,
					    gettext("%s: processor %s: %s\n"),
					    cmdname, *argv, strerror(EINVAL));
					errors = 2;
				}
			} else {
				/* range of processors */
				int first, last;
				int found = 0;

				if (verbosity == 0) {
					usage(gettext("must specify exactly "
					    "one processor if -s used"));
				}
				first = (int)strtol(*argv, &errptr, 10);
				if (*errptr++ != '-') {
					(void) fprintf(stderr,
					    gettext("%s: invalid processor "
					    "range %s\n"), cmdname, *argv);
					errors = 2;
					continue;
				}
				last = (int)strtol(errptr, &errptr, 10);
				if ((errptr != NULL && *errptr != '\0') ||
				    last < first || first < 0) {
					(void) fprintf(stderr,
					    gettext("%s: invalid processor "
					    "range %s\n"), cmdname, *argv);
					errors = 2;
					continue;
				}
				for (cpu = first; cpu <= last; cpu++) {
					if (ksp = kstat_lookup(kc, "cpu_info",
					    cpu, NULL)) {
						found = 1;
						cpu_info(kc, ksp, verbosity,
						    phys_view, 1);
					}
				}
				if (!found) {
					(void) fprintf(stderr,
					    gettext("%s: no processors in "
					    "range %d-%d\n"), cmdname,
					    first, last);
				}
			}
		}
	}

	if (phys_view) {
		int i;

		switch (verbosity) {
		case 0:
			/*
			 * Print "1" if all the cores on this chip are
			 * online.  "0" otherwise.
			 */
			for (i = 0; i <= max_chip_id; i++) {
				struct chip *c = &chips[i];

				if (!c->visible)
					continue;
				(void) printf("%d\n",
				    c->online == c->core_count);
				exit(0);
			}
			break;
		case 1:
			/*
			 * Print the number of unique chips represented by
			 * all the cores specified on the command line
			 * (or, with no args, all the cores in the system).
			 */
			(void) printf("%d\n", chip_count);
			break;
		case 3:
			/*
			 * Print a report on each chip.
			 */
			for (i = 0; i <= max_chip_id; i++) {
				int j;
				struct chip *c = &chips[i];

				if (!c->visible)
					continue;

				(void) printf(gettext("The physical "
				    "processor has %d virtual %s ("),
				    c->core_count,
				    c->core_count == 1 ?
					gettext("processor") :
					gettext("processors"));
				for (j = 0; j < c->core_count; j++) {
					if (j > 0)
						(void) printf(", ");
					(void) printf("%d", c->cores[j]);
				}
				(void) printf(")\n");

				(void) printf("  %s\n", c->impl);

				/*
				 * If the "brand" has already been embedded
				 * at the front of the "impl" string, don't
				 * print it out again .. otherwise give it
				 * a fresh line to bask upon ..
				 */
				if (strncmp(c->impl, c->brand,
				    strlen(c->brand)) != 0)
					(void) printf("\t%s\n", c->brand);
			}
			break;
		}
	}

	return (errors);
}

#define	GETLONG(name)	((kstat_named_t *)kstat_data_lookup(ksp, name))->value.l
#define	GETSTR(name)	((kstat_named_t *)kstat_data_lookup(ksp, name))->value.c
#define	GETLONGSTR(name) \
	KSTAT_NAMED_STR_PTR((kstat_named_t *)kstat_data_lookup(ksp, name))

/*
 * Utility function to retrieve the localized version of the cpu state string.
 */
static const char *
get_cpu_state(const char *state)
{
	int i;

	for (i = 0; i < NCPUSTATES; i++)
		if (strcmp(cpu_states[i].state, state) == 0)
			return (cpu_states[i].lstate);
	return (gettext("(unknown)"));
}

static void
cpu_info(kstat_ctl_t *kc, kstat_t *ksp, int verbosity, int phys_view,
    int visible)
{
	char	curtime[40], start[40];
	processorid_t cpu_id = ksp->ks_instance;
	time_t	now = time(NULL);

	if (kstat_read(kc, ksp, NULL) == -1) {
		(void) fprintf(stderr,
		    gettext("%s: kstat_read() failed for cpu %d: %s\n"),
		    cmdname, cpu_id, strerror(errno));
		exit(1);
	}

	if (phys_view) {
		kstat_named_t *k =
		    (kstat_named_t *)kstat_data_lookup(ksp, "chip_id");
		struct chip *c;

		if (k == NULL) {
			(void) fprintf(stderr,
			    gettext("%s: Physical processor view "
			    "not supported\n"),
			    cmdname);
			exit(1);
		}

		c = &chips[k->value.i32];

		if (visible && c->core_count != c->visible) {
			/*
			 * We've already inventoried this chip.  And the user
			 * specified a range of CPUIDs.  So, now we just
			 * need to note that this is one of the chips to
			 * display.
			 */
			c->visible++;
			return;
		}

		if (c->core_count == 0) {
			char *str;

			str = GETLONGSTR("implementation");
			(void) strlcpy(c->impl, str ? str : "(unknown)",
			    sizeof (c->impl));

			str = GETLONGSTR("brand");
			(void) strlcpy(c->brand, str ? str : "(unknown)",
			    sizeof (c->brand));

			chip_count++;
		}

		c->cores[c->core_count] = cpu_id;
		c->core_count++;
		c->online += strcmp(GETSTR("state"), "on-line") == 0;

		if (visible)
			c->visible++;
		return;
	}

	(void) strftime(start, sizeof (start), gettext("%m/%d/%Y %T"),
	    localtime((time_t *)&GETLONG("state_begin")));
	(void) strftime(curtime, sizeof (curtime), gettext("%m/%d/%Y %T"),
	    localtime(&now));

	if (verbosity == 0) {
		(void) printf("%d\n", strcmp(GETSTR("state"), "on-line") == 0);
		return;
	}
	if (verbosity == 1) {
		(void) printf(gettext("%d\t%-8s  since %s\n"), cpu_id,
		    get_cpu_state(GETSTR("state")), start);
		return;
	}

	(void) printf(gettext("Status of virtual processor %d as of: %s\n"),
	    cpu_id, curtime);
	(void) printf(gettext("  %s since %s.\n"),
	    get_cpu_state(GETSTR("state")), start);

	if (GETLONG("clock_MHz") != 0)
		(void) printf(gettext("  The %s processor operates at %d MHz"),
		    GETSTR("cpu_type"), GETLONG("clock_MHz"));
	else
		(void) printf(gettext("  The %s processor operates at an "
		    "unknown frequency"), GETSTR("cpu_type"));

	if (GETSTR("fpu_type")[0] == '\0')
		(void) printf(gettext(",\n\tand has no floating point "
		    "processor.\n"));
	else if (strchr("aeiouy", GETSTR("fpu_type")[0]))
		(void) printf(gettext(",\n\tand has an %s floating point "
		    "processor.\n"), GETSTR("fpu_type"));
	else
		(void) printf(gettext(",\n\tand has a %s floating point "
		    "processor.\n"), GETSTR("fpu_type"));
}
