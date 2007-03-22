/* Copyright (c) 2005, Google Inc.
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
 */
#ifndef _LINUX_CORE_SUPPORT_H
#define _LINUX_CORE_SUPPORT_H

/* We currently only support x86-32, x86-64, and ARM on Linux. Porting to
 * other related platforms should not be difficult.
 */
#if (defined(__i386__) || defined(__x86_64__) || defined(__ARM_ARCH_3__)) && \
    defined(__linux)

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

/* Definitions missing from the standard header files                        */
#ifndef O_DIRECTORY
#if defined(__ARM_ARCH_3__)
#define O_DIRECTORY 0040000
#else
#define O_DIRECTORY 0200000
#endif
#endif
#ifndef NT_PRFPXREG
#define NT_PRFPXREG       20
#endif
#ifndef PTRACE_GETFPXREGS
#define PTRACE_GETFPXREGS ((enum __ptrace_request)18)
#endif
#ifndef PR_GET_DUMPABLE
#define PR_GET_DUMPABLE   3
#endif
#ifndef PR_SET_DUMPABLE
#define PR_SET_DUMPABLE   4
#endif

#if defined(__i386__)
#ifndef __NR_getdents64
#define __NR_getdents64   220
#endif
#ifndef __NR_gettid
#define __NR_gettid       224
#endif
#ifndef __NR_futex
#define __NR_futex        240
#endif
/* End of i386 definitions                                                   */
#elif defined(__ARM_ARCH_3__)
#ifndef __NR_getdents64
#define __NR_getdents64   217
#endif
#ifndef __NR_gettid
#define __NR_gettid       224
#endif
#ifndef __NR_futex
#define __NR_futex        240
#endif
/* End of ARM 3 definitions                                                  */
#elif defined(__x86_64__)
#ifndef __NR_getdents64
#define __NR_getdents64   217
#endif
#ifndef __NR_gettid
#define __NR_gettid       186
#endif
#ifndef __NR_futex
#define __NR_futex        202
#endif
/* End of x86-64 definitions                                                 */
#endif

/* libc defines versions of stat, dirent, and dirent64 that are incompatible
 * with the structures that the kernel API expects. If you wish to use
 * sys_fstat(), sys_stat(), sys_getdents(), or sys_getdents64(), you will
 * need to include the kernel headers in your code.
 *
 *   asm/posix_types.h
 *   asm/stat.h
 *   asm/types.h
 *   linux/dirent.h
 */
