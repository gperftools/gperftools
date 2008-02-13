// Copyright (c) 2005, Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// ---
// Author: Sanjay Ghemawat
//         Chris Demetriou (refactoring)
//
// Profile current program by sampling stack-trace every so often
//
// TODO: Detect whether or not setitimer() applies to all threads in
// the process.  If so, instead of starting and stopping by changing
// the signal handler, start and stop by calling setitimer() and
// do nothing in the per-thread registration code.

#include "config.h"
#include "getpc.h"      // should be first to get the _GNU_SOURCE dfn
#include <signal.h>
#include <assert.h>
#include <stdio.h>
#include <errno.h>
#include <ucontext.h>
#include <string.h>
#include <sys/time.h>
#include <string>
#include <google/profiler.h>
#include <google/stacktrace.h>
#include "base/commandlineflags.h"
#include "base/logging.h"
#include "base/googleinit.h"
#include "base/spinlock.h"
#include "base/sysinfo.h"             /* for GetUniquePathFromEnv, etc */
#include "profiledata.h"
#ifdef HAVE_CONFLICT_SIGNAL_H
#include "conflict-signal.h"          /* used on msvc machines */
#endif

using std::string;

DEFINE_string(cpu_profile, "",
              "Profile file name (used if CPUPROFILE env var not specified)");

// Collects up all profile data.  This is a singleton, which is
// initialized by a constructor at startup.
class CpuProfiler {
 public:
  CpuProfiler();
  ~CpuProfiler();

  // Start profiler to write profile info into fname
  bool Start(const char* fname, bool (*filter)(void*), void* filter_arg);

  // Stop profiling and write the data to disk.
  void Stop();

  // Write the data to disk (and continue profiling).
  void FlushTable();

  bool Enabled();

  void GetCurrentState(ProfilerState* state);

  // Start interval timer for the current thread.  We do this for
  // every known thread.  If profiling is off, the generated signals
  // are ignored, otherwise they are captured by prof_handler().
  void RegisterThread();

  static CpuProfiler instance_;

 private:
  static const int kMaxFrequency = 4000;        // Largest allowed frequency
  static const int kDefaultFrequency = 100;     // Default frequency

  // Sample frequency, read-only after construction.
  int           frequency_;

  // These locks implement the locking requirements described in the
  // ProfileData documentation, specifically:
  //
  // control_lock_ is held all over all collector_ method calls except for
  // the 'Add' call made from the signal handler, to protect against
  // concurrent use of collector_'s control routines.
  //
  // signal_lock_ is held over calls to 'Start', 'Stop', 'Flush', and
  // 'Add', to protect against concurrent use of data collection and
  // writing routines.  Code other than the signal handler must disable
  // the timer signal while holding signal_lock, to prevent deadlock.
  //
  // Locking order is control_lock_ first, and then signal_lock_.
  // signal_lock_ is acquired by the prof_handler without first
  // acquiring control_lock_.
  SpinLock      control_lock_;
  SpinLock      signal_lock_;
  ProfileData   collector_;

  // Filter function and its argument, if any.  (NULL means include
  // all samples).  Set at start, read-only while running.  Written
  // while holding both control_lock_ and signal_lock_, read and
  // executed under signal_lock_.
  bool          (*filter_)(void*);
  void*         filter_arg_;

  // Sets the timer interrupt signal handler to one that stores the pc.
  static void EnableHandler();

  // Disables (ignores) the timer interrupt signal.
  static void DisableHandler();

  // Signale handler that records the interrupted pc in the profile data
  static void prof_handler(int sig, siginfo_t*, void* signal_ucontext);
};

// Profile data structure singleton: Constructor will check to see if
// profiling should be enabled.  Destructor will write profile data
// out to disk.
CpuProfiler CpuProfiler::instance_;

