/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
 * Copyright (c) 2024, gperftools Contributors
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

// Note, this is somewhat "poor man's" equivalent of config.h that is
// normally produced by autotools. It detects features at compile
// time, so it is easier to integrate with stuff like bazel, but it
// supports fewer platforms. As of this writing it is tested on modern
// GNU/Linux and Windows (MSVC).
#ifndef GPERFTOOLS_CONFIG_H_
#define GPERFTOOLS_CONFIG_H_

/* enable aggressive decommit by default */
/* #undef ENABLE_AGGRESSIVE_DECOMMIT_BY_DEFAULT */

/* Build runtime detection for sized delete */
/* #undef ENABLE_DYNAMIC_SIZED_DELETE */

/* report large allocation */
/* #undef ENABLE_LARGE_ALLOC_REPORT */

/* Build sized deletion operators */
/* #undef ENABLE_SIZED_DELETE */

/* Define to 1 if you have the <asm/ptrace.h> header file. */
#if defined __has_include
#  if __has_include(<asm/ptrace.h>)
#    define HAVE_ASM_PTRACE_H 1
#  endif
#endif

/* Whether <cxxabi.h> contains __cxxabiv1::__cxa_demangle */
#ifdef __GNUC__
#define HAVE_CXA_DEMANGLE 1
#endif

/* Define to 1 if you have the declaration of 'backtrace', and to 0 if you
   don't. */
/* #undef HAVE_DECL_BACKTRACE */

/* Define to 1 if you have the declaration of 'backtrace_symbols', and to 0 if
   you don't. */
// #undef HAVE_DECL_BACKTRACE_SYMBOLS 1

/* Define to 1 if you have the declaration of 'memalign', and to 0 if you
   don't. */
// memalign is legacy API. BSDs have already removed it.
#if __linux__
#define HAVE_DECL_MEMALIGN 1
#endif

/* Define to 1 if you have the declaration of 'nanosleep', and to 0 if you
   don't. */
/* #undef HAVE_DECL_NANOSLEEP */

/* Define to 1 if you have the declaration of 'posix_memalign', and to 0 if
   you don't. */
// We keep it simple for generic config.h and assume anything
// non-windows is modern enough.
#ifndef _WIN32
#define HAVE_DECL_POSIX_MEMALIGN 1
#endif

/* Define to 1 if you have the declaration of 'pvalloc', and to 0 if you
   don't. */
#if __linux__
// same as memalign above.
#define HAVE_DECL_PVALLOC 1
#endif

/* Define to 1 if you have the declaration of 'sleep', and to 0 if you don't.
   */
/* #undef HAVE_DECL_SLEEP */

/* Define to 1 if you have the declaration of 'valloc', and to 0 if you don't.
   */
#if __linux__
// same as memalign above
#define HAVE_DECL_VALLOC 1
#endif

/* Define to 1 if you have the <dlfcn.h> header file. */
#if defined __has_include
#  if __has_include(<dlfcn.h>)
#    define HAVE_DLFCN_H 1
#  endif
#endif

/* Define to 1 if you have the <execinfo.h> header file. */
#if defined __has_include
#  if __has_include(<execinfo.h>)
#    define HAVE_EXECINFO_H 1
#  endif
#endif

/* Define to 1 if you have the <fcntl.h> header file. */
#if defined __has_include
#  if __has_include(<fcntl.h>)
#    define HAVE_FCNTL_H 1
#  endif
#endif

/* Define to 1 if you have the <features.h> header file. */
#if defined __has_include
#  if __has_include(<features.h>)
#    include <features.h> // for __GLIBC__ define below and elsewhere
#  endif
#endif

/* Define to 1 if you have the 'geteuid' function. */
#ifndef _WIN32
#define HAVE_GETEUID 1
#endif

/* Define to 1 if you have the <glob.h> header file. */
#if defined __has_include
#  if __has_include(<glob.h>)
#    define HAVE_GLOB_H 1
#  endif
#endif

/* Define to 1 if you have the <inttypes.h> header file. */
#if defined __has_include
#  if __has_include(<inttypes.h>)
#    define HAVE_INTTYPES_H 1
#  endif
#endif

/* Define to 1 if you have the <libunwind.h> header file. */
#if defined __has_include
#  if __has_include(<libunwind.h>)
#    define HAVE_LIBUNWIND_H 1
#  endif
#endif

