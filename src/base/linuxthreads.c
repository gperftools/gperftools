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

#include "base/linuxthreads.h"

#ifdef THREADS

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "base/thread_lister.h"

#ifndef O_DIRECTORY
#define O_DIRECTORY 0200000
#endif

#if __BOUNDED_POINTERS__
  #error "Need to port invocations of syscalls for bounded ptrs"
#else
  /* (Most of) the code in this file gets executed after threads have been
   * suspended. As a consequence, we cannot call any functions that acquire
   * locks. Unfortunately, libc wraps most system calls (e.g. in order to
   * implement pthread_atfork, and to make calls cancellable), which means
   * we cannot call these functions. Instead, we have to call syscall()
   * directly.
   */
  #include <asm/stat.h>
  #include <asm/posix_types.h>
  #include <asm/types.h>
  #include <linux/dirent.h>
  #include <stdarg.h>
  #include <syscall.h>
  #ifdef __x86_64__
    #define sys_socket(d,t,p)  syscall(SYS_socket,   (d), (t), (p))
    #define sys_waitpid(p,s,o) syscall(SYS_wait4,    (p), (s), (o), (void *)0)
  #else
    static int sys_socketcall(int op, ...) {
      int rc;
      va_list ap;
      va_start(ap, op);
      rc = syscall(SYS_socketcall, op, ap);
      va_end(ap);
      return rc;
    }
    #define sys_socket(d,t,p)  sys_socketcall(1,     (d), (t), (p))
    #define sys_waitpid(p,s,o) syscall(SYS_waitpid,  (p), (s), (o))
  #endif

  #define sys_close(f)         syscall(SYS_close,    (f))
  #define sys_fcntl(f,c,a)     syscall(SYS_fcntl,    (f), (c), (a))
  #define sys_fstat(f,b)       syscall(SYS_fstat,    (f), (b))
  #define sys_getdents(f,d,c)  syscall(SYS_getdents, (f), (d), (c))
  #define sys_getpid()         syscall(SYS_getpid)
  #define sys_lseek(f,o,w)     syscall(SYS_lseek,    (f), (o), (w))
  #define sys_open(f,p,m)      syscall(SYS_open,     (f), (p), (m))
  #define sys_prctl(o,a)       syscall(SYS_prctl,    (o), (a))
  #define sys_ptrace(r,p,a,d)  syscall(SYS_ptrace,   (r), (p), (a), (d))
  #define sys_stat(f,b)        syscall(SYS_stat,     (f), (b))
#endif


/* itoa() is not a standard function, and we cannot safely call printf()
 * after suspending threads. So, we just implement our own copy. A
 * recursive approach is the easiest here.
 */
static char *local_itoa(char *buf, int i) {
  if (i < 0) {
    *buf++ = '-';
    return local_itoa(buf, -i);
  } else {
    if (i >= 10)
      buf = local_itoa(buf, i/10);
    *buf++ = (i%10) + '0';
    *buf   = '\000';
    return buf;
  }
}


/* Local substitute for the atoi() function, which is not necessarily safe
 * to call once threads are suspended (depending on whether libc looks up
 * locale information,  when executing atoi()).
 */
static int local_atoi(const char *s) {
  int n   = 0;
  int neg = *s == '-';
  if (neg)
    s++;
  while (*s >= '0' && *s <= '9')
    n = 10*n + (*s++ - '0');
  return neg ? -n : n;
}

/* Re-runs fn until it doesn't cause EINTR
 */
#define NO_INTR(fn)   do {} while ((fn) < 0 && errno == EINTR)

/* Wrapper for open() which is guaranteed to never return EINTR.
 */
static int c_open(const char *fname, int flags, int mode) {
  ssize_t rc;
  NO_INTR(rc = sys_open(fname, flags, mode));
  return rc;
}

/* This function gets the list of all linux threads of the current process
 * but this one and passes them to the 'callback' along with the 'parameter'
 * pointer; at the call back call time all the threads are paused via
 * PTRACE_ATTACH.
 * 'callback' is supposed to do or arrange for ResumeAllProcessThreads.
 * We return -1 on error and the return value of 'callback' on success.
 */
