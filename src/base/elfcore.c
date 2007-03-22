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

#include "base/elfcore.h"
#if defined DUMPER

#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/unistd.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#include "base/thread_lister.h"

/* Definitions missing from the standard header files                        */
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


/* Data structures found in x86-32/64 core dumps on Linux; similar data
 * structures are defined in /usr/include/{linux,asm}/... but those
 * headers conflict with the rest of the libc headers. So we cannot
 * include them here.
 */

typedef struct i386_fpxregs {   /* SSE registers                             */
  uint16_t  cwd;
  uint16_t  swd;
  uint16_t  twd;
  uint16_t  fop;
  uint32_t  fip;
  uint32_t  fcs;
  uint32_t  foo;
  uint32_t  fos;
  uint32_t  mxcsr;
  uint32_t  mxcsr_mask;
  uint32_t  st_space[32];       /*  8*16 bytes for each FP-reg  = 128 bytes  */
  uint32_t  xmm_space[64];      /* 16*16 bytes for each XMM-reg = 128 bytes  */
  uint32_t  padding[24];
} i386_fpxregs;


#ifdef __x86_64__
/* Linux on x86-64 stores all FPU registers in the SSE structure             */
typedef  i386_fpxregs i386_fpregs;
#else
typedef struct i386_fpregs {    /* FPU registers                             */
  uint32_t  cwd;
  uint32_t  swd;
  uint32_t  twd;
  uint32_t  fip;
  uint32_t  fcs;
  uint32_t  foo;
  uint32_t  fos;
  uint32_t  st_space[20];       /* 8*10 bytes for each FP-reg = 80 bytes     */
} i386_fpregs;
#endif


typedef struct i386_timeval {   /* Time value with microsecond resolution    */
  long tv_sec;                  /* Seconds                                   */
  long tv_usec;                 /* Microseconds                              */
} i386_timeval;


typedef struct i386_siginfo {   /* Information about signal (unused)         */
  int32_t si_signo;             /* Signal number                             */
  int32_t si_code;              /* Extra code                                */
  int32_t si_errno;             /* Errno                                     */
} i386_siginfo;


typedef struct i386_prstatus {  /* Information about thread; includes CPU reg*/
  struct i386_siginfo pr_info;  /* Info associated with signal               */
  uint16_t       pr_cursig;     /* Current signal                            */
  unsigned long  pr_sigpend;    /* Set of pending signals                    */
  unsigned long  pr_sighold;    /* Set of held signals                       */
  pid_t          pr_pid;        /* Process ID                                */
  pid_t          pr_ppid;       /* Parent's process ID                       */
  pid_t          pr_pgrp;       /* Group ID                                  */
  pid_t          pr_sid;        /* Session ID                                */
  i386_timeval   pr_utime;      /* User time                                 */
  i386_timeval   pr_stime;      /* System time                               */
  i386_timeval   pr_cutime;     /* Cumulative user time                      */
  i386_timeval   pr_cstime;     /* Cumulative system time                    */
  i386_regs      pr_reg;        /* CPU registers                             */
  uint32_t       pr_fpvalid;    /* True if math co-processor being used      */
} i386_prstatus;


typedef struct i386_prpsinfo {  /* Information about process                 */
  unsigned char  pr_state;      /* Numeric process state                     */
  char           pr_sname;      /* Char for pr_state                         */
  unsigned char  pr_zomb;       /* Zombie                                    */
  signed char    pr_nice;       /* Nice val                                  */
  unsigned long  pr_flag;       /* Flags                                     */
#ifdef __x86_64__
  uint32_t       pr_uid;        /* User ID                                   */
  uint32_t       pr_gid;        /* Group ID                                  */
#else
  uint16_t       pr_uid;        /* User ID                                   */
  uint16_t       pr_gid;        /* Group ID                                  */
#endif
  pid_t          pr_pid;        /* Process ID                                */
  pid_t          pr_ppid;       /* Parent's process ID                       */
  pid_t          pr_pgrp;       /* Group ID                                  */
  pid_t          pr_sid;        /* Session ID                                */
  char           pr_fname[16];  /* Filename of executable                    */
  char           pr_psargs[80]; /* Initial part of arg list                  */
} i386_prpsinfo;


typedef struct i386_user {      /* Ptrace returns this data for thread state */
  i386_regs      regs;          /* CPU registers                             */
  unsigned long  fpvalid;       /* True if math co-processor being used      */
  i386_fpregs    fpregs;        /* FPU registers                             */
  unsigned long  tsize;         /* Text segment size in pages                */
  unsigned long  dsize;         /* Data segment size in pages                */
  unsigned long  ssize;         /* Stack segment size in pages               */
  unsigned long  start_code;    /* Starting virtual address of text          */
  unsigned long  start_stack;   /* Starting virtual address of stack area    */
  unsigned long  signal;        /* Signal that caused the core dump          */
  unsigned long  reserved;      /* No longer used                            */
  i386_regs      *regs_ptr;     /* Used by gdb to help find the CPU registers*/
  i386_fpregs    *fpregs_ptr;   /* Pointer to FPU registers                  */
  unsigned long  magic;         /* Magic for old A.OUT core files            */
  char           comm[32];      /* User command that was responsible         */
  unsigned long  debugreg[8];
  unsigned long  error_code;    /* CPU error code or 0                       */
  unsigned long  fault_address; /* CR3 or 0                                  */
} i386_user;


