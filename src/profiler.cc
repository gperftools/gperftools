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

// On __x86_64__ systems, we may need _XOPEN_SOURCE defined to get access
// to the struct ucontext, via signal.h.  We need _GNU_SOURCE to get access
// to REG_RIP.  (We can use some trickery to get around that need, though.)
// Note this #define must come first!
#define _GNU_SOURCE 1
// If #define _GNU_SOURCE causes problems, this might work instead
// for __x86_64__.  It will cause problems for FreeBSD though!, because
// it turns off the needed __BSD_VISIBLE.
//#define _XOPEN_SOURCE 500
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
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <sys/time.h>
#include <string.h>
#include <fcntl.h>
#include <google/profiler.h>
#include <google/stacktrace.h>
#include "base/commandlineflags.h"
#include "base/googleinit.h"
#include "base/mutex.h"
#include "base/spinlock.h"
#ifdef HAVE_CONFLICT_SIGNAL_H
#include "conflict-signal.h"          /* used on msvc machines */
#endif
#include "base/logging.h"

using std::string;

DEFINE_string(cpu_profile, "",
              "Profile file name (used if CPUPROFILE env var not specified)");


// This might be used by GetPC, below.
// If the profiler interrupt happened just when the current function was
// entering the stack frame, or after leaving it but just before
// returning, then the stack trace cannot see the caller function
// anymore.
// GetPC tries to unwind the current function call in this case to avoid
// false edges in the profile graph skipping over a function.
// A static array of this struct helps GetPC detect these situations.
//
// This is a best effort patch -- if we fail to detect such a situation, or
// mess up the PC, nothing happens; the returned PC is not used for any
// further processing.
struct CallUnrollInfo {
  // Offset from (e)ip register where this instruction sequence should be
  // matched. Interpreted as bytes. Offset 0 is the next instruction to
  // execute. Be extra careful with negative offsets in architectures of
  // variable instruction length (like x86) - it is not that easy as taking
  // an offset to step one instruction back!
  int pc_offset;
  // The actual instruction bytes. Feel free to make it larger if you need
  // a longer sequence.
  char ins[16];
  // How many byutes to match from ins array?
  size_t ins_size;
  // The offset from the stack pointer (e)sp where to look for the call
  // return address. Interpreted as bytes.
  int return_sp_offset;
};


// TODO: gather the necessary instruction bytes for other architectures.
#if defined __i386
static CallUnrollInfo callunrollinfo[] = {
  // Entry to a function:  push %ebp;  mov  %esp,%ebp
  // Top-of-stack contains the caller IP.
  { 0,
    {0x55, 0x89, 0xe5}, 3,
    0
  },
  // Entry to a function, second instruction:  push %ebp;  mov  %esp,%ebp
  // Top-of-stack contains the old frame, caller IP is +4.
  { -1,
    {0x55, 0x89, 0xe5}, 3,
    4
  },
  // Return from a function: RET.
  // Top-of-stack contains the caller IP.
  { 0,
    {0xc3}, 1,
    0
  }
};

#endif

// Figure out how to get the PC for our architecture.

// __i386, including (at least) Linux
#if defined HAVE_STRUCT_SIGCONTEXT_EIP
typedef struct sigcontext SigStructure;
inline void* GetPC(const SigStructure& sig_structure ) {
#if defined __i386
  // See comment above struct CallUnrollInfo.
  // Only try instruction flow matching if both eip and esp looks
  // reasonable.
  if ((sig_structure.eip & 0xffff0000) != 0 &&
      (~sig_structure.eip & 0xffff0000) != 0 &&
      (sig_structure.esp & 0xffff0000) != 0) {
    char* eip = (char*)sig_structure.eip;
    for (int i = 0; i < arraysize(callunrollinfo); ++i) {
      if (!memcmp(eip + callunrollinfo[i].pc_offset, callunrollinfo[i].ins,
                  callunrollinfo[i].ins_size)) {
        // We have a match.
        void **retaddr = (void**)(sig_structure.esp +
                                  callunrollinfo[i].return_sp_offset);
        return *retaddr;
      }
    }
  }
#endif
  return (void*)sig_structure.eip;
}