int GetAllProcessThreads(void *parameter,
                         GetAllProcessThreadsCallBack callback) {
  int              marker = -1, proc = -1, dumpable = 1;
  int              num_threads = 0, max_threads = 0;
  char             marker_name[48], *marker_path;
  struct stat      proc_sb, marker_sb;
  pid_t            my_pid = sys_getpid();

  /* Create "marker" that we can use to detect threads sharing the same
   * address space and the same file handles. By setting the FD_CLOEXEC flag
   * we minimize the risk of misidentifying child processes as threads;
   * and since there is still a race condition,  we will filter those out
   * later, anyway.
   */
  if ((marker = sys_socket(PF_LOCAL, SOCK_DGRAM, 0)) < 0 ||
      sys_fcntl(marker, F_SETFD, FD_CLOEXEC) < 0)
    goto failure;

  local_itoa(strrchr(strcpy(marker_name, "/proc/self/fd/"), '\000'), marker);
  marker_path = marker_name + 10; /* Skip "/proc/self"                       */
  if (sys_stat(marker_name, &marker_sb) < 0)
    goto failure;

  /* Make this process "dumpable". This is necessary in order to ptrace()
   * after having called setuid().
   */
  dumpable = sys_prctl(PR_GET_DUMPABLE, 0);
  if (!dumpable)
    sys_prctl(PR_SET_DUMPABLE, 1);

  /* Read process directories in /proc/...                                   */
  for (;;) {
    /* Some kernels know about threads, and hide them in "/proc" (although they
     * are still there, if you know the process id). Threads are moved into
     * a separate "task" directory. We check there first, and then fall back
     * on the older naming convention if necessary.
     */
    if (((proc = c_open("/proc/self/task/", O_RDONLY|O_DIRECTORY, 0)) < 0 &&
         (proc = c_open("/proc/", O_RDONLY|O_DIRECTORY, 0)) < 0) ||
        sys_fstat(proc, &proc_sb) < 0)
      goto failure;

    /* Since we are suspending threads, we cannot call any libc functions that
     * might acquire locks. Most notably, we cannot call malloc(). So, we have
     * to allocate memory on the stack, instead. Since we do not know how
     * much memory we need, we make a best guess. And if we guessed incorrectly
     * we retry on a second iteration (by jumping to "detach_threads").
     *
     * Unless the number of threads is increasing very rapidly, we should
     * never need to do so, though, as our guestimate is very conservative.
     */
    if (max_threads < proc_sb.st_nlink + 100)
      max_threads = proc_sb.st_nlink + 100;

    /* scope */ {
      pid_t pids[max_threads];
      int   result, added_entries = 0;
      for (;;) {
        struct dirent *entry;
        char buf[proc_sb.st_blksize];
        ssize_t nbytes = sys_getdents(proc, (struct dirent *)buf, sizeof(buf));
        if (nbytes < 0)
          goto failure;
        else if (nbytes == 0) {
          if (added_entries) {
            /* Need to keep iterating over "/proc" in multiple passes until
             * we no longer find any more threads. This algorithm eventually
             * completes, when all threads have been suspended.
             */
            added_entries = 0;
            sys_lseek(proc, 0, SEEK_SET);
            continue;
          }
          break;
        }
        for (entry = (struct dirent *)buf;
             entry < (struct dirent *)&buf[nbytes];
             entry = (struct dirent *)((char *)entry + entry->d_reclen)) {
          if (entry->d_ino != 0) {
            const char *ptr = entry->d_name;
            pid_t pid;

            /* Some kernels hide threads by preceding the pid with a '.'     */
            if (*ptr == '.')
              ptr++;

            /* If the directory is not numeric, it cannot be a process/thread*/
            if (*ptr < '0' || *ptr > '9')
              continue;
            pid = local_atoi(ptr);

            /* Attach (and suspend) all threads other than the current one   */
            if (pid && pid != my_pid) {
              struct stat tmp_sb;
              char fname[entry->d_reclen + 48];
              strcat(strcat(strcpy(fname, "/proc/"),
                            entry->d_name), marker_path);

              /* Check if the marker is identical to the one in our thread   */
              if (sys_stat(fname, &tmp_sb) >= 0 &&
                  marker_sb.st_dev == tmp_sb.st_dev &&
                  marker_sb.st_ino == tmp_sb.st_ino) {
                int i, j;

                /* Found one of our threads, make sure it is no duplicate    */
                for (i = 0; i < num_threads; i++) {
                  /* Linear search is slow, but should not matter much for
                   * the typically small number of threads.
                   */
                  if (pids[i] == pid) {
                    /* Found a duplicate; most likely on second pass of scan */
                    goto next_entry;
                  }
                }

                /* Check whether data structure needs growing                */
                if (num_threads >= max_threads) {
                  /* Back to square one, this time with more memory allocated*/
                  NO_INTR(sys_close(proc));
                  goto detach_threads;
                }

                /* Attaching to thread suspends it                           */
                if (sys_ptrace(PTRACE_ATTACH, pid, (void *)0, (void *)0) < 0) {
                  /* If operation failed, ignore thread. Maybe it just died?
                   * There might also be a race condition with a concurrent
                   * core dumper or with a debugger. In that case, we will
                   * just make a best effort, rather than failing entirely.
                   */
                  goto next_entry;
                }
                while (sys_waitpid(pid, (void *)0, __WALL) < 0) {
                  if (errno != EINTR) {
                    sys_ptrace(PTRACE_DETACH, pid, (void *)0, (void *)0);
                    goto next_entry;
                  }
                }

                if (sys_ptrace(PTRACE_PEEKDATA, pid, &i, &j) || i++ != j ||
                    sys_ptrace(PTRACE_PEEKDATA, pid, &i, &j) || i   != j) {
                  /* Address spaces are distinct, even though both processes
                   * show the "marker". This is probably a forked child
                   * process rather than a thread.
                   */
                  sys_ptrace(PTRACE_DETACH, pid, (void *)0, (void *)0);
                } else {
                  pids[num_threads++] = pid;
                  added_entries++;
                }
              }
            }
          }
       next_entry:;
        }
      }
      NO_INTR(sys_close(marker));
      NO_INTR(sys_close(proc));

      /* Now we are ready to call the callback,
       * which takes care of resuming the threads for us.
       */
      result = callback(parameter, num_threads, pids);

      /* Restore the "dumpable" state of the process                         */
      if (!dumpable)
        sys_prctl(PR_SET_DUMPABLE, dumpable);
      return result;

   detach_threads:
      /* Resume all threads prior to retrying the operation                  */
      ResumeAllProcessThreads(num_threads, pids);
      num_threads = 0;
      max_threads += 100;
    }
  }

failure:
  if (!dumpable)
    sys_prctl(PR_SET_DUMPABLE, dumpable);
  if (proc >= 0)
    NO_INTR(sys_close(proc));
  if (marker >= 0)
    NO_INTR(sys_close(marker));
  return -1;
}

/* This function resumes the list of all linux threads that
 * GetAllProcessThreads pauses before giving to its callback.
 */
void ResumeAllProcessThreads(int num_threads, pid_t *thread_pids) {
  while (num_threads-- > 0) {
    sys_ptrace(PTRACE_DETACH, thread_pids[num_threads], (void *)0, (void *)0);
  }
}

#endif