/* Define if this is Linux that has SIGEV_THREAD_ID */
#if __linux__
#define HAVE_LINUX_SIGEV_THREAD_ID 1
#endif

/* Define to 1 if you have the <malloc.h> header file. */
#if defined __has_include
#  if __has_include(<malloc.h>)
#    define HAVE_MALLOC_H 1
#  endif
#endif

/* Define to 1 if you have a working `mmap' system call. */
#ifndef _WIN32
#define HAVE_MMAP 1
#endif

/* define if libc has program_invocation_name */
#if __linux__
#define HAVE_PROGRAM_INVOCATION_NAME 1
#endif

/* Define to 1 if you have the 'sbrk' function. */
#if __linux__
// Some BSDs already started removing sbrk. So we keep it simple for
// now.
#define HAVE_SBRK 1
#endif

/* Define to 1 if you have the <sched.h> header file. */
#if defined __has_include
#  if __has_include(<sched.h>)
#    define HAVE_SCHED_H 1
#  endif
#endif


/* Define to 1 if you have the <sys/cdefs.h> header file. */
#if defined __has_include
#  if __has_include(<sys/cdefs.h>)
#    define HAVE_SYS_CDEFS_H 1
#  endif
#endif

/* Define to 1 if you have the <sys/stat.h> header file. */
#if defined __has_include
#  if __has_include(<sys/stat.h>)
#    define HAVE_SYS_STAT_H 1
#  endif
#endif

/* Define to 1 if you have the <sys/syscall.h> header file. */
#if defined __has_include
#  if __has_include(<sys/syscall.h>)
#    define HAVE_SYS_SYSCALL_H 1
#  endif
#endif

/* Define to 1 if you have the <sys/types.h> header file. */
#if defined __has_include
#  if __has_include(<sys/types.h>)
#    define HAVE_SYS_TYPES_H 1
#  endif
#endif

/* Define to 1 if you have the <sys/ucontext.h> header file. */
#if defined __has_include
#  if __has_include(<sys/ucontext.h>)
#    define HAVE_SYS_UCONTEXT_H 1
#  endif
#endif

/* Define to 1 if you have the <ucontext.h> header file. */
#if defined __has_include
#  if __has_include(<ucontext.h>)
#    define HAVE_UCONTEXT_H 1
#  endif
#endif

/* Define to 1 if you have the <unistd.h> header file. */
#if defined __has_include
#  if __has_include(<unistd.h>)
#    define HAVE_UNISTD_H 1
#  endif
#endif

/* Whether <unwind.h> contains _Unwind_Backtrace */
// Apparently both clang (even with -stdlib=libc++) and gcc have
// Unwind_Backtrace. But we seem to be avoiding it on OSX and FreeBSD,
// reportedly due to recursing back into malloc (see matching comment
// in configure.ac)
#if defined(__GNUC__) && !defined(__APPLE__) && !defined(__FreeBSD__)
#define HAVE_UNWIND_BACKTRACE 1
#endif

#ifdef __GNUC__
/* define if your compiler has __attribute__ */
#define HAVE___ATTRIBUTE__ 1
/* define if your compiler supports alignment of functions */
#define HAVE___ATTRIBUTE__ALIGNED_FN 1
#endif

/* Define to 1 if compiler supports __environ */
#if __GLIBC__
#define HAVE___ENVIRON 1
#endif

/* dllexport or attribute visibility */
#define PERFTOOLS_DLL_DECL /**/

/* if libgcc stacktrace method should be default */
/* #undef PREFER_LIBGCC_UNWINDER */

/* Define 8 bytes of allocation alignment for tcmalloc */
/* #undef TCMALLOC_ALIGN_8BYTES */

/* Define internal page size for tcmalloc as number of left bitshift */
/* #undef TCMALLOC_PAGE_SIZE_SHIFT */

/* libunwind.h was found and is working */
//#define USE_LIBUNWIND 1

/* C99 says: define this to get the PRI... macros from stdint.h */
#ifndef __STDC_FORMAT_MACROS
# define __STDC_FORMAT_MACROS 1
#endif


#ifdef _WIN32
// TODO(csilvers): include windows/port.h in every relevant source file instead?
#include "windows/port.h"
#endif

#endif  /* #ifndef GPERFTOOLS_CONFIG_H_ */