// Initialize profiling: activated if getenv("CPUPROFILE") exists.
CpuProfiler::CpuProfiler() {
  // Get frequency of interrupts (if specified)
  char junk;
  const char* fr = getenv("CPUPROFILE_FREQUENCY");
  if (fr != NULL && (sscanf(fr, "%d%c", &frequency_, &junk) == 1) &&
      (frequency_ > 0)) {
    // Limit to kMaxFrequency
    frequency_ = (frequency_ > kMaxFrequency) ? kMaxFrequency : frequency_;
  } else {
    frequency_ = kDefaultFrequency;
  }

  // Ignore signals until we decide to turn profiling on.  (Paranoia;
  // should already be ignored.)
  DisableHandler();

  RegisterThread();

  // Should profiling be enabled automatically at start?
  char fname[PATH_MAX];
  if (!GetUniquePathFromEnv("CPUPROFILE", fname)) {
    return;
  }
  // We don't enable profiling if setuid -- it's a security risk
#ifdef HAVE_GETEUID
  if (getuid() != geteuid())
    return;
#endif

  if (!Start(fname, NULL, NULL)) {
    RAW_LOG(FATAL, "Can't turn on cpu profiling for '%s': %s\n",
            fname, strerror(errno));
  }
}

bool CpuProfiler::Start(const char* fname,
                        bool (*filter)(void*), void* filter_arg) {
  SpinLockHolder cl(&control_lock_);

  if (collector_.enabled()) {
    return false;
  }

  {
    // spin lock really is needed to protect init here, since it's
    // conceivable that prof_handler may still be running from a
    // previous profiler run.  (For instance, if prof_handler just
    // started, had not grabbed the spinlock, then was switched out,
    // it might start again right now.)  Any such late sample will be
    // recorded against the new profile, but there's no harm in that.
    SpinLockHolder sl(&signal_lock_);

    if (!collector_.Start(fname, frequency_)) {
      return false;
    }

    filter_ = filter;
    filter_arg_ = filter_arg;

    // Must unlock before setting prof_handler to avoid deadlock
    // with signal delivered to this thread.
  }

  // Setup handler for SIGPROF interrupts
  EnableHandler();

  return true;
}

CpuProfiler::~CpuProfiler() {
  Stop();
}

// Stop profiling and write out any collected profile data
void CpuProfiler::Stop() {
  SpinLockHolder cl(&control_lock_);

  if (!collector_.enabled()) {
    return;
  }

  // Ignore timer signals.  Note that the handler may have just
  // started and might not have taken signal_lock_ yet.  Holding
  // signal_lock_ here along with the semantics of collector_.Add()
  // (which does nothing if collection is not enabled) prevents that
  // late sample from causing a problem.
  DisableHandler();

  {
    SpinLockHolder sl(&signal_lock_);
    collector_.Stop();
  }
}

void CpuProfiler::FlushTable() {
  SpinLockHolder cl(&control_lock_);

  if (!collector_.enabled()) {
    return;
  }

  // Disable timer signal while hoding signal_lock_, to prevent deadlock
  // if we take a timer signal while flushing.
  DisableHandler();
  {
    SpinLockHolder sl(&signal_lock_);
    collector_.FlushTable();
  }
  EnableHandler();
}

bool CpuProfiler::Enabled() {
  SpinLockHolder cl(&control_lock_);
  return collector_.enabled();
}

void CpuProfiler::GetCurrentState(ProfilerState* state) {
  ProfileData::State collector_state;
  {
    SpinLockHolder cl(&control_lock_);
    collector_.GetCurrentState(&collector_state);
  }

  state->enabled = collector_state.enabled;
  state->start_time = static_cast<time_t>(collector_state.start_time);
  state->samples_gathered = collector_state.samples_gathered;
  int buf_size = sizeof(state->profile_name);
  strncpy(state->profile_name, collector_state.profile_name, buf_size);
  state->profile_name[buf_size-1] = '\0';
}