#ifdef __x86_64__
  #define ELF_CLASS ELFCLASS64
  #define ELF_ARCH  EM_X86_64
  #define Ehdr      Elf64_Ehdr
  #define Phdr      Elf64_Phdr
  #define Shdr      Elf64_Shdr
  #define Nhdr      Elf64_Nhdr
#else
  #define ELF_CLASS ELFCLASS32
  #define ELF_ARCH  EM_386
  #define Ehdr      Elf32_Ehdr
  #define Phdr      Elf32_Phdr
  #define Shdr      Elf32_Shdr
  #define Nhdr      Elf32_Nhdr
#endif


/* After forking, we must make sure to only call system calls.               */
#if __BOUNDED_POINTERS__
  #error "Need to port invocations of syscalls for bounded ptrs"
#else
  /* The code in this file gets executed after threads have been suspended.
   * As a consequence, we cannot call any functions that acquire locks.
   * Unfortunately, libc wraps most system calls (e.g. in order to implement
   * pthread_atfork, and to make calls cancellable), which means we cannot
   * call these functions. Instead, we have to call syscall() directly.
   */
  #include <stdarg.h>
  #include <syscall.h>
  #ifdef __x86_64__
    #define sys_recvmsg(s,m,f)      syscall(SYS_recvmsg,    (s), (m), (f))
    #define sys_sendmsg(s,m,f)      syscall(SYS_sendmsg,    (s), (m), (f))
    #define sys_shutdown(s,h)       syscall(SYS_shutdown,   (s), (h))
    #define sys_sigaction(s,a,o)    syscall(SYS_rt_sigaction,    (s), (a),(o),\
                                                                       _NSIG/8)
    #define sys_sigprocmask(h,s,o)  syscall(SYS_rt_sigprocmask,  (h), (s),(o),\
                                                                       _NSIG/8)
    #define sys_socketpair(d,t,p,s) syscall(SYS_socketpair, (d), (t), (p),(s))
    #define sys_waitpid(p,s,o)      syscall(SYS_wait4, (p), (s), (o),(void *)0)
  #else
    static int sys_socketcall(int op, ...) {
      int rc;
      va_list ap;
      va_start(ap, op);
      rc = syscall(SYS_socketcall, op, ap);
      va_end(ap);
      return rc;
    }
    #define sys_recvmsg(s,m,f)      sys_socketcall(17,      (s), (m), (f))
    #define sys_sendmsg(s,m,f)      sys_socketcall(16,      (s), (m), (f))
    #define sys_shutdown(s,h)       sys_socketcall(13,      (s), (h))
    #define sys_sigaction(s,a,o)    syscall(SYS_sigaction,  (s), (a), (o))
    #define sys_sigprocmask(h,s,o)  syscall(SYS_sigprocmask,(h), (s), (o))
    #define sys_socketpair(d,t,p,s) sys_socketcall(8,       (d), (t), (p),(s))
    #define sys_waitpid(p,s,o)      syscall(SYS_waitpid,    (p), (s), (o))
  #endif
  #define sys_close(f)              syscall(SYS_close,      (f))
  #define sys_exit(r)               syscall(SYS_exit,       (r))
  #define sys_fork()                syscall(SYS_fork)
  #define sys_getegid()             syscall(SYS_getegid)
  #define sys_geteuid()             syscall(SYS_geteuid)
  #define sys_getpgrp()             syscall(SYS_getpgrp)
  #define sys_getpid()              syscall(SYS_getpid)
  #define sys_getppid()             syscall(SYS_getppid)
  #define sys_getpriority(a,b)      syscall(SYS_getpriority)
  #define sys_getrlimit(r,l)        syscall(SYS_getrlimit,  (r), (l))
  #define sys_getsid(p)             syscall(SYS_getsid,     (p))
  #define sys_open(f,p,m)           syscall(SYS_open,       (f), (p), (m))
  #define sys_pipe(f)               syscall(SYS_pipe,       (f))
  #define sys_prctl(o,a)            syscall(SYS_prctl,      (o), (a))
  #define sys_ptrace(r,p,a,d)       syscall(SYS_ptrace,     (r), (p), (a),(d))
  #define sys_read(f,b,c)           syscall(SYS_read,       (f), (b), (c))
  #define sys_readlink(p,b,s)       syscall(SYS_readlink,   (p), (b), (s))
  #define sys_write(f,b,c)          syscall(SYS_write,      (f), (b), (c))

  static int sys_sysconf(int name) {
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

  static pid_t sys_gettid() {
    #ifndef SYS_gettid
      #define SYS_gettid 224
    #endif
    pid_t tid = syscall(SYS_gettid);
    if (tid != -1) {
      return tid;
    }
    return sys_getpid();
  }
#endif


/* Re-runs fn until it doesn't cause EINTR
 */

#define NO_INTR(fn)   do {} while ((fn) < 0 && errno == EINTR)

/* Wrapper for read() which is guaranteed to never return EINTR.
 */
static ssize_t c_read(int f, const void *buf, size_t bytes) {
  if (bytes > 0) {
    ssize_t rc;
    NO_INTR(rc = sys_read(f, buf, bytes));
    return rc;
  }
  return 0;
}

/* Wrapper for write() which is guaranteed to never return EINTR nor
 * short writes.
 */
static ssize_t c_write(int f, const void *void_buf, size_t bytes) {
  const unsigned char *buf = (const unsigned char*)void_buf;
  size_t len = bytes;
  while (len > 0) {
    ssize_t rc;
    NO_INTR(rc = sys_write(f, buf, len));
    if (rc < 0)
      return rc;
    else if (rc == 0)
      break;
    buf += rc;
    len -= rc;
  }
  return bytes;
}


struct io {
  int fd;
  unsigned char *data, *end;
  unsigned char buf[4096];
};


/* Reads one character from the "io" file. This function has the same
 * semantics as fgetc(), but we cannot call any library functions at this
 * time.
 */
static int GetChar(struct io *io) {
  unsigned char *ptr = io->data;
  if (ptr == io->end) {
    /* Even though we are parsing one character at a time, read in larger
     * chunks.
     */
    ssize_t n = c_read(io->fd, io->buf, sizeof(io->buf));
    if (n <= 0) {
      if (n == 0)
        errno = 0;
      return -1;
    }
    ptr = &io->buf[0];
    io->end = &io->buf[n];
  }
  io->data = ptr+1;
  return *ptr;
}


/* Place the hex number read from "io" into "*hex".  The first non-hex
 * character is returned (or -1 in the case of end-of-file).
 */
static int GetHex(struct io *io, size_t *hex) {
  int ch;
  *hex = 0;
  while (((ch = GetChar(io)) >= '0' && ch <= '9') ||
         (ch >= 'A' && ch <= 'F') || (ch >= 'a' && ch <= 'f'))
    *hex = (*hex << 4) | (ch < 'A' ? ch - '0' : (ch & 0xF) + 9);
  return ch;
}


/* Computes the amount of leading zeros in a memory region.
 */
static size_t LeadingZeros(int *loopback, void *mem, size_t len,
                           size_t pagesize) {
  char   buf[pagesize];
  size_t count;
  char   *ptr = 0;
  for (count = 0; count < len; ) {
    /* Read a page by going through the pipe. Assume that we can write at
     * least one page without blocking.
     *
     * "Normal" kernels do not require this hack. But some of the security
     * patches (e.g. grsec) can be configured to disallow read access of
     * executable pages. So, directly scanning the memory range would
     * result in a segmentation fault.
     *
     * If we cannot access a page, we assume that it was all zeros.
     */
    if ((count % pagesize) == 0) {
      if (c_write(loopback[1], (char *)mem + count, pagesize) < 0 ||
          c_read(loopback[0],  buf,                 pagesize) < 0) {
        count += pagesize;
        continue;
      } else
        ptr = buf;
    }
    if (*ptr++)
      break;
    count++;
  }
  return count & ~(pagesize-1);
}


/* This function is invoked from a seperate process. It has access to a
 * copy-on-write copy of the parents address space, and all crucial
 * information about the parent has been computed by the caller.
 */
static void CreateElfCore(int fd, i386_prpsinfo *prpsinfo, i386_user *user,
                          i386_prstatus *prstatus, int num_threads,
                          pid_t *pids, i386_regs *regs, i386_fpregs *fpregs,
                          i386_fpxregs *fpxregs, size_t pagesize) {
  /* Count the number of mappings in "/proc/self/maps". We are guaranteed
   * that this number is not going to change while this function executes.
   */
  int       num_mappings = 0;
  struct io io;
  int       loopback[2] = { -1, -1 };

  if (sys_pipe(loopback) < 0)
    goto done;

  io.data = io.end = 0;
  NO_INTR(io.fd = sys_open("/proc/self/maps", O_RDONLY, 0));
  if (io.fd >= 0) {
    int i, ch;
    while ((ch = GetChar(&io)) >= 0) {
      num_mappings += (ch == '\n');
    }
    if (errno != 0) {
   read_error:
      NO_INTR(sys_close(io.fd));
      goto done;
    }
    NO_INTR(sys_close(io.fd));

    /* Read all mappings. This requires re-opening "/proc/self/maps"         */
    /* scope */ {
      struct {
        size_t start_address, end_address, offset;
        int   flags;
      } mappings[num_mappings];
      io.data = io.end = 0;
      NO_INTR(io.fd = sys_open("/proc/self/maps", O_RDONLY, 0));
      if (io.fd >= 0) {
        size_t note_align;
        /* Parse entries of the form:
         * "^[0-9A-F]*-[0-9A-F]* [r-][w-][x-][p-] [0-9A-F]*.*$"
         */
        for (i = 0; i < num_mappings;) {
          static const char * const dev_zero = "/dev/zero";
          const char *dev = dev_zero;
          int    j, is_device;
          size_t zeros;

          memset(&mappings[i], 0, sizeof(mappings[i]));

          /* Read start and end addresses                                    */
          if (GetHex(&io, &mappings[i].start_address) != '-' ||
              GetHex(&io, &mappings[i].end_address)   != ' ')
            goto read_error;

          /* Read flags                                                      */
          while ((ch = GetChar(&io)) != ' ') {
            if (ch < 0)
              goto read_error;
            mappings[i].flags = (mappings[i].flags << 1) | (ch != '-');
          }
          /* Drop the private/shared bit. This makes the flags compatible with
           * the ELF access bits
           */
          mappings[i].flags >>= 1;

          /* Read offset                                                     */
          if ((ch = GetHex(&io, &mappings[i].offset)) != ' ')
            goto read_error;

          /* Skip over device numbers, and inode number                      */
          for (j = 0; j < 2; j++) {
            while (ch == ' ') {
              ch = GetChar(&io);
            }
            while (ch != ' ' && ch != '\n') {
              if (ch < 0)
                goto read_error;
              ch = GetChar(&io);
            }
            while (ch == ' ') {
              ch = GetChar(&io);
            }
            if (ch < 0)
              goto read_error;
          }

          /* Check whether this is a mapping for a device                    */
          while (*dev && ch == *dev) {
            ch = GetChar(&io);
            dev++;
          }
          is_device = dev >= dev_zero + 5 &&
                      ((ch != '\n' && ch != ' ') || *dev != '\000');

          /* Skip until end of line                                          */
          while (ch != '\n') {
            if (ch < 0)
              goto read_error;
            ch = GetChar(&io);
          }

          /* Skip leading zeroed pages (as found in the stack segment)       */
          if ((mappings[i].flags & PF_R) && !is_device) {
            zeros = LeadingZeros(loopback, (void *)mappings[i].start_address,
                         mappings[i].end_address - mappings[i].start_address,
                         pagesize);
            mappings[i].start_address += zeros;
          }

          /* Remove mapping, if it was not readable, or completely zero
           * anyway. The former is usually the case of stack guard pages, and
           * the latter occasionally happens for unused memory.
           * Also, be careful not to touch mapped devices.
           */
          if ((mappings[i].flags & PF_R) == 0 ||
              mappings[i].start_address == mappings[i].end_address ||
              is_device) {
            num_mappings--;
          } else {
            i++;
          }
        }
        NO_INTR(sys_close(io.fd));

        /* Write out the ELF header                                          */
        /* scope */ {
          Ehdr ehdr;
          memset(&ehdr, 0, sizeof(ehdr));
          ehdr.e_ident[0] = ELFMAG0;
          ehdr.e_ident[1] = ELFMAG1;
          ehdr.e_ident[2] = ELFMAG2;
          ehdr.e_ident[3] = ELFMAG3;
          ehdr.e_ident[4] = ELF_CLASS;
          ehdr.e_ident[5] = ELFDATA2LSB;
          ehdr.e_ident[6] = EV_CURRENT;
          ehdr.e_type     = ET_CORE;
          ehdr.e_machine  = ELF_ARCH;
          ehdr.e_version  = EV_CURRENT;
          ehdr.e_phoff    = sizeof(ehdr);
          ehdr.e_ehsize   = sizeof(ehdr);
          ehdr.e_phentsize= sizeof(Phdr);
          ehdr.e_phnum    = num_mappings + 1;
          ehdr.e_shentsize= sizeof(Shdr);
          if (c_write(fd, &ehdr, sizeof(ehdr)) != sizeof(ehdr)) {
            goto done;
          }
        }

        /* Write program headers, starting with the PT_NOTE entry            */
        /* scope */ {
          Phdr   phdr;
          size_t offset   = sizeof(Ehdr) + (num_mappings + 1)*sizeof(Phdr);
          size_t filesz   = sizeof(Nhdr) + 4 + sizeof(i386_prpsinfo) +
                            sizeof(Nhdr) + 4 + sizeof(i386_user) +
                            num_threads*(
                            + sizeof(Nhdr) + 4 + sizeof(i386_prstatus)
                            + sizeof(Nhdr) + 4 + sizeof(i386_fpregs));
          #ifndef __x86_64__
          if (fpxregs) {
            filesz       += num_threads*(
                              sizeof(Nhdr) + 4 + sizeof(i386_fpxregs));
          }
          #endif
          memset(&phdr, 0, sizeof(phdr));
          phdr.p_type     = PT_NOTE;
          phdr.p_offset   = offset;
          phdr.p_filesz   = filesz;
          if (c_write(fd, &phdr, sizeof(phdr)) != sizeof(phdr)) {
            goto done;
          }

          /* Now follow with program headers for each of the memory segments */
          phdr.p_type     = PT_LOAD;
          phdr.p_align    = pagesize;
          phdr.p_paddr    = 0;
          note_align      = phdr.p_align - ((offset+filesz) % phdr.p_align);
          if (note_align == phdr.p_align)
            note_align    = 0;
          offset         += note_align;
          for (i = 0; i < num_mappings; i++) {
            offset       += filesz;
            filesz        = mappings[i].end_address -mappings[i].start_address;
            phdr.p_offset = offset;
            phdr.p_vaddr  = mappings[i].start_address;
            phdr.p_memsz  = filesz;

            /* Do not write contents for memory segments that are read-only  */
            if ((mappings[i].flags & PF_W) == 0)
              filesz      = 0;
            phdr.p_filesz = filesz;
            phdr.p_flags  = mappings[i].flags;
            if (c_write(fd, &phdr, sizeof(phdr)) != sizeof(phdr)) {
              goto done;
            }
          }
        }

        /* Write note section                                                */
        /* scope */ {
          Nhdr nhdr;
          memset(&nhdr, 0, sizeof(nhdr));
          nhdr.n_namesz   = 4;
          nhdr.n_descsz   = sizeof(i386_prpsinfo);
          nhdr.n_type     = NT_PRPSINFO;
          if (c_write(fd, &nhdr, sizeof(nhdr)) != sizeof(nhdr) ||
              c_write(fd, "CORE", 4) != 4 ||
              c_write(fd, prpsinfo, sizeof(i386_prpsinfo)) !=
              sizeof(i386_prpsinfo)) {
            goto done;
          }
          nhdr.n_descsz   = sizeof(i386_user);
          nhdr.n_type     = NT_PRXREG;
          if (c_write(fd, &nhdr, sizeof(nhdr)) != sizeof(nhdr) ||
              c_write(fd, "CORE", 4) != 4 ||
              c_write(fd, user, sizeof(i386_user)) != sizeof(i386_user)) {
            goto done;
          }

          for (i = num_threads; i-- > 0; ) {
            /* Process status and integer registers                          */
            nhdr.n_descsz = sizeof(i386_prstatus);
            nhdr.n_type   = NT_PRSTATUS;
            prstatus->pr_pid = pids[i];
            prstatus->pr_reg = regs[i];
            if (c_write(fd, &nhdr, sizeof(nhdr)) != sizeof(nhdr) ||
                c_write(fd, "CORE", 4) != 4 ||
                c_write(fd, prstatus, sizeof(i386_prstatus)) !=
                sizeof(i386_prstatus)) {
              goto done;
            }

            /* FPU registers                                                 */
            nhdr.n_descsz = sizeof(i386_fpregs);
            nhdr.n_type   = NT_FPREGSET;
            if (c_write(fd, &nhdr, sizeof(nhdr)) != sizeof(nhdr) ||
                c_write(fd, "CORE", 4) != 4 ||
                c_write(fd, fpregs+1, sizeof(i386_fpregs)) !=
                sizeof(i386_fpregs)) {
              goto done;
            }

            /* SSE registers                                                 */
            #ifndef __x86_64__
            /* Linux on x86-64 stores all FPU registers in the SSE structure */
            if (fpxregs) {
              nhdr.n_descsz = sizeof(i386_fpxregs);
              nhdr.n_type   = NT_PRFPXREG;
              if (c_write(fd, &nhdr, sizeof(nhdr)) != sizeof(nhdr) ||
                  c_write(fd, "CORE", 4) != 4 ||
                  c_write(fd, fpxregs+1, sizeof(i386_fpxregs)) !=
                  sizeof(i386_fpxregs)) {
                goto done;
              }
            }
            #endif
          }
        }

        /* Align all following segments to multiples of page size            */
        if (note_align) {
          char scratch[note_align];
          memset(scratch, 0, sizeof(scratch));
          if (c_write(fd, scratch, sizeof(scratch)) != sizeof(scratch)) {
            goto done;
          }
        }

        /* Write all memory segments                                         */
        for (i = 0; i < num_mappings; i++) {
          if (mappings[i].flags & PF_W &&
              c_write(fd, (void *)mappings[i].start_address,
                      mappings[i].end_address - mappings[i].start_address) !=
                      mappings[i].end_address - mappings[i].start_address) {
            goto done;
          }
        }
      }
    }
  }

done:
  if (loopback[0] >= 0)
    NO_INTR(sys_close(loopback[0]));
  if (loopback[1] >= 0)
    NO_INTR(sys_close(loopback[1]));
  NO_INTR(sys_close(fd));
  return;
}


/* Internal function for generating a core file. This function works for
 * both single- and multi-threaded core files. It assumes that all threads
 * are already suspended, and will resume them before returning.
 *
 * The caller must make sure that prctl(PR_SET_DUMPABLE, 1) has been called,
 * or this function might fail.
 */
int InternalGetCoreDump(void *frame, int num_threads, pid_t *thread_pids) {
  long          i;
  int           rc = -1, fd = -1, threads = num_threads, hasSSE = 0;
  i386_prpsinfo prpsinfo;
  i386_prstatus prstatus;
  pid_t         pids[threads           + 1];
  i386_regs     thread_regs[threads    + 1];
  i386_fpregs   thread_fpregs[threads  + 1];
  i386_fpxregs  thread_fpxregs[threads + 1];
  int           pair[2];
  int           main_pid = sys_gettid();

  /* Get thread status                                                       */
  if (threads)
    memcpy(pids, thread_pids, threads * sizeof(pid_t));
  memset(thread_regs,    0, (threads + 1) * sizeof(i386_regs));
  memset(thread_fpregs,  0, (threads + 1) * sizeof(i386_fpregs));
  memset(thread_fpxregs, 0, (threads + 1) * sizeof(i386_fpxregs));

  /* Threads are already attached, read their registers now                  */
  for (i = 0; i < threads; i++) {
    char scratch[4096];
    memset(scratch, 0xFF, sizeof(scratch));
    if (sys_ptrace(PTRACE_GETREGS, pids[i], scratch, scratch) == 0) {
      memcpy(thread_regs + i, scratch, sizeof(i386_regs));
      memset(scratch, 0xFF, sizeof(scratch));
      if (sys_ptrace(PTRACE_GETFPREGS, pids[i], scratch, scratch) == 0) {
        memcpy(thread_fpregs + i, scratch, sizeof(i386_fpregs));
        memset(scratch, 0xFF, sizeof(scratch));
        #ifndef __x86_64__
        /* Linux on x86-64 stores all FPU registers in the SSE structure     */
        if (sys_ptrace(PTRACE_GETFPXREGS, pids[i], scratch, scratch) == 0) {
          memcpy(thread_fpxregs + i, scratch, sizeof(i386_fpxregs));
        } else {
          hasSSE = 0;
        }
        #endif
      } else
        goto ptrace;
    } else {
   ptrace: /* Oh, well, undo everything and get out of here                  */
      ResumeAllProcessThreads(threads, pids);
      goto error;
    }
  }

  /* Build the PRPSINFO data structure                                       */
  memset(&prpsinfo, 0, sizeof(prpsinfo));
  prpsinfo.pr_sname = 'R';
  prpsinfo.pr_nice  = sys_getpriority(PRIO_PROCESS, 0);
  prpsinfo.pr_uid   = sys_geteuid();
  prpsinfo.pr_gid   = sys_getegid();
  prpsinfo.pr_pid   = main_pid;
  prpsinfo.pr_ppid  = sys_getppid();
  prpsinfo.pr_pgrp  = sys_getpgrp();
  prpsinfo.pr_sid   = sys_getsid(0);
  /* scope */ {
    char scratch[4096], *cmd = scratch, *ptr;
    ssize_t size, len;
    int cmd_fd;
    memset(&scratch, 0, sizeof(scratch));
    size = sys_readlink("/proc/self/exe", scratch, sizeof(scratch));
    len = 0;
    for (ptr = cmd; *ptr != '\000' && size-- > 0; ptr++) {
      if (*ptr == '/') {
        cmd = ptr+1;
        len = 0;
      } else
        len++;
    }
    memcpy(prpsinfo.pr_fname, cmd,
           len > sizeof(prpsinfo.pr_fname) ? sizeof(prpsinfo.pr_fname) : len);
    NO_INTR(cmd_fd = sys_open("/proc/self/cmdline", O_RDONLY, 0));
    if (cmd_fd >= 0) {
      char *ptr;
      ssize_t size = c_read(cmd_fd, &prpsinfo.pr_psargs,
                            sizeof(prpsinfo.pr_psargs));
      for (ptr = prpsinfo.pr_psargs; size-- > 0; ptr++)
        if (*ptr == '\000')
          *ptr = ' ';
      NO_INTR(sys_close(cmd_fd));
    }
  }

  /* Build the PRSTATUS data structure                                       */
  /* scope */ {
    int stat_fd;
    memset(&prstatus, 0, sizeof(prstatus));
    prstatus.pr_pid     = prpsinfo.pr_pid;
    prstatus.pr_ppid    = prpsinfo.pr_ppid;
    prstatus.pr_pgrp    = prpsinfo.pr_pgrp;
    prstatus.pr_sid     = prpsinfo.pr_sid;
    prstatus.pr_fpvalid = 1;
    NO_INTR(stat_fd = sys_open("/proc/self/stat", O_RDONLY, 0));
    if (stat_fd >= 0) {
      char scratch[4096];
      ssize_t size = c_read(stat_fd, scratch, sizeof(scratch) - 1);
      if (size >= 0) {
        unsigned long tms;
        char *ptr = scratch;
        scratch[size] = '\000';

        /* User time                                                         */
        for (i = 13; i && *ptr; ptr++) if (*ptr == ' ') i--;
        tms = 0;
        while (*ptr && *ptr != ' ') tms = 10*tms + *ptr++ - '0';
        prstatus.pr_utime.tv_sec  = tms / 1000;
        prstatus.pr_utime.tv_usec = (tms % 1000) * 1000;

        /* System time                                                       */
        if (*ptr) ptr++;
        tms = 0;
        while (*ptr && *ptr != ' ') tms = 10*tms + *ptr++ - '0';
        prstatus.pr_stime.tv_sec  = tms / 1000;
        prstatus.pr_stime.tv_usec = (tms % 1000) * 1000;

        /* Cumulative user time                                              */
        if (*ptr) ptr++;
        tms = 0;
        while (*ptr && *ptr != ' ') tms = 10*tms + *ptr++ - '0';
        prstatus.pr_cutime.tv_sec  = tms / 1000;
        prstatus.pr_cutime.tv_usec = (tms % 1000) * 1000;

        /* Cumulative system time                                            */
        if (*ptr) ptr++;
        tms = 0;
        while (*ptr && *ptr != ' ') tms = 10*tms + *ptr++ - '0';
        prstatus.pr_cstime.tv_sec  = tms / 1000;
        prstatus.pr_cstime.tv_usec = (tms % 1000) * 1000;

        /* Pending signals                                                   */
        for (i = 14; i && *ptr; ptr++) if (*ptr == ' ') i--;
        while (*ptr && *ptr != ' ')
          prstatus.pr_sigpend = 10*prstatus.pr_sigpend + *ptr++ - '0';

        /* Held signals                                                      */
        if (*ptr) ptr++;
        while (*ptr && *ptr != ' ')
          prstatus.pr_sigpend = 10*prstatus.pr_sigpend + *ptr++ - '0';
      }
      NO_INTR(sys_close(stat_fd));
    }
  }

  /* Create a file descriptor that can be used for reading data from
   * our child process. This is a little complicated because we need
   * to make sure there is no race condition with other threads
   * calling fork() at the same time (this is somewhat mitigated,
   * because our threads are supposedly suspended at this time). We
   * have to avoid other processes holding our file handles open. We
   * can do this by creating the pipe in the child and passing the
   * file handle back to the parent.
   */
  if (sys_socketpair(AF_UNIX, SOCK_STREAM, 0, pair) >= 0) {
    int openmax  = sys_sysconf(_SC_OPEN_MAX);
    int pagesize = sys_sysconf(_SC_PAGESIZE);

    /* Block signals prior to forking. Technically, POSIX requires us to call
     * pthread_sigmask(), if this is a threaded application. When using
     * glibc, we are OK calling sigprocmask(), though. We will end up
     * blocking additional signals that libpthread uses internally, but that
     * is actually exactly what we want.
     *
     * Also, POSIX claims that this should not actually be necessarily, but
     * reality says otherwise.
     */
    sigset_t old_signals, blocked_signals;
    sigfillset(&blocked_signals);
    sys_sigprocmask(SIG_BLOCK, &blocked_signals, &old_signals);

    /* Create a new core dump in child process; call sys_fork() in order to
     * avoid complications with pthread_atfork() handlers. In the child
     * process, we should only ever call system calls.
     */
    if ((rc = sys_fork()) == 0) {
      i386_user user;
      int       fds[2];

      /* All signals are blocked at this time, but we could still end up
       * executing synchronous signals (such as SIGILL, SIGFPE, SIGSEGV,
       * SIGBUS, or SIGTRAP). Reset them to SIG_DFL.
       */
      static const int signals[] = { SIGABRT, SIGILL, SIGFPE, SIGSEGV, SIGBUS};
      for (i = 0; i < sizeof(signals)/sizeof(*signals); i++) {
        struct sigaction act;
        memset(&act, 0, sizeof(act));
        act.sa_handler = SIG_DFL;
        act.sa_flags   = SA_RESTART;
        sys_sigaction(signals[i], &act, NULL);
      }

      /* Get parent's CPU registers, and user data structure                 */
      if (sys_ptrace(PTRACE_ATTACH, main_pid, (void *)0, (void *)0) >= 0) {
        char scratch[4096];
        while (sys_waitpid(main_pid, (void *)0, __WALL) < 0) {
          if (errno != EINTR)
            sys_exit(1);
        }
        for (i = 0; i < sizeof(user); i += sizeof(int))
          ((int *)&user)[i/sizeof(int)] = sys_ptrace(PTRACE_PEEKUSER,
                                              main_pid, (void *)i, (void *) i);
        memset(scratch, 0xFF, sizeof(scratch));
        if (sys_ptrace(PTRACE_GETREGS, main_pid, scratch, scratch) == 0) {
          memcpy(thread_regs + threads, scratch, sizeof(i386_regs));
          memset(scratch, 0xFF, sizeof(scratch));
          if (sys_ptrace(PTRACE_GETFPREGS, main_pid, scratch, scratch) == 0) {
            memcpy(thread_fpregs + threads, scratch, sizeof(i386_fpregs));
            memset(scratch, 0xFF, sizeof(scratch));
            #ifndef __x86_64__
            /* Linux on x86-64 stores all FPU regs in the SSE structure      */
            if (sys_ptrace(PTRACE_GETFPXREGS,main_pid,scratch,scratch) == 0) {
              memcpy(thread_fpxregs +threads,scratch,sizeof(i386_fpxregs));
            } else {
              hasSSE = 0;
            }
            #endif
          } else
            sys_exit(1);
        } else
          sys_exit(1);
      } else
        sys_exit(1);
      sys_ptrace(PTRACE_DETACH, main_pid, (void *)0, (void *)0);

      /* Fake a somewhat reasonable looking stack frame for the
       * getCoreDump() function.
       */
      SET_FRAME(*(Frame *)frame, thread_regs[threads]);
      memcpy(&user.regs, thread_regs + threads, sizeof(i386_regs));
      pids[threads++] = main_pid;

      /* Create a pipe for communicating with parent                         */
      if (sys_pipe(fds) < 0)
        sys_exit(1);

      /* Pass file handle to parent                                          */
      /* scope */ {
        char cmsg_buf[CMSG_SPACE(sizeof(int))];
        struct iovec  iov;
        struct msghdr msg;
        struct cmsghdr *cmsg;
        memset(&iov, 0, sizeof(iov));
        memset(&msg, 0, sizeof(msg));
        iov.iov_base            = (void *)"";
        iov.iov_len             = 1;
        msg.msg_iov             = &iov;
        msg.msg_iovlen          = 1;
        msg.msg_control         = &cmsg_buf;
        msg.msg_controllen      = sizeof(cmsg_buf);
        cmsg                    = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level        = SOL_SOCKET;
        cmsg->cmsg_type         = SCM_RIGHTS;
        cmsg->cmsg_len          = CMSG_LEN(sizeof(int));
        *(int *)CMSG_DATA(cmsg) = fds[0];
        while (sys_sendmsg(pair[1], &msg, 0) < 0) {
          if (errno != EINTR)
            sys_exit(1);
        }
        while (sys_shutdown(pair[1], SHUT_RDWR) < 0) {
          if (errno != EINTR)
            sys_exit(1);
        }
      }

      /* Close all file handles other than the write end of our pipe         */
      for (i = 0; i < openmax; i++)
        if (i != fds[1])
          NO_INTR(sys_close(i));

      /* Turn into a daemon process, so that "init" can reap us              */
      if ((rc = sys_fork()) == 0) {
        CreateElfCore(fds[1], &prpsinfo, &user, &prstatus, threads,
                      pids, thread_regs, thread_fpregs,
                      hasSSE ? thread_fpxregs : NULL, pagesize);
        sys_exit(0);
      } else {
        sys_exit(rc < 0 ? 1 : 0);
      }

      /* Make the compiler happy. We never actually get here.                */
      return 0;
    }

    /* In the parent                                                         */
    sys_sigprocmask(SIG_SETMASK, &old_signals, (void *)0);
    NO_INTR(sys_close(pair[1]));

    /* Get pipe file handle from child                                       */
    /* scope */ {
      char buffer[1], cmsg_buf[CMSG_SPACE(sizeof(int))];
      struct iovec  iov;
      struct msghdr msg;
      for (;;) {
        int nbytes;
        memset(&iov, 0, sizeof(iov));
        memset(&msg, 0, sizeof(msg));
        iov.iov_base       = buffer;
        iov.iov_len        = 1;
        msg.msg_iov        = &iov;
        msg.msg_iovlen     = 1;
        msg.msg_control    = &cmsg_buf;
        msg.msg_controllen = sizeof(cmsg_buf);
        if ((nbytes = sys_recvmsg(pair[0], &msg, 0)) > 0) {
          struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
          if (cmsg != NULL && cmsg->cmsg_level == SOL_SOCKET &&
              cmsg->cmsg_type == SCM_RIGHTS)
            fd = *(int *)CMSG_DATA(cmsg);
          break;
        } else if (nbytes == 0 || errno != EINTR) {
          break;
        }
      }
    }
    sys_shutdown(pair[0], SHUT_RDWR);
    NO_INTR(sys_close(pair[0]));
  }

  ResumeAllProcessThreads(threads, pids);

  /* Wait for child to detach itself                                         */
  if (rc > 0) {
    int status;
    while (sys_waitpid(rc, &status, 0) < 0) {
      if (errno != EINTR)
        goto error;
    }
    rc = WEXITSTATUS(status) ? -1 : 0;
  }

  /* Check if child process ran successfully                                 */
  if (rc >= 0) {
    return fd;
  }

error:
  if (fd > 0)
    NO_INTR(sys_close(fd));
  return -1;
}
#endif
