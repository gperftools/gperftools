/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
/* A pregenerated copy of config.h for VSProj-based builds
 *
 * Use of this source code is governed by a BSD-style license that can
 * be found in the LICENSE file.
 */

#if !defined(_MSC_VER) || defined __MINGW32__
#error "This config.h should only be consumed by the VSProj build!"
#endif

#ifndef GPERFTOOLS_CONFIG_H_
#define GPERFTOOLS_CONFIG_H_

/* Enable aggressive decommit by default */
/* #undef ENABLE_AGGRESSIVE_DECOMMIT_BY_DEFAULT */

/* Build runtime detection for sized delete */
/* #undef ENABLE_DYNAMIC_SIZED_DELETE */

/* Report large allocation */
/* #undef ENABLE_LARGE_ALLOC_REPORT */

/* Build sized deletion operators */
/* #undef ENABLE_SIZED_DELETE */

/* Define to 1 if you have the <asm/ptrace.h> header file. */
/* #undef HAVE_ASM_PTRACE_H */

/* Define to 1 if you have the <cygwin/signal.h> header file. */
/* #undef HAVE_CYGWIN_SIGNAL_H */

/* Define to 1 if you have the declaration of `backtrace', and to 0 if you
   don't. */
/* #undef HAVE_DECL_BACKTRACE */

/* Define to 1 if you have the declaration of `cfree', and to 0 if you don't.
   */
#define HAVE_DECL_CFREE 0

/* Define to 1 if you have the declaration of `memalign', and to 0 if you
   don't. */
#define HAVE_DECL_MEMALIGN 0

/* Define to 1 if you have the declaration of `nanosleep', and to 0 if you
   don't. */
#define HAVE_DECL_NANOSLEEP 0

/* Define to 1 if you have the declaration of `posix_memalign', and to 0 if
   you don't. */
#define HAVE_DECL_POSIX_MEMALIGN 0

/* Define to 1 if you have the declaration of `pvalloc', and to 0 if you
   don't. */
#define HAVE_DECL_PVALLOC 0

/* Define to 1 if you have the declaration of `sleep', and to 0 if you don't.
   */
#define HAVE_DECL_SLEEP 0

/* Define to 1 if you have the declaration of `valloc', and to 0 if you don't.
   */
#define HAVE_DECL_VALLOC 0

/* Define to 1 if you have the <dlfcn.h> header file. */
/* #undef HAVE_DLFCN_H */

/* Define to 1 if the system has the type `Elf32_Versym'. */
/* #undef HAVE_ELF32_VERSYM */

/* Define to 1 if you have the <execinfo.h> header file. */
/* #undef HAVE_EXECINFO_H */

/* Define to 1 if you have the <fcntl.h> header file. */
#define HAVE_FCNTL_H 1

/* Define to 1 if you have the <features.h> header file. */
/* #undef HAVE_FEATURES_H */

/* Define to 1 if you have the `geteuid' function. */
/* #undef HAVE_GETEUID */

/* Define to 1 if you have the <glob.h> header file. */
/* #undef HAVE_GLOB_H */

/* Define to 1 if you have the <grp.h> header file. */
/* #undef HAVE_GRP_H */

/* Define to 1 if you have the <libunwind.h> header file. */
/* #undef HAVE_LIBUNWIND_H */

/* Define if this is Linux that has SIGEV_THREAD_ID */
/* #undef HAVE_LINUX_SIGEV_THREAD_ID */

/* Define to 1 if you have the <malloc.h> header file. */
#define HAVE_MALLOC_H 1

/* Define to 1 if you have the <memory.h> header file. */
#define HAVE_MEMORY_H 1

/* Define to 1 if you have a working `mmap' system call. */
/* #undef HAVE_MMAP */

/* Define to 1 if you have the <poll.h> header file. */
/* #undef HAVE_POLL_H */

/* define if libc has program_invocation_name */
/* #undef HAVE_PROGRAM_INVOCATION_NAME */

