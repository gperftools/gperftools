/* Copyright (c) 2006, Google Inc.
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
 * Author: Sanjay Ghemawat
 */

#include "config.h"
#include <time.h>       /* For nanosleep() */
#include <sched.h>      /* For sched_yield() */
#ifdef HAVE_UNISTD_H
#include <unistd.h>     /* For nanosleep() on Windows, read() */
#endif
#if defined(__MACH__) && defined(__APPLE__)
#include <sys/types.h>
#include <sys/sysctl.h> /* how we figure out numcpu's on OS X */
#endif
#include <fcntl.h>      /* for open(), O_RDONLY */
#include <string.h>     /* for strncmp */
#include <errno.h>
#include "base/spinlock.h"

static int adaptive_spin_count = 0;
static int num_cpus = -1;

// It's important this not call malloc! -- it is called at global-construct
// time, before we've set up all our proper malloc hooks and such.
static int NumCPUs() {
  if (num_cpus > 0)
    return num_cpus;         // value is cached

#if defined(__MACH__) && defined(__APPLE__)
  int cpus;
  size_t size = sizeof(cpus);
  int numcpus_name[] = { CTL_HW, HW_NCPU };
  if (::sysctl(numcpus_name, arraysize(numcpus_name), &cpus, &size, 0, 0) == 0
      && (size == sizeof(cpus)))
    num_cpus = cpus;
  else
    num_cpus = 1;           // conservative assumption
  return num_cpus;
#else   /* hope the information is in /proc */
  const char* pname = "/proc/cpuinfo";
  int fd = open(pname, O_RDONLY);
  if (fd == -1) {
    num_cpus = 1;            // conservative assumption; TODO: do better
    return num_cpus;
  }
  char line[1024];
  int chars_read;
  num_cpus = 0;
  do {   // a line at a time
    chars_read = 0;
    char* linepos = line;
    while (linepos - line < sizeof(line)-1 &&
           (chars_read=read(fd, linepos, 1)) == 1 &&
           *linepos != '\n')   // read one line
      linepos++;
    *linepos = '\0';           // terminate the line at the \n
    if (strncmp(line, "processor  ", sizeof("processor  ")-1) == 0)
      num_cpus++;
  } while (chars_read > 0);    // stop when we get to EOF
  close(fd);
  if (num_cpus == 0)
    num_cpus = 1;              // conservative assumption
  return num_cpus;
#endif
}

struct SpinLock_InitHelper {
  SpinLock_InitHelper() {
    // On multi-cpu machines, spin for longer before yielding
    // the processor or sleeping.  Reduces idle time significantly.
    if (NumCPUs() > 1) {
      adaptive_spin_count = 1000;
    }
  }
};

// Hook into global constructor execution:
// We do not do adaptive spinning before that,
// but nothing lock-intensive should be going on at that time.
static SpinLock_InitHelper init_helper;

void SpinLock::SlowLock() {
  int saved_errno = errno; // save and restore errno for signal safety
  int c = adaptive_spin_count;

  // Spin a few times in the hope that the lock holder releases the lock
  while ((c > 0) && (lockword_ != 0)) {
    c--;
  }

  if (lockword_ == 1) {
    sched_yield();          // Spinning failed. Let's try to be gentle.
  }

  while (Acquire_CompareAndSwap(&lockword_, 0, 1) != 0) {
    // This code was adapted from the ptmalloc2 implementation of
    // spinlocks which would sched_yield() upto 50 times before
    // sleeping once for a few milliseconds.  Mike Burrows suggested
    // just doing one sched_yield() outside the loop and always
    // sleeping after that.  This change helped a great deal on the
    // performance of spinlocks under high contention.  A test program
    // with 10 threads on a dual Xeon (four virtual processors) went
    // from taking 30 seconds to 16 seconds.

    // Sleep for a few milliseconds
    struct timespec tm;
    tm.tv_sec = 0;
    tm.tv_nsec = 2000001;
    nanosleep(&tm, NULL);
  }
  errno = saved_errno;
}
