/* Copyright (c) 2005-2007, Google Inc.
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
 * Author: Markus Gutschke
 */

/* This file includes Linux-specific support functions common to the
 * coredumper and the thread lister; primarily, this is a collection
 * of direct system calls, and a couple of symbols missing from
 * standard header files.
 * There are a few options that the including file can set to control
 * the behavior of this file:
 *
 * SYS_CPLUSPLUS:
 *   The entire header file will normally be wrapped in 'extern "C" { }",
 *   making it suitable for compilation as both C and C++ source. If you
 *   do not want to do this, you can set the SYS_CPLUSPLUS macro to inhibit
 *   the wrapping. N.B. doing so will suppress inclusion of all prerequisite
 *   system header files, too. It is the caller's responsibility to provide
 *   the necessary definitions.
 *
 * SYS_ERRNO:
 *   All system calls will update "errno" unless overriden by setting the
 *   SYS_ERRNO macro prior to including this file. SYS_ERRNO should be
 *   an l-value.
 *
 * SYS_INLINE:
 *   New symbols will be defined "static inline", unless overridden by
 *   the SYS_INLINE macro.
 *
 * SYS_LINUX_SYSCALL_SUPPORT_H
 *   This macro is used to avoid multiple inclusions of this header file.
 *   If you need to include this file more than once, make sure to
 *   unset SYS_LINUX_SYSCALL_SUPPORT_H before each inclusion.
 *
 * SYS_PREFIX:
 *   New system calls will have a prefix of "sys_" unless overridden by
 *   the SYS_PREFIX macro. Valid values for this macro are [0..9] which
 *   results in prefixes "sys[0..9]_". It is also possible to set this
 *   macro to -1, which avoids all prefixes.
 *
 * This file defines a few internal symbols that all start with "LSS_".
 * Do not access these symbols from outside this file. They are not part
 * of the supported API.
 */
#ifndef SYS_LINUX_SYSCALL_SUPPORT_H
#define SYS_LINUX_SYSCALL_SUPPORT_H

/* We currently only support x86-32, x86-64, ARM, and MIPS on Linux.
 * Porting to other related platforms should not be difficult.
 */
#if (defined(__i386__) || defined(__x86_64__) || defined(__ARM_ARCH_3__) || \
     defined(mips)) && defined(__linux)

#ifndef SYS_CPLUSPLUS
#ifdef __cplusplus
/* Some system header files in older versions of gcc neglect to properly
 * handle being included from C++. As it appears to be harmless to have
 * multiple nested 'extern "C"' blocks, just add another one here.
 */
