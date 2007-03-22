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
// All Rights Reserved.
//
// Author: Maxim Lifantsev
//

// NOTE: We almost never use CHECK and LOG in this module
//       because we might be running before/after the logging susbystem
//       is set up correctly.

#include "config.h"

#include <string>
#include <vector>
#include <map>
#include <google/perftools/hash_set.h>
#include <algorithm>

#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/poll.h>
#include <sys/types.h>
#include <fcntl.h>
#include <assert.h>

#ifdef HAVE_LINUX_PTRACE_H
#include <linux/ptrace.h>
#endif
#ifdef HAVE_SYSCALL_H
#include <syscall.h>
#endif

#include <google/stacktrace.h>
#include <google/heap-profiler.h>
#include <google/heap-checker.h>
#include "heap-profiler-inl.h"
#include "addressmap-inl.h"

#include "base/basictypes.h"
#include "base/commandlineflags.h"
#include "base/logging.h"
#include "base/elfcore.h"              // for i386_regs
#include "base/thread_lister.h"

#ifdef HAVE_INTTYPES_H
#define __STDC_FORMAT_MACROS
#include <inttypes.h>
// TODO: have both SCNd64 and PRId64.  We don't bother since they're the same
#define LLX    "%"SCNx64             // how to read 64-bit hex
#define LLD    "%"SCNd64             // how to read 64-bit deciman
#else
#define LLX    "%llx"                // hope for the best
#define LLD    "%lld"
#endif

#ifndef	PATH_MAX
#ifdef MAXPATHLEN
#define	PATH_MAX	MAXPATHLEN
#else
#define	PATH_MAX	4096         // seems conservative for max filename len!
#endif
#endif

using std::string;
using std::map;
using std::vector;
using std::swap;
using std::make_pair;
using std::min;
using std::max;
using HASH_NAMESPACE::hash_set;

//----------------------------------------------------------------------
// Flags that control heap-checking
//----------------------------------------------------------------------

DEFINE_bool(heap_check_report, true,
            "If overall heap check should report the found leaks via pprof");

// These are not so much flags as internal configuration parameters that
// are set based on the argument to StartFromMain().
DEFINE_bool(heap_check_before_constructors, true,
            "deprecated; pretty much always true now");

DEFINE_bool(heap_check_after_destructors, false,
            "If overall heap check is to end after global destructors "
            "or right after all REGISTER_HEAPCHECK_CLEANUP's");

DEFINE_bool(heap_check_strict_check, true,
            "If overall heap check is to be done "
            "via HeapLeakChecker::*SameHeap "
            "or HeapLeakChecker::*NoLeaks call");
            // heap_check_strict_check == false
            // is useful only when heap_check_before_constructors == false

DEFINE_bool(heap_check_ignore_global_live, true,
            "If overall heap check is to ignore heap objects reachable "
            "from the global data");

DEFINE_bool(heap_check_ignore_thread_live, true,
            "If set to true, objects reachable from thread stacks "
            "and registers are not reported as leaks");

// Normally we'd make this a flag, but we can't do that in this case
// because it may need to be accessed after global destructors have
// started to run, which would delete flags.  Instead we make it a pointer,
// which will never get destroyed.
static string* flags_heap_profile_pprof = NULL;

// External accessors for the above
void HeapLeakChecker::set_heap_check_report(bool b) {
  FLAGS_heap_check_report = b;
}
void HeapLeakChecker::set_pprof_path(const char* s) {
  if (flags_heap_profile_pprof == NULL) {
    flags_heap_profile_pprof = new string(s);
  } else {
    flags_heap_profile_pprof->assign(s);
  }
}

void HeapLeakChecker::set_dump_directory(const char* s) {
  if (dump_directory_ == NULL)  dump_directory_ = new string;
  dump_directory_->assign(s);
}

bool HeapLeakChecker::heap_check_report() {
  return FLAGS_heap_check_report;
}
const char* HeapLeakChecker::pprof_path() {
  if (flags_heap_profile_pprof == NULL) {
    return INSTALL_PREFIX "/bin/pprof";  // our default value
  } else {
    return flags_heap_profile_pprof->c_str();
  }
}
const char* HeapLeakChecker::dump_directory() {
  if (dump_directory_ == NULL) {
    return "/tmp";  // our default value
  } else {
    return dump_directory_->c_str();
  }
}

//----------------------------------------------------------------------

DECLARE_int32(heap_profile_log); // in heap-profiler.cc

//----------------------------------------------------------------------
// HeapLeakChecker global data
//----------------------------------------------------------------------

// Global lock for the global data of this module
static pthread_mutex_t heap_checker_lock = PTHREAD_MUTEX_INITIALIZER;

// the disabled regexp accumulated
// via HeapLeakChecker::DisableChecksIn
static string* disabled_regexp = NULL;

//----------------------------------------------------------------------

// Heap profile prefix for leak checking profiles,
static string* profile_prefix = NULL;

// whole-program heap leak checker
static HeapLeakChecker* main_heap_checker = NULL;

// if we are doing (or going to do) any kind of heap-checking
// heap_checker_on == true implies HeapProfiler::is_on_ == true
static bool heap_checker_on = false;
// pid of the process that does whole-program heap leak checking
static pid_t heap_checker_pid = 0;

// if we did heap profiling during global constructors execution
static bool constructor_heap_profiling = false;

//----------------------------------------------------------------------
// HeapLeakChecker live object tracking components
//----------------------------------------------------------------------

// Cases of live object placement we distinguish
enum ObjectPlacement {
  MUST_BE_ON_HEAP,  // Must point to a live object of the matching size in the
                    // map of the heap in HeapProfiler when we get to it.
  IGNORED_ON_HEAP,  // Is a live (ignored) object on heap.
  IN_GLOBAL_DATA,   // Is part of global data region of the executable.
  THREAD_STACK,     // Part of a thread stack
};

// Information about an allocated object
struct AllocObject {
  void* ptr;              // the object
  uintptr_t size;         // its size
  ObjectPlacement place;  // where ptr points to

  AllocObject(void* p, size_t s, ObjectPlacement l)
    : ptr(p), size(s), place(l) { }
};

// All objects (memory ranges) ignored via HeapLeakChecker::IgnoreObject
// Key is the object's address; value is its size.
typedef map<uintptr_t, size_t> IgnoredObjectsMap;
static IgnoredObjectsMap* ignored_objects = NULL;

// All objects (memory ranges) that we consider to be the sources of pointers
// to live (not leaked) objects.
// At different times this holds (what can be reached from) global data regions
// and the objects we've been told to ignore.
// For any AllocObject::ptr "live_objects" is supposed to contain at most one
// record at any time. We maintain this by checking with HeapProfiler's map
// of the heap and removing the live heap objects we've handled from it.
// This vector is maintained as a stack and the frontier of reachable
// live heap objects in our flood traversal of them.
typedef vector<AllocObject> LiveObjectsStack;
static LiveObjectsStack* live_objects = NULL;

// A placeholder to fill-in the starting values for live_objects
// for each library so we can keep the library-name association for logging.
typedef map<string, LiveObjectsStack> LibraryLiveObjectsStacks;
static LibraryLiveObjectsStacks* library_live_objects = NULL;

// The disabled program counter addresses for profile dumping
// that are registered with HeapLeakChecker::DisableChecksUp
typedef hash_set<uintptr_t> DisabledAddressSet;
static DisabledAddressSet* disabled_addresses = NULL;

