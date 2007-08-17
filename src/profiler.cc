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
#include <stdlib.h>
#if defined HAVE_STDINT_H
#include <stdint.h>
#elif defined HAVE_INTTYPES_H
#include <inttypes.h>
#else
#include <sys/types.h>
#endif
#include <errno.h>
#ifdef HAVE_UCONTEXT_H
#include <ucontext.h>           // for ucontext_t (and also mcontext_t)
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <sys/time.h>
#include <string.h>
#include <fcntl.h>
#include <google/profiler.h>
#include <google/stacktrace.h>
#include "base/commandlineflags.h"
#include "base/logging.h"
#include "base/googleinit.h"
#include "base/mutex.h"
#include "base/spinlock.h"
#include "base/sysinfo.h"
#ifdef HAVE_CONFLICT_SIGNAL_H
#include "conflict-signal.h"          /* used on msvc machines */
#endif

using std::string;

DEFINE_string(cpu_profile, "",
              "Profile file name (used if CPUPROFILE env var not specified)");

// This takes as an argument an environment-variable name (like
// CPUPROFILE) whose value is supposed to be a file-path, and sets
// path to that path, and returns true.  If the env var doesn't exist,
// or is the empty string, leave path unchanged and returns false.
// The reason this is non-trivial is that this function handles munged
// pathnames.  Here's why:
//
// If we're a child process of the 'main' process, we can't just use
// getenv("CPUPROFILE") -- the parent process will be using that path.
// Instead we append our pid to the pathname.  How do we tell if we're a
// child process?  Ideally we'd set an environment variable that all
// our children would inherit.  But -- and this is seemingly a bug in
// gcc -- if you do a setenv() in a shared libarary in a global
// constructor, the environment setting is lost by the time main() is
// called.  The only safe thing we can do in such a situation is to
// modify the existing envvar.  So we do a hack: in the parent, we set
// the high bit of the 1st char of CPUPROFILE.  In the child, we
// notice the high bit is set and append the pid().  This works
// assuming cpuprofile filenames don't normally have the high bit set
// in their first character!  If that assumption is violated, we'll
// still get a profile, but one with an unexpected name.
// TODO(csilvers): set an envvar instead when we can do it reliably.
static bool GetUniquePathFromEnv(const char* env_name, string* path) {
  char* envval = getenv(env_name);
  if (envval == NULL || *envval == '\0')
    return false;
  if (envval[0] & 128) {                    // high bit is set
    char pid[64];              // pids are smaller than this!
    snprintf(pid, sizeof(pid), "%u", (unsigned int)(getpid()));
    *path = envval;
    *path += "_";
    *path += pid;
    (*path)[0] &= 127;
  } else {
    *path = string(envval);
    envval[0] |= 128;                       // set high bit for kids to see
  }
  return true;
}


// Collects up all profile data
class ProfileData {
 public:
  ProfileData();
  ~ProfileData();

  // Is profiling turned on at all
  inline bool enabled() const { return out_ >= 0; }

  // What is the frequency of interrupts (ticks per second)
  inline int frequency() const { return frequency_; }

  // Record an interrupt at "pc"
  void Add(void* pc);

  void FlushTable();

  // Start profiler to write profile info into fname
  bool Start(const char* fname);
  // Stop profiling and flush the data
  void Stop();

  void GetCurrentState(ProfilerState* state);

 private:
  static const int kMaxStackDepth = 64;         // Max stack depth profiled
  static const int kMaxFrequency = 4000;        // Largest allowed frequency
  static const int kDefaultFrequency = 100;     // Default frequency
  static const int kAssociativity = 4;          // For hashtable
  static const int kBuckets = 1 << 10;          // For hashtable
  static const int kBufferLength = 1 << 18;     // For eviction buffer

  // Type of slots: each slot can be either a count, or a PC value
  typedef uintptr_t Slot;

  // Hash-table/eviction-buffer entry
  struct Entry {
    Slot count;                  // Number of hits
    Slot depth;                  // Stack depth
    Slot stack[kMaxStackDepth];  // Stack contents
  };

  // Hash table bucket
  struct Bucket {
    Entry entry[kAssociativity];
  };

  // Invariant: table_lock_ is only grabbed by handler, or by other code
  // when the signal is being ignored (via SIG_IGN).
  //
  // Locking order is "state_lock_" first, and then "table_lock_"
  Mutex         state_lock_;    // Protects filename, etc.(not used in handler)
  SpinLock      table_lock_;    // SpinLock is safer in signal handlers
  Bucket*       hash_;          // hash table