extern "C" {
#endif

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/ptrace.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/types.h>
#include <syscall.h>
#include <unistd.h>
#include <linux/unistd.h>
#include <endian.h>

/* libc defines versions of stat, dirent, dirent64, and statfs64 that are
 * incompatible with the structures that the kernel API expects. If you wish
 * to use sys_fstat(), sys_stat(), sys_getdents(), sys_getdents64(),
 * sys_statfs64(), or sys_statfs() you will need to include the kernel
 * headers in your code.
 *
 *   asm/posix_types.h
 *   asm/stat.h
 *   asm/statfs.h
 *   asm/types.h
 *   linux/dirent.h
 */
struct dirent64;
struct dirent;
struct iovec;
struct msghdr;
struct pollfd;
struct rlimit;
struct sockaddr;
struct stat;
struct stat64;
struct statfs;
struct statfs64;

#endif


/* Definitions missing from the standard header files                        */
#ifndef O_DIRECTORY
#if defined(__ARM_ARCH_3__)
#define O_DIRECTORY             0040000
#else
#define O_DIRECTORY             0200000
#endif
#endif
#ifndef NT_PRXFPREG
#define NT_PRXFPREG             0x46e62b7f
#endif
#ifndef PTRACE_GETFPXREGS
#define PTRACE_GETFPXREGS       ((enum __ptrace_request)18)
#endif
#ifndef PR_GET_DUMPABLE
#define PR_GET_DUMPABLE         3
#endif
#ifndef PR_SET_DUMPABLE
#define PR_SET_DUMPABLE         4
#endif
#ifndef AT_FDCWD
#define AT_FDCWD                (-100)
#endif
#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW     0x100
#endif
#ifndef AT_REMOVEDIR
#define AT_REMOVEDIR            0x200
#endif
#ifndef MREMAP_FIXED
#define MREMAP_FIXED            2
#endif

#if defined(__i386__)
#ifndef __NR_setresuid
#define __NR_setresuid          164
#define __NR_setresgid          170
#endif
#ifndef __NR_ugetrlimit
#define __NR_ugetrlimit         191
#endif
#ifndef __NR_setresuid32
#define __NR_setresuid32        208
#define __NR_setresgid32        210
#endif
#ifndef __NR_setfsuid32
#define __NR_setfsuid32         215
#define __NR_setfsgid32         216
#endif
#ifndef __NR_getdents64
#define __NR_getdents64         220
#endif
#ifndef __NR_gettid
#define __NR_gettid             224
#endif
#ifndef __NR_readahead
#define __NR_readahead          225
#endif
#ifndef __NR_setxattr
#define __NR_setxattr           226
#endif
#ifndef __NR_lsetxattr
#define __NR_lsetxattr          227
#endif
#ifndef __NR_getxattr
#define __NR_getxattr           229
#endif
#ifndef __NR_lgetxattr
#define __NR_lgetxattr          230
#endif
#ifndef __NR_futex
#define __NR_futex              240
#endif
#ifndef __NR_sched_setaffinity
#define __NR_sched_setaffinity  241
#define __NR_sched_getaffinity  242
#endif
#ifndef __NR_set_tid_address
#define __NR_set_tid_address    258
#endif
#ifndef __NR_statfs64
#define __NR_statfs64           268
#endif
#ifndef __NR_fstatfs64
#define __NR_fstatfs64          269
#endif
#ifndef __NR_fadvise64_64
#define __NR_fadvise64_64       272
#endif
#ifndef __NR_openat
#define __NR_openat             295
#endif
#ifndef __NR_fstatat64
#define __NR_fstatat64          300
#endif
#ifndef __NR_unlinkat
#define __NR_unlinkat           301
#endif
#ifndef __NR_move_pages
#define __NR_move_pages         317
#endif
/* End of i386 definitions                                                   */
#elif defined(__ARM_ARCH_3__)
#ifndef __NR_setresuid
#define __NR_setresuid          (__NR_SYSCALL_BASE + 164)
#define __NR_setresgid          (__NR_SYSCALL_BASE + 170)
#endif
#ifndef __NR_ugetrlimit
#define __NR_ugetrlimit         (__NR_SYSCALL_BASE + 191)
#endif
#ifndef __NR_setresuid32
#define __NR_setresuid32        (__NR_SYSCALL_BASE + 208)
#define __NR_setresgid32        (__NR_SYSCALL_BASE + 210)
#endif
#ifndef __NR_setfsuid32
#define __NR_setfsuid32         (__NR_SYSCALL_BASE + 215)
#define __NR_setfsgid32         (__NR_SYSCALL_BASE + 216)
#endif
#ifndef __NR_getdents64
#define __NR_getdents64         (__NR_SYSCALL_BASE + 217)
#endif
#ifndef __NR_gettid
#define __NR_gettid             (__NR_SYSCALL_BASE + 224)
#endif
#ifndef __NR_readahead
#define __NR_readahead          (__NR_SYSCALL_BASE + 225)
#endif
#ifndef __NR_setxattr
#define __NR_setxattr           (__NR_SYSCALL_BASE + 226)
#endif
#ifndef __NR_lsetxattr
#define __NR_lsetxattr          (__NR_SYSCALL_BASE + 227)
#endif
#ifndef __NR_getxattr
#define __NR_getxattr           (__NR_SYSCALL_BASE + 229)
#endif
#ifndef __NR_lgetxattr
#define __NR_lgetxattr          (__NR_SYSCALL_BASE + 230)
#endif
#ifndef __NR_futex
#define __NR_futex              (__NR_SYSCALL_BASE + 240)
#endif
#ifndef __NR_sched_setaffinity
#define __NR_sched_setaffinity  (__NR_SYSCALL_BASE + 241)
#define __NR_sched_getaffinity  (__NR_SYSCALL_BASE + 242)
#endif
#ifndef __NR_set_tid_address
#define __NR_set_tid_address    (__NR_SYSCALL_BASE + 256)
#endif
#ifndef __NR_statfs64
#define __NR_statfs64           (__NR_SYSCALL_BASE + 266)
#endif
#ifndef __NR_fstatfs64
#define __NR_fstatfs64          (__NR_SYSCALL_BASE + 267)
#endif
#ifndef __NR_move_pages
#define __NR_move_pages         (__NR_SYSCALL_BASE + 344)
#endif
/* End of ARM 3 definitions                                                  */
#elif defined(__x86_64__)
#ifndef __NR_setresuid
#define __NR_setresuid          117
#define __NR_setresgid          119
#endif
#ifndef __NR_gettid
#define __NR_gettid             186
#endif
#ifndef __NR_readahead
#define __NR_readahead          187
#endif
#ifndef __NR_setxattr
#define __NR_setxattr           188
#endif
#ifndef __NR_lsetxattr
#define __NR_lsetxattr          189
#endif
#ifndef __NR_getxattr
#define __NR_getxattr           191
#endif
#ifndef __NR_lgetxattr
#define __NR_lgetxattr          192
#endif
#ifndef __NR_futex
#define __NR_futex              202
#endif
#ifndef __NR_sched_setaffinity
#define __NR_sched_setaffinity  203
#define __NR_sched_getaffinity  204
#endif
#ifndef __NR_getdents64
#define __NR_getdents64         217
#endif
#ifndef __NR_set_tid_address
#define __NR_set_tid_address    218
#endif
#ifndef __NR_fadvise64
#define __NR_fadvise64          221
#endif
#ifndef __NR_openat
#define __NR_openat             257
#endif
#ifndef __NR_newfstatat
#define __NR_newfstatat         262
#endif
#ifndef __NR_unlinkat
#define __NR_unlinkat           263
#endif
#ifndef __NR_move_pages
#define __NR_move_pages         279
#endif
/* End of x86-64 definitions                                                 */
#elif defined(mips)
#if _MIPS_SIM == _MIPS_SIM_ABI32
#ifndef __NR_setresuid
#define __NR_setresuid          (__NR_Linux + 185)
#define __NR_setresgid          (__NR_Linux + 190)
#endif
#ifndef __NR_getdents64
#define __NR_getdents64         (__NR_Linux + 219)
#endif
#ifndef __NR_gettid
#define __NR_gettid             (__NR_Linux + 222)
#endif
#ifndef __NR_readahead
#define __NR_readahead          (__NR_Linux + 223)
#endif
#ifndef __NR_setxattr
#define __NR_setxattr           (__NR_Linux + 224)
#endif
#ifndef __NR_lsetxattr
#define __NR_lsetxattr          (__NR_Linux + 225)
#endif
#ifndef __NR_getxattr
#define __NR_getxattr           (__NR_Linux + 227)
#endif
#ifndef __NR_lgetxattr
#define __NR_lgetxattr          (__NR_Linux + 228)
#endif
#ifndef __NR_futex
#define __NR_futex              (__NR_Linux + 238)
#endif
#ifndef __NR_sched_setaffinity
#define __NR_sched_setaffinity  (__NR_Linux + 239)
#define __NR_sched_getaffinity  (__NR_Linux + 240)
#endif
#ifndef __NR_set_tid_address
#define __NR_set_tid_address    (__NR_Linux + 252)
#endif
#ifndef __NR_statfs64
#define __NR_statfs64           (__NR_Linux + 255)
#endif
#ifndef __NR_fstatfs64
#define __NR_fstatfs64          (__NR_Linux + 256)
#endif
#ifndef __NR_openat
#define __NR_openat             (__NR_Linux + 288)
#endif
#ifndef __NR_fstatat
#define __NR_fstatat            (__NR_Linux + 293)
#endif
#ifndef __NR_unlinkat
#define __NR_unlinkat           (__NR_Linux + 294)
#endif
#ifndef __NR_move_pages
#define __NR_move_pages         (__NR_Linux + 308)
#endif
/* End of MIPS (old 32bit API) definitions */
#elif  _MIPS_SIM == _MIPS_SIM_ABI64
#ifndef __NR_setresuid
#define __NR_setresuid          (__NR_Linux + 115)
#define __NR_setresgid          (__NR_Linux + 117)
#endif
#ifndef __NR_gettid
#define __NR_gettid             (__NR_Linux + 178)
#endif
#ifndef __NR_readahead
#define __NR_readahead          (__NR_Linux + 179)
#endif
#ifndef __NR_setxattr
#define __NR_setxattr           (__NR_Linux + 180)
#endif
#ifndef __NR_lsetxattr
#define __NR_lsetxattr          (__NR_Linux + 181)
#endif
#ifndef __NR_getxattr
#define __NR_getxattr           (__NR_Linux + 183)
#endif
#ifndef __NR_lgetxattr
#define __NR_lgetxattr          (__NR_Linux + 184)
#endif
#ifndef __NR_futex
#define __NR_futex              (__NR_Linux + 194)
#endif
#ifndef __NR_sched_setaffinity
#define __NR_sched_setaffinity  (__NR_Linux + 195)
#define __NR_sched_getaffinity  (__NR_Linux + 196)
#endif
#ifndef __NR_set_tid_address
#define __NR_set_tid_address    (__NR_Linux + 212)
#endif
#ifndef __NR_openat
#define __NR_openat             (__NR_Linux + 247)
#endif
#ifndef __NR_fstatat
#define __NR_fstatat            (__NR_Linux + 252)
#endif
#ifndef __NR_unlinkat
#define __NR_unlinkat           (__NR_Linux + 253)
#endif
#ifndef __NR_move_pages
#define __NR_move_pages         (__NR_Linux + 267)
#endif
/* End of MIPS (64bit API) definitions */
#else
#ifndef __NR_setresuid
#define __NR_setresuid          (__NR_Linux + 115)
#define __NR_setresgid          (__NR_Linux + 117)
#endif
#ifndef __NR_gettid
#define __NR_gettid             (__NR_Linux + 178)
#endif
#ifndef __NR_readahead
#define __NR_readahead          (__NR_Linux + 179)
#endif
#ifndef __NR_setxattr
#define __NR_setxattr           (__NR_Linux + 180)
#endif
#ifndef __NR_lsetxattr
#define __NR_lsetxattr          (__NR_Linux + 181)
#endif
#ifndef __NR_getxattr
#define __NR_getxattr           (__NR_Linux + 183)
#endif
#ifndef __NR_lgetxattr
#define __NR_lgetxattr          (__NR_Linux + 184)
#endif
#ifndef __NR_futex
#define __NR_futex              (__NR_Linux + 194)
#endif
#ifndef __NR_sched_setaffinity
#define __NR_sched_setaffinity  (__NR_Linux + 195)
#define __NR_sched_getaffinity  (__NR_Linux + 196)
#endif
#ifndef __NR_set_tid_address
#define __NR_set_tid_address    (__NR_Linux + 213)
#endif
#ifndef __NR_statfs64
#define __NR_statfs64           (__NR_Linux + 217)
#endif
#ifndef __NR_fstatfs64
#define __NR_fstatfs64          (__NR_Linux + 218)
#endif
#ifndef __NR_openat
#define __NR_openat             (__NR_Linux + 251)
#endif
#ifndef __NR_fstatat
#define __NR_fstatat            (__NR_Linux + 256)
#endif
#ifndef __NR_unlinkat
#define __NR_unlinkat           (__NR_Linux + 257)
#endif
#ifndef __NR_move_pages
#define __NR_move_pages         (__NR_Linux + 271)
#endif
/* End of MIPS (new 32bit API) definitions */
#endif
/* End of MIPS definitions */
#endif


/* After forking, we must make sure to only call system calls.               */
#if __BOUNDED_POINTERS__
  #error "Need to port invocations of syscalls for bounded ptrs"
#else
  /* The core dumper and the thread lister get executed after threads
   * have been suspended. As a consequence, we cannot call any functions
   * that acquire locks. Unfortunately, libc wraps most system calls
   * (e.g. in order to implement pthread_atfork, and to make calls
   * cancellable), which means we cannot call these functions. Instead,
   * we have to call syscall() directly.
   */
  #undef LSS_ERRNO
  #ifdef SYS_ERRNO
    /* Allow the including file to override the location of errno. This can
     * be useful when using clone() with the CLONE_VM option.
     */
    #define LSS_ERRNO SYS_ERRNO
  #else
    #define LSS_ERRNO errno
  #endif

  #undef LSS_INLINE
  #ifdef SYS_INLINE
    #define LSS_INLINE SYS_INLINE
  #else
    #define LSS_INLINE static inline
  #endif

  /* Allow the including file to override the prefix used for all new
   * system calls. By default, it will be set to "sys_".
   */
  #undef LSS_NAME
  #ifndef SYS_PREFIX
    #define LSS_NAME(name) sys_##name
  #elif SYS_PREFIX < 0
    #define LSS_NAME(name) name
  #elif SYS_PREFIX == 0
    #define LSS_NAME(name) sys0_##name
  #elif SYS_PREFIX == 1
    #define LSS_NAME(name) sys1_##name
  #elif SYS_PREFIX == 2
    #define LSS_NAME(name) sys2_##name
  #elif SYS_PREFIX == 3
    #define LSS_NAME(name) sys3_##name
  #elif SYS_PREFIX == 4
    #define LSS_NAME(name) sys4_##name
  #elif SYS_PREFIX == 5
    #define LSS_NAME(name) sys5_##name
  #elif SYS_PREFIX == 6
    #define LSS_NAME(name) sys6_##name
  #elif SYS_PREFIX == 7
    #define LSS_NAME(name) sys7_##name
  #elif SYS_PREFIX == 8
    #define LSS_NAME(name) sys8_##name
  #elif SYS_PREFIX == 9
    #define LSS_NAME(name) sys9_##name
  #endif

  #undef  LSS_RETURN
  #ifndef mips
  /* Failing system calls return a negative result in the range of
   * -1..-4095. These are "errno" values with the sign inverted.
   */
  #define LSS_RETURN(type, res)                                               \
    do {                                                                      \
      if ((unsigned long)(res) >= (unsigned long)(-4095)) {                   \
        LSS_ERRNO = -(res);                                                   \
        res = -1;                                                             \
      }                                                                       \
      return (type) (res);                                                    \
    } while (0)
  #else
  /* On MIPS, failing system calls return -1, and set errno in a
   * separate CPU register.
   */
  #define LSS_RETURN(type, res, err)                                          \
    do {                                                                      \
      if (err) {                                                              \
        LSS_ERRNO = (res);                                                    \
        res = -1;                                                             \
      }                                                                       \
      return (type) (res);                                                    \
    } while (0)
  #endif
  #if defined(__i386__)
    /* In PIC mode (e.g. when building shared libraries), gcc for i386
     * reserves ebx. Unfortunately, most distribution ship with implementations
     * of _syscallX() which clobber ebx.
     * Also, most definitions of _syscallX() neglect to mark "memory" as being
     * clobbered. This causes problems with compilers, that do a better job
     * at optimizing across __asm__ calls.
     * So, we just have to redefine all of the _syscallX() macros.
     */
    #undef  LSS_BODY
    #define LSS_BODY(type,args...)                                            \
      long __res;                                                             \
      __asm__ __volatile__("push %%ebx\n"                                     \
                           "movl %2,%%ebx\n"                                  \
                           "int $0x80\n"                                      \
                           "pop %%ebx"                                        \
                           args                                               \
                           : "memory");                                       \
      LSS_RETURN(type,__res)
    #undef  _syscall0
    #define _syscall0(type,name)                                              \
      type LSS_NAME(name)(void) {                                             \
        long __res;                                                           \
        __asm__ volatile("int $0x80"                                          \
                         : "=a" (__res)                                       \
                         : "0" (__NR_##name)                                  \
                         : "memory");                                         \
        LSS_RETURN(type,__res);                                               \
      }
    #undef  _syscall1
    #define _syscall1(type,name,type1,arg1)                                   \
      type LSS_NAME(name)(type1 arg1) {                                       \
        LSS_BODY(type,                                                        \
             : "=a" (__res)                                                   \
             : "0" (__NR_##name), "ri" ((long)(arg1)));                       \
      }
    #undef  _syscall2
    #define _syscall2(type,name,type1,arg1,type2,arg2)                        \
      type LSS_NAME(name)(type1 arg1,type2 arg2) {                            \
        LSS_BODY(type,                                                        \
             : "=a" (__res)                                                   \
             : "0" (__NR_##name),"ri" ((long)(arg1)), "c" ((long)(arg2)));    \
      }
    #undef  _syscall3
    #define _syscall3(type,name,type1,arg1,type2,arg2,type3,arg3)             \
      type LSS_NAME(name)(type1 arg1,type2 arg2,type3 arg3) {                 \
        LSS_BODY(type,                                                        \
             : "=a" (__res)                                                   \
             : "0" (__NR_##name), "ri" ((long)(arg1)), "c" ((long)(arg2)),    \
               "d" ((long)(arg3)));                                           \
      }
    #undef  _syscall4
    #define _syscall4(type,name,type1,arg1,type2,arg2,type3,arg3,type4,arg4)  \
      type LSS_NAME(name)(type1 arg1, type2 arg2, type3 arg3, type4 arg4) {   \
        LSS_BODY(type,                                                        \
             : "=a" (__res)                                                   \
             : "0" (__NR_##name), "ri" ((long)(arg1)), "c" ((long)(arg2)),    \
               "d" ((long)(arg3)),"S" ((long)(arg4)));                        \
      }
    #undef  _syscall5
    #define _syscall5(type,name,type1,arg1,type2,arg2,type3,arg3,type4,arg4,  \
                      type5,arg5)                                             \
      type LSS_NAME(name)(type1 arg1, type2 arg2, type3 arg3, type4 arg4,     \
                          type5 arg5) {                                       \
        long __res;                                                           \
        __asm__ __volatile__("push %%ebx\n"                                   \
                             "movl %2,%%ebx\n"                                \
                             "movl %1,%%eax\n"                                \
                             "int  $0x80\n"                                   \
                             "pop  %%ebx"                                     \
                             : "=a" (__res)                                   \
                             : "i" (__NR_##name), "ri" ((long)(arg1)),        \
                               "c" ((long)(arg2)), "d" ((long)(arg3)),        \
                               "S" ((long)(arg4)), "D" ((long)(arg5))         \
                             : "memory");                                     \
        LSS_RETURN(type,__res);                                               \
      }
    #undef  _syscall6
    #define _syscall6(type,name,type1,arg1,type2,arg2,type3,arg3,type4,arg4,  \
                      type5,arg5,type6,arg6)                                  \
      type LSS_NAME(name)(type1 arg1, type2 arg2, type3 arg3, type4 arg4,     \
                          type5 arg5, type6 arg6) {                           \
        long __res;                                                           \
        struct { long __a1; long __a6; } __s = { (long)arg1, (long) arg6 };   \
        __asm__ __volatile__("push %%ebp\n"                                   \
                             "push %%ebx\n"                                   \
                             "movl 4(%2),%%ebp\n"                             \
                             "movl 0(%2), %%ebx\n"                            \
                             "movl %1,%%eax\n"                                \
                             "int  $0x80\n"                                   \
                             "pop  %%ebx\n"                                   \
                             "pop  %%ebp"                                     \
                             : "=a" (__res)                                   \
                             : "i" (__NR_##name),  "0" ((long)(&__s)),        \
                               "c" ((long)(arg2)), "d" ((long)(arg3)),        \
                               "S" ((long)(arg4)), "D" ((long)(arg5))         \
                             : "memory");                                     \
        LSS_RETURN(type,__res);                                               \
      }
    LSS_INLINE int LSS_NAME(clone)(int (*fn)(void *), void *child_stack,
                                   int flags, void *arg, int *parent_tidptr,
                                   void *newtls, int *child_tidptr) {
      long __res;
      __asm__ __volatile__(/* if (fn == NULL)
                            *   return -EINVAL;
                            */
                           "movl   %3,%%ecx\n"
                           "jecxz  1f\n"

                           /* if (child_stack == NULL)
                            *   return -EINVAL;
                            */
                           "movl   %4,%%ecx\n"
                           "jecxz  1f\n"

                           /* Set up alignment of the child stack:
                            * child_stack = (child_stack & ~0xF) - 20;
                            */
                           "andl   $-16,%%ecx\n"
                           "subl   $20,%%ecx\n"

                           /* Push "arg" and "fn" onto the stack that will be
                            * used by the child.
                            */
                           "movl   %6,%%eax\n"
                           "movl   %%eax,4(%%ecx)\n"
                           "movl   %3,%%eax\n"
                           "movl   %%eax,(%%ecx)\n"

                           /* %eax = syscall(%eax = __NR_clone,
                            *                %ebx = flags,
                            *                %ecx = child_stack,
                            *                %edx = parent_tidptr,
                            *                %esi = newtls,
                            *                %edi = child_tidptr)
                            * Also, make sure that %ebx gets preserved as it is
                            * used in PIC mode.
                            */
                           "movl   %8,%%esi\n"
                           "movl   %7,%%edx\n"
                           "movl   %5,%%eax\n"
                           "movl   %9,%%edi\n"
                           "pushl  %%ebx\n"
                           "movl   %%eax,%%ebx\n"
                           "movl   %2,%%eax\n"
                           "int    $0x80\n"

                           /* In the parent: restore %ebx
                            * In the child:  move "fn" into %ebx
                            */
                           "popl   %%ebx\n"

                           /* if (%eax != 0)
                            *   return %eax;
                            */
                           "test   %%eax,%%eax\n"
                           "jnz    1f\n"

                           /* In the child, now. Terminate frame pointer chain.
                            */
                           "movl   $0,%%ebp\n"

                           /* Call "fn". "arg" is already on the stack.
                            */
                           "call   *%%ebx\n"

                           /* Call _exit(%ebx). Unfortunately older versions
                            * of gcc restrict the number of arguments that can
                            * be passed to asm(). So, we need to hard-code the
                            * system call number.
                            */
                           "movl   %%eax,%%ebx\n"
                           "movl   $1,%%eax\n"
                           "int    $0x80\n"

                           /* Return to parent.
                            */
                         "1:\n"
                           : "=a" (__res)
                           : "0"(-EINVAL), "i"(__NR_clone),
                             "m"(fn), "m"(child_stack), "m"(flags), "m"(arg),
                             "m"(parent_tidptr), "m"(newtls), "m"(child_tidptr)
                           : "memory", "ecx", "edx", "esi", "edi");
      LSS_RETURN(int, __res);
    }

    #define __NR__fadvise64_64 __NR_fadvise64_64
    LSS_INLINE _syscall6(int, _fadvise64_64, int, fd,
                         unsigned, offset_lo, unsigned, offset_hi,
                         unsigned, len_lo, unsigned, len_hi,
                         int, advice)

    LSS_INLINE int LSS_NAME(fadvise64)(int fd, loff_t offset,
                                       loff_t len, int advice) {
      return LSS_NAME(_fadvise64_64)(fd,
                                     (unsigned)offset, (unsigned)(offset >> 32),
                                     (unsigned)len, (unsigned)(len >> 32),
                                     advice);
    }

    #define __NR__readahead32 __NR_readahead
    LSS_INLINE _syscall4(int, _readahead32, int, fd, unsigned, offset_lo,
                         unsigned, offset_hi, unsigned, len);
    LSS_INLINE int LSS_NAME(readahead)(int fd, loff_t offset, int len) {
      return LSS_NAME(_readahead32)(fd,
                                 (unsigned)offset, (unsigned)(offset >> 32),
                                 (unsigned)len);
    }

  #elif defined(__x86_64__)
    /* There are no known problems with any of the _syscallX() macros
     * currently shipping for x86_64, but we still need to be able to define
     * our own version so that we can override the location of the errno
     * location (e.g. when using the clone() system call with the CLONE_VM
     * option).
     */
    #undef  LSS_BODY
    #define LSS_BODY(type,name, ...)                                          \
          long __res;                                                         \
          __asm__ __volatile__("syscall" : "=a" (__res) : "0" (__NR_##name),  \
            ##__VA_ARGS__ : "r11", "rcx", "memory");                          \
          LSS_RETURN(type, __res)
    #undef _syscall0
    #define _syscall0(type,name)                                              \
      type LSS_NAME(name)() {                                                 \
        LSS_BODY(type, name);                                                 \
      }
    #undef _syscall1
    #define _syscall1(type,name,type1,arg1)                                   \
      type LSS_NAME(name)(type1 arg1) {                                       \
        LSS_BODY(type, name, "D" ((long)(arg1)));                             \
      }
    #undef _syscall2
    #define _syscall2(type,name,type1,arg1,type2,arg2)                        \
      type LSS_NAME(name)(type1 arg1, type2 arg2) {                           \
        LSS_BODY(type, name, "D" ((long)(arg1)), "S" ((long)(arg2)));         \
      }
    #undef _syscall3
    #define _syscall3(type,name,type1,arg1,type2,arg2,type3,arg3)             \
      type LSS_NAME(name)(type1 arg1, type2 arg2, type3 arg3) {               \
        LSS_BODY(type, name, "D" ((long)(arg1)), "S" ((long)(arg2)),          \
                             "d" ((long)(arg3)));                             \
      }
    #undef _syscall4
    #define _syscall4(type,name,type1,arg1,type2,arg2,type3,arg3,type4,arg4)  \
      type LSS_NAME(name)(type1 arg1, type2 arg2, type3 arg3, type4 arg4) {   \
          long __res;                                                         \
          __asm__ __volatile__("movq %5,%%r10; syscall" :                     \
            "=a" (__res) : "0" (__NR_##name),                                 \
            "D" ((long)(arg1)), "S" ((long)(arg2)), "d" ((long)(arg3)),       \
            "g" ((long)(arg4)) : "r10", "r11", "rcx", "memory");              \
          LSS_RETURN(type, __res);                                            \
      }
    #undef _syscall5
    #define _syscall5(type,name,type1,arg1,type2,arg2,type3,arg3,type4,arg4,  \
                      type5,arg5)                                             \
      type LSS_NAME(name)(type1 arg1, type2 arg2, type3 arg3, type4 arg4,     \
                          type5 arg5) {                                       \
          long __res;                                                         \
          __asm__ __volatile__("movq %5,%%r10; movq %6,%%r8; syscall" :       \
            "=a" (__res) : "0" (__NR_##name),                                 \
            "D" ((long)(arg1)), "S" ((long)(arg2)), "d" ((long)(arg3)),       \
            "g" ((long)(arg4)), "g" ((long)(arg5)) :                          \
            "r8", "r10", "r11", "rcx", "memory");                             \
          LSS_RETURN(type, __res);                                            \
      }
    #undef _syscall6
    #define _syscall6(type,name,type1,arg1,type2,arg2,type3,arg3,type4,arg4,  \
                      type5,arg5,type6,arg6)                                  \
      type LSS_NAME(name)(type1 arg1, type2 arg2, type3 arg3, type4 arg4,     \
                          type5 arg5, type6 arg6) {                           \
          long __res;                                                         \
          __asm__ __volatile__("movq %5,%%r10; movq %6,%%r8; movq %7,%%r9;"   \
                               "syscall" :                                    \
            "=a" (__res) : "0" (__NR_##name),                                 \
            "D" ((long)(arg1)), "S" ((long)(arg2)), "d" ((long)(arg3)),       \
            "g" ((long)(arg4)), "g" ((long)(arg5)), "g" ((long)(arg6)) :      \
            "r8", "r9", "r10", "r11", "rcx", "memory");                       \
          LSS_RETURN(type, __res);                                            \
      }
    LSS_INLINE int LSS_NAME(clone)(int (*fn)(void *), void *child_stack,
                                   int flags, void *arg, int *parent_tidptr,
                                   void *newtls, int *child_tidptr) {
      long __res;
      {
        register void *__tls  __asm__("r8")  = newtls;
        register int  *__ctid __asm__("r10") = child_tidptr;
        __asm__ __volatile__(/* if (fn == NULL)
                              *   return -EINVAL;
                              */
                             "testq  %4,%4\n"
                             "jz     1f\n"

                             /* if (child_stack == NULL)
                              *   return -EINVAL;
                              */
                             "testq  %5,%5\n"
                             "jz     1f\n"

                             /* childstack -= 2*sizeof(void *);
                              */
                             "subq   $16,%5\n"

                             /* Push "arg" and "fn" onto the stack that will be
                              * used by the child.
                              */
                             "movq   %7,8(%5)\n"
                             "movq   %4,0(%5)\n"

                             /* %rax = syscall(%rax = __NR_clone,
                              *                %rdi = flags,
                              *                %rsi = child_stack,
                              *                %rdx = parent_tidptr,
                              *                %r8  = new_tls,
                              *                %r10 = child_tidptr)
                              */
                             "movq   %2,%%rax\n"
                             "syscall\n"

                             /* if (%rax != 0)
                              *   return;
                              */
                             "testq  %%rax,%%rax\n"
                             "jnz    1f\n"

                             /* In the child. Terminate frame pointer chain.
                              */
                             "xorq   %%rbp,%%rbp\n"

                             /* Call "fn(arg)".
                              */
                             "popq   %%rax\n"
                             "popq   %%rdi\n"
                             "call   *%%rax\n"

                             /* Call _exit(%ebx).
                              */
                             "movq   %%rax,%%rdi\n"
                             "movq   %3,%%rax\n"
                             "syscall\n"

                             /* Return to parent.
                              */
                           "1:\n"
                             : "=a" (__res)
                             : "0"(-EINVAL), "i"(__NR_clone), "i"(__NR_exit),
                               "r"(fn), "S"(child_stack), "D"(flags), "r"(arg),
                               "d"(parent_tidptr), "r"(__tls), "r"(__ctid)
                             : "memory", "r11", "rcx");
      }
      LSS_RETURN(int, __res);
    }
    LSS_INLINE _syscall4(int, fadvise64, int, fd, loff_t, offset, loff_t, len,
                         int,  advice)
    LSS_INLINE _syscall2(int, statfs, const char *, path, struct statfs *, buf)
    LSS_INLINE _syscall2(int, fstatfs, int, fd, struct statfs *, buf)
  #elif defined(__ARM_ARCH_3__)
    /* Most definitions of _syscallX() neglect to mark "memory" as being
     * clobbered. This causes problems with compilers, that do a better job
     * at optimizing across __asm__ calls.
     * So, we just have to redefine all fo the _syscallX() macros.
     */
    #undef LSS_REG
    #define LSS_REG(r,a) register long __r##r __asm__("r"#r) = (long)a
    #undef  LSS_BODY
    #define LSS_BODY(type,name,args...)                                       \
          register long __res_r0 __asm__("r0");                               \
          long __res;                                                         \
          __asm__ __volatile__ (__syscall(name)                               \
                                : "=r"(__res_r0) : args : "lr", "memory");    \
          __res = __res_r0;                                                   \
          LSS_RETURN(type, __res)
    #undef _syscall0
    #define _syscall0(type, name)                                             \
      type LSS_NAME(name)() {                                                 \
        LSS_BODY(type, name);                                                 \
      }
    #undef _syscall1
    #define _syscall1(type, name, type1, arg1)                                \
      type LSS_NAME(name)(type1 arg1) {                                       \
        LSS_REG(0, arg1); LSS_BODY(type, name, "r"(__r0));                    \
      }
    #undef _syscall2
    #define _syscall2(type, name, type1, arg1, type2, arg2)                   \
      type LSS_NAME(name)(type1 arg1, type2 arg2) {                           \
        LSS_REG(0, arg1); LSS_REG(1, arg2);                                   \
        LSS_BODY(type, name, "r"(__r0), "r"(__r1));                           \
      }
    #undef _syscall3
    #define _syscall3(type, name, type1, arg1, type2, arg2, type3, arg3)      \
      type LSS_NAME(name)(type1 arg1, type2 arg2, type3 arg3) {               \
        LSS_REG(0, arg1); LSS_REG(1, arg2); LSS_REG(2, arg3);                 \
        LSS_BODY(type, name, "r"(__r0), "r"(__r1), "r"(__r2));                \
      }
    #undef _syscall4
    #define _syscall4(type,name,type1,arg1,type2,arg2,type3,arg3,type4,arg4)  \
      type LSS_NAME(name)(type1 arg1, type2 arg2, type3 arg3, type4 arg4) {   \
        LSS_REG(0, arg1); LSS_REG(1, arg2); LSS_REG(2, arg3);                 \
        LSS_REG(3, arg4);                                                     \
        LSS_BODY(type, name, "r"(__r0), "r"(__r1), "r"(__r2), "r"(__r3));     \
      }
    #undef _syscall5
    #define _syscall5(type,name,type1,arg1,type2,arg2,type3,arg3,type4,arg4,  \
                      type5,arg5)                                             \
      type LSS_NAME(name)(type1 arg1, type2 arg2, type3 arg3, type4 arg4,     \
                          type5 arg5) {                                       \
        LSS_REG(0, arg1); LSS_REG(1, arg2); LSS_REG(2, arg3);                 \
        LSS_REG(3, arg4); LSS_REG(4, arg5);                                   \
        LSS_BODY(type, name, "r"(__r0), "r"(__r1), "r"(__r2), "r"(__r3),      \
                             "r"(__r4));                                      \
      }
    #undef _syscall6
    #define _syscall6(type,name,type1,arg1,type2,arg2,type3,arg3,type4,arg4,  \
                      type5,arg5,type6,arg6)                                  \
      type LSS_NAME(name)(type1 arg1, type2 arg2, type3 arg3, type4 arg4,     \
                          type5 arg5, type6 arg6) {                           \
        LSS_REG(0, arg1); LSS_REG(1, arg2); LSS_REG(2, arg3);                 \
        LSS_REG(3, arg4); LSS_REG(4, arg5); LSS_REG(5, arg6);                 \
        LSS_BODY(type, name, "r"(__r0), "r"(__r1), "r"(__r2), "r"(__r3),      \
                             "r"(__r4), "r"(__r5));                           \
      }
    LSS_INLINE int LSS_NAME(clone)(int (*fn)(void *), void *child_stack,
                                   int flags, void *arg, int *parent_tidptr,
                                   void *newtls, int *child_tidptr) {
      long __res;
      {
        register int   __flags __asm__("r0") = flags;
        register void *__stack __asm__("r1") = child_stack;
        register void *__ptid  __asm__("r2") = parent_tidptr;
        register void *__tls   __asm__("r3") = newtls;
        register int  *__ctid  __asm__("r4") = child_tidptr;
        __asm__ __volatile__(/* if (fn == NULL || child_stack == NULL)
                              *   return -EINVAL;
                              */
                             "cmp   %2,#0\n"
                             "cmpne %3,#0\n"
                             "moveq %0,%1\n"
                             "beq   1f\n"

                             /* Push "arg" and "fn" onto the stack that will be
                              * used by the child.
                              */
                             "str   %5,[%3,#-4]!\n"
                             "str   %2,[%3,#-4]!\n"

                             /* %r0 = syscall(%r0 = flags,
                              *               %r1 = child_stack,
                              *               %r2 = parent_tidptr,
                              *               %r3 = newtls,
                              *               %r4 = child_tidptr)
                              */
                             __syscall(clone)"\n"

                             /* if (%r0 != 0)
                              *   return %r0;
                              */
                             "movs  %0,r0\n"
                             "bne   1f\n"

                             /* In the child, now. Call "fn(arg)".
                              */
                             "ldr   r0,[sp, #4]\n"
                             "mov   lr,pc\n"
                             "ldr   pc,[sp]\n"

                             /* Call _exit(%r0).
                              */
                             __syscall(exit)"\n"
                           "1:\n"
                             : "=r" (__res)
                             : "i"(-EINVAL),
                               "r"(fn), "r"(__stack), "r"(__flags), "r"(arg),
                               "r"(__ptid), "r"(__tls), "r"(__ctid)
                             : "lr", "memory");
      }
      LSS_RETURN(int, __res);
    }
  #elif defined(mips)
    #undef LSS_REG
    #define LSS_REG(r,a) register unsigned long __r##r __asm__("$"#r) =       \
                                 (unsigned long)(a)
    #undef  LSS_BODY
    #define LSS_BODY(type,name,r7,...)                                        \
          register unsigned long __v0 __asm__("$2") = __NR_##name;            \
          __asm__ __volatile__ ("syscall\n"                                   \
                                : "=&r"(__v0), r7 (__r7)                      \
                                : "0"(__v0), ##__VA_ARGS__                    \
                                : "$8", "$9", "$10", "$11", "$12",            \
                                  "$13", "$14", "$15", "$24", "memory");      \
          LSS_RETURN(type, __v0, __r7)
    #undef _syscall0
    #define _syscall0(type, name)                                             \
      type LSS_NAME(name)() {                                                 \
        register unsigned long __r7 __asm__("$7");                            \
        LSS_BODY(type, name, "=r");                                           \
      }
    #undef _syscall1
    #define _syscall1(type, name, type1, arg1)                                \
      type LSS_NAME(name)(type1 arg1) {                                       \
        register unsigned long __r7 __asm__("$7");                            \
        LSS_REG(4, arg1); LSS_BODY(type, name, "=r", "r"(__r4));              \
      }
    #undef _syscall2
    #define _syscall2(type, name, type1, arg1, type2, arg2)                   \
      type LSS_NAME(name)(type1 arg1, type2 arg2) {                           \
        register unsigned long __r7 __asm__("$7");                            \
        LSS_REG(4, arg1); LSS_REG(5, arg2);                                   \
        LSS_BODY(type, name, "=r", "r"(__r4), "r"(__r5));                     \
      }
    #undef _syscall3
    #define _syscall3(type, name, type1, arg1, type2, arg2, type3, arg3)      \
      type LSS_NAME(name)(type1 arg1, type2 arg2, type3 arg3) {               \
        register unsigned long __r7 __asm__("$7");                            \
        LSS_REG(4, arg1); LSS_REG(5, arg2); LSS_REG(6, arg3);                 \
        LSS_BODY(type, name, "=r", "r"(__r4), "r"(__r5), "r"(__r6));          \
      }
    #undef _syscall4
    #define _syscall4(type,name,type1,arg1,type2,arg2,type3,arg3,type4,arg4)  \
      type LSS_NAME(name)(type1 arg1, type2 arg2, type3 arg3, type4 arg4) {   \
        LSS_REG(4, arg1); LSS_REG(5, arg2); LSS_REG(6, arg3);                 \
        LSS_REG(7, arg4);                                                     \
        LSS_BODY(type, name, "+r", "r"(__r4), "r"(__r5), "r"(__r6));          \
      }
    #undef _syscall5
    #if _MIPS_SIM == _MIPS_SIM_ABI32
    /* The old 32bit MIPS system call API passes the fifth and sixth argument
     * on the stack, whereas the new APIs use registers "r8" and "r9".
     */
    #define _syscall5(type,name,type1,arg1,type2,arg2,type3,arg3,type4,arg4,  \
                      type5,arg5)                                             \
      type LSS_NAME(name)(type1 arg1, type2 arg2, type3 arg3, type4 arg4,     \
                          type5 arg5) {                                       \
        LSS_REG(4, arg1); LSS_REG(5, arg2); LSS_REG(6, arg3);                 \
        LSS_REG(7, arg4);                                                     \
        register unsigned long __v0 __asm__("$2");                            \
        __asm__ __volatile__ (".set noreorder\n"                              \
                              "lw    $2, %6\n"                                \
                              "subu  $29, 32\n"                               \
                              "sw    $2, 16($29)\n"                           \
                              "li    $2, %2\n"                                \
                              "syscall\n"                                     \
                              "addiu $29, 32\n"                               \
                              ".set reorder\n"                                \
                              : "=&r"(__v0), "+r" (__r7)                      \
                              : "i" (__NR_##name), "r"(__r4), "r"(__r5),      \
                                "r"(__r6), "m" ((unsigned long)arg5)          \
                              : "$8", "$9", "$10", "$11", "$12",              \
                                "$13", "$14", "$15", "$24", "memory");        \
        LSS_RETURN(type, __v0, __r7);                                         \
      }
    #else
    #define _syscall5(type,name,type1,arg1,type2,arg2,type3,arg3,type4,arg4,  \
                      type5,arg5)                                             \
      type LSS_NAME(name)(type1 arg1, type2 arg2, type3 arg3, type4 arg4,     \
                          type5 arg5) {                                       \
        LSS_REG(4, arg1); LSS_REG(5, arg2); LSS_REG(6, arg3);                 \
        LSS_REG(7, arg4); LSS_REG(8, arg5);                                   \
        LSS_BODY(type, name, "+r", "r"(__r4), "r"(__r5), "r"(__r6),           \
                 "r"(__r8));                                                  \
      }
    #endif
    #undef _syscall6
    #if _MIPS_SIM == _MIPS_SIM_ABI32
    /* The old 32bit MIPS system call API passes the fifth and sixth argument
     * on the stack, whereas the new APIs use registers "r8" and "r9".
     */
    #define _syscall6(type,name,type1,arg1,type2,arg2,type3,arg3,type4,arg4,  \
                      type5,arg5,type6,arg6)                                  \
      type LSS_NAME(name)(type1 arg1, type2 arg2, type3 arg3, type4 arg4,     \
                          type5 arg5, type6 arg6) {                           \
        LSS_REG(4, arg1); LSS_REG(5, arg2); LSS_REG(6, arg3);                 \
        LSS_REG(7, arg4);                                                     \
        register unsigned long __v0 __asm__("$2");                            \
        __asm__ __volatile__ (".set noreorder\n"                              \
                              "lw    $2, %6\n"                                \
                              "lw    $8, %7\n"                                \
                              "subu  $29, 32\n"                               \
                              "sw    $2, 16($29)\n"                           \
                              "sw    $8, 20($29)\n"                           \
                              "li    $2, %2\n"                                \
                              "syscall\n"                                     \
                              "addiu $29, 32\n"                               \
                              ".set reorder\n"                                \
                              : "=&r"(__v0), "+r" (__r7)                      \
                              : "i" (__NR_##name), "r"(__r4), "r"(__r5),      \
                                "r"(__r6), "m" ((unsigned long)arg5),         \
                                "m" ((unsigned long)arg6)                     \
                              : "$8", "$9", "$10", "$11", "$12",              \
                                "$13", "$14", "$15", "$24", "memory");        \
        LSS_RETURN(type, __v0, __r7);                                         \
      }
    #else
    #define _syscall5(type,name,type1,arg1,type2,arg2,type3,arg3,type4,arg4,  \
                      type5,arg5,type6,arg6)                                  \
      type LSS_NAME(name)(type1 arg1, type2 arg2, type3 arg3, type4 arg4,     \
                          type5 arg5,type6 arg6) {                            \
        LSS_REG(4, arg1); LSS_REG(5, arg2); LSS_REG(6, arg3);                 \
        LSS_REG(7, arg4); LSS_REG(8, arg5); LSS_REG(9, arg6);                 \
        LSS_BODY(type, name, "+r", "r"(__r4), "r"(__r5), "r"(__r6),           \
                 "r"(__r8), "r"(__r9));                                       \
      }
    #endif
    LSS_INLINE int LSS_NAME(clone)(int (*fn)(void *), void *child_stack,
                                   int flags, void *arg, int *parent_tidptr,
                                   void *newtls, int *child_tidptr) {
      register unsigned long __v0 __asm__("$2");
      register unsigned long __r7 __asm__("$7") = (unsigned long)newtls;
      {
        register int   __flags __asm__("$4") = flags;
        register void *__stack __asm__("$5") = child_stack;
        register void *__ptid  __asm__("$6") = parent_tidptr;
        register int  *__ctid  __asm__("$8") = child_tidptr;
        __asm__ __volatile__(
          #if _MIPS_SIM == _MIPS_SIM_ABI32 && _MIPS_SZPTR == 32
                             "subu  $29,24\n"
          #elif _MIPS_SIM == _MIPS_SIM_NABI32
                             "sub   $29,16\n"
          #else
                             "dsubu $29,16\n"
          #endif

                             /* if (fn == NULL || child_stack == NULL)
                              *   return -EINVAL;
                              */
                             "li    %0,%2\n"
                             "beqz  %5,1f\n"
                             "beqz  %6,1f\n"

                             /* Push "arg" and "fn" onto the stack that will be
                              * used by the child.
                              */
          #if _MIPS_SIM == _MIPS_SIM_ABI32 && _MIPS_SZPTR == 32
                             "subu  %6,32\n"
                             "sw    %5,0(%6)\n"
                             "sw    %8,4(%6)\n"
          #elif _MIPS_SIM == _MIPS_SIM_NABI32
                             "sub   %6,32\n"
                             "sw    %5,0(%6)\n"
                             "sw    %8,8(%6)\n"
          #else
                             "dsubu %6,32\n"
                             "sd    %5,0(%6)\n"
                             "sd    %8,8(%6)\n"
          #endif

                             /* $7 = syscall($4 = flags,
                              *              $5 = child_stack,
                              *              $6 = parent_tidptr,
                              *              $7 = newtls,
                              *              $8 = child_tidptr)
                              */
                             "li    $2,%3\n"
                             "syscall\n"

                             /* if ($7 != 0)
                              *   return $2;
                              */
                             "bnez  $7,1f\n"
                             "bnez  $2,1f\n"

                             /* In the child, now. Call "fn(arg)".
                              */
          #if _MIPS_SIM == _MIPS_SIM_ABI32 && _MIPS_SZPTR == 32
                            "lw    $25,0($29)\n"
                            "lw    $4,4($29)\n"
          #elif _MIPS_SIM == _MIPS_SIM_NABI32
                            "lw    $25,0($29)\n"
                            "lw    $4,8($29)\n"
          #else
                            "ld    $25,0($29)\n"
                            "ld    $4,8($29)\n"
          #endif
                            "jalr  $25\n"

                             /* Call _exit($2)
                              */
                            "move  $4,$2\n"
                            "li    $2,%4\n"
                            "syscall\n"

                           "1:\n"
          #if _MIPS_SIM == _MIPS_SIM_ABI32 && _MIPS_SZPTR == 32
                             "addu  $29, 24\n"
          #elif _MIPS_SIM == _MIPS_SIM_NABI32
                             "add   $29, 16\n"
          #else
                             "daddu $29,16\n"
          #endif
                             : "=&r" (__v0), "=r" (__r7)
                             : "i"(-EINVAL), "i"(__NR_clone), "i"(__NR_exit),
                               "r"(fn), "r"(__stack), "r"(__flags), "r"(arg),
                               "r"(__ptid), "r"(__r7), "r"(__ctid)
                             : "$9", "$10", "$11", "$12", "$13", "$14", "$15",
                               "$24", "memory");
      }
      LSS_RETURN(int, __v0, __r7);
    }
  #endif
  #define __NR__exit   __NR_exit
  #define __NR__gettid __NR_gettid
  #define __NR__mremap __NR_mremap
  LSS_INLINE _syscall1(int,     chdir,           const char *,p)
  LSS_INLINE _syscall1(int,     close,           int,         f)
  LSS_INLINE _syscall1(int,     dup,             int,         f)
  LSS_INLINE _syscall2(int,     dup2,            int,         s,
                       int,            d)
  LSS_INLINE _syscall3(int,     execve,          const char*, f,
                       const char*const*,a,const char*const*, e)
  LSS_INLINE _syscall1(int,     _exit,           int,         e)
  LSS_INLINE _syscall3(int,     fcntl,           int,         f,
                       int,            c, long,   a)
  LSS_INLINE _syscall0(pid_t,   fork)
  LSS_INLINE _syscall2(int,     fstat,           int,         f,
                       struct stat*,   b)
  LSS_INLINE _syscall4(int,     futex,           int*,        a,
                       int,            o, int,    v, struct timespec *, t)
  LSS_INLINE _syscall3(int,     getdents,        int,         f,
                       struct dirent*, d, int,    c)
  LSS_INLINE _syscall3(int,     getdents64,      int,         f,
                       struct dirent64*, d, int,    c)
  LSS_INLINE _syscall0(gid_t,   getegid)
  LSS_INLINE _syscall0(uid_t,   geteuid)
  LSS_INLINE _syscall0(pid_t,   getpgrp)
  LSS_INLINE _syscall0(pid_t,   getpid)
  LSS_INLINE _syscall0(pid_t,   getppid)
  LSS_INLINE _syscall2(int,     getpriority,     int,         a,
                       int,            b)
  LSS_INLINE _syscall2(int,     getrlimit,       int,         r,
                       struct rlimit*, l)
  LSS_INLINE _syscall1(pid_t,   getsid,          pid_t,       p)
  LSS_INLINE _syscall0(pid_t,   _gettid)
  LSS_INLINE _syscall5(int,     setxattr,        const char *,p,
                       const char *,   n,        const void *,v,
                       size_t,         s,        int,         f)
  LSS_INLINE _syscall5(int,     lsetxattr,       const char *,p,
                       const char *,   n,        const void *,v,
                       size_t,         s,        int,         f)
  LSS_INLINE _syscall4(ssize_t, getxattr,        const char *,p,
                       const char *,   n,        void *,      v, size_t, s)
  LSS_INLINE _syscall4(ssize_t, lgetxattr,       const char *,p,
                       const char *,   n,        void *,      v, size_t, s)
  LSS_INLINE _syscall2(int,     kill,            pid_t,       p,
                       int,            s)
  LSS_INLINE _syscall3(off_t,   lseek,           int,         f,
                       off_t,          o, int,    w)
  LSS_INLINE _syscall2(int,     munmap,          void*,       s,
                       size_t,         l)
  LSS_INLINE _syscall6(long,    move_pages,      pid_t,       p,
                       unsigned long,  n, void **,g, int *,   d,
                       int *,          s, int,    f)
  LSS_INLINE _syscall5(void*,   _mremap,         void*,       o,
                       size_t,         os,       size_t,      ns,
                       unsigned long,  f, void *, a)
  LSS_INLINE _syscall3(int,     open,            const char*, p,
                       int,            f, int,    m)
  LSS_INLINE _syscall3(int,     poll,            struct pollfd*, u,
                       unsigned int,   n, int,    t)
  LSS_INLINE _syscall2(int,     prctl,           int,         o,
                       long,           a)
  LSS_INLINE _syscall4(long,    ptrace,          int,         r,
                       pid_t,          p, void *, a, void *, d)
  LSS_INLINE _syscall3(ssize_t, read,            int,         f,
                       void *,         b, size_t, c)
  LSS_INLINE _syscall3(int,     readlink,        const char*, p,
                       char*,          b, size_t, s)
  LSS_INLINE _syscall3(int,     sched_getaffinity,pid_t,      p,
                       unsigned int,   l, unsigned long *, m)
  LSS_INLINE _syscall3(int,     sched_setaffinity,pid_t,      p,
                       unsigned int,   l, unsigned long *, m)
  LSS_INLINE _syscall0(int,     sched_yield)
  LSS_INLINE _syscall1(long,    set_tid_address, int *,       t)
  LSS_INLINE _syscall1(int,     setfsgid,        gid_t,       g)
  LSS_INLINE _syscall1(int,     setfsuid,        uid_t,       u)
  LSS_INLINE _syscall2(int,     setpgid,         pid_t,       p,
                       pid_t,          g)
  LSS_INLINE _syscall3(int,     setpriority,     int,         a,
                       int,            b, int,    p)
  LSS_INLINE _syscall3(int,     setresgid,       gid_t,       r,
                       gid_t,          e, gid_t,  s)
  LSS_INLINE _syscall3(int,     setresuid,       uid_t,       r,
                       uid_t,          e, uid_t,  s)
  LSS_INLINE _syscall2(int,     setrlimit,       int,         r,
                       const struct rlimit*, l)
  LSS_INLINE _syscall0(pid_t,    setsid)
  LSS_INLINE _syscall2(int,     sigaltstack,     const stack_t*, s,
                       const stack_t*, o)
  LSS_INLINE _syscall2(int,     stat,            const char*, f,
                       struct stat*,   b)
  LSS_INLINE _syscall3(ssize_t, write,            int,        f,
                       const void *,   b, size_t, c)
  LSS_INLINE _syscall3(ssize_t, writev,           int,        f,
                       const struct iovec *, v, size_t, c)
  #if defined(__x86_64__) ||                                                  \
     (defined(mips) && _MIPS_SIM != _MIPS_SIM_ABI32)
    LSS_INLINE _syscall3(int, recvmsg,            int,   s,
                         struct msghdr*,          m, int, f)
    LSS_INLINE _syscall3(int, sendmsg,            int,   s,
                         const struct msghdr*,    m, int, f)
    LSS_INLINE _syscall6(int, sendto,             int,   s,
                         const void*,             m, size_t, l,
                         int,                     f,
                         const struct sockaddr*,  a, int, t)
    LSS_INLINE _syscall2(int, shutdown,           int,   s,
                         int,                     h)
    LSS_INLINE _syscall3(int, socket,             int,   d,
                         int,                     t, int,       p)
    LSS_INLINE _syscall4(int, socketpair,         int,   d,
                         int,                     t, int,       p, int*, s)
  #endif
  #if defined(__x86_64__)
    LSS_INLINE _syscall6(void*, mmap,              void*, s,
                         size_t,                   l, int,               p,
                         int,                      f, int,               d,
                         __off64_t,                o)
    LSS_INLINE _syscall4(int, newfstatat,         int,   d,
                         const char *,            p,
                         struct stat *,           b, int, f)
    LSS_INLINE _syscall4(int, rt_sigaction,       int,   s,
                         const struct sigaction*, a,
                         struct sigaction*,       o, int,      c)
    LSS_INLINE _syscall2(int, rt_sigpending,      sigset_t*, s,
                         int,                     c)
    LSS_INLINE _syscall4(int, rt_sigprocmask,     int,   h,
                         const sigset_t*,         s, sigset_t*, o, int,  c);

    LSS_INLINE int LSS_NAME(setfsgid32)(gid_t gid) {
      return LSS_NAME(setfsgid)(gid);
    }

    LSS_INLINE int LSS_NAME(setfsuid32)(uid_t uid) {
      return LSS_NAME(setfsuid)(uid);
    }

    LSS_INLINE int LSS_NAME(setresgid32)(gid_t rgid, gid_t egid, gid_t sgid) {
      return LSS_NAME(setresgid)(rgid, egid, sgid);
    }

    LSS_INLINE int LSS_NAME(setresuid32)(uid_t ruid, uid_t euid, uid_t suid) {
      return LSS_NAME(setresuid)(ruid, euid, suid);
    }

    LSS_INLINE int LSS_NAME(sigaction)(int signum,
                                       const struct sigaction *act,
                                       struct sigaction *oldact) {
      return LSS_NAME(rt_sigaction)(signum, act, oldact, (_NSIG+6)/8);
    }

    LSS_INLINE int LSS_NAME(sigpending)(sigset_t *set) {
      return LSS_NAME(rt_sigpending)(set, (_NSIG+6)/8);
    }

    LSS_INLINE int LSS_NAME(sigprocmask)(int how, const sigset_t *set,
                                         sigset_t *oldset) {
      return LSS_NAME(rt_sigprocmask)(how, set, oldset, (_NSIG+6)/8);
    }
  #endif
  #if defined(__x86_64__) || defined(__ARM_ARCH_3__) ||                       \
     (defined(mips) && _MIPS_SIM != _MIPS_SIM_ABI32)
    LSS_INLINE _syscall4(pid_t, wait4,            pid_t, p,
                         int*,                    s, int,       o,
                         struct rusage*,          r)

    LSS_INLINE pid_t LSS_NAME(waitpid)(pid_t pid, int *status, int options){
      return LSS_NAME(wait4)(pid, status, options, 0);
    }
  #endif
  #if defined(__i386__) || defined(__x86_64__)
    LSS_INLINE _syscall4(int, openat, int, d, const char *, p, int, f, int, m)
    LSS_INLINE _syscall3(int, unlinkat, int, d, const char *, p, int, f)
  #endif
  #if defined(__i386__) || defined(__ARM_ARCH_3__)
    #define __NR__setfsgid32  __NR_setfsgid32
    #define __NR__setfsuid32  __NR_setfsuid32
    #define __NR__setresgid32 __NR_setresgid32
    #define __NR__setresuid32 __NR_setresuid32
    LSS_INLINE _syscall2(int,   ugetrlimit,        int, r,struct rlimit*,l)
    LSS_INLINE _syscall1(int,     _setfsgid32,      gid_t,       f)
    LSS_INLINE _syscall1(int,     _setfsuid32,      uid_t,       f)
    LSS_INLINE _syscall3(int,     _setresgid32,    gid_t,       r,
                         gid_t,          e, gid_t,  s)
    LSS_INLINE _syscall3(int,     _setresuid32,    uid_t,       r,
                         uid_t,          e, uid_t,  s)

    LSS_INLINE int LSS_NAME(setfsgid32)(gid_t gid) {
      int rc;
      if ((rc = LSS_NAME(_setfsgid32)(gid)) < 0 &&
          LSS_ERRNO == ENOSYS) {
        if ((unsigned int)gid & ~0xFFFFu) {
          rc = EINVAL;
        } else {
          rc = LSS_NAME(setfsgid)(gid);
        }
      }
      return rc;
    }

    LSS_INLINE int LSS_NAME(setfsuid32)(uid_t uid) {
      int rc;
      if ((rc = LSS_NAME(_setfsuid32)(uid)) < 0 &&
          LSS_ERRNO == ENOSYS) {
        if ((unsigned int)uid & ~0xFFFFu) {
          rc = EINVAL;
        } else {
          rc = LSS_NAME(setfsuid)(uid);
        }
      }
      return rc;
    }

    LSS_INLINE int LSS_NAME(setresgid32)(gid_t rgid, gid_t egid, gid_t sgid) {
      int rc;
      if ((rc = LSS_NAME(_setresgid32)(rgid, egid, sgid)) < 0 &&
          LSS_ERRNO == ENOSYS) {
        if ((unsigned int)rgid & ~0xFFFFu ||
            (unsigned int)egid & ~0xFFFFu ||
            (unsigned int)sgid & ~0xFFFFu) {
          rc = EINVAL;
        } else {
          rc = LSS_NAME(setresgid)(rgid, egid, sgid);
        }
      }
      return rc;
    }

    LSS_INLINE int LSS_NAME(setresuid32)(uid_t ruid, uid_t euid, uid_t suid) {
      int rc;
      if ((rc = LSS_NAME(_setresuid32)(ruid, euid, suid)) < 0 &&
          LSS_ERRNO == ENOSYS) {
        if ((unsigned int)ruid & ~0xFFFFu ||
            (unsigned int)euid & ~0xFFFFu ||
            (unsigned int)suid & ~0xFFFFu) {
          rc = EINVAL;
        } else {
          rc = LSS_NAME(setresuid)(ruid, euid, suid);
        }
      }
      return rc;
    }
  #endif
  #if defined(__i386__) || defined(__ARM_ARCH_3__) ||                         \
     (defined(mips) && _MIPS_SIM == _MIPS_SIM_ABI32)
    #define __NR__socketcall  __NR_socketcall
    LSS_INLINE _syscall5(int, _llseek,     uint, fd, ulong, hi, ulong, lo,
                         loff_t *, res, uint, wh)
    LSS_INLINE _syscall1(void*, mmap,              void*, a)
    LSS_INLINE _syscall6(void*, mmap2,             void*, s,
                         size_t,                   l, int,               p,
                         int,                      f, int,               d,
                         __off64_t,                o)
    LSS_INLINE _syscall3(int,     sigaction,       int,   s,
                         const struct sigaction*,  a, struct sigaction*, o)
    LSS_INLINE _syscall1(int,     sigpending,      sigset_t*, s)
    LSS_INLINE _syscall3(int,     sigprocmask,     int,   h,
                         const sigset_t*,          s, sigset_t*,         o)
    LSS_INLINE _syscall2(int,      _socketcall,    int,   c,
                         va_list,                  a)

    LSS_INLINE int LSS_NAME(socketcall)(int op, ...) {
      int rc;
      va_list ap;
      va_start(ap, op);
      rc = LSS_NAME(_socketcall)(op, ap);
      va_end(ap);
      return rc;
    }

    LSS_INLINE ssize_t LSS_NAME(recvmsg)(int s,struct msghdr*msg,int flags){
      return (ssize_t)LSS_NAME(socketcall)(17, s, msg, flags);
    }

    LSS_INLINE ssize_t LSS_NAME(sendmsg)(int s, const struct msghdr *msg,
                                         int flags) {
      return (ssize_t)LSS_NAME(socketcall)(16, s, msg, flags);
    }

    LSS_INLINE ssize_t LSS_NAME(sendto)(int s, const void *buf, size_t len,
                                        int flags, const struct sockaddr*to,
                                        unsigned int tolen) {
      return (ssize_t)LSS_NAME(socketcall)(11, s, buf, len, flags, to, tolen);
    }

    LSS_INLINE int LSS_NAME(shutdown)(int s, int how) {
      return LSS_NAME(socketcall)(13, s, how);
    }

    LSS_INLINE int LSS_NAME(socket)(int domain, int type, int protocol) {
      return LSS_NAME(socketcall)(1, domain, type, protocol);
    }

    LSS_INLINE int LSS_NAME(socketpair)(int d, int type, int protocol,
                                        int sv[2]) {
      return LSS_NAME(socketcall)(8, d, type, protocol, sv);
    }
  #endif
  #if defined(__i386__)
    LSS_INLINE _syscall4(int,   fstatat64,        int,   d,
                         const char *,      p,
                         struct stat64 *,   b,    int,   f)
  #endif
  #if defined(__i386__) ||                                                    \
     (defined(mips) && _MIPS_SIM == _MIPS_SIM_ABI32)
    LSS_INLINE _syscall3(pid_t, waitpid,          pid_t, p,
                         int*,              s,    int,   o)
  #endif
  #if defined(mips)
    /* sys_pipe() on MIPS has non-standard calling conventions, as it returns
     * both file handles through CPU registers.
     */
    LSS_INLINE int LSS_NAME(pipe)(int *p) {
      register unsigned long __v0 __asm__("$2") = __NR_pipe;
      register unsigned long __v1 __asm__("$3");
      register unsigned long __r7 __asm__("$7");
      __asm__ __volatile__ ("syscall\n"
                            : "=&r"(__v0), "=&r"(__v1), "+r" (__r7)
                            : "0"(__v0)
                            : "$8", "$9", "$10", "$11", "$12",
                              "$13", "$14", "$15", "$24", "memory");
      if (__r7) {
        LSS_ERRNO = __v0;
        return -1;
      } else {
        p[0] = __v0;
        p[1] = __v1;
        return 0;
      }
    }
  #else
    LSS_INLINE _syscall1(int,     pipe,           int *, p)
  #endif
  #if defined(__i386__) || defined(__ARM_ARCH_3__) ||                         \
     (defined(mips) && _MIPS_SIM != _MIPS_SIM_ABI64)
    LSS_INLINE _syscall2(int, statfs64,     const char*, p,
                         struct statfs64 *, b)
    LSS_INLINE _syscall2(int, fstatfs64,          int,   f,
                         struct statfs64 *, b)
  #endif

  LSS_INLINE int LSS_NAME(execv)(const char *path, const char * const argv[]) {
    extern char **environ;
    return LSS_NAME(execve)(path, argv, (const char * const *)environ);
  }

  LSS_INLINE pid_t LSS_NAME(gettid)() {
    pid_t tid = LSS_NAME(_gettid)();
    if (tid != -1) {
      return tid;
    }
    return LSS_NAME(getpid)();
  }

  LSS_INLINE void *LSS_NAME(mremap)(void *old_address, size_t old_size,
                                    size_t new_size, int flags, ...) {
    va_list ap;
    void *new_address, *rc;
    va_start(ap, flags);
    new_address = va_arg(ap, void *);
    rc = LSS_NAME(_mremap)(old_address, old_size, new_size,
                           flags, new_address);
    va_end(ap);
    return rc;
  }

  LSS_INLINE int LSS_NAME(ptrace_detach)(pid_t pid) {
    /* PTRACE_DETACH can sometimes forget to wake up the tracee and it
     * then sends job control signals to the real parent, rather than to
     * the tracer. We reduce the risk of this happening by starting a
     * whole new time slice, and then quickly sending a SIGCONT signal
     * right after detaching from the tracee.
     */
    int rc, err;
    LSS_NAME(sched_yield)();
    rc = LSS_NAME(ptrace)(PTRACE_DETACH, pid, (void *)0, (void *)0);
    err = LSS_ERRNO;
    LSS_NAME(kill)(pid, SIGCONT);
    LSS_ERRNO = err;
    return rc;
  }

  LSS_INLINE int LSS_NAME(raise)(int sig) {
    return LSS_NAME(kill)(LSS_NAME(getpid)(), sig);
  }

  LSS_INLINE int LSS_NAME(setpgrp)() {
    return LSS_NAME(setpgid)(0, 0);
  }

  LSS_INLINE int LSS_NAME(sysconf)(int name) {
    extern int __getpagesize(void);
    switch (name) {
      case _SC_OPEN_MAX: {
        unsigned long limit[2];
        return LSS_NAME(getrlimit)(RLIMIT_NOFILE, (struct rlimit *)limit)<0
               ? 8192 : limit[0];
      }
      case _SC_PAGESIZE:
        return __getpagesize();
      default:
        errno = ENOSYS;
        return -1;
    }
  }

  #if defined(__x86_64__) ||                                                  \
     (defined(mips) && _MIPS_SIM == _MIPS_SIM_ABI64)
    LSS_INLINE _syscall3(int, readahead, int, fd, loff_t, offset,
                         unsigned, len)
  #elif defined(mips) || defined(__ARM_ARCH_3__)
    LSS_INLINE _syscall4(int, _readahead32, int, fd, unsigned, offset_lo,
                         unsigned, offset_hi, unsigned, len);
    LSS_INLINE int LSS_NAME(readahead)(int fd, loff_t offset, int len) {
      return LSS_NAME(_readahead32)(fd,
        #if (defined(__BYTE_ORDER) && (__BYTE_ORDER == __LITTLE_ENDIAN)) ||   \
            (defined(__LITTLE_ENDIAN__))
          (unsigned)offset, (unsigned)(offset >> 32),
        #elif (defined(__BYTE_ORDER) && (__BYTE_ORDER == __BIG_ENDIAN)) ||    \
              (defined(__BIG_ENDIAN__))
          (unsigned)(offset >> 32), (unsigned)offset,
        #else
        #error "unknown endinness, don't know how to pass syscall arguments"
        #endif
        (unsigned)len);
    }
  #endif

#endif

#if defined(__cplusplus) && !defined(SYS_CPLUSPLUS)
}
#endif

#endif
#endif
