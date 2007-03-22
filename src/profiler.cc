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
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>                 // for getuid() and geteuid()
#if defined HAVE_STDINT_H
#include <stdint.h>
#elif defined HAVE_INTTYPES_H
#include <inttypes.h>
#else
#include <sys/types.h>
#endif
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/time.h>
#include <string.h>
#include <fcntl.h>
#include "google/profiler.h"
#include "google/stacktrace.h"
#include "base/commandlineflags.h"
#include "base/googleinit.h"
#ifdef HAVE_CONFLICT_SIGNAL_H
#include "conflict-signal.h"          /* used on msvc machines */
#endif
#include "base/logging.h"

#ifndef	PATH_MAX
#ifdef MAXPATHLEN
#define	PATH_MAX	MAXPATHLEN
#else
#define	PATH_MAX	4096         // seems conservative for max filename len!
#endif
#endif

#if HAVE_PTHREAD
#  include <pthread.h>
#  define LOCK(m) pthread_mutex_lock(m)
#  define UNLOCK(m) pthread_mutex_unlock(m)
// Macro for easily checking return values from pthread routines
#  define PCALL(f) do { int __r = f;  if (__r != 0) { fprintf(stderr, "%s: %s\n", #f, strerror(__r)); abort(); } } while (0)
#else
#  define LOCK(m)
#  define UNLOCK(m)
#  define PCALL(f)
#endif

// For now, keep logging as a noop.  TODO: do something better?
#undef LOG
#define LOG(msg)

DEFINE_string(cpu_profile, "",
              "Profile file name (used if CPUPROFILE env var not specified)");

// Figure out how to get the PC for our architecture
#if defined HAVE_STRUCT_SIGINFO_SI_FADDR
typedef struct siginfo SigStructure;
inline void* GetPC(const SigStructure& sig_structure ) {
  return (void*)sig_structure.si_faddr; // maybe not correct
} 

#elif defined HAVE_STRUCT_SIGCONTEXT_SC_EIP
typedef struct sigcontext SigStructure;
inline void* GetPC(const SigStructure& sig_structure ) {
  return (void*)sig_structure.sc_eip;
}

#elif defined HAVE_STRUCT_SIGCONTEXT_EIP
typedef struct sigcontext SigStructure;
inline void* GetPC(const SigStructure& sig_structure ) {
  return (void*)sig_structure.eip;
}

#elif defined HAVE_STRUCT_SIGCONTEXT_RIP
typedef struct sigcontext SigStructure;
inline void* GetPC(const SigStructure& sig_structure ) {
  return (void*)sig_structure.rip;
}

#elif defined HAVE_STRUCT_SIGCONTEXT_SC_IP
typedef struct sigcontext SigStructure;
inline void* GetPC(const SigStructure& sig_structure ) {
  return (void*)sig_structure.ip;
}