  Slot*         evict_;         // evicted entries
  int           num_evicted_;   // how many evicted entries?
  int           out_;           // fd for output file
  int           count_;         // How many interrupts recorded
  int           evictions_;     // How many evictions
  size_t        total_bytes_;   // How much output
  char*         fname_;         // Profile file name
  int           frequency_;     // Interrupts per second
  time_t        start_time_;    // Start time, or 0

  // Add "pc -> count" to eviction buffer
  void Evict(const Entry& entry);

  // Write contents of eviction buffer to disk
  void FlushEvicted();

  // Handler that records the interrupted pc in the profile data
  static void prof_handler(int sig, siginfo_t*, void* signal_ucontext);

  // Sets the timer interrupt signal handler to one that stores the pc
  static void EnableHandler();
  // "Turn off" the timer interrupt signal handler
  static void DisableHandler();
};

// Evict the specified entry to the evicted-entry buffer
inline void ProfileData::Evict(const Entry& entry) {
  const int d = entry.depth;
  const int nslots = d + 2;     // Number of slots needed in eviction buffer
  if (num_evicted_ + nslots > kBufferLength) {
    FlushEvicted();
    assert(num_evicted_ == 0);
    assert(nslots <= kBufferLength);
  }
  evict_[num_evicted_++] = entry.count;
  evict_[num_evicted_++] = d;
  memcpy(&evict_[num_evicted_], entry.stack, d * sizeof(Slot));
  num_evicted_ += d;
}

// Initialize profiling: activated if getenv("CPUPROFILE") exists.
ProfileData::ProfileData()
    : hash_(0),
      evict_(0),
      num_evicted_(0),
      out_(-1),
      count_(0),
      evictions_(0),
      total_bytes_(0),
      fname_(0),
      frequency_(0),
      start_time_(0) {
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

  // Ignore signals until we decide to turn profiling on
  DisableHandler();

  ProfilerRegisterThread();

  // Should profiling be enabled automatically at start?
  string fname;
  if (!GetUniquePathFromEnv("CPUPROFILE", &fname)) {
    return;
  }
  // We don't enable profiling if setuid -- it's a security risk
#ifdef HAVE_GETEUID
  if (getuid() != geteuid())
    return;
#endif

  if (!Start(fname.c_str())) {
    RAW_LOG(FATAL, "Can't turn on cpu profiling for '%s': %s\n",
            fname.c_str(), strerror(errno));
  }
}

bool ProfileData::Start(const char* fname) {
  MutexLock l(&state_lock_);
  if (enabled()) {
    // profiling is already enabled
    return false;
  }

  // Open output file and initialize various data structures
  int fd = open(fname, O_CREAT | O_WRONLY | O_TRUNC, 0666);
  if (fd < 0) {
    // Can't open outfile for write
    return false;
  }

  start_time_ = time(NULL);
  fname_ = strdup(fname);

  {
    SpinLockHolder l2(&table_lock_);

    // Reset counters
    num_evicted_ = 0;
    count_       = 0;
    evictions_   = 0;
    total_bytes_ = 0;
    // But leave frequency_ alone (i.e., ProfilerStart() doesn't affect
    // their values originally set in the constructor)

    out_  = fd;

    hash_ = new Bucket[kBuckets];
    evict_ = new Slot[kBufferLength];
    memset(hash_, 0, sizeof(hash_[0]) * kBuckets);

    // Record special entries
    evict_[num_evicted_++] = 0;                     // count for header
    evict_[num_evicted_++] = 3;                     // depth for header
    evict_[num_evicted_++] = 0;                     // Version number
    evict_[num_evicted_++] = 1000000 / frequency_;  // Period (microseconds)
    evict_[num_evicted_++] = 0;                     // Padding

    // Must unlock before setting prof_handler to avoid deadlock
    // with signal delivered to this thread.
  }

  // Setup handler for SIGPROF interrupts
  EnableHandler();

  return true;
}

// Write out any collected profile data
ProfileData::~ProfileData() {
  Stop();
}

// Dump /proc/maps data to fd.  Copied from heap-profile-table.cc.
#define NO_INTR(fn)  do {} while ((fn) < 0 && errno == EINTR)

static void FDWrite(int fd, const char* buf, size_t len) {
  while (len > 0) {
    ssize_t r;
    NO_INTR(r = write(fd, buf, len));
    RAW_CHECK(r >= 0, "write failed");
    buf += r;
    len -= r;
  }
}