// Value stored in the map of disabled address ranges;
// its key is the end of the address range.
// We'll ignore allocations with a return address in a disabled range
// if the address occurs at 'max_depth' or less in the stack trace.
struct HeapLeakChecker::RangeValue {
  uintptr_t start_address;  // the start of the range
  int       max_depth;      // the maximal stack depth to disable at
};
typedef map<uintptr_t, HeapLeakChecker::RangeValue> DisabledRangeMap;
// The disabled program counter address ranges for profile dumping
// that are registered with HeapLeakChecker::DisableChecksFromTo.
static DisabledRangeMap* disabled_ranges = NULL;

// Stack range map: maps from the start address to the end address.
// These are used to not disable all allocated memory areas
// that are used for stacks so that we do treat stack pointers
// from dead stack frmes as live.
typedef map<uintptr_t, uintptr_t> StackRangeMap;
static StackRangeMap* stack_ranges = NULL;

// We put the registers from other threads here
// to make pointers stored in them live.
static vector<void*>* thread_registers = NULL;

// This routine is called for every thread stack we know about.
static void RegisterStackRange(void* top, void* bottom) {
  char* p1 = min(reinterpret_cast<char*>(top),
                 reinterpret_cast<char*>(bottom));
  char* p2 = max(reinterpret_cast<char*>(top),
                 reinterpret_cast<char*>(bottom));
  if (HeapProfiler::kMaxLogging) {
    HeapProfiler::MESSAGE(1, "HeapChecker: Thread stack %p..%p (%d bytes)\n",
                          p1, p2, int(p2-p1));
  }
  live_objects->push_back(AllocObject(p1, uintptr_t(p2-p1), THREAD_STACK));
  stack_ranges->insert(make_pair(reinterpret_cast<uintptr_t>(p1),
                                 reinterpret_cast<uintptr_t>(p2)));
}

// Iterator for HeapProfiler::allocation_ to make objects allocated from
// disabled code regions live.
static void MakeDisabledLiveCallback(void* ptr, HeapProfiler::AllocValue v) {
  bool stack_disable = false;
  bool range_disable = false;
  for (int depth = 0; depth < v.bucket->depth_; depth++) {
    uintptr_t addr = reinterpret_cast<uintptr_t>(v.bucket->stack_[depth]);
    if (disabled_addresses  &&
        disabled_addresses->find(addr) != disabled_addresses->end()) {
      stack_disable = true;  // found; dropping
      break;
    }
    if (disabled_ranges) {
      DisabledRangeMap::const_iterator iter
        = disabled_ranges->upper_bound(addr);
      if (iter != disabled_ranges->end()) {
        assert(iter->first > addr);
        if (iter->second.start_address < addr  &&
            iter->second.max_depth > depth) {
          range_disable = true;  // in range; dropping
          break;
        }
      }
    }
  }
  if (stack_disable || range_disable) {
    uintptr_t start_address = reinterpret_cast<uintptr_t>(ptr);
    uintptr_t end_address = start_address + v.bytes;
    StackRangeMap::const_iterator iter
      = stack_ranges->lower_bound(start_address);
    if (iter != stack_ranges->end()) {
      assert(iter->first >= start_address);
      if (iter->second <= end_address) {
        // We do not disable (treat as live) whole allocated regions
        // if they are used to hold thread call stacks
        // (i.e. when we find a stack inside).
        // The reason is that we'll treat as live the currently used
        // stack portions anyway (see RegisterStackRange),
        // and the rest of the region where the stack lives can well
        // contain outdated stack variables which are not live anymore,
        // hence should not be treated as such.
        HeapProfiler::MESSAGE(2, "HeapChecker: "
                                 "Not %s-disabling %"PRIuS" bytes at %p"
                                 ": have stack inside: %p-%p\n",
                                 (stack_disable ? "stack" : "range"),
                                 v.bytes, ptr,
                                 (void*)iter->first, (void*)iter->second);
        return;
      }
    }
    if (HeapProfiler::kMaxLogging) {
      HeapProfiler::MESSAGE(2, "HeapChecker: "
                               "%s-disabling %"PRIuS" bytes at %p\n",
                               (stack_disable ? "stack" : "range"),
                               v.bytes, ptr);
    }
    live_objects->push_back(AllocObject(ptr, v.bytes, MUST_BE_ON_HEAP));
  }
}

static int GetStatusOutput(const char*  command, string* output) {
  // We don't want the heapchecker to run in the child helper
  // processes that we fork() as part of this process' heap check.

  // setenv() can call realloc(), so we don't want to call it while
  // the heap profiling is disabled. Instead just overwrite the final
  // char of the env var name, so it has a different name and gets
  // ignored in the child.  We assume the env looks like 'VAR=VALUE\0VAR=VALUE'
  char *env_heapcheck = getenv("HEAPCHECK");
  char *env_ldpreload = getenv("LD_PRELOAD");

  if (env_heapcheck) {
    assert(env_heapcheck[-1] == '=');
    env_heapcheck[-2] = '?';
  }
  if (env_ldpreload) {
    assert(env_ldpreload[-1] == '=');
    env_ldpreload[-2] = '?';
  }

  FILE* f = popen(command, "r");
  if (f == NULL) {
    fprintf(stderr, "popen returned NULL!!!\n");  // This shouldn't happen
    exit(1);
  }

  if (env_heapcheck) env_heapcheck[-2] = 'K';
  if (env_ldpreload) env_heapcheck[-2] = 'D';

  const int kMaxOutputLine = 10000;
  char line[kMaxOutputLine];
  while (fgets(line, sizeof(line), f) != NULL) {
    if (output)
      *output += line;
  }

  return pclose(f);
}

// A ProcMapsTask to record global data to ignore later
// that belongs to 'library' mapped at 'start_address' with 'file_offset'.
static void RecordGlobalDataLocked(const char* library,
                                   uint64 start_address,
                                   uint64 file_offset) {
  HeapProfiler::MESSAGE(2, "HeapChecker: Looking into %s\n", library);
  string command("/usr/bin/objdump -hw ");
  command.append(library);
  string output;
  if (GetStatusOutput(command.c_str(), &output) != 0) {
    HeapProfiler::MESSAGE(-1, "HeapChecker: "
                          "Failed executing %s\n", command.c_str());
    abort();
  }
  const char* output_start = output.c_str();

  if (FLAGS_heap_profile_log >= 5) {
    HeapProfiler::MESSAGE(5, "HeapChecker: Looking at objdump\n");
    write(STDERR_FILENO, output.data(), output.size());
  }

  while (1) {
    char sec_name[11];
    uint64 sec_size, sec_vmaddr, sec_lmaddr, sec_offset;
    if (sscanf(output_start, "%*d .%10s "LLX" "LLX" "LLX" "LLX" ",
               sec_name, &sec_size, &sec_vmaddr,
               &sec_lmaddr, &sec_offset) == 5) {
      if (strcmp(sec_name, "data") == 0 ||
          strcmp(sec_name, "bss") == 0) {
        uint64 real_start = start_address + sec_offset - file_offset;
        HeapProfiler::MESSAGE(4, "HeapChecker: "
                              "Got section %s: %p of "LLX" bytes\n",
                              sec_name,
                              reinterpret_cast<void*>(real_start),
                              sec_size);
        (*library_live_objects)[library].
          push_back(AllocObject(reinterpret_cast<void*>(real_start),
                                sec_size, IN_GLOBAL_DATA));
      }
    }
    // skip to the next line
    const char* next = strpbrk(output_start, "\n\r");
    if (next == NULL) break;
    output_start = next + 1;
  }
}

// See if 'library' from /proc/self/maps has base name 'library_base'
// i.e. contains it and has '.' or '-' after it.
static bool IsLibraryNamed(const char* library, const char* library_base) {
  const char* p = strstr(library, library_base);
  size_t sz = strlen(library_base);
  return p != NULL  &&  (p[sz] == '.'  ||  p[sz] == '-');
}