#elif defined HAVE_STRUCT_UCONTEXT_UC_MCONTEXT
typedef struct ucontext SigStructure;
inline void* GetPC(const SigStructure& sig_structure ) {
  return (void*)sig_structure.uc_mcontext.gregs[REG_RIP];

#else
#error I dont know what your PC is

#endif


// Collects up all profile data
class ProfileData {
 public:
  ProfileData();
  ~ProfileData();

  // Is profiling turned on at all
  inline bool enabled() { return out_ >= 0; }
    
  // What is the frequency of interrupts (ticks per second)
  inline int frequency() { return frequency_; }

  // Record an interrupt at "pc"
  void Add(unsigned long pc);

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
    Slot count;                 // Number of hits
    Slot depth;                 // Stack depth
    Slot stack[kMaxStackDepth]; // Stack contents
  };

  // Hash table bucket
  struct Bucket {
    Entry entry[kAssociativity];
  };

#ifdef HAVE_PTHREAD
  // Invariant: table_lock_ is only grabbed by handler, or by other code
  // when the signal is being ignored (via SIG_IGN).
  //
  // Locking order is "state_lock_" first, and then "table_lock_"
  pthread_mutex_t state_lock_;  // Protects filename, etc.(not used in handler)
  pthread_mutex_t table_lock_;  // Cannot use "Mutex" in signal handlers
#endif
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
  static void prof_handler(int sig, SigStructure sig_structure );

  // Sets the timer interrupt signal handler to the specified routine
  static void SetHandler(void (*handler)(int));
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
ProfileData::ProfileData() :
  hash_(0),
  evict_(0),
  num_evicted_(0),
  out_(-1),
  count_(0),
  evictions_(0),
  total_bytes_(0),
  fname_(0),
  frequency_(0),
  start_time_(0) {

  PCALL(pthread_mutex_init(&state_lock_, NULL));
  PCALL(pthread_mutex_init(&table_lock_, NULL));

  // Get frequency of interrupts (if specified)
  char junk;
  const char* fr = getenv("PROFILEFREQUENCY");
  if (fr != NULL && (sscanf(fr, "%d%c", &frequency_, &junk) == 1) &&
      (frequency_ > 0)) {
    // Limit to kMaxFrequency
    frequency_ = (frequency_ > kMaxFrequency) ? kMaxFrequency : frequency_;
  } else {
    frequency_ = kDefaultFrequency;
  }

  // Ignore signals until we decide to turn profiling on
  SetHandler(SIG_IGN);

  ProfilerRegisterThread();

  // Should profiling be enabled automatically at start?
  char* cpuprofile = getenv("CPUPROFILE");
  if (!cpuprofile || cpuprofile[0] == '\0') {
    return;
  }
  // We don't enable profiling if setuid -- it's a security risk
  if (getuid() != geteuid())
    return;

  // If we're a child process of the 'main' process, we can't just use
  // the name CPUPROFILE -- the parent process will be using that.
  // Instead we append our pid to the name.  How do we tell if we're a
  // child process?  Ideally we'd set an environment variable that all
  // our children would inherit.  But -- and perhaps this is a bug in
  // gcc -- if you do a setenv() in a shared libarary in a global
  // constructor, the environment setting is lost by the time main()
  // is called.  The only safe thing we can do in such a situation is
  // to modify the existing envvar.  So we do a hack: in the parent,
  // we set the high bit of the 1st char of CPUPROFILE.  In the child,
  // we notice the high bit is set and append the pid().  This works
  // assuming cpuprofile filenames don't normally have the high bit
  // set in their first character!  If that assumption is violated,
  // we'll still get a profile, but one with an unexpected name.
  // TODO(csilvers): set an envvar instead when we can do it reliably.
  char fname[PATH_MAX];
  if (cpuprofile[0] & 128) {                    // high bit is set
    snprintf(fname, sizeof(fname), "%c%s_%u",   // add pid and clear high bit
             cpuprofile[0] & 127, cpuprofile+1, (unsigned int)(getpid()));
  } else {
    snprintf(fname, sizeof(fname), "%s", cpuprofile);
    cpuprofile[0] |= 128;                       // set high bit for kids to see
  }

  // process being profiled.  CPU profiles are messed up in that case.

  if (!Start(fname)) {
    fprintf(stderr, "Can't turn on cpu profiling: ");
    perror(fname); 
    exit(1); 
  }
}

bool ProfileData::Start(const char* fname) {
  LOCK(&state_lock_);
  if (enabled()) {
    // profiling is already enabled
    UNLOCK(&state_lock_);
    return false;
  }

  // Open output file and initialize various data structures
  int fd = open(fname, O_CREAT | O_WRONLY | O_TRUNC, 0666);
  if (fd < 0) {
    // Can't open outfile for write
    UNLOCK(&state_lock_);
    return false;
  }

  start_time_ = time(NULL);
  fname_ = strdup(fname);

  LOCK(&table_lock_);
  
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

  UNLOCK(&table_lock_);

  // Setup handler for SIGPROF interrupts
  SetHandler((void (*)(int)) prof_handler);

  UNLOCK(&state_lock_);
  return true;
}

// Write out any collected profile data
ProfileData::~ProfileData() {
  Stop();
}

// Stop profiling and write out any collected profile data
void ProfileData::Stop() {
  LOCK(&state_lock_);

  // Prevent handler from running anymore
  SetHandler(SIG_IGN);

  // This lock prevents interference with signal handlers in other threads
  LOCK(&table_lock_);

  if (out_ < 0) {
    // Profiling is not enabled
    UNLOCK(&table_lock_);
    UNLOCK(&state_lock_);
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
  int maps = open("/proc/self/maps", O_RDONLY);
  if (maps >= 0) {
    char buf[100];
    ssize_t r;
    while ((r = read(maps, buf, sizeof(buf))) > 0) {
      write(out_, buf, r);
    }
    close(maps);
  }

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
  UNLOCK(&table_lock_);
  UNLOCK(&state_lock_);
}

void ProfileData::GetCurrentState(ProfilerState* state) {
  LOCK(&state_lock_);
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
  UNLOCK(&state_lock_);
}

void ProfileData::SetHandler(void (*handler)(int)) {
  struct sigaction sa;
  sa.sa_handler = handler;
  sa.sa_flags   = SA_RESTART;
  sigemptyset(&sa.sa_mask);
  if (sigaction(SIGPROF, &sa, NULL) != 0) {
    perror("sigaction(SIGPROF)");
    exit(1);
  }
}

void ProfileData::FlushTable() {
  if (out_ < 0) {
    // Profiling is not enabled
    return;
  }

  LOCK(&state_lock_); {
    SetHandler(SIG_IGN);       // Disable timer interrupts while we're flushing
    LOCK(&table_lock_); {
      // Move data from hash table to eviction buffer
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
    } UNLOCK(&table_lock_);
    SetHandler((void (*)(int)) prof_handler);
  } UNLOCK(&state_lock_);
}

// Record the specified "pc" in the profile data
void ProfileData::Add(unsigned long pc) {
  void* stack[kMaxStackDepth];
  stack[0] = (void*)pc;
  int depth = GetStackTrace(stack+1, kMaxStackDepth-1, 
                            3/*Removes sighandlers*/);
  depth++;              // To account for pc value

  // Make hash-value
  Slot h = 0;
  for (int i = 0; i < depth; i++) {
    Slot slot = reinterpret_cast<Slot>(stack[i]);
    h = (h << 8) | (h >> (8*(sizeof(h)-1)));
    h += (slot * 31) + (slot * 7) + (slot * 3);
  }

  LOCK(&table_lock_);
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
  UNLOCK(&table_lock_);
}

// Write all evicted data to the profile file
void ProfileData::FlushEvicted() {
  if (num_evicted_ > 0) {
    const char* buf = reinterpret_cast<char*>(evict_);
    size_t bytes = sizeof(evict_[0]) * num_evicted_;
    total_bytes_ += bytes;
    while (bytes > 0) {
      ssize_t r = write(out_, buf, bytes);
      if (r < 0) {
        perror("write");
        exit(1);
      }
      buf += r;
      bytes -= r;
    }
  }
  num_evicted_ = 0;
}

// Profile data structure: Constructor will check to see if profiling
// should be enabled.  Destructor will write profile data out to disk.
static ProfileData pdata;

// Signal handler that records the pc in the profile-data structure
void ProfileData::prof_handler(int sig, SigStructure sig_structure) {
  int saved_errno = errno;
  pdata.Add( (unsigned long int)GetPC( sig_structure ) );
  errno = saved_errno;
}

// Start interval timer for the current thread.  We do this for
// every known thread.  If profiling is off, the generated signals
// are ignored, otherwise they are captured by prof_handler().
void ProfilerRegisterThread() {
  // TODO: Randomize the initial interrupt value?
  // TODO: Randomize the inter-interrupt period on every interrupt?
  struct itimerval timer;
  timer.it_interval.tv_sec = 0;
  timer.it_interval.tv_usec = 1000000 / pdata.frequency();
  timer.it_value = timer.it_interval;
  setitimer(ITIMER_PROF, &timer, 0);
}

// DEPRECATED routines
void ProfilerEnable() { }
void ProfilerDisable() { }

void ProfilerFlush() {
  pdata.FlushTable();
}

bool ProfilingIsEnabledForAllThreads() { 
  return pdata.enabled();
}

bool ProfilerStart(const char* fname) {
  return pdata.Start(fname);
}

void ProfilerStop() {
  pdata.Stop();
}

void ProfilerGetCurrentState(ProfilerState* state) {
  pdata.GetCurrentState(state);
}


REGISTER_MODULE_INITIALIZER(profiler, {
  if (!FLAGS_cpu_profile.empty()) {
    ProfilerStart(FLAGS_cpu_profile.c_str());
  }
});
