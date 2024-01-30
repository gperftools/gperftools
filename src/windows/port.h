// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
/* Copyright (c) 2007, Google Inc.
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
 *
 * ---
 * Author: Craig Silverstein
 *
 * These are some portability typedefs and defines to make it a bit
 * easier to compile this code under VC++.
 *
 * Several of these are taken from glib:
 *    http://developer.gnome.org/doc/API/glib/glib-windows-compatability-functions.html
 */

#ifndef GOOGLE_BASE_WINDOWS_H_
#define GOOGLE_BASE_WINDOWS_H_

/* You should never include this file directly, but always include it from config.h */
#ifndef GPERFTOOLS_CONFIG_H_
# error "port.h should only be included from config.h"
#endif

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN  /* We always want minimal includes */
#endif

// windows.h whatevevs defines min and max preprocessor macros and
// that breaks ::max() in various places (like numeric_limits)
#ifndef NOMINMAX
#define NOMINMAX
#endif

// This must be defined before the windows.h is included.  We need at
// least 0x0400 for mutex.h to have access to TryLock, and at least
// 0x0501 for patch_functions.cc to have access to GetModuleHandleEx.
// (This latter is an optimization we could take out if need be.)
#ifndef _WIN32_WINNT
# define _WIN32_WINNT 0x0501
#endif

// We want to make sure not to ever try to #include heap-checker.h
#define NO_HEAP_CHECK 1

#if defined(__MINGW32__) && __MSVCRT_VERSION__ < 0x0700
// Older version of the mingw msvcrt don't define _aligned_malloc
# define PERFTOOLS_NO_ALIGNED_MALLOC 1
#endif

#include <windows.h>
#include <io.h>              /* because we so often use open/close/etc */
#include <direct.h>          /* for _getcwd */
#include <process.h>         /* for _getpid */
#include <limits.h>          /* for PATH_MAX */
#include <stdarg.h>          /* for va_list */
#include <stdio.h>           /* need this to override stdio's (v)snprintf */
#include <sys/types.h>       /* for _off_t */
#include <assert.h>
#include <stdlib.h>          /* for rand, srand, _strtoxxx */

#if defined(_MSC_VER) && _MSC_VER >= 1900
#define _TIMESPEC_DEFINED
#include <time.h>
#endif

/*
 * 4018: signed/unsigned mismatch is common (and ok for signed_i < unsigned_i)
 * 4244: otherwise we get problems when subtracting two size_t's to an int
 * 4288: VC++7 gets confused when a var is defined in a loop and then after it
 * 4267: too many false positives for "conversion gives possible data loss"
 * 4290: it's ok windows ignores the "throw" directive
 * 4996: Yes, we're ok using "unsafe" functions like vsnprintf and getenv()
 * 4146: internal_logging.cc intentionally negates an unsigned value
 */
#ifdef _MSC_VER
#pragma warning(disable:4018 4244 4288 4267 4290 4996 4146)
#endif

#ifndef __cplusplus
/* MSVC does not support C99 */
# if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 199901L
#  ifdef _MSC_VER
#    define inline __inline
#  else
#    define inline static
#  endif
# endif
#endif

#ifdef __cplusplus
# define EXTERN_C  extern "C"
#else
# define EXTERN_C  extern
#endif

/* ----------------------------------- BASIC TYPES */

/* I guess MSVC's <types.h> doesn't include ssize_t by default? */
#ifdef _MSC_VER
typedef intptr_t ssize_t;
#endif

/* ----------------------------------- THREADS */

/*
 * __declspec(thread) isn't usable in a dll opened via LoadLibrary().
 * But it doesn't work to LoadLibrary() us anyway, because of all the
 * things we need to do before main()!  So this kind of TLS is safe for us.
 */
#define __thread __declspec(thread)

/* ----------------------------------- MMAP and other memory allocation */

#ifndef HAVE_MMAP   /* not true for MSVC, but may be true for msys */
#define MAP_FAILED  0
#define MREMAP_FIXED  2  /* the value in linux, though it doesn't really matter */
/* These, when combined with the mmap invariants below, yield the proper action */
#define PROT_READ      PAGE_READWRITE
#define PROT_WRITE     PAGE_READWRITE
#define MAP_ANONYMOUS  MEM_RESERVE
#define MAP_PRIVATE    MEM_COMMIT
#define MAP_SHARED     MEM_RESERVE   /* value of this #define is 100% arbitrary */

#if __STDC__ && !defined(__MINGW32__)
typedef _off_t off_t;
#endif