void HeapLeakChecker::DisableLibraryAllocs(const char* library,
                                           void* start_address,
                                           void* end_address) {
  // TODO(maxim): maybe this should be extended to also use objdump
  //              and pick the text portion of the library more precisely.
  if (IsLibraryNamed(library, "/libpthread")  ||
        // pthread has a lot of small "system" leaks we don't care about
      IsLibraryNamed(library, "/libdl")  ||
      IsLibraryNamed(library, "/ld")  ||
        // library loaders leak some "system" heap that we don't care about
      IsLibraryNamed(library, "/libcrypto")
      // Sometimes libcrypto of OpenSSH is compiled with -fomit-frame-pointer
      // (any library can be, of course, but this one often is because speed
      // is so important for making crypto usable).  We ignore all its
      // allocations because we can't see the call stacks.
     ) {
    HeapProfiler::MESSAGE(1, "HeapChecker: "
                          "Disabling direct allocations from %s :\n",
                          library);
    DisableChecksFromTo(start_address, end_address,
                        1);  // only disable allocation calls directly
                             // from the library code
  }
}

void HeapLeakChecker::UseProcMaps(ProcMapsTask proc_maps_task) {
  FILE* const fp = fopen("/proc/self/maps", "r");
  if (!fp) {
    int errsv = errno;
    HeapProfiler::MESSAGE(-1, "HeapChecker:  "
                          "Could not open /proc/self/maps: errno=%d.  "
                          "Libraries will not be handled correctly.\n",
                          errsv);
    return;
  }
  char proc_map_line[1024];
  while (fgets(proc_map_line, sizeof(proc_map_line), fp) != NULL) {
    // All lines starting like
    // "401dc000-4030f000 r??p 00132000 03:01 13991972  lib/bin"
    // identify a data and code sections of a shared library or our binary
    uint64 start_address, end_address, file_offset, inode;
    int size;
    char permissions[5];
    if (sscanf(proc_map_line, LLX"-"LLX" %4s "LLX" %*x:%*x "LLD" %n",
               &start_address, &end_address, permissions,
               &file_offset, &inode, &size) != 5) continue;
    proc_map_line[strlen(proc_map_line) - 1] = '\0';  // zap the newline
    HeapProfiler::MESSAGE(4, "HeapChecker: "
                          "Looking at /proc/self/maps line:\n  %s\n",
                          proc_map_line);
    if (proc_maps_task == DISABLE_LIBRARY_ALLOCS  &&
        strncmp(permissions, "r-xp", 4) == 0  &&  inode != 0) {
      if (start_address >= end_address)  abort();
      DisableLibraryAllocs(proc_map_line + size,
                           reinterpret_cast<void*>(start_address),
                           reinterpret_cast<void*>(end_address));
    }
    if (proc_maps_task == RECORD_GLOBAL_DATA_LOCKED  &&
        // grhat based on Red Hat Linux 9
        (strncmp(permissions, "rw-p", 4) == 0  ||
         // Fedora Core 3
         strncmp(permissions, "rwxp", 4) == 0)  &&
        inode != 0) {
      if (start_address >= end_address)  abort();
      RecordGlobalDataLocked(proc_map_line + size, start_address, file_offset);
    }
  }
  fclose(fp);
}

// Total number and size of live objects dropped from the profile.
static int64 live_objects_total = 0;
static int64 live_bytes_total = 0;

// Arguments from the last call to IgnoreLiveThreads,
// so we can resume the threads later.
static int last_num_threads = 0;
static pid_t* last_thread_pids = NULL;

// Callback for GetAllProcessThreads to ignore
// thread stacks and registers for all our threads.
static int IgnoreLiveThreads(void* parameter,
                             int num_threads,
                             pid_t* thread_pids) {
  last_num_threads = num_threads;
  assert(last_thread_pids == NULL);
  last_thread_pids = new pid_t[num_threads];
  memcpy(last_thread_pids, thread_pids, num_threads * sizeof(pid_t));

  int failures = 0;
  for (int i = 0; i < num_threads; ++i) {
    if (HeapProfiler::kMaxLogging) {
      HeapProfiler::MESSAGE(2, "HeapChecker: Handling thread with pid %d\n",
                            thread_pids[i]);
    }
#if defined(HAVE_LINUX_PTRACE_H) && defined(HAVE_SYSCALL_H) && defined(DUMPER)
    i386_regs thread_regs;
#define sys_ptrace(r,p,a,d)  syscall(SYS_ptrace, (r), (p), (a), (d))
    // We use sys_ptrace to avoid thread locking
    // because this is called from GetAllProcessThreads
    // when all but this thread are suspended.
    // (This does not seem to matter much though: allocations and
    //  logging with HeapProfiler::MESSAGE seem to work just fine.)
    if (sys_ptrace(PTRACE_GETREGS, thread_pids[i], NULL, &thread_regs) == 0) {
      void* stack_top;
      void* stack_bottom;
      if (GetStackExtent((void*) thread_regs.BP, &stack_top, &stack_bottom)) {
        // Need to use SP, not BP here to also get the data
        // from the very last stack frame:
        RegisterStackRange((void*) thread_regs.SP, stack_bottom);
      } else {
        failures += 1;
      }
      // Make registers live (just in case PTRACE_ATTACH resulted in some
      // register pointers still being in the registers and not on the stack):
      for (void** p = (void**)&thread_regs;
           p < (void**)(&thread_regs + 1); ++p) {
        if (HeapProfiler::kMaxLogging) {
          HeapProfiler::MESSAGE(3, "HeapChecker: Thread register %p\n", *p);
        }
        thread_registers->push_back(*p);
      }
    } else {
      failures += 1;
    }
#else
    failures += 1;
#endif
  }
  return failures;
}

// Info about the self thread stack extent
struct HeapLeakChecker::StackExtent {
  bool have;
  void* top;
  void* bottom;
};