struct stat;
struct dirent;
struct dirent64;

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
  #define RETURN(type, res)                                                   \
    do {                                                                      \
      if ((unsigned long)(res) >= (unsigned long)(-4095)) {                   \
        errno = -(res);                                                       \
        res = -1;                                                             \
      }                                                                       \
      return (type) (res);                                                    \
    } while (0)
  #if defined(__i386__)
    /* In PIC mode (e.g. when building shared libraries), gcc for i386
     * reserves ebx. Unfortunately, most distribution ship with implementations
     * of _syscallX() which clobber ebx.
     * Also, most definitions of _syscallX() neglect to mark "memory" as being
     * clobbered. This causes problems with compilers, that do a better job
     * at optimizing across __asm__ calls.
     * So, we just have to redefine all of the _syscallX() macros.
     */
    #define BODY(type,args...)                                                \
      long __res;                                                             \
      __asm__ __volatile__("push %%ebx\n"                                     \
                           "movl %2,%%ebx\n"                                  \
                           "int $0x80\n"                                      \
                           "pop %%ebx"                                        \
                           args                                               \
                           : "memory");                                       \
      RETURN(type,__res)
    #undef  _syscall0
    #define _syscall0(type,name)                                              \
      type name(void) {                                                       \
      long __res;                                                             \
      __asm__ volatile("int $0x80"                                            \
                       : "=a" (__res)                                         \
                       : "0" (__NR_##name)                                    \
                       : "memory");                                           \
      RETURN(type,__res);                                                     \
      }
    #undef  _syscall1
    #define _syscall1(type,name,type1,arg1)                                   \
    type name(type1 arg1) {                                                   \
      BODY(type,                                                              \
           : "=a" (__res)                                                     \
           : "0" (__NR_##name), "ri" ((long)(arg1)));                         \
      }
    #undef  _syscall2
    #define _syscall2(type,name,type1,arg1,type2,arg2)                        \
    type name(type1 arg1,type2 arg2) {                                        \
      BODY(type,                                                              \
           : "=a" (__res)                                                     \
           : "0" (__NR_##name),"ri" ((long)(arg1)), "c" ((long)(arg2)));      \
      }
    #undef  _syscall3
    #define _syscall3(type,name,type1,arg1,type2,arg2,type3,arg3)             \
    type name(type1 arg1,type2 arg2,type3 arg3) {                             \
      BODY(type,                                                              \
           : "=a" (__res)                                                     \
           : "0" (__NR_##name), "ri" ((long)(arg1)), "c" ((long)(arg2)),      \
             "d" ((long)(arg3)));                                             \
    }
    #undef  _syscall4
    #define _syscall4(type,name,type1,arg1,type2,arg2,type3,arg3,type4,arg4)  \
    type name (type1 arg1, type2 arg2, type3 arg3, type4 arg4) {              \
      BODY(type,                                                              \
           : "=a" (__res)                                                     \
           : "0" (__NR_##name), "ri" ((long)(arg1)), "c" ((long)(arg2)),      \
             "d" ((long)(arg3)),"S" ((long)(arg4)));                          \
    }
    #undef  _syscall5
    #define _syscall5(type,name,type1,arg1,type2,arg2,type3,arg3,type4,arg4,  \
                      type5,arg5)                                             \
    type name (type1 arg1, type2 arg2, type3 arg3, type4 arg4, type5 arg5) {  \
      long __res;                                                             \
      __asm__ __volatile__("push %%ebx\n"                                     \
                           "movl %2,%%ebx\n"                                  \
                           "movl %1,%%eax\n"                                  \
                           "int  $0x80\n"                                     \
                           "pop  %%ebx"                                       \
                           : "=a" (__res)                                     \
                           : "i" (__NR_##name), "ri" ((long)(arg1)),          \
                             "c" ((long)(arg2)), "d" ((long)(arg3)),          \
                             "S" ((long)(arg4)), "D" ((long)(arg5))           \
                           : "memory");                                       \
      RETURN(type,__res);                                                     \
    }
    #undef  _syscall6
    #define _syscall6(type,name,type1,arg1,type2,arg2,type3,arg3,type4,arg4,  \
                      type5,arg5,type6,arg6)                                  \
    type name(type1 arg1, type2 arg2, type3 arg3, type4 arg4,                 \
              type5 arg5, type6 arg6) {                                       \
      long __res;                                                             \
      struct { long __a1; long __a6; } __s = { (long)arg1, (long) arg6 };     \
      __asm__ __volatile__("push %%ebp\n"                                     \
                           "push %%ebx\n"                                     \
                           "movl 4(%2),%%ebp\n"                               \
                           "movl 0(%2), %%ebx\n"                              \
                           "movl %1,%%eax\n"                                  \
                           "int  $0x80\n"                                     \
                           "pop  %%ebx\n"                                     \
                           "pop  %%ebp"                                       \
                           : "=a" (__res)                                     \
                           : "i" (__NR_##name),  "0" ((long)(&__s)),          \
                             "c" ((long)(arg2)), "d" ((long)(arg3)),          \
                             "S" ((long)(arg4)), "D" ((long)(arg5))           \
                           : "memory");                                       \
      RETURN(type,__res);                                                     \
    }
  #elif defined(__ARM_ARCH_3__)
    /* Most definitions of _syscallX() neglect to mark "memory" as being
     * clobbered. This causes problems with compilers, that do a better job
     * at optimizing across __asm__ calls.
     * So, we just have to redefine all fo the _syscallX() macros.
     */
    #define REG(r,a) register long __r##r __asm__("r"#r) = (long)a
    #define BODY(type,name,args...)                                           \
          register long __res_r0 __asm__("r0");                               \
          long __res;                                                         \
          __asm__ __volatile__ (__syscall(name)                               \
                                : "=r"(__res_r0) : args : "lr", "memory");    \
          __res = __res_r0;                                                   \
          RETURN(type, __res)
    #undef _syscall0
    #define _syscall0(type, name)                                             \
        type name() {                                                         \
          BODY(type, name);                                                   \
        }
    #undef _syscall1
    #define _syscall1(type, name, type1, arg1)                                \
        type name(type1 arg1) {                                               \
          REG(0, arg1); BODY(type, name, "r"(__r0));                          \
        }
    #undef _syscall2
    #define _syscall2(type, name, type1, arg1, type2, arg2)                   \
        type name(type1 arg1, type2 arg2) {                                   \
          REG(0, arg1); REG(1, arg2); BODY(type, name, "r"(__r0), "r"(__r1)); \
        }
    #undef _syscall3
    #define _syscall3(type, name, type1, arg1, type2, arg2, type3, arg3)      \
        type name(type1 arg1, type2 arg2, type3 arg3) {                       \
          REG(0, arg1); REG(1, arg2); REG(2, arg3);                           \
          BODY(type, name, "r"(__r0), "r"(__r1), "r"(__r2));                  \
        }
    #undef _syscall4
    #define _syscall4(type,name,type1,arg1,type2,arg2,type3,arg3,type4,arg4)  \
        type name(type1 arg1, type2 arg2, type3 arg3, type4 arg4) {           \
          REG(0, arg1); REG(1, arg2); REG(2, arg3); REG(3, arg4);             \
          BODY(type, name, "r"(__r0), "r"(__r1), "r"(__r2), "r"(__r3));       \
        }
    #undef _syscall5
    #define _syscall5(type,name,type1,arg1,type2,arg2,type3,arg3,type4,arg4,  \
                      type5,arg5)                                             \
        type name(type1 arg1, type2 arg2, type3 arg3, type4 arg4,             \
                  type5 arg5) {                                               \
          REG(0, arg1); REG(1, arg2); REG(2, arg3); REG(3, arg4);             \
          REG(4, arg5);                                                       \
          BODY(type, name, "r"(__r0), "r"(__r1), "r"(__r2), "r"(__r3),        \
               "r"(__r4));                                                    \
        }
    #undef _syscall6
    #define _syscall6(type,name,type1,arg1,type2,arg2,type3,arg3,type4,arg4,  \
                      type5,arg5,type6,arg6)                                  \
        type name(type1 arg1, type2 arg2, type3 arg3, type4 arg4,             \
                  type5 arg5, type6 arg6) {                                   \
          REG(0, arg1); REG(1, arg2); REG(2, arg3); REG(3, arg4);             \
          REG(4, arg5); REG(5, arg6);                                         \
          BODY(type, name, "r"(__r0), "r"(__r1), "r"(__r2), "r"(__r3),        \
               "r"(__r4), "r"(__r5));                                         \
        }
  #endif
  #if defined(__x86_64__)
    struct msghdr;
    #define __NR_sys_mmap           __NR_mmap
    #define __NR_sys_recvmsg        __NR_recvmsg
    #define __NR_sys_sendmsg        __NR_sendmsg
    #define __NR_sys_shutdown       __NR_shutdown
    #define __NR_sys_rt_sigaction   __NR_rt_sigaction
    #define __NR_sys_rt_sigprocmask __NR_rt_sigprocmask
    #define __NR_sys_socket         __NR_socket
    #define __NR_sys_socketpair     __NR_socketpair
    static inline _syscall6(void*, sys_mmap,          void*, s,
                            size_t,                   l, int,               p,
                            int,                      f, int,               d,
                            __off64_t,                o);
    static inline _syscall3(int, sys_recvmsg,        int,   s,
                            struct msghdr*,          m, int, f);
    static inline _syscall3(int, sys_sendmsg,        int,   s,
                            const struct msghdr*,    m, int, f);
    static inline _syscall2(int, sys_shutdown,       int,   s,
                            int,                     h);
    static inline _syscall4(int, sys_rt_sigaction,   int,   s,
                            const struct sigaction*, a,
                            struct sigaction*,       o, int,      c);
    static inline _syscall4(int, sys_rt_sigprocmask, int,   h,
                            const sigset_t*,         s, sigset_t*, o, int,  c);
    static inline _syscall3(int, sys_socket,         int,   d,
                            int,                     t, int,       p);
    static inline _syscall4(int, sys_socketpair,     int,   d,
                            int,                     t, int,       p, int*, s);
    #define sys_sigaction(s,a,o)    sys_rt_sigaction((s), (a), (o),           \
                                                     (_NSIG+7)/8)
    #define sys_sigprocmask(h,s,o)  sys_rt_sigprocmask((h), (s),(o),          \
                                                       (_NSIG+7)/8)
  #endif
  #if defined(__x86_64__) || defined(__ARM_ARCH_3__)
    #define __NR_sys_wait4          __NR_wait4

    static inline _syscall4(pid_t, sys_wait4,        pid_t, p,
                            int*,                    s, int,       o,
                            struct rusage*,          r);

    #define sys_waitpid(p,s,o)      sys_wait4((p), (s), (o), 0)
  #endif
  #if defined(__i386__) || defined(__ARM_ARCH_3__)
    #define __NR_sys__llseek     __NR__llseek
    #define __NR_sys_mmap        __NR_mmap
    #define __NR_sys_mmap2       __NR_mmap2
    #define __NR_sys_sigaction   __NR_sigaction
    #define __NR_sys_sigprocmask __NR_sigprocmask
    #define __NR_sys__socketcall __NR_socketcall

    static inline _syscall5(int, sys__llseek, uint, fd, ulong, hi, ulong, lo,
                            loff_t *, res, uint, wh);
    static inline _syscall1(void*, sys_mmap,          void*, a);
    static inline _syscall6(void*, sys_mmap2,         void*, s,
                            size_t,                   l, int,               p,
                            int,                      f, int,               d,
                            __off64_t,                o);
    static inline _syscall3(int,     sys_sigaction,   int,   s,
                            const struct sigaction*,  a, struct sigaction*, o);
    static inline _syscall3(int,     sys_sigprocmask, int,   h,
                            const sigset_t*,          s, sigset_t*,         o);
    static inline _syscall2(int,      sys__socketcall,int,   c,
                            va_list,                  a);
    static inline int sys_socketcall(int op, ...) {
      int rc;
      va_list ap;
      va_start(ap, op);
      rc = sys__socketcall(op, ap);
      va_end(ap);
      return rc;
    }
    #define sys_recvmsg(s,m,f)      sys_socketcall(17,      (s), (m), (f))
    #define sys_sendmsg(s,m,f)      sys_socketcall(16,      (s), (m), (f))
    #define sys_shutdown(s,h)       sys_socketcall(13,      (s), (h))
    #define sys_socket(d,t,p)       sys_socketcall(1,       (d), (t), (p))
    #define sys_socketpair(d,t,p,s) sys_socketcall(8,       (d), (t), (p),(s))
  #endif
  #if defined(__i386__)
    #define __NR_sys_waitpid     __NR_waitpid
    static inline _syscall3(pid_t, sys_waitpid,      pid_t, p,
                            int*,              s,    int,   o);
  #endif
  #define __NR_sys_close        __NR_close
  #define __NR_sys_dup          __NR_dup
  #define __NR_sys_dup2         __NR_dup2
  #define __NR_sys_execve       __NR_execve
  #define __NR_sys__exit        __NR_exit
  #define __NR_sys_fcntl        __NR_fcntl
  #define __NR_sys_fork         __NR_fork
  #define __NR_sys_fstat        __NR_fstat
  #define __NR_sys_getdents     __NR_getdents
  #define __NR_sys_getdents64   __NR_getdents64
  #define __NR_sys_getegid      __NR_getegid
  #define __NR_sys_geteuid      __NR_geteuid
  #define __NR_sys_getpgrp      __NR_getpgrp
  #define __NR_sys_getpid       __NR_getpid
  #define __NR_sys_getppid      __NR_getppid
  #define __NR_sys_getpriority  __NR_getpriority
  #define __NR_sys_getrlimit    __NR_getrlimit
  #define __NR_sys_getsid       __NR_getsid
  #define __NR__gettid          __NR_gettid
  #define __NR_sys_kill         __NR_kill
  #define __NR_sys_lseek        __NR_lseek
  #define __NR_sys_munmap       __NR_munmap
  #define __NR_sys_open         __NR_open
  #define __NR_sys_pipe         __NR_pipe
  #define __NR_sys_prctl        __NR_prctl
  #define __NR_sys_ptrace       __NR_ptrace
  #define __NR_sys_read         __NR_read
  #define __NR_sys_readlink     __NR_readlink
  #define __NR_sys_sched_yield  __NR_sched_yield
  #define __NR_sys_sigaltstack  __NR_sigaltstack
  #define __NR_sys_stat         __NR_stat
  #define __NR_sys_write        __NR_write
  #define __NR_sys_futex        __NR_futex
  static inline _syscall1(int,     sys_close,       int,         f);
  static inline _syscall1(int,     sys_dup,         int,         f);
  static inline _syscall2(int,     sys_dup2,        int,         s,
                          int,            d);
  static inline _syscall3(int,     sys_execve,      const char*, f,
                          const char*const*,a,const char*const*, e);
  static inline _syscall1(int,     sys__exit,       int,         e);
  static inline _syscall3(int,     sys_fcntl,       int,         f,
                          int,            c, long,   a);
  static inline _syscall0(pid_t,   sys_fork);
  static inline _syscall2(int,     sys_fstat,       int,         f,
                          struct stat*,   b);
  static inline _syscall3(int,   sys_getdents,      int,         f,
                          struct dirent*, d, int,    c);
  static inline _syscall3(int,   sys_getdents64,    int,         f,
                          struct dirent64*, d, int,    c);
  static inline _syscall0(gid_t,   sys_getegid);
  static inline _syscall0(uid_t,   sys_geteuid);
  static inline _syscall0(pid_t,   sys_getpgrp);
  static inline _syscall0(pid_t,   sys_getpid);
  static inline _syscall0(pid_t,   sys_getppid);
  static inline _syscall2(int,     sys_getpriority, int,         a,
                          int,            b);
  static inline _syscall2(int,     sys_getrlimit,   int,         r,
                          struct rlimit*, l);
  static inline _syscall1(pid_t,   sys_getsid,      pid_t,       p);
  static inline _syscall0(pid_t,   _gettid);
  static inline _syscall2(int,     sys_kill,        pid_t,       p,
                          int,            s);
  static inline _syscall3(off_t,   sys_lseek,       int,         f,
                          off_t,          o, int,    w);
  static inline _syscall2(int,     sys_munmap,      void*,       s,
                          size_t,         l);
  static inline _syscall3(int,     sys_open,        const char*, p,
                          int,            f, int,    m);
  static inline _syscall1(int,     sys_pipe,        int*,        p);
  static inline _syscall2(int,     sys_prctl,       int,         o,
                          long,           a);
  static inline _syscall4(long,    sys_ptrace,      int,         r,
                          pid_t,          p, void *, a, void *, d);
  static inline _syscall3(ssize_t, sys_read,        int,         f,
                          void *,         b, size_t, c);
  static inline _syscall3(int,     sys_readlink,    const char*, p,
                          char*,          b, size_t, s);
  static inline _syscall0(int,     sys_sched_yield);
  static inline _syscall2(int,     sys_sigaltstack, const stack_t*, s,
                          const stack_t*, o);
  static inline _syscall2(int,     sys_stat,        const char*, f,
                          struct stat*,   b);
  static inline _syscall3(ssize_t, sys_write,        int,        f,
                          const void *,   b, size_t, c);
  static inline _syscall4(int, sys_futex, int*, addrx, int, opx, int, valx,
                          struct timespec *, timeoutx);

  static inline int sys_sysconf(int name) {
    extern int __getpagesize(void);
    switch (name) {
      case _SC_OPEN_MAX: {
        struct rlimit ru;
        return sys_getrlimit(RLIMIT_NOFILE, &ru) < 0 ? 8192 : ru.rlim_cur;
      }
      case _SC_PAGESIZE:
        return __getpagesize();
      default:
        errno = ENOSYS;
        return -1;
    }
  }

  static inline pid_t sys_gettid() {
    pid_t tid = _gettid();
    if (tid != -1) {
      return tid;
    }
    return sys_getpid();
  }

  static inline int sys_ptrace_detach(pid_t pid) {
    /* PTRACE_DETACH can sometimes forget to wake up the tracee and it
     * then sends job control signals to the real parent, rather than to
     * the tracer. We reduce the risk of this happening by starting a
     * whole new time slice, and then quickly sending a SIGCONT signal
     * right after detaching from the tracee.
     */
    int rc, err;
    sys_sched_yield();
    rc = sys_ptrace(PTRACE_DETACH, pid, (void *)0, (void *)0);
    err = errno;
    sys_kill(pid, SIGCONT);
    errno = err;
    return rc;
  }
  #undef REG
  #undef BODY
  #undef RETURN
#endif


#endif
#endif