/* VirtualAlloc only replaces for mmap when certain invariants are kept. */
inline void *mmap(void *addr, size_t length, int prot, int flags,
                  int fd, off_t offset) {
  if (addr == NULL && fd == -1 && offset == 0 &&
      prot == (PROT_READ|PROT_WRITE) && flags == (MAP_PRIVATE|MAP_ANONYMOUS)) {
    return VirtualAlloc(0, length, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  } else {
    return NULL;
  }
}

inline int munmap(void *addr, size_t length) {
  return VirtualFree(addr, 0, MEM_RELEASE) ? 0 : -1;
}
#endif  /* HAVE_MMAP */

/* We could maybe use VirtualAlloc for sbrk as well, but no need */
inline void *sbrk(intptr_t increment) {
  // sbrk returns -1 on failure
  return (void*)-1;
}


/* ----------------------------------- FILE IO */

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif
#ifndef __MINGW32__
enum { STDIN_FILENO = 0, STDOUT_FILENO = 1, STDERR_FILENO = 2 };
#endif
#ifndef O_RDONLY
#define O_RDONLY  _O_RDONLY
#endif

#if __STDC__ && !defined(__MINGW32__)
/* These functions are considered non-standard */
inline int access(const char *pathname, int mode) {
  return _access(pathname, mode);
}
inline int open(const char *pathname, int flags, int mode = 0) {
  return _open(pathname, flags, mode);
}
inline int close(int fd) {
  return _close(fd);
}
inline ssize_t read(int fd, void *buf, size_t count) {
  return _read(fd, buf, count);
}
inline ssize_t write(int fd, const void *buf, size_t count) {
  return _write(fd, buf, count);
}
inline off_t lseek(int fd, off_t offset, int whence) {
  return _lseek(fd, offset, whence);
}
inline char *getcwd(char *buf, size_t size) {
  return _getcwd(buf, size);
}
inline int mkdir(const char *pathname, int) {
  return _mkdir(pathname);
}

inline FILE *popen(const char *command, const char *type) {
  return _popen(command, type);
}
inline int pclose(FILE *stream) {
  return _pclose(stream);
}
#endif

EXTERN_C PERFTOOLS_DLL_DECL void WriteToStderr(const char* buf, int len);

/* ----------------------------------- SYSTEM/PROCESS */

/* Handle case when poll is used to simulate sleep. */
inline int poll(struct pollfd* fds, int nfds, int timeout) {
  assert(fds == NULL);
  assert(nfds == 0);
  Sleep(timeout);
  return 0;
}

EXTERN_C PERFTOOLS_DLL_DECL int getpagesize();   /* in port.cc */

/* ----------------------------------- OTHER */

inline void srandom(unsigned int seed) { srand(seed); }
inline long random(void) { return rand(); }

#ifndef HAVE_DECL_SLEEP
#define HAVE_DECL_SLEEP 0
#endif

#if !HAVE_DECL_SLEEP
inline unsigned int sleep(unsigned int seconds) {
  Sleep(seconds * 1000);
  return 0;
}
#endif

// mingw64 seems to define timespec (though mingw.org mingw doesn't),
// protected by the _TIMESPEC_DEFINED macro.
#ifndef _TIMESPEC_DEFINED
struct timespec {
  int tv_sec;
  int tv_nsec;
};
#endif

#ifndef HAVE_DECL_NANOSLEEP
#define HAVE_DECL_NANOSLEEP 0
#endif

// latest mingw64 has nanosleep. Earlier mingw and MSVC do not
#if !HAVE_DECL_NANOSLEEP
inline int nanosleep(const struct timespec *req, struct timespec *rem) {
  Sleep(req->tv_sec * 1000 + req->tv_nsec / 1000000);
  return 0;
}
#endif

#ifndef __MINGW32__
#if defined(_MSC_VER) && _MSC_VER < 1800
inline long long int strtoll(const char *nptr, char **endptr, int base) {
    return _strtoi64(nptr, endptr, base);
}
inline unsigned long long int strtoull(const char *nptr, char **endptr,
                                       int base) {
    return _strtoui64(nptr, endptr, base);
}
inline long long int strtoq(const char *nptr, char **endptr, int base) {
    return _strtoi64(nptr, endptr, base);
}
#endif
inline unsigned long long int strtouq(const char *nptr, char **endptr,
                                      int base) {
    return _strtoui64(nptr, endptr, base);
}
inline long long atoll(const char *nptr) {
  return _atoi64(nptr);
}
#endif

#define __THROW throw()

/* ----------------------------------- TCMALLOC-SPECIFIC */

/* tcmalloc.cc calls this so we can patch VirtualAlloc() et al. */
extern void PatchWindowsFunctions();

#endif  /* _WIN32 */

#undef inline
#undef EXTERN_C

#endif  /* GOOGLE_BASE_WINDOWS_H_ */