// For this call we are free to call new/delete from this thread:
// heap profiler will ignore them without acquiring its lock:
void HeapLeakChecker::
IgnoreAllLiveObjectsLocked(const StackExtent& self_stack) {
  if (live_objects)  abort();
  live_objects = new LiveObjectsStack;
  thread_registers = new vector<void*>;
  IgnoreObjectLocked(thread_registers, true);
    // in case we are not ignoring global data
  stack_ranges = new StackRangeMap;
  if (HeapProfiler::ignored_objects_)  abort();
  HeapProfiler::ignored_objects_ = new HeapProfiler::IgnoredObjectSet;
  // Record global data as live:
  // We need to do it before we stop the threads in GetAllProcessThreads below;
  // otherwise deadlocks are possible
  // when we try to fork to execute objdump in UseProcMaps.
  if (FLAGS_heap_check_ignore_global_live) {
    library_live_objects = new LibraryLiveObjectsStacks;
    UseProcMaps(RECORD_GLOBAL_DATA_LOCKED);
  }
  // Ignore all thread stacks:
  if (FLAGS_heap_check_ignore_thread_live) {
    // We fully suspend the threads right here before any liveness checking
    // and keep them suspended for the whole time of liveness checking
    // (they can't (de)allocate due to profiler's lock but they could still
    //  mess with the pointer graph while we walk it).
    int r = GetAllProcessThreads(NULL, IgnoreLiveThreads);
    if (r == -1) {
      HeapProfiler::MESSAGE(0, "HeapChecker: Could not find thread stacks; "
                               "may get false leak reports\n");
    } else if (r != 0) {
      HeapProfiler::MESSAGE(0, "HeapChecker: Thread stacks not found "
                               "for %d threads; may get false leak reports\n",
                            r);
    }
    IgnoreLiveObjectsLocked("thread (stack) data", "");
  }
  // Register our own stack:
  if (HeapProfiler::kMaxLogging) {
    HeapProfiler::MESSAGE(2, "HeapChecker: Handling self thread with pid %d\n",
                          getpid());
  }
  if (self_stack.have) {
    RegisterStackRange(self_stack.top, self_stack.bottom);
      // DoNoLeaks sets these
    IgnoreLiveObjectsLocked("stack data", "");
  } else {
    HeapProfiler::MESSAGE(0, "HeapChecker: Stack not found "
                             "for this thread; may get false leak reports\n");
  }
  // Make objects we were told to ignore live:
  if (ignored_objects) {
    HeapProfiler::AllocValue alloc_value;
    for (IgnoredObjectsMap::const_iterator object = ignored_objects->begin();
         object != ignored_objects->end(); ++object) {
      void* ptr = reinterpret_cast<void*>(object->first);
      live_objects->
        push_back(AllocObject(ptr, object->second, MUST_BE_ON_HEAP));
      // we do this liveness check for ignored_objects before doing any
      // live heap walking to make sure it does not fail needlessly:
      bool have_on_heap =
        HeapProfiler::HaveOnHeapLocked(&ptr, &alloc_value);
      if (!(have_on_heap  &&  object->second == alloc_value.bytes)) {
        HeapProfiler::MESSAGE(-1, "HeapChecker: "
                              "%p of %"PRIuS" bytes "
                              "from an IgnoreObject() disappeared\n",
                              ptr, object->second);
        abort();
      }
    }
    IgnoreLiveObjectsLocked("ignored objects", "");
  }
  // Make code-address-disabled objects live and ignored:
  // This in particular makes all thread-specific data live
  // because the basic data structure to hold pointers to thread-specific data
  // is allocated from libpthreads and we have range-disabled that
  // library code with UseProcMaps(DISABLE_LIBRARY_ALLOCS);
  // so now we declare all thread-specific data reachable from there as live.
  HeapProfiler::allocation_->Iterate(MakeDisabledLiveCallback);
  IgnoreLiveObjectsLocked("disabled code", "");
  // Actually make global data live:
  if (FLAGS_heap_check_ignore_global_live) {
    for (LibraryLiveObjectsStacks::iterator l = library_live_objects->begin();
         l != library_live_objects->end(); ++l) {
      if (live_objects->size()) abort();
      live_objects->swap(l->second);
      IgnoreLiveObjectsLocked("in globals of\n  ", l->first.c_str());
    }
    delete library_live_objects;
  }
  // Can now resume the threads:
  if (FLAGS_heap_check_ignore_thread_live) {
    ResumeAllProcessThreads(last_num_threads, last_thread_pids);
    delete [] last_thread_pids;
    last_thread_pids = NULL;
  }
  if (live_objects_total) {
    HeapProfiler::MESSAGE(0, "HeapChecker: "
                          "Ignoring "LLD" reachable "
                          "objects of "LLD" bytes\n",
                          live_objects_total, live_bytes_total);
  }
  // Free these: we made them here and heap profiler never saw them
  delete live_objects;
  live_objects = NULL;
  ignored_objects->erase(reinterpret_cast<uintptr_t>(thread_registers));
  delete thread_registers;
  thread_registers = NULL;
  delete stack_ranges;
  stack_ranges = NULL;
}

// This function does not change HeapProfiler's state:
// we record ignored live objects in HeapProfiler::ignored_objects_
// instead of modifying the heap profile.
void HeapLeakChecker::IgnoreLiveObjectsLocked(const char* name,
                                              const char* name2) {
  int64 live_object_count = 0;
  int64 live_byte_count = 0;
  while (!live_objects->empty()) {
    void* object = live_objects->back().ptr;
    size_t size = live_objects->back().size;
    const ObjectPlacement place = live_objects->back().place;
    live_objects->pop_back();
    HeapProfiler::AllocValue alloc_value;
    if (place == MUST_BE_ON_HEAP  &&
        HeapProfiler::HaveOnHeapLocked(&object, &alloc_value)  &&
        HeapProfiler::ignored_objects_
          ->insert(reinterpret_cast<uintptr_t>(object)).second) {
      live_object_count += 1;
      live_byte_count += size;
    }
    HeapProfiler::MESSAGE(5, "HeapChecker: "
                          "Looking for heap pointers "
                          "in %p of %"PRIuS" bytes\n", object, size);
    // Try interpretting any byte sequence in object,size as a heap pointer
    const size_t alignment = sizeof(void*);
      // alignment at which we should consider pointer positions here
      // use 1 if any alignment is ok
    const size_t remainder = reinterpret_cast<uintptr_t>(object) % alignment;
    if (remainder) {
      reinterpret_cast<char*&>(object) += alignment - remainder;
      if (size >= alignment - remainder) {
        size -= alignment - remainder;
      } else {
        size = 0;
      }
    }
    while (size >= sizeof(void*)) {
// TODO(jandrews): Make this part of the configure script.
#define UNALIGNED_LOAD32(_p) (*reinterpret_cast<const uint32 *>(_p))
      void* ptr = reinterpret_cast<void*>(UNALIGNED_LOAD32(object));
      void* current_object = object;
      reinterpret_cast<char*&>(object) += alignment;
      size -= alignment;
      if (ptr == NULL)  continue;
      HeapProfiler::MESSAGE(8, "HeapChecker: "
                            "Trying pointer to %p at %p\n",
                            ptr, current_object);
      // Do not need the following since the data for these
      // is not recorded by heap-profiler:
      // if (ptr == live_objects  ||
      //     ptr == HeapProfiler::ignored_objects_)  continue;
      if (HeapProfiler::HaveOnHeapLocked(&ptr, &alloc_value)  &&
          HeapProfiler::ignored_objects_
            ->insert(reinterpret_cast<uintptr_t>(ptr)).second) {
        // We take the (hopefully low) risk here of encountering by accident
        // a byte sequence in memory that matches an address of
        // a heap object which is in fact leaked.
        // I.e. in very rare and probably not repeatable/lasting cases
        // we might miss some real heap memory leaks.
        HeapProfiler::MESSAGE(5, "HeapChecker: "
                              "Found pointer to %p"
                              " of %"PRIuS" bytes at %p\n",
                              ptr, alloc_value.bytes, current_object);
        live_object_count += 1;
        live_byte_count += alloc_value.bytes;
        live_objects->push_back(AllocObject(ptr, alloc_value.bytes,
                                            IGNORED_ON_HEAP));
      }
    }
  }
  live_objects_total += live_object_count;
  live_bytes_total += live_byte_count;
  if (live_object_count) {
    HeapProfiler::MESSAGE(1, "HeapChecker: "
                          "Removed "LLD" live heap objects"
                          " of "LLD" bytes: %s%s\n",
                          live_object_count, live_byte_count, name, name2);
  }
}

//----------------------------------------------------------------------
// HeapLeakChecker leak check disabling components
//----------------------------------------------------------------------

void HeapLeakChecker::DisableChecksUp(int stack_frames) {
  if (!heap_checker_on) return;
  if (stack_frames < 1)  abort();
  void* stack[1];
  if (GetStackTrace(stack, 1, stack_frames) != 1)  abort();
  DisableChecksAt(stack[0]);
}

void HeapLeakChecker::DisableChecksAt(void* address) {
  if (!heap_checker_on) return;
  if (pthread_mutex_lock(&heap_checker_lock) != 0)  abort();
  DisableChecksAtLocked(address);
  if (pthread_mutex_unlock(&heap_checker_lock) != 0)  abort();
}