static void DumpProcSelfMaps(int fd) {
  ProcMapsIterator::Buffer iterbuf;
  ProcMapsIterator it(0, &iterbuf);   // 0 means "current pid"

  uint64 start, end, offset;
  int64 inode;
  char *flags, *filename;
  ProcMapsIterator::Buffer linebuf;
  while (it.Next(&start, &end, &flags, &offset, &inode, &filename)) {
    int written = it.FormatLine(linebuf.buf_, sizeof(linebuf.buf_),
                                start, end, flags, offset, inode, filename,
                                0);
    FDWrite(fd, linebuf.buf_, written);
  }
}

// Stop profiling and write out any collected profile data
void ProfileData::Stop() {
  MutexLock l(&state_lock_);

  // Prevent handler from running anymore
  DisableHandler();

  // This lock prevents interference with signal handlers in other threads
  SpinLockHolder l2(&table_lock_);

  if (out_ < 0) {
    // Profiling is not enabled
    return;
  }

  // Move data from hash table to eviction buffer
  for (int b = 0; b < kBuckets; b++) {
    Bucket* bucket = &hash_[b];
    for (int a = 0; a < kAssociativity; a++) {
      if (bucket->entry[a].count > 0) {
        Evict(bucket->entry[a]);
      }
    }
  }

  if (num_evicted_ + 3 > kBufferLength) {
    // Ensure there is enough room for end of data marker
    FlushEvicted();
  }

  // Write end of data marker
  evict_[num_evicted_++] = 0;         // count
  evict_[num_evicted_++] = 1;         // depth
  evict_[num_evicted_++] = 0;         // end of data marker
  FlushEvicted();

  // Dump "/proc/self/maps" so we get list of mapped shared libraries
  DumpProcSelfMaps(out_);

  close(out_);
  fprintf(stderr, "PROFILE: interrupts/evictions/bytes = %d/%d/%" PRIuS "\n",
          count_, evictions_, total_bytes_);
  delete[] hash_;
  hash_ = 0;
  delete[] evict_;
  evict_ = 0;
  free(fname_);
  fname_ = 0;
  start_time_ = 0;

  out_ = -1;
}

void ProfileData::GetCurrentState(ProfilerState* state) {
  MutexLock l(&state_lock_);
  if (enabled()) {
    state->enabled = true;
    state->start_time = start_time_;
    state->samples_gathered = count_;
    int buf_size = sizeof(state->profile_name);
    strncpy(state->profile_name, fname_, buf_size);
    state->profile_name[buf_size-1] = '\0';
  } else {
    state->enabled = false;
    state->start_time = 0;
    state->samples_gathered = 0;
    state->profile_name[0] = '\0';
  }
}

void ProfileData::EnableHandler() {
  struct sigaction sa;
  sa.sa_sigaction = prof_handler;
  sa.sa_flags = SA_RESTART | SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  RAW_CHECK(sigaction(SIGPROF, &sa, NULL) == 0, "sigaction failed");
}

void ProfileData::DisableHandler() {
  struct sigaction sa;
  sa.sa_handler = SIG_IGN;
  sa.sa_flags = SA_RESTART;
  sigemptyset(&sa.sa_mask);
  RAW_CHECK(sigaction(SIGPROF, &sa, NULL) == 0, "sigaction failed");
}


void ProfileData::FlushTable() {
  MutexLock l(&state_lock_);
  if (out_ < 0) {
    // Profiling is not enabled
    return;
  }
  DisableHandler();       // Disable timer interrupts while we're flushing
  {
    // Move data from hash table to eviction buffer
    SpinLockHolder l(&table_lock_);
    for (int b = 0; b < kBuckets; b++) {
      Bucket* bucket = &hash_[b];
      for (int a = 0; a < kAssociativity; a++) {
        if (bucket->entry[a].count > 0) {
          Evict(bucket->entry[a]);
          bucket->entry[a].depth = 0;
          bucket->entry[a].count = 0;
        }
      }
    }

    // Write out all pending data
    FlushEvicted();
  }
  EnableHandler();
}

