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
 * Copyright 2005 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

/*
 * This is where all the interfaces that are internal to libc
 * which do not have a better home live
 */

#ifndef _LIBC_H
#define	_LIBC_H

#pragma ident	"%Z%%M%	%I%	%E% SMI"

#include <thread.h>
#include <stdio.h>
#include <sys/dirent.h>
#include <ucontext.h>
#include <nsswitch.h>
#include <stddef.h>
#include <poll.h>
#include <sys/dl.h>
#include <sys/door.h>
#include <sys/ieeefp.h>
#include <sys/mount.h>
#include <floatingpoint.h>
#include <nl_types.h>
#include <regex.h>
#include <stdarg.h>
#include <wchar.h>
#include <wctype.h>

#ifdef	__cplusplus
extern "C" {
#endif

extern void __set_panicstr(const char *);
extern void _rewind_unlocked(FILE *);
extern int _rename(const char *, const char *);
extern long _sysconfig(int);
extern int kill(pid_t pid, int sig);

extern int primary_link_map;
extern int thr_main(void);
extern int thr_kill(thread_t tid, int sig);
extern thread_t thr_self(void);
extern int mutex_lock(mutex_t *mp);
extern int mutex_unlock(mutex_t *mp);
extern int fork_lock_enter(const char *);
extern void fork_lock_exit(void);
extern void *lmalloc(size_t);
extern void lfree(void *, size_t);
extern void *libc_malloc(size_t);
extern void *libc_realloc(void *, size_t);
extern void libc_free(void *);
extern char *libc_strdup(const char *);
extern int _sigwait(sigset_t *);
extern int _thr_getspecific(thread_key_t key, void **valuep);
extern int _thr_setspecific(unsigned int key, void *value);
extern int _thr_keycreate(thread_key_t *pkey, void (*destructor)(void *));
extern void *_pthread_getspecific(thread_key_t);
extern int _pollsys(struct pollfd *, nfds_t, const timespec_t *,
	const sigset_t *);
extern void _private_testcancel(void);

#if !defined(_LP64)
/*
 * getdents64 transitional interface is intentionally internal to libc
 */
extern int getdents64(int, struct dirent64 *, size_t);
#endif

extern int _scrwidth(wchar_t);

extern int64_t __div64(int64_t, int64_t);
extern int64_t __rem64(int64_t, int64_t);
extern uint64_t __udiv64(uint64_t, uint64_t);
extern uint64_t __urem64(uint64_t, uint64_t);
extern uint64_t _umul32x32to64(uint32_t, uint32_t);
extern int64_t __mul64(int64_t, int64_t);
extern uint64_t __umul64(uint64_t, uint64_t);


/*
 * Rounding direction functions
 */
#if defined(__i386) || defined(__amd64)
extern enum fp_direction_type __xgetRD(void);
#elif defined(__sparc)
extern enum fp_direction_type _QgetRD(void);
#else
#error Unknown architecture!
#endif


/*
 * defined in hex_bin.c
 */
extern void __hex_to_single(decimal_record *, enum fp_direction_type,
	single *, fp_exception_field_type *);
extern void __hex_to_double(decimal_record *, enum fp_direction_type,
	double *, fp_exception_field_type *);
#if defined(__sparc)
extern void __hex_to_quadruple(decimal_record *, enum fp_direction_type,
	quadruple *, fp_exception_field_type *);
#elif defined(__i386) || defined(__amd64)
extern void __hex_to_extended(decimal_record *, enum fp_direction_type,
	extended *, fp_exception_field_type *);
#else
#error Unknown architecture
#endif

/*
 * defined in ctime.c
 */
extern char	*__posix_asctime_r(const struct tm *, char *);

/*
 * Internal routine from fsync.c
 */
extern int __fdsync(int, int);	/* 2nd arg may be wrong in 64bit mode */

/*
 * Internal routine from _xregs_clrptr.c
 */
extern void _xregs_clrptr(ucontext_t *);

/*
 * Internal routine from nfssys.c
 */
extern int _nfssys(int, void *); /* int in 64bit mode ???, void * ??? */

/*
 * Internal routine from psetsys.c
 */
extern int _pset(int, ...); /* int in 64bit mode ??? */

/*
 * defined in sigpending.s
 */
extern int __sigfillset(sigset_t *);

/*
 * defined in sparc/fp/_Q_set_except.c and i386/fp/exception.c
 */
extern int _Q_set_exception(unsigned int);

/*
 * defined in nsparse.c
 */
extern struct __nsw_switchconfig *_nsw_getoneconfig(const char *name,
	char *linep, enum __nsw_parse_err *);
extern struct __nsw_switchconfig_v1 *_nsw_getoneconfig_v1(const char *name,
	char *linep, enum __nsw_parse_err *);

/*
 * Internal routine from getusershell.c
 */
extern char *getusershell(void);

/*
 * defined in _sigaction.s
 */
extern int __sigaction(int, const struct sigaction *, struct sigaction *);

/*
 * defined in _getsp.s
 */
extern greg_t _getsp(void);

/*
 * defined in _so_setsockopt.s
 */
extern int _so_setsockopt(int, int, int, const char *, int);

/*
 * defined in _so_getsockopt.s
 */
extern int _so_getsockopt(int, int, int, char *, int *);

/*
 * defined in lsign.s
 */
extern int lsign(dl_t);

/*
 * defined in ucontext.s
 * __getcontext() is exported by libc
 * __getcontext_syscall() is private to libc
 * otherwise they are the same function
 */
extern int __getcontext(ucontext_t *);
extern int __getcontext_syscall(ucontext_t *);

/*
 * defined in door.s
 */
extern int __door_info(int, door_info_t *);
extern int __door_call(int, door_arg_t *);

/*
 * defined in _portfs.s
 */
extern int64_t _portfs(int, uintptr_t, uintptr_t, uintptr_t, uintptr_t,
    uintptr_t);

/*
 * defined in xpg4.c
 */
extern int __xpg4;

/*
 * i18n prototypes - strong symbols (weak symbols are in libintl.h)
 */
extern char *_textdomain(const char *);
extern char *_bindtextdomain(const char *, const char *);
extern char *_dcgettext(const char *, const char *, const int);
extern char *_dgettext(const char *, const char *);
extern char *_gettext(const char *);
extern int _fnmatch(const char *, const char *, int);


/*
 * defined in port/stdio/doscan.c
 */
extern int _doscan(FILE *, const char *, va_list);
extern int __doscan_u(FILE *, const char *, va_list, int);
extern int __wdoscan_u(FILE *, const wchar_t *, va_list, int);

#ifndef _LP64
/* Flag for _ndoprnt() and _doscan_u() */
#define	_F_INTMAX32	0x1	/* if set read 4 bytes for u/intmax %j */
extern int _fprintf_c89(FILE *, const char *, ...);
extern int _printf_c89(const char *, ...);
extern int _snprintf_c89(char *, size_t, const char *, ...);
extern int _sprintf_c89(char *, const char *, ...);
extern int _wprintf_c89(const wchar_t *, ...);
extern int _fwprintf_c89(FILE *, const wchar_t *, ...);
extern int _swprintf_c89(wchar_t *, size_t, const wchar_t *, ...);
extern int _vfprintf_c89(FILE *, const char *, va_list);
extern int _vprintf_c89(const char *, va_list);
extern int _vsnprintf_c89(char *, size_t, const char *, va_list);
extern int _vsprintf_c89(char *, const char *, va_list);
extern int _vwprintf_c89(const wchar_t *, va_list);
extern int _vfwprintf_c89(FILE *, const wchar_t *, va_list);
extern int _vswprintf_c89(wchar_t *, size_t, const wchar_t *, va_list);
extern int _scanf_c89(const char *, ...);
extern int _fscanf_c89(FILE *, const char *, ...);
extern int _sscanf_c89(const char *, const char *, ...);
extern int _vscanf_c89(const char *, va_list);
extern int _vfscanf_c89(FILE *, const char *, va_list);
extern int _vsscanf_c89(const char *, const char *, va_list);
extern int _vwscanf_c89(const wchar_t *, va_list);
extern int _vfwscanf_c89(FILE *, const wchar_t *, va_list);
extern int _vswscanf_c89(const wchar_t *, const wchar_t *, va_list);
extern int _wscanf_c89(const wchar_t *, ...);
extern int _fwscanf_c89(FILE *, const wchar_t *, ...);
extern int _swscanf_c89(const wchar_t *, const wchar_t *, ...);
#endif	/*	_LP64	*/

/*
 * defined in port/stdio/popen.c
 */
extern int _insert(pid_t pid, int fd);
extern pid_t _delete(int fd);

/*
 * defined in port/print/doprnt.c
 */
extern ssize_t	_wdoprnt(const wchar_t *, va_list, FILE *);

/*
 * defined in fgetwc.c
 */
extern wint_t _fgetwc_unlocked(FILE *);
extern wint_t __getwc_xpg5(FILE *);
extern wint_t __fgetwc_xpg5(FILE *);
extern wint_t _getwc(FILE *);

/*
 * defined in fputwc.c
 */
extern wint_t __putwc_xpg5(wint_t, FILE *);
extern wint_t _putwc(wint_t, FILE *);

/*
 * defined in ungetwc.c
 */
extern wint_t	__ungetwc_xpg5(wint_t, FILE *);

/*
 * defined in wscmp.c
 */
extern int	_wcscmp(const wchar_t *, const wchar_t *);

/*
 * defined in wslen.c
 */
extern size_t	_wcslen(const wchar_t *);

/*
 * defined in wscpy.c
 */
extern wchar_t	*_wcscpy(wchar_t *, const wchar_t *);

#ifdef __sparc
extern ulong_t caller(void);
extern ulong_t getfp(void);
#endif

/*
 * misc synonyms
 */
extern int _wctomb(char *, wchar_t);
extern wint_t _towlower(wint_t);
extern int _doscan(FILE *, const char *, va_list);
extern int _wcscoll(const wchar_t *, const wchar_t *);
extern size_t _wcsxfrm(wchar_t *, const wchar_t *, size_t);
extern wint_t	_fputwc(wint_t, FILE *);
#ifdef	__cplusplus
}
#endif

#endif /* _LIBC_H */