bool HeapLeakChecker::HaveDisabledChecksUp(int stack_frames) {
  if (!heap_checker_on) return false;
  if (stack_frames < 1)  abort();
  void* stack[1];
  if (GetStackTrace(stack, 1, stack_frames) != 1)  abort();
  return HaveDisabledChecksAt(stack[0]);
}

bool HeapLeakChecker::HaveDisabledChecksAt(void* address) {
  if (!heap_checker_on) return false;
  if (pthread_mutex_lock(&heap_checker_lock) != 0)  abort();
  bool result = disabled_addresses != NULL  &&
                disabled_addresses->
                  find(reinterpret_cast<uintptr_t>(address)) !=
                disabled_addresses->end();
  if (pthread_mutex_unlock(&heap_checker_lock) != 0)  abort();
  return result;
}

void HeapLeakChecker::DisableChecksIn(const char* pattern) {
  if (!heap_checker_on) return;
  if (pthread_mutex_lock(&heap_checker_lock) != 0)  abort();
  DisableChecksInLocked(pattern);
  if (pthread_mutex_unlock(&heap_checker_lock) != 0)  abort();
}

void* HeapLeakChecker::GetDisableChecksStart() {
  if (!heap_checker_on) return NULL;
  void* start_address;
  if (GetStackTrace(&start_address, 1, 0) != 1)  abort();
  return start_address;
}

void HeapLeakChecker::DisableChecksToHereFrom(void* start_address) {
  if (!heap_checker_on) return;
  void* end_address;
  if (GetStackTrace(&end_address, 1, 0) != 1)  abort();
  if (start_address > end_address)  swap(start_address, end_address);
  DisableChecksFromTo(start_address, end_address,
                      10000);  // practically no stack depth limit:
                               // heap profiler keeps much shorter stack traces
}

void HeapLeakChecker::IgnoreObject(void* ptr) {
  if (!heap_checker_on) return;
  if (pthread_mutex_lock(&heap_checker_lock) != 0)  abort();
  IgnoreObjectLocked(ptr, false);
  if (pthread_mutex_unlock(&heap_checker_lock) != 0)  abort();
}

void HeapLeakChecker::IgnoreObjectLocked(void* ptr, bool profiler_locked) {
  HeapProfiler::AllocValue alloc_value;
  if (profiler_locked ? HeapProfiler::HaveOnHeapLocked(&ptr, &alloc_value)
                      : HeapProfiler::HaveOnHeap(&ptr, &alloc_value)) {
    HeapProfiler::MESSAGE(1, "HeapChecker: "
                          "Going to ignore live object "
                          "at %p of %"PRIuS" bytes\n",
                          ptr, alloc_value.bytes);
    if (ignored_objects == NULL)  {
      ignored_objects = new IgnoredObjectsMap;
      IgnoreObjectLocked(ignored_objects, profiler_locked);
        // ignore self in case we are not ignoring global data
    }
    if (!ignored_objects->insert(make_pair(reinterpret_cast<uintptr_t>(ptr),
                                           alloc_value.bytes)).second) {
      HeapProfiler::MESSAGE(-1, "HeapChecker: "
                            "%p is already being ignored\n", ptr);
      abort();
    }
  }
}

void HeapLeakChecker::UnIgnoreObject(void* ptr) {
  if (!heap_checker_on) return;
  HeapProfiler::AllocValue alloc_value;
  if (pthread_mutex_lock(&heap_checker_lock) != 0)  abort();
  bool ok = HeapProfiler::HaveOnHeap(&ptr, &alloc_value);
  if (ok) {
    ok = false;
    if (ignored_objects) {
      IgnoredObjectsMap::iterator object =
        ignored_objects->find(reinterpret_cast<uintptr_t>(ptr));
      if (object != ignored_objects->end()  &&
          alloc_value.bytes == object->second) {
        ignored_objects->erase(object);
        ok = true;
        HeapProfiler::MESSAGE(1, "HeapChecker: "
                              "Now not going to ignore live object "
                              "at %p of %"PRIuS" bytes\n",
                              ptr, alloc_value.bytes);
      }
    }
  }
  if (pthread_mutex_unlock(&heap_checker_lock) != 0)  abort();
  if (!ok) {
    HeapProfiler::MESSAGE(-1, "HeapChecker: "
                          "%p has not been ignored\n", ptr);
    abort();
  }
}

//----------------------------------------------------------------------
// HeapLeakChecker non-static functions
//----------------------------------------------------------------------

void HeapLeakChecker::DumpProfileLocked(bool start,
                                        const StackExtent& self_stack) {
  assert(!HeapProfiler::dumping_);  // not called from dumping code
  HeapProfiler::MESSAGE(0, "HeapChecker: %s check \"%s\"\n",
                        (start ? "Starting" : "Ending"), name_);
  // Make the heap profile while letting our thread work with the heap
  // without profiling this activity into the regular heap profile,
  // while at the same time we hold the lock
  // and do not let other threads work with the heap:
  assert(!HeapProfiler::self_disable_);
  HeapProfiler::self_disabled_tid_ = pthread_self();
  // stop normal heap profiling in our thread:
  HeapProfiler::self_disable_ = true;
  { // scope
    IgnoreAllLiveObjectsLocked(self_stack);
    HeapProfiler::dump_for_leaks_ = true;
    string* file_name = new string(*profile_prefix + "." + name_ +
                                   (start ? "-beg.heap" : "-end.heap"));
    HeapProfiler::DumpLocked("leak check", file_name->c_str());
    delete file_name;  // want explicit control of the destruction point
    HeapProfiler::dump_for_leaks_ = false;
    delete HeapProfiler::ignored_objects_;
    HeapProfiler::ignored_objects_ = NULL;
  }
  // resume normal heap profiling in our thread:
  HeapProfiler::self_disable_ = false;
  // Check that we made no heap changes ourselves
  // while normal heap profiling was paused:
  int64 self_disabled_bytes = HeapProfiler::self_disabled_.alloc_size_ -
                              HeapProfiler::self_disabled_.free_size_;
  int64 self_disabled_allocs = HeapProfiler::self_disabled_.allocs_ -
                               HeapProfiler::self_disabled_.frees_;
  if (self_disabled_bytes != 0  ||  self_disabled_allocs != 0) {
    HeapProfiler::MESSAGE(-1, "HeapChecker: "
                          "internal HeapChecker leak of "LLD" objects "
                          "and/or "LLD" bytes\n",
                          self_disabled_allocs, self_disabled_bytes);
    abort();
  }
}

void HeapLeakChecker::Create(const char *name) {
  name_ = NULL;
  has_checked_ = false;
  char* n = new char[strlen(name) + 1];   // do this before we lock
  IgnoreObject(n);  // otherwise it might be treated as live due to our stack
  if (pthread_mutex_lock(&heap_checker_lock) != 0)  abort();
  // Heap activity in other threads is paused for this whole function.
  HeapProfiler::Lock();
  if (heap_checker_on) {
    assert(strchr(name, '/') == NULL);  // must be a simple name
    assert(name_ == NULL);  // so this is not a memory leak
    name_ = n;
    memcpy(name_, name, strlen(name) + 1);
    // get our stack range to make its proper portion live
    StackExtent self_stack;
    self_stack.have = GetStackExtent(NULL, &self_stack.top, &self_stack.bottom);
    DumpProfileLocked(true, self_stack);  // start
    start_inuse_bytes_ = static_cast<size_t>(HeapProfiler::profile_.alloc_size_ -
                                             HeapProfiler::profile_.free_size_);
    start_inuse_allocs_ = static_cast<size_t>(HeapProfiler::profile_.allocs_ -
                                              HeapProfiler::profile_.frees_);
    if (HeapProfiler::kMaxLogging) {
      HeapProfiler::MESSAGE(1, "HeapChecker: "
                               "Start check \"%s\" profile: "
                               "%"PRIuS"d bytes in %"PRIuS"d objects\n",
                               name_, start_inuse_bytes_, start_inuse_allocs_);
    }
  } else {
    HeapProfiler::MESSAGE(-1, "HeapChecker: "
                          "Heap checker is not active, "
                          "hence checker \"%s\" will do nothing!\n", name);
    HeapProfiler::MESSAGE(-1, "HeapChecker: "
                          "To activate set the HEAPCHECK environment "
                          "variable.\n");
  }
  HeapProfiler::Unlock();
  if (pthread_mutex_unlock(&heap_checker_lock) != 0)  abort();
  if (name_ == NULL) {
    UnIgnoreObject(n);
    delete[] n;  // must be done after we unlock
  }
}