/* Define to 1 if you have the <pwd.h> header file. */
/* #undef HAVE_PWD_H */

/* Define to 1 if you have the `sbrk' function. */
/* #undef HAVE_SBRK */

/* Define to 1 if you have the <sched.h> header file. */
/* #undef HAVE_SCHED_H */

/* Define to 1 if the system has the type `struct mallinfo'. */
/* #undef HAVE_STRUCT_MALLINFO */

/* Define to 1 if the system has the type `struct mallinfo2'. */
/* #undef HAVE_STRUCT_MALLINFO2 */

/* Define to 1 if you have the <sys/cdefs.h> header file. */
/* #undef HAVE_SYS_CDEFS_H */

/* Define to 1 if you have the <sys/resource.h> header file. */
/* #undef HAVE_SYS_RESOURCE_H */

/* Define to 1 if you have the <sys/socket.h> header file. */
/* #undef HAVE_SYS_SOCKET_H */

/* Define to 1 if you have the <sys/stat.h> header file. */
#define HAVE_SYS_STAT_H 1

/* Define to 1 if you have the <sys/syscall.h> header file. */
/* #undef HAVE_SYS_SYSCALL_H */

/* Define to 1 if you have the <sys/types.h> header file. */
#define HAVE_SYS_TYPES_H 1

/* Define to 1 if you have the <sys/ucontext.h> header file. */
/* #undef HAVE_SYS_UCONTEXT_H */

/* Define to 1 if you have the <sys/wait.h> header file. */
/* #undef HAVE_SYS_WAIT_H */

/* Define to 1 if you have the <ucontext.h> header file. */
/* #undef HAVE_UCONTEXT_H */

/* Define to 1 if you have the <unistd.h> header file. */
/* #undef HAVE_UNISTD_H */

/* Whether <unwind.h> contains _Unwind_Backtrace */
/* #undef HAVE_UNWIND_BACKTRACE */

/* Define to 1 if you have the <unwind.h> header file. */
/* #undef HAVE_UNWIND_H */

/* define if your compiler has __attribute__ */
/* #undef HAVE___ATTRIBUTE__ */

/* define if your compiler supports alignment of functions */
/* #undef HAVE___ATTRIBUTE__ALIGNED_FN */

/* Define to 1 if compiler supports __environ */
/* #undef HAVE___ENVIRON */

/* Define to 1 if you have the `__sbrk' function. */
/* #undef HAVE___SBRK */

/* prefix where we look for installed files */
/* #undef INSTALL_PREFIX */

/* Define to the sub-directory where libtool stores uninstalled libraries. */
/* #undef LT_OBJDIR */

/* Always the empty-string on non-windows systems. On windows, should be
   "__declspec(dllexport)". This way, when we compile the dll, we export our
   functions/classes. It's safe to define this here because config.h is only
   used internally, to compile the DLL, and every DLL source file #includes
   "config.h" before anything else. */
#ifndef PERFTOOLS_DLL_DECL
# define PERFTOOLS_IS_A_DLL 1   /* not set if you're statically linking */
# define PERFTOOLS_DLL_DECL __declspec(dllexport)
# define PERFTOOLS_DLL_DECL_FOR_UNITTESTS __declspec(dllimport)
#endif

/* Define 8 bytes of allocation alignment for tcmalloc */
/* #undef TCMALLOC_ALIGN_8BYTES */

/* Define internal page size for tcmalloc as number of left bitshift */
/* #undef TCMALLOC_PAGE_SIZE_SHIFT */

/* C99 says: define this to get the PRI... macros from stdint.h */
#ifndef __STDC_FORMAT_MACROS
# define __STDC_FORMAT_MACROS 1
#endif

// TODO(csilvers): include windows/port.h in every relevant source file instead?
#include "windows/port.h"

#endif  /* GPERFTOOLS_CONFIG_H_ */