void CpuProfiler::RegisterThread() {
  // TODO: Randomize the initial interrupt value?
  // TODO: Randomize the inter-interrupt period on every interrupt?
  struct itimerval timer;
  timer.it_interval.tv_sec = 0;
  timer.it_interval.tv_usec = 1000000 / frequency_;
  timer.it_value = timer.it_interval;
  setitimer(ITIMER_PROF, &timer, 0);
}

void CpuProfiler::EnableHandler() {
  struct sigaction sa;
  sa.sa_sigaction = prof_handler;
  sa.sa_flags = SA_RESTART | SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  RAW_CHECK(sigaction(SIGPROF, &sa, NULL) == 0, "sigaction failed");
}

void CpuProfiler::DisableHandler() {
  struct sigaction sa;
  sa.sa_handler = SIG_IGN;
  sa.sa_flags = SA_RESTART;
  sigemptyset(&sa.sa_mask);
  RAW_CHECK(sigaction(SIGPROF, &sa, NULL) == 0, "sigaction failed");
}

// Signal handler that records the pc in the profile-data structure
//
// NOTE: it is possible for profiling to be disabled just as this
// signal handler starts, before signal_lock_ is acquired.  Therefore,
// collector_.Add must check whether profiling is enabled before
// trying to record any data.  (See also comments in Start and Stop.)
void CpuProfiler::prof_handler(int sig, siginfo_t*, void* signal_ucontext) {
  int saved_errno = errno;

  // Hold the spin lock while we're gathering the trace because there's
  // no real harm in holding it and there's little point in releasing
  // and re-acquiring it.  (We'll only be blocking Start, Stop, and
  // Flush.)  We make sure to release it before restoring errno.
  {
    SpinLockHolder sl(&instance_.signal_lock_);

    if (instance_.filter_ == NULL ||
        (*instance_.filter_)(instance_.filter_arg_)) {
      void* stack[ProfileData::kMaxStackDepth];

      // The top-most active routine doesn't show up as a normal
      // frame, but as the "pc" value in the signal handler context.
      stack[0] = GetPC(*reinterpret_cast<ucontext_t*>(signal_ucontext));

      // We skip the top two stack trace entries (this function and one
      // signal handler frame) since they are artifacts of profiling and
      // should not be measured.  Other profiling related frames may be
      // removed by "pprof" at analysis time.  Instead of skipping the top
      // frames, we could skip nothing, but that would increase the
      // profile size unnecessarily.
      int depth = GetStackTrace(stack + 1, arraysize(stack) - 1, 2);
      depth++;              // To account for pc value in stack[0];

      instance_.collector_.Add(depth, stack);
    }
  }

  errno = saved_errno;
}

extern "C" void ProfilerRegisterThread() {
  CpuProfiler::instance_.RegisterThread();
}

// DEPRECATED routines
extern "C" void ProfilerEnable() { }
extern "C" void ProfilerDisable() { }

extern "C" void ProfilerFlush() {
  CpuProfiler::instance_.FlushTable();
}

extern "C" bool ProfilingIsEnabledForAllThreads() {
  return CpuProfiler::instance_.Enabled();
}

extern "C" bool ProfilerStart(const char* fname) {
  return CpuProfiler::instance_.Start(fname, NULL, NULL);
}

extern "C" bool ProfilerStartFiltered(const char* fname,
                                      bool (*filter_in_thread)(void* arg),
                                      void *filter_in_thread_arg) {
  return CpuProfiler::instance_.Start(fname, filter_in_thread,
                                      filter_in_thread_arg);
}

extern "C" void ProfilerStop() {
  CpuProfiler::instance_.Stop();
}

extern "C" void ProfilerGetCurrentState(ProfilerState* state) {
  CpuProfiler::instance_.GetCurrentState(state);
}


REGISTER_MODULE_INITIALIZER(profiler, {
  if (!FLAGS_cpu_profile.empty()) {
    ProfilerStart(FLAGS_cpu_profile.c_str());
  }
});