HeapLeakChecker::HeapLeakChecker(const char *name) {
  assert(strcmp(name, "_main_") != 0);  // reserved
  Create(name);
}

DECLARE_int64(heap_profile_allocation_interval);
DECLARE_int64(heap_profile_inuse_interval);

// Save pid of main thread for using in naming dump files
int32 HeapLeakChecker::main_thread_pid_ = getpid();
// Directory in which to dump profiles
string* HeapLeakChecker::dump_directory_ = NULL;
#ifdef HAVE_PROGRAM_INVOCATION_NAME
extern char* program_invocation_name;
extern char* program_invocation_short_name;
static const char* invocation_name() { return program_invocation_short_name; }
static const char* invocation_path() { return program_invocation_name; }
#else
static const char* invocation_name() { return "heap_checker"; }
static const char* invocation_path() { return "heap_checker"; }  // I guess?
#endif

HeapLeakChecker::HeapLeakChecker() {
  Create("_main_");
}

ssize_t HeapLeakChecker::BytesLeaked() const {
  if (!has_checked_) {
    HeapProfiler::MESSAGE(-1, "HeapChecker: "
                          "*NoLeaks|SameHeap must execute before this call\n");
    abort();
  }
  return inuse_bytes_increase_;
}

ssize_t HeapLeakChecker::ObjectsLeaked() const {
  if (!has_checked_) {
    HeapProfiler::MESSAGE(-1, "HeapChecker: "
                          "*NoLeaks|SameHeap must execute before this call\n");
    abort();
  }
  return inuse_allocs_increase_;
}

bool HeapLeakChecker::DoNoLeaks(bool same_heap,
                                bool do_full,
                                bool do_report) {
  // Heap activity in other threads is paused for this function
  // until we got all profile difference info.
  if (pthread_mutex_lock(&heap_checker_lock) != 0)  abort();
  HeapProfiler::Lock();
  if (heap_checker_on) {
    if (name_ == NULL) {
      HeapProfiler::MESSAGE(-1, "HeapChecker: "
                            "*NoLeaks|SameHeap must be called only once"
                            " and profiling must be not turned on "
                            "after construction of a HeapLeakChecker\n");
      abort();
    }
    // get our stack range to make its proper portion live
    StackExtent self_stack;
    self_stack.have = GetStackExtent(NULL, &self_stack.top, &self_stack.bottom);
    DumpProfileLocked(false, self_stack);  // end
    const bool use_initial_profile =
      !(FLAGS_heap_check_before_constructors  &&  this == main_heap_checker);
    if (!use_initial_profile) {  // compare against empty initial profile
      start_inuse_bytes_ = 0;
      start_inuse_allocs_ = 0;
    }
    int64 end_inuse_bytes = HeapProfiler::profile_.alloc_size_ -
                            HeapProfiler::profile_.free_size_;
    int64 end_inuse_allocs = HeapProfiler::profile_.allocs_ -
                             HeapProfiler::profile_.frees_;
    if (HeapProfiler::kMaxLogging) {
      HeapProfiler::MESSAGE(1, "HeapChecker: "
                               "End check \"%s\" profile: "
                               ""LLD" bytes in "LLD" objects\n",
                               name_, end_inuse_bytes, end_inuse_allocs);
    }
    inuse_bytes_increase_ = (ssize_t)(end_inuse_bytes - start_inuse_bytes_);
    inuse_allocs_increase_ = (ssize_t)(end_inuse_allocs - start_inuse_allocs_);
    has_checked_ = true;
    HeapProfiler::Unlock();
    if (pthread_mutex_unlock(&heap_checker_lock) != 0)  abort();
    bool see_leaks =
      (same_heap ? (inuse_bytes_increase_ != 0 || inuse_allocs_increase_ != 0)
                 : (inuse_bytes_increase_ > 0 || inuse_allocs_increase_ > 0));
    if (see_leaks || do_full) {
      const char* gv_command_tail
        = " --edgefraction=1e-10 --nodefraction=1e-10 --gv 2>/dev/null";
      string ignore_re;
      if (disabled_regexp) {
        ignore_re += " --ignore='^";
        ignore_re += *disabled_regexp;
        ignore_re += "$'";
      }
      // It would be easier to use a string here than a static buffer, but
      // some STLs can give us spurious leak alerts (since the STL tries to
      // do its own memory pooling), so we avoid it by using STL as little
      // as possible for "big" objects that might require "lots" of memory.
      char command[6 * PATH_MAX + 200];
      if (use_initial_profile) {
        // compare against initial profile only if need to
        const char* drop_negative = same_heap ? "" : " --drop_negative";
        snprintf(command, sizeof(command), "%s --base=\"%s.%s-beg.heap\" %s ",
                 pprof_path(), profile_prefix->c_str(), name_,
                 drop_negative);
      } else {
        snprintf(command, sizeof(command), "%s",
                 pprof_path());
      }
      snprintf(command + strlen(command), sizeof(command) - strlen(command),
               " %s \"%s.%s-end.heap\" %s --inuse_objects --lines",
               invocation_path(), profile_prefix->c_str(),
               name_, ignore_re.c_str());
                   // --lines is important here to catch leaks when !see_leaks
      char cwd[PATH_MAX+1];
      if (getcwd(cwd, sizeof(cwd)) != cwd)  abort();
      if (see_leaks) {
        HeapProfiler::MESSAGE(-1, "HeapChecker: "
                              "Heap memory leaks of "LLD" bytes and/or "
                              ""LLD" allocations detected by check \"%s\".\n\n",
                              (int64)inuse_bytes_increase_,
                              (int64)inuse_allocs_increase_,
                              name_);
        HeapProfiler::MESSAGE(-1, "HeapChecker: "			      
                              "To investigate leaks manually use e.g.\n"
                              "cd %s; "  // for proper symbol resolution
                              "%s%s\n\n",
                              cwd, command, gv_command_tail);
      }
      string output;
      int checked_leaks = 0;
      if ((see_leaks && do_report) || do_full) {
        if (access(pprof_path(), X_OK|R_OK) != 0) {
          HeapProfiler::MESSAGE(-1, "HeapChecker: "
                                "WARNING: Skipping pprof check:"
                                " could not run it at %s\n",
                                pprof_path());
        } else {
          // We don't care about pprof's stderr as long as it
          // succeeds with empty report:
          checked_leaks = GetStatusOutput(command, &output);
          if (checked_leaks != 0) {
            HeapProfiler::MESSAGE(-1, "ERROR: Could not run pprof at %s\n",
                                  pprof_path());
            abort();
          }
        }
        if (see_leaks && output.empty() && checked_leaks == 0) {
          HeapProfiler::MESSAGE(-1, "HeapChecker: "
                                "These must be leaks that we disabled"
                                " (pprof succeeded)! This check WILL FAIL"
                                " if the binary is strip'ped!\n");
          see_leaks = false;
        }
        // do not fail the check just due to us being a stripped binary
        if (!see_leaks  &&  strstr(output.c_str(), "nm: ") != NULL  &&
            strstr(output.c_str(), ": no symbols") != NULL)  output.resize(0);
        if (!(see_leaks || checked_leaks == 0))  abort();
      }
      if (see_leaks  &&  use_initial_profile) {
        HeapProfiler::MESSAGE(-1, "HeapChecker: "
                              "CAVEAT: Some of the reported leaks might have "
                              "occurred before check \"%s\" was started!\n",
                              name_);
      }
      bool tricky_leaks = !output.empty();
      if (!see_leaks && tricky_leaks) {
        HeapProfiler::MESSAGE(-1, "HeapChecker: "
                              "Tricky heap memory leaks of"
                              " no bytes and no allocations "
                              "detected by check \"%s\".\n", name_);
        HeapProfiler::MESSAGE(-1, "HeapChecker: "
                              "To investigate leaks manually uge e.g.\n"
                              "cd %s; "  // for proper symbol resolution
                              "%s%s\n\n",
                              name_, cwd, command, gv_command_tail);
        if (use_initial_profile) {
          HeapProfiler::MESSAGE(-1, "HeapChecker: "
                                "CAVEAT: Some of the reported leaks might have "
                                "occurred before check \"%s\" was started!\n",
                                name_);
        }
        see_leaks = true;
      }
      if (see_leaks && do_report) {
        if (checked_leaks == 0) {
          HeapProfiler::MESSAGE(-1, "HeapChecker: "
                                "Below is this pprof's output:\n\n");
          write(STDERR_FILENO, output.data(), output.size());
          HeapProfiler::MESSAGE(-1, "\n\n");
        } else {
          HeapProfiler::MESSAGE(-1, "HeapChecker: "
                                "pprof has failed\n\n");
        }
      }
    } else {
      HeapProfiler::MESSAGE(0, "HeapChecker: No leaks found\n");
    }
    UnIgnoreObject(name_);
    delete [] name_;
    name_ = NULL;
    return !see_leaks;
  } else {
    if (name_ != NULL) {
      HeapProfiler::MESSAGE(-1, "HeapChecker: "
                            "Profiling must stay enabled "
                            "during leak checking\n");
      abort();
    }
    HeapProfiler::Unlock();
    if (pthread_mutex_unlock(&heap_checker_lock) != 0)  abort();
    return true;
  }
}