// Record the specified "pc" in the profile data
void ProfileData::Add(void* pc) {
  void* stack[kMaxStackDepth];

  // The top-most active routine doesn't show up as a normal
  // frame, but as the "pc" value in the signal handler context.
  stack[0] = pc;

  // We remove the top three entries (typically Add, prof_handler, and
  // a signal handler setup routine) since they are artifacts of
  // profiling and should not be measured.  Other profiling related
  // frames (signal handler setup) will be removed by "pprof" at
  // analysis time.  Instead of skipping the top frames, we could skip
  // nothing, but that would increase the profile size unnecessarily.
  int depth = GetStackTrace(stack+1, kMaxStackDepth-1, 3);
  depth++;              // To account for pc value

  // Make hash-value
  Slot h = 0;
  for (int i = 0; i < depth; i++) {
    Slot slot = reinterpret_cast<Slot>(stack[i]);
    h = (h << 8) | (h >> (8*(sizeof(h)-1)));
    h += (slot * 31) + (slot * 7) + (slot * 3);
  }

  SpinLockHolder l(&table_lock_);

  // If the signal handler starts executing (and calls this function)
  // just as a thread calls Stop, it is possible for Stop to acquire
  // state_lock_, disable the signal, and acquire table_lock_ before
  // this function can grab table_lock_.  If that happens, Stop will
  // delete hash_ and the rest of the allocated structures before this
  // function grabs table_lock_ and continues.  In that case, hash_
  // will be NULL here.
  if (hash_ == NULL) {
    return;
  }

  count_++;

  // See if table already has an entry for this stack trace
  bool done = false;
  Bucket* bucket = &hash_[h % kBuckets];
  for (int a = 0; a < kAssociativity; a++) {
    Entry* e = &bucket->entry[a];
    if (e->depth == depth) {
      bool match = true;
      for (int i = 0; i < depth; i++) {
        if (e->stack[i] != reinterpret_cast<Slot>(stack[i])) {
          match = false;
          break;
        }
      }
      if (match) {
        e->count++;
        done = true;
        break;
      }
    }
  }

  if (!done) {
    // Evict entry with smallest count
    Entry* e = &bucket->entry[0];
    for (int a = 1; a < kAssociativity; a++) {
      if (bucket->entry[a].count < e->count) {
        e = &bucket->entry[a];
      }
    }
    if (e->count > 0) {
      evictions_++;
      Evict(*e);
    }

    // Use the newly evicted entry
    e->depth = depth;
    e->count = 1;
    for (int i = 0; i < depth; i++) {
      e->stack[i] = reinterpret_cast<Slot>(stack[i]);
    }
  }
}

// Write all evicted data to the profile file
void ProfileData::FlushEvicted() {
  if (num_evicted_ > 0) {
    const char* buf = reinterpret_cast<char*>(evict_);
    size_t bytes = sizeof(evict_[0]) * num_evicted_;
    total_bytes_ += bytes;
    FDWrite(out_, buf, bytes);
  }
  num_evicted_ = 0;
}

// Profile data structure: Constructor will check to see if profiling
// should be enabled.  Destructor will write profile data out to disk.
static ProfileData pdata;

// Signal handler that records the pc in the profile-data structure
void ProfileData::prof_handler(int sig, siginfo_t*, void* signal_ucontext) {
  int saved_errno = errno;
  pdata.Add(GetPC(*reinterpret_cast<ucontext_t*>(signal_ucontext)));
  errno = saved_errno;
}

// Start interval timer for the current thread.  We do this for
// every known thread.  If profiling is off, the generated signals
// are ignored, otherwise they are captured by prof_handler().
extern "C" void ProfilerRegisterThread() {
  // TODO: Randomize the initial interrupt value?
  // TODO: Randomize the inter-interrupt period on every interrupt?
  struct itimerval timer;
  timer.it_interval.tv_sec = 0;
  timer.it_interval.tv_usec = 1000000 / pdata.frequency();
  timer.it_value = timer.it_interval;
  setitimer(ITIMER_PROF, &timer, 0);
}

// DEPRECATED routines
extern "C" void ProfilerEnable() { }
extern "C" void ProfilerDisable() { }

extern "C" void ProfilerFlush() {
  pdata.FlushTable();
}

extern "C" bool ProfilingIsEnabledForAllThreads() {
  return pdata.enabled();
}

extern "C" bool ProfilerStart(const char* fname) {
  return pdata.Start(fname);
}

extern "C" void ProfilerStop() {
  pdata.Stop();
}

extern "C" void ProfilerGetCurrentState(ProfilerState* state) {
  pdata.GetCurrentState(state);
}


REGISTER_MODULE_INITIALIZER(profiler, {
  if (!FLAGS_cpu_profile.empty()) {
    ProfilerStart(FLAGS_cpu_profile.c_str());
  }
});