// freebsd (__i386, I assume) and Mac OS X (i386)
#elif defined HAVE_STRUCT_SIGCONTEXT_SC_EIP
typedef struct sigcontext SigStructure;
inline void* GetPC(const SigStructure& sig_structure ) {
  return (void*)sig_structure.sc_eip;
}

// __ia64
#elif defined HAVE_STRUCT_SIGCONTEXT_SC_IP
typedef struct sigcontext SigStructure;
inline void* GetPC(const SigStructure& sig_structure ) {
  return (void*)sig_structure.sc_ip;
}

// __x86_64__
// This may require _XOPEN_SOURCE to have access to ucontext
#elif defined HAVE_STRUCT_UCONTEXT_UC_MCONTEXT
typedef struct ucontext SigStructure;
inline void* GetPC(const SigStructure& sig_structure ) {
  // For this architecture, the regs are stored in a data structure that can
  // be accessed either as a flat array or (via some trickery) as a struct.
  // Alas, the index we need into the array, is only defined for _GNU_SOURCE.
  // Lacking that, we can interpret the register array as a struct sigcontext.
  // This works in practice, but is probably not guaranteed by anything.
# ifdef REG_RIP    // only defined if _GNU_SOURCE is 1, probably
  return (void*)(sig_structure.uc_mcontext.gregs[REG_RIP]);
# else
  const struct sigcontext* const sc =
      reinterpret_cast<const struct sigcontext*>(&sig_structure.uc_mcontext.gregs);
  return (void*)(sc->rip);
# endif
}

// solaris (probably solaris-x86, but I'm not sure)
#elif defined HAVE_STRUCT_SIGINFO_SI_FADDR
typedef struct siginfo SigStructure;
inline void* GetPC(const SigStructure& sig_structure ) {
  return (void*)sig_structure.si_faddr; // maybe not correct
}

// mac (OS X) powerpc, 32 bit (64-bit would use sigcontext64, on OS X 10.4+)
#elif defined HAVE_STRUCT_SIGCONTEXT_SC_IR
typedef struct sigcontext SigStructure;
inline void* GetPC(const SigStructure& sig_structure ) {
  return (void*)sig_structure.sc_ir;  // a guess, based on the comment /* pc */
}

// ibook powerpc
#elif defined HAVE_STRUCT_SIGCONTEXT_REGS__NIP
typedef struct sigcontext SigStructure;
inline void* GetPC(const SigStructure& sig_structure ) {
  return (void*)sig_structure.regs->nip;
}

#else
#error I dont know what your PC is

#endif

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
  SetHandler(SIG_IGN);

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
    fprintf(stderr, "Can't turn on cpu profiling: ");
    perror(fname.c_str());
    exit(1);
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
  SetHandler((void (*)(int)) prof_handler);

  return true;
}

// Write out any collected profile data
ProfileData::~ProfileData() {
  Stop();
}

// Stop profiling and write out any collected profile data
void ProfileData::Stop() {
  MutexLock l(&state_lock_);

  // Prevent handler from running anymore
  SetHandler(SIG_IGN);

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
  MutexLock l(&state_lock_);
  if (out_ < 0) {
    // Profiling is not enabled
    return;
  }
  SetHandler(SIG_IGN);       // Disable timer interrupts while we're flushing
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
  SetHandler((void (*)(int)) prof_handler);
}

// Record the specified "pc" in the profile data
void ProfileData::Add(unsigned long pc) {
  void* stack[kMaxStackDepth];

  // The top-most active routine doesn't show up as a normal
  // frame, but as the "pc" value in the signal handler context.
  stack[0] = (void*)pc;

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