HeapLeakChecker::~HeapLeakChecker() {
  if (name_ != NULL) {
    HeapProfiler::MESSAGE(-1, "HeapChecker: "
                          "Some *NoLeaks|SameHeap method"
                          " must be called on the checker\n");
    abort();
  }
}

//----------------------------------------------------------------------
// HeapLeakChecker overall heap check components
//----------------------------------------------------------------------

vector<HeapCleaner::void_function>* HeapCleaner::heap_cleanups_ = NULL;

// When a HeapCleaner object is intialized, add its function to the static list
// of cleaners to be run before leaks checking.
HeapCleaner::HeapCleaner(void_function f) {
  if (heap_cleanups_ == NULL)
    heap_cleanups_ = new vector<HeapCleaner::void_function>;
  heap_cleanups_->push_back(f);
}

// Run all of the cleanup functions and delete the vector.
void HeapCleaner::RunHeapCleanups() {
  if (!heap_cleanups_)
    return;
  for (int i = 0; i < heap_cleanups_->size(); i++) {
    void (*f)(void) = (*heap_cleanups_)[i];
    f();
  }
  delete heap_cleanups_;
  heap_cleanups_ = NULL;
}

// Program exit heap cleanup registered with atexit().
// Will not get executed when we crash on a signal.
void HeapLeakChecker::RunHeapCleanups() {
  if (heap_checker_pid == getpid()) {  // can get here (via forks?)
                                       // with other pids
    HeapCleaner::RunHeapCleanups();
    if (!FLAGS_heap_check_after_destructors) {
      DoMainHeapCheck();
    }
  }
}

// Called from main() immediately after setting any requisite parameters
// from HeapChecker and HeapProfiler.
void HeapLeakChecker::InternalInitStart(const string& heap_check_type) {
  if (heap_check_type.empty()) {
    heap_checker_on = false;
  } else {
    if (main_heap_checker) {
      // means this method was called twice.  We'll just ignore the 2nd call
      return;
    }
    if (!constructor_heap_profiling) {
      HeapProfiler::MESSAGE(-1, "HeapChecker: Can not start so late. "
                            "You have to enable heap checking by\n"
                            "setting the environment variable HEAPCHECK.\n");
      abort();
    }
    // Set all flags
    if (heap_check_type == "minimal") {
      // The least we can check.
      FLAGS_heap_check_before_constructors = false;  // (ignore more)
      FLAGS_heap_check_after_destructors = false;  // to after cleanup
                                                   // (most data is live)
      FLAGS_heap_check_strict_check = false;  // < profile check (ignore more)
      FLAGS_heap_check_ignore_thread_live = true;  // ignore all live
      FLAGS_heap_check_ignore_global_live = true;  // ignore all live
    } else if (heap_check_type == "normal") {
      // Faster than 'minimal' and not much stricter.
      FLAGS_heap_check_before_constructors = true;  // from no profile (fast)
      FLAGS_heap_check_after_destructors = false;  // to after cleanup
                                                   // (most data is live)
      FLAGS_heap_check_strict_check = true;  // == profile check (fast)
      FLAGS_heap_check_ignore_thread_live = true;  // ignore all live
      FLAGS_heap_check_ignore_global_live = true;  // ignore all live
    } else if (heap_check_type == "strict") {
      // A bit stricter than 'normal': global destructors must fully clean up
      // after themselves if they are present.
      FLAGS_heap_check_before_constructors = true;  // from no profile (fast)
      FLAGS_heap_check_after_destructors = true;  // to after destructors
                                                  // (less data live)
      FLAGS_heap_check_strict_check = true;  // == profile check (fast)
      FLAGS_heap_check_ignore_thread_live = true;  // ignore all live
      FLAGS_heap_check_ignore_global_live = true;  // ignore all live
    } else if (heap_check_type == "draconian") {
      // Drop not very portable and not very exact live heap flooding.
      FLAGS_heap_check_before_constructors = true;  // from no profile (fast)
      FLAGS_heap_check_after_destructors = true;  // to after destructors
                                                  // (need them)
      FLAGS_heap_check_strict_check = true;  // == profile check (fast)
      FLAGS_heap_check_ignore_thread_live = false;  // no live flood (stricter)
      FLAGS_heap_check_ignore_global_live = false;  // no live flood (stricter)
    } else if (heap_check_type == "as-is") {
      // do nothing: use other flags as is
    } else if (heap_check_type == "local") {
      // do nothing
    } else {
      LogPrintf(FATAL, "Unsupported heap_check flag: %s",
                heap_check_type.c_str());
    }
    assert(heap_checker_pid == getpid());
    heap_checker_on = true;
    if (!HeapProfiler::is_on_)  abort();
    UseProcMaps(DISABLE_LIBRARY_ALLOCS);
      // might neeed to do this more than once
      // if one later dynamically loads libraries that we want disabled

    // make a good place and name for heap profile leak dumps
    profile_prefix = new string(dump_directory());
    *profile_prefix += "/";
    *profile_prefix += invocation_name();
    HeapProfiler::CleanupProfiles(profile_prefix->c_str());

    // Finalize prefix for dumping leak checking profiles.
    char pid_buf[15];
    if (main_thread_pid_ == 0)  // possible if we're called before constructors
      main_thread_pid_ = getpid();
    snprintf(pid_buf, sizeof(pid_buf), ".%d", main_thread_pid_);
    *profile_prefix += pid_buf;
    assert(HeapProfiler::need_for_leaks_);

    // Make sure new/delete hooks are installed properly
    // and heap profiler is indeed able to keep track
    // of the objects being allocated.
    // We test this to make sure we are indeed checking for leaks.
    HeapProfiler::AllocValue alloc_value;
    char* test_str = new char[5];
    void* ptr = test_str;
    if (!HeapProfiler::HaveOnHeap(&ptr, &alloc_value))  abort();
    ptr = test_str;
    delete [] test_str;
    if (HeapProfiler::HaveOnHeap(&ptr, &alloc_value))  abort();
    // If we crash in the above code, it probably means that
    // "nm <this_binary> | grep new" will show that tcmalloc's new/delete
    // implementation did not get linked-in into this binary
    // (i.e. nm will list __builtin_new and __builtin_vec_new as undefined).
    // This is probably impossible.

    if (heap_check_type != "local") {
      // Schedule registered heap cleanup
      atexit(RunHeapCleanups);
      assert(main_heap_checker == NULL);
      main_heap_checker = new HeapLeakChecker();
    }
  }
  if (!heap_checker_on  &&  constructor_heap_profiling) {
    // turns out do not need checking in the end; can stop profiling
    HeapProfiler::MESSAGE(0, "HeapChecker: Turning itself off\n");
    HeapProfiler::StopForLeaks();
  }
}

void HeapLeakChecker::DoMainHeapCheck() {
  assert(heap_checker_pid == getpid());
  if (main_heap_checker) {
    bool same_heap = FLAGS_heap_check_strict_check;
    if (FLAGS_heap_check_before_constructors)  same_heap = true;
      // false here just would make it slower in this case
      // (we don't use the starting profile anyway)
    bool do_full = !same_heap;  // do it if it can help ignore false leaks
    bool do_report = FLAGS_heap_check_report;
    HeapProfiler::MESSAGE(0, "HeapChecker: "
                             "Checking for whole-program memory leaks\n");
    if (!main_heap_checker->DoNoLeaks(same_heap, do_full, do_report)) {
      HeapProfiler::MESSAGE(-1, "ERROR: Leaks found in main heap check, aborting\n");
      abort();
    }
    delete main_heap_checker;
    main_heap_checker = NULL;
  }
}

//----------------------------------------------------------------------
// HeapLeakChecker global constructor/destructor ordering components
//----------------------------------------------------------------------

void HeapLeakChecker::BeforeConstructors() {
  // The user indicates a desire for heap-checking via the HEAPCHECK
  // environment variable.  If it's not set, there's no way to do
  // heap-checking.
  if (!getenv("HEAPCHECK")) {
    return;
  }

  // heap-checker writes out files.  Thus, for security reasons, we don't
  // recognize the env. var. to turn on heap-checking if we're setuid.
  if (getuid() != geteuid()) {
    HeapProfiler::MESSAGE(0, ("HeapChecker: ignoring HEAPCHECK because "
                              "program seems to be setuid\n"));
    return;
  }

  if (constructor_heap_profiling)  abort();
  constructor_heap_profiling = true;
  HeapProfiler::Init();  // only necessary if our constructor runs before theirs
  // If the user has HEAPPROFILE set, Init() will have turned on profiling.
  // If not, we need to do it manually here.
  HeapProfiler::StartForLeaks();
  heap_checker_on = true;

  // The value of HEAPCHECK is the mode they want.  If we don't
  // recognize it, we default to "normal".
  const char* heap_check_type = getenv("HEAPCHECK");
  assert(heap_check_type);  // we checked that in the if above
  if ( heap_check_type[0] == '\0') {
    // don't turn on heap checking for missing or empty env. var.
  } else if ( !strcmp(heap_check_type, "minimal") ||
              !strcmp(heap_check_type, "normal") ||
              !strcmp(heap_check_type, "strict") ||
              !strcmp(heap_check_type, "draconian") ||
              !strcmp(heap_check_type, "local") ) {
    HeapLeakChecker::InternalInitStart(heap_check_type);
  } else {
    HeapLeakChecker::InternalInitStart("normal");         // the default
  }
}

extern bool heap_leak_checker_bcad_variable;  // in heap-checker-bcad.cc

// Whenever the heap checker library is linked in, this should be called before
// all global object constructors run.  This can be tricky and depends on
// heap-checker-bcad.o being the last file linked in.
void HeapLeakChecker_BeforeConstructors() {
  heap_checker_pid = getpid();  // set it always
  // just to reference it, so that heap-checker-bcad.o is linked in
  heap_leak_checker_bcad_variable = true;
  HeapLeakChecker::BeforeConstructors();
}

// This function is executed after all global object destructors run.
void HeapLeakChecker_AfterDestructors() {
  if (heap_checker_pid == getpid()) {  // can get here (via forks?)
                                       // with other pids
    if (FLAGS_heap_check_after_destructors && main_heap_checker) {
      HeapLeakChecker::DoMainHeapCheck();
      poll(NULL, 0, 500);
      // Need this hack to wait for other pthreads to exit.
      // Otherwise tcmalloc finds errors on a free() call from pthreads.
    }
  }
}

//----------------------------------------------------------------------
// HeapLeakChecker disabling helpers
//----------------------------------------------------------------------

// These functions are at the end of the file to prevent their inlining:

void HeapLeakChecker::DisableChecksInLocked(const char* pattern) {
  // make disabled_regexp
  if (disabled_regexp == NULL) {
    disabled_regexp = new string;
    IgnoreObjectLocked(disabled_regexp, false);
      // in case we are not ignoring global data
  }
  HeapProfiler::MESSAGE(1, "HeapChecker: "
                        "Disabling leaks checking in stack traces "
                        "under frames maching \"%s\"\n", pattern);
  if (disabled_regexp->size())  *disabled_regexp += '|';
  *disabled_regexp += pattern;
}

void HeapLeakChecker::DisableChecksFromTo(void* start_address,
                                          void* end_address,
                                          int max_depth) {
  assert(start_address < end_address);
  if (pthread_mutex_lock(&heap_checker_lock) != 0)  abort();
  if (disabled_ranges == NULL) {
    disabled_ranges = new DisabledRangeMap;
    IgnoreObjectLocked(disabled_ranges, false);
      // in case we are not ignoring global data
  }
  RangeValue value;
  value.start_address = reinterpret_cast<uintptr_t>(start_address);
  value.max_depth = max_depth;
  if (disabled_ranges->
        insert(make_pair(reinterpret_cast<uintptr_t>(end_address),
                         value)).second) {
    HeapProfiler::MESSAGE(1, "HeapChecker: "
                          "Disabling leaks checking in stack traces "
                          "under frame addresses between %p..%p\n",
                          start_address, end_address);
  }
  if (pthread_mutex_unlock(&heap_checker_lock) != 0)  abort();
}

void HeapLeakChecker::DisableChecksAtLocked(void* address) {
  if (disabled_addresses == NULL) {
    disabled_addresses = new DisabledAddressSet;
    IgnoreObjectLocked(disabled_addresses, false);
      // in case we are not ignoring global data
  }
  // disable the requested address
  if (disabled_addresses->insert(reinterpret_cast<uintptr_t>(address)).second) {
    HeapProfiler::MESSAGE(1, "HeapChecker: "
                          "Disabling leaks checking in stack traces "
                          "under frame address %p\n",
                          address);
  }
}
