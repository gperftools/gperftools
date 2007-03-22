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

#include "google/perftools/config.h"

#include <string>
#include <vector>
#include <map>
#include <google/perftools/hash_set.h>
#include <algorithm>

#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <netinet/in.h>         // inet_ntoa
#include <arpa/inet.h>          // inet_ntoa
#include <execinfo.h>           // backtrace
#include <sys/poll.h>
#include <sys/types.h>
#include <fcntl.h>
#include <assert.h>

#include <google/stacktrace.h>
#include <google/heap-profiler.h>
#include <google/heap-checker.h>
#include "heap-profiler-inl.h"

#include "base/commandlineflags.h"
#include "base/logging.h"

#ifdef HAVE_INTTYPES_H
#define __STDC_FORMAT_MACROS
#include <inttypes.h>
// TODO: have both SCNd64 and PRId64.  We don't bother since they're the same
#define LLX    "%"SCNx64               // how to read 64-bit hex
#define LLD    "%"SCNd64               // how to read 64-bit deciman
#else
#define LLX    "%llx"                  // hope for the best
#define LLD    "%lld"
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
            "If overall heap check reports the found leaks via pprof");

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

DEFINE_bool(heap_check_ignore_told_live, true,
            "If overall heap check is to ignore heap objects reachable "
            "from what was given to HeapLeakChecker::IgnoreObject");

DEFINE_bool(heap_check_ignore_global_live, true,
            "If overall heap check is to ignore heap objects reachable "
            "from the global data");

DEFINE_bool(heap_check_ignore_thread_live, true,
            "If set to true, objects reachable from thread stacks "
            "are not reported as leaks");

DEFINE_string(heap_profile_pprof, INSTALL_PREFIX "/bin/pprof",
              "Path to pprof to call for full leaks checking.");

// External accessors for the above
void HeapLeakChecker::set_heap_check_report(bool b) {
  FLAGS_heap_check_report = b;
}
void HeapLeakChecker::set_pprof_path(const char* s) {
  FLAGS_heap_profile_pprof = s;
}
void HeapLeakChecker::set_dump_directory(const char* s) {
  dump_directory_ = s;
}

bool HeapLeakChecker::heap_check_report() {
  return FLAGS_heap_check_report;
}
const char* HeapLeakChecker::pprof_path() {
  return FLAGS_heap_profile_pprof.c_str();
}
const char* HeapLeakChecker::dump_directory() {
  return dump_directory_.c_str();
}

//----------------------------------------------------------------------

DECLARE_string(heap_profile);    // in heap-profiler.cc
DECLARE_int32(heap_profile_log); // in heap-profiler.cc

//----------------------------------------------------------------------
// HeapLeakChecker global data
//----------------------------------------------------------------------

// Global lock for the global data of this module
static pthread_mutex_t hc_lock = PTHREAD_MUTEX_INITIALIZER;

// the disabled regexp accumulated
// via HeapLeakChecker::DisableChecksIn
static string* disabled_regexp = NULL;

//----------------------------------------------------------------------

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
  WAS_ON_HEAP,      // Is a live object on heap, but now deleted from
                    // the map of the heap objects in HeapProfiler.
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

// This variable is set to non-NULL by thread/thread.cc if it has
// threads whose stacks have to be scanned.
typedef void (*StackRangeIterator)(void*, void*);
int (*heap_checker_thread_stack_extractor)(StackRangeIterator) = NULL;


// This routine is called by thread code for every thread stack it knows about
static void RegisterStackRange(void* base, void* top) {
  char* p1 = min(reinterpret_cast<char*>(base), reinterpret_cast<char*>(top));
  char* p2 = max(reinterpret_cast<char*>(base), reinterpret_cast<char*>(top));
  HeapProfiler::MESSAGE(1, "HeapChecker: Thread stack %p..%p (%d bytes)\n",
                        p1, p2, int(p2-p1));
  live_objects->push_back(AllocObject(p1, uintptr_t(p2-p1), THREAD_STACK));
}

static int GetStatusOutput(const char*  command, string* output) {
  FILE* f = popen(command, "r");
  if (f == NULL) {
    fprintf(stderr, "popen returned NULL!!!\n");  // This shouldn't happen
    exit(1);
  }

  const int kMaxOutputLine = 10000;
  char line[kMaxOutputLine];
  while (fgets(line, sizeof(line), f) != NULL) {
    if (output)
      *output += line;
  }

  return pclose(f);
}

void HeapLeakChecker::IgnoreGlobalDataLocked(const char* library,
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
        live_objects->push_back(AllocObject(reinterpret_cast<void*>(real_start),
                                            sec_size, IN_GLOBAL_DATA));
      }
    }
    // skip to the next line
    const char* next = strpbrk(output_start, "\n\r");
    if (next == NULL) break;
    output_start = next + 1;
  }
  IgnoreLiveObjectsLocked("in globals of\n  ", library);
}

// See if 'library' from /proc/self/maps has base name 'library_base'
// i.e. contains it and has '.' or '-' after it.
static bool IsLibraryNamed(const char* library, const char* library_base) {
  const char* p = strstr(library, library_base);
  size_t sz = strlen(library_base);
  return p != NULL  &&  (p[sz] == '.'  ||  p[sz] == '-');
}

void HeapLeakChecker::DisableLibraryAllocs(const char* library,
                                           uint64 start_address,
                                           uint64 end_address) {
  // TODO(maxim): maybe this should be extended to also use objdump
  //              and pick the text portion of the library more precisely.
  if (IsLibraryNamed(library, "/libpthread")  ||
        // pthread has a lot of small "system" leaks we don't care about
      IsLibraryNamed(library, "/libdl")  ||
        // library loaders leak some "system" heap that we don't care about
      IsLibraryNamed(library, "/ld")) {
    HeapProfiler::MESSAGE(1, "HeapChecker: "
                          "Disabling direct allocations from %s :\n",
                          library);
    DisableChecksFromTo(reinterpret_cast<void*>(start_address),
                        reinterpret_cast<void*>(end_address),
                        1);  // only disable allocation calls directly
                             // from the library code
  }
}

void HeapLeakChecker::UseProcMaps(ProcMapsTask proc_maps_task) {
  FILE* const fp = fopen("/proc/self/maps", "r");
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
                           start_address, end_address);
    }
    if (proc_maps_task == IGNORE_GLOBAL_DATA_LOCKED  && 
        // grhat based on Red Hat Linux 9
        (strncmp(permissions, "rw-p", 4) == 0 ||
         // Fedora Core 3
         strncmp(permissions, "rwxp", 4) == 0) &&
        inode != 0) {
      if (start_address >= end_address)  abort();
      IgnoreGlobalDataLocked(proc_map_line + size, start_address, file_offset);
    }
  }
  fclose(fp);
}

// Total number and size of live objects dropped from the profile.
static int64 live_objects_total = 0;
static int64 live_bytes_total = 0;

// This pointer needs to be outside, rather than inside, the function
// HeapLeakChecker::IgnoreAllLiveObjectsLocked() so that the compiler, in
// this case gcc 3.4.1, does not complain that it is an unused variable.
// Nevertheless, the value's not actually used elsewhere, just retained.
static IgnoredObjectsMap* reach_ignored_objects = NULL;

void HeapLeakChecker::IgnoreAllLiveObjectsLocked() {
  // the leaks of building live_objects below are ignored in our caller
  CHECK(live_objects == NULL);
  live_objects = new LiveObjectsStack;
  if (FLAGS_heap_check_ignore_thread_live &&
      (heap_checker_thread_stack_extractor != NULL)) {
    int drop = (*heap_checker_thread_stack_extractor)(&RegisterStackRange);
    if (drop > 0) {
      HeapProfiler::MESSAGE(0, "HeapChecker: Thread stacks not found "
                            "for %d threads; may get false leak reports\n",
                            drop);
    }
  }
  if (FLAGS_heap_check_ignore_told_live && ignored_objects) {
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
    IgnoreLiveObjectsLocked("ignored", "");
  }
  // Just a pointer for reachability of ignored_objects;
  // we can't delete them here because the deletions won't be recorded
  // by profiler, whereas the allocations might have been.
  reach_ignored_objects = ignored_objects;
  ignored_objects = NULL;
  if (FLAGS_heap_check_ignore_global_live) {
    UseProcMaps(IGNORE_GLOBAL_DATA_LOCKED);
  }
  if (live_objects_total) {
    HeapProfiler::MESSAGE(0, "HeapChecker: "
                          "Not reporting "LLD" reachable "
                          "objects of "LLD" bytes\n",
                          live_objects_total, live_bytes_total);
  }
  // Free these: we made them here and heap profiler never saw them
  delete live_objects;
  live_objects = NULL;
}

// This function irreparably changes HeapProfiler's state by dropping from it
// the objects we consider live here.
// But we don't care, since it is called only at program exit.
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
        HeapProfiler::HaveOnHeapLocked(&object, &alloc_value)) {
      HeapProfiler::RecordFreeLocked(object);  // drop it from the profile
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
      HeapProfiler::MESSAGE(6, "HeapChecker: "
                            "Trying pointer to %p at %p\n",
                            ptr, current_object);
      // Do not need the following since the data for live_objects
      // is not recorded by heap-profiler:
      // if (ptr == live_objects)  continue;
      if (HeapProfiler::HaveOnHeapLocked(&ptr, &alloc_value)) {
        // We take the (hopefully low) risk here of encountering by accident
        // a byte sequence in memory that matches an address of
        // a heap object which is in fact leaked.
        // I.e. in very rare and probably not repeatable/lasting cases
        // we might miss some real heap memory leaks.
        HeapProfiler::MESSAGE(5, "HeapChecker: "
                              "Found pointer to %p"
                              " of %"PRIuS" bytes at %p\n",
                              ptr, alloc_value.bytes, current_object);
        HeapProfiler::RecordFreeLocked(ptr);  // drop it from the profile
        live_object_count += 1;
        live_byte_count += alloc_value.bytes;
        live_objects->push_back(AllocObject(ptr, alloc_value.bytes,
                                            WAS_ON_HEAP));
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
  if (pthread_mutex_lock(&hc_lock) != 0)  abort();
  DisableChecksAtLocked(address);
  if (pthread_mutex_unlock(&hc_lock) != 0)  abort();
}

void HeapLeakChecker::DisableChecksIn(const char* pattern) {
  if (!heap_checker_on) return;
  if (pthread_mutex_lock(&hc_lock) != 0)  abort();
  DisableChecksInLocked(pattern);
  if (pthread_mutex_unlock(&hc_lock) != 0)  abort();
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
  HeapProfiler::AllocValue alloc_value;
  if (pthread_mutex_lock(&hc_lock) != 0)  abort();
  if (HeapProfiler::HaveOnHeap(&ptr, &alloc_value)) {
    HeapProfiler::MESSAGE(1, "HeapChecker: "
                          "Going to ignore live object "
                          "at %p of %"PRIuS" bytes\n",
                          ptr, alloc_value.bytes);
    if (ignored_objects == NULL)  ignored_objects = new IgnoredObjectsMap;
    if (!ignored_objects->insert(make_pair(reinterpret_cast<uintptr_t>(ptr),
                                           alloc_value.bytes)).second) {
      HeapProfiler::MESSAGE(-1, "HeapChecker: "
                            "%p is already being ignored\n", ptr);
      abort();
    }
  }
  if (pthread_mutex_unlock(&hc_lock) != 0)  abort();
}

void HeapLeakChecker::UnIgnoreObject(void* ptr) {
  if (!heap_checker_on) return;
  HeapProfiler::AllocValue alloc_value;
  if (pthread_mutex_lock(&hc_lock) != 0)  abort();
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
  if (pthread_mutex_unlock(&hc_lock) != 0)  abort();
  if (!ok) {
    HeapProfiler::MESSAGE(-1, "HeapChecker: "
                          "%p has not been ignored\n", ptr);
    abort();
  }
}

//----------------------------------------------------------------------
// HeapLeakChecker non-static functions
//----------------------------------------------------------------------

void HeapLeakChecker::Create(const char *name) {
  name_ = NULL;
  if (!HeapProfiler::is_on_) return;  // fast escape
  name_length_ = strlen(name);
  char* n = new char[name_length_ + 4 + 1];
  // Heap activity in other threads is paused for this whole function.
  HeapProfiler::Lock();
  if (HeapProfiler::is_on_  &&  HeapProfiler::filename_prefix_) {
    if (!heap_checker_on) {
      HeapProfiler::MESSAGE(0, "HeapChecker: "
                            "Checking was not activated via "
                            "the heap_check command line flag. "
                            "You might hence get more false leak reports!\n");
      heap_checker_on = true;
    }
    assert(!HeapProfiler::dumping_);  // not called from dumping code
    assert(strchr(name, '/') == NULL);  // must be a simple name
    name_ = n;
    memcpy(name_, name, name_length_);
    memcpy(name_ + name_length_, "-beg", 4 + 1);
    // To make the profile let our thread work with the heap
    // without profiling this while we hold the lock.
    assert(!HeapProfiler::temp_disable_);
    HeapProfiler::temp_disabled_tid_ = pthread_self();
    HeapProfiler::temp_disable_ = true;
    HeapProfiler::dump_for_leaks_ = true;
    HeapProfiler::DumpLocked("leak check start", name_);
    HeapProfiler::dump_for_leaks_ = false;
    HeapProfiler::temp_disable_ = false;
    start_inuse_bytes_ = HeapProfiler::profile_.alloc_size_ -
                         HeapProfiler::profile_.free_size_;
    start_inuse_allocs_ = HeapProfiler::profile_.allocs_ -
                          HeapProfiler::profile_.frees_;
  } else {
    HeapProfiler::MESSAGE(-1, "HeapChecker: "
                          "Heap profiler is not active, "
                          "hence checker \"%s\" will do nothing!\n", name);
  }
  HeapProfiler::Unlock();
  if (name_ == NULL)  delete[] n;
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
string HeapLeakChecker::dump_directory_ = "/tmp";
#ifdef HAVE_PROGRAM_INVOCATION_NAME
extern char* program_invocation_name;
extern char* program_invocation_short_name;
const char* HeapLeakChecker::invocation_name_ = program_invocation_short_name;
const char* HeapLeakChecker::invocation_path_ = program_invocation_name;
#else
const char* HeapLeakChecker::invocation_name_ = "heap-checker";
const char* HeapLeakChecker::invocation_path_ = "heap-checker";  // I guess?
#endif

HeapLeakChecker::HeapLeakChecker(Kind kind) {
  if (!(kind == MAIN  ||  kind == MAIN_DEBUG))  abort();
  bool start = true;
  if (kind == MAIN_DEBUG)  start = false;
  if (start) {
    if (FLAGS_heap_profile.empty()) {
      // doing just leaks checking: no periodic dumps
      FLAGS_heap_profile_allocation_interval = kint64max;
      FLAGS_heap_profile_inuse_interval = kint64max;
    }
    char pid_buf[15];
    snprintf(pid_buf, sizeof(pid_buf), ".%d", main_thread_pid_);
    HeapProfilerStart((dump_directory_ + "/" +
                       invocation_name_ +
                       pid_buf).c_str());
  }
  Create("_main_");
}

// Copy of FLAGS_heap_profile_pprof.
// Need this since DoNoLeaks can happen
// after FLAGS_heap_profile_pprof is destroyed.
static string* flags_heap_profile_pprof = &FLAGS_heap_profile_pprof;

// CAVEAT: Threads, liveness, and heap leak check:
// It might be possible for to have a race leak condition
// for a whole-program leak check due to heap activity in other threads
// when HeapLeakChecker::DoNoLeaks is called at program's exit.
// It can occur if after allocating a heap object a thread does not
// quickly make the object reachable from some global/static variable
// or from the thread's own stack variable.
// Good news is that the only way to achieve this for a thread seems to be
// to keep the only pointer to an allocated object in a CPU register
// (i.e. in particular not call any other functions).
// Probably thread context switching and thread stack boundary
// acquisition via heap_checker_thread_stack_extractor
// do not make the above in-CPU-pointer scenario possible.

bool HeapLeakChecker::DoNoLeaks(bool same_heap,
                                bool do_full,
                                bool do_report) {
  // Heap activity in other threads is paused for this function
  // until we got all profile difference info.
  HeapProfiler::Lock();
  if (HeapProfiler::is_on_  &&  this == main_heap_checker) {
    // We do this only for the main atexit check
    // not to distort the heap profile in the other cases.
    if (FLAGS_heap_check_ignore_told_live  ||
        FLAGS_heap_check_ignore_thread_live  ||
        FLAGS_heap_check_ignore_global_live) {
      // Give other threads some time (just in case)
      // to make live-reachable the objects that they just allocated
      // before we got the HeapProfiler's lock:
      poll(NULL, 0, 100);
      if (pthread_mutex_lock(&hc_lock) != 0)  abort();
      assert(!HeapProfiler::temp_disable_);
      HeapProfiler::temp_disabled_tid_ = pthread_self();
      HeapProfiler::temp_disable_ = true;
      // For this call we are free to call new/delete from this thread:
      // heap profiler will ignore them without acquiring its lock:
      IgnoreAllLiveObjectsLocked();
      HeapProfiler::temp_disable_ = false;
      if (pthread_mutex_unlock(&hc_lock) != 0)  abort();
    }
  }
  assert(!HeapProfiler::dumping_);  // not called from dumping code
  if (HeapProfiler::is_on_  &&  HeapProfiler::filename_prefix_) {
    if (name_ == NULL) {
      HeapProfiler::MESSAGE(-1, "HeapChecker: "
                            "*NoLeaks|SameHeap must be called only once"
                            " and profiling must be not turned on "
                            "after construction of a HeapLeakChecker\n");
      abort();
    }
    memcpy(name_ + name_length_, "-end", 4 + 1);
    // To make the profile let our thread work with the heap
    // without profiling this while we hold the lock.
    assert(!HeapProfiler::temp_disable_);
    HeapProfiler::temp_disabled_tid_ = pthread_self();
    HeapProfiler::temp_disable_ = true;
    HeapProfiler::dump_for_leaks_ = true;
    HeapProfiler::DumpLocked("leak check end", name_);
    HeapProfiler::dump_for_leaks_ = false;
    HeapProfiler::temp_disable_ = false;
    int64 disabled_bytes = HeapProfiler::disabled_.alloc_size_ -
                           HeapProfiler::disabled_.free_size_;
    int64 disabled_allocs = HeapProfiler::disabled_.allocs_ -
                            HeapProfiler::disabled_.frees_;
    if (disabled_bytes) {
      HeapProfiler::MESSAGE(0, "HeapChecker: "
                            "Not reporting "LLD" disabled objects"
                            " of "LLD" bytes\n",
                            disabled_allocs, disabled_bytes);
    }
    if (FLAGS_heap_check_before_constructors  &&  this == main_heap_checker) {
      // compare against empty initial profile
      start_inuse_bytes_ = 0;
      start_inuse_allocs_ = 0;
    }
    int64 increase_bytes =
      (HeapProfiler::profile_.alloc_size_ -
       HeapProfiler::profile_.free_size_) - start_inuse_bytes_;
    int64 increase_allocs =
      (HeapProfiler::profile_.allocs_ -
       HeapProfiler::profile_.frees_) - start_inuse_allocs_;
    HeapProfiler::Unlock();
    bool see_leaks =
      (same_heap ? (increase_bytes != 0 || increase_allocs != 0)
                 : (increase_bytes > 0 || increase_allocs > 0));
    if (see_leaks || do_full) {
      name_[name_length_] = '\0';
      const char* gv_command_tail
        = " --edgefraction=1e-10 --nodefraction=1e-10 --gv";
      string ignore_re;
      if (disabled_regexp) {
        ignore_re += " --ignore=\"^";
        ignore_re += *disabled_regexp;
        ignore_re += "$\"";
      }
      // XXX(jandrews): This fix masks a bug where we detect STL leaks
      // spuriously because the STL allocator allocates memory and never gives
      // it back.  This did not occur before because we overrode the STL
      // allocator to use tcmalloc, which called our hooks appropriately.
      // The solution is probably to find a way to ignore memory held by the
      // STL allocator, which may cause leaks in local variables to be ignored.
      char command[6 * PATH_MAX + 200];
      const char* drop_negative = same_heap ? "" : " --drop_negative";
      if (this != main_heap_checker  ||
          !FLAGS_heap_check_before_constructors) {
        // compare against initial profile only if need to
        snprintf(command, sizeof(command), "%s --base=\"%s.%s-beg.heap\" %s ",
                 flags_heap_profile_pprof->c_str(),
                 HeapProfiler::filename_prefix_,
                 name_, drop_negative);
      } else {
        snprintf(command, sizeof(command), "%s",
                 flags_heap_profile_pprof->c_str());
      }
      snprintf(command + strlen(command), sizeof(command) - strlen(command),
               " %s \"%s.%s-end.heap\" %s --inuse_objects --lines",
               invocation_path_, HeapProfiler::filename_prefix_,
               name_, ignore_re.c_str());
                   // --lines is important here to catch leaks when !see_leaks
      char cwd[PATH_MAX+1];
      if (getcwd(cwd, PATH_MAX) != cwd)  abort();
      if (see_leaks) {
        HeapProfiler::MESSAGE(-1, "HeapChecker: "
                              "Heap memory leaks of "LLD" bytes and/or "
                              ""LLD" allocations detected by check \"%s\".\n\n"
                              "To investigate leaks manually use e.g.\n"
                              "cd %s; "  // for proper symbol resolution
                              "%s%s\n\n",
                              increase_bytes, increase_allocs, name_,
                              cwd, command, gv_command_tail);
      }
      string output;
      int checked_leaks = 0;
      if ((see_leaks && do_report) || do_full) {
        if (access(flags_heap_profile_pprof->c_str(), X_OK|R_OK) != 0) {
          HeapProfiler::MESSAGE(-1, "HeapChecker: "
                                "WARNING: Skipping pprof check:"
                                " could not run it at %s\n",
                                flags_heap_profile_pprof->c_str());
        } else {
          checked_leaks = GetStatusOutput(command, &output);
          if (checked_leaks != 0) {
            HeapProfiler::MESSAGE(-1, "ERROR: Could not run pprof at %s\n",
                                  flags_heap_profile_pprof->c_str());
            abort();
          }
        }
        if (see_leaks && output.empty() && checked_leaks == 0) {
          HeapProfiler::MESSAGE(-1, "HeapChecker: "
                                "These must be leaks that we disabled"
                                " (pprof succeded)!\n");
          see_leaks = false;
        }
        // do not fail the check just due to us being a stripped binary
        if (!see_leaks  &&  strstr(output.c_str(), "nm: ") != NULL  &&
            strstr(output.c_str(), ": no symbols") != NULL)  output.resize(0);
        if (!(see_leaks || checked_leaks == 0))  abort();
      }
      bool tricky_leaks = !output.empty();
      if (!see_leaks && tricky_leaks) {
        HeapProfiler::MESSAGE(-1, "HeapChecker: "
                              "Tricky heap memory leaks of"
                              " no bytes and no allocations "
                              "detected by check \"%s\".\n"
                              "To investigate leaks manually uge e.g.\n"
                              "cd %s; "  // for proper symbol resolution
                              "%s%s\n\n",
                              name_, cwd, command, gv_command_tail);
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
void HeapLeakChecker::RunHeapCleanups(void) {
  if (heap_checker_pid == getpid()) {  // can get here (via forks?)
                                       // with other pids
    HeapCleaner::RunHeapCleanups();
    if (!FLAGS_heap_check_after_destructors) {
      DoMainHeapCheck();
      // Disable further dumping
      if (HeapProfiler::is_on_)
        HeapProfilerStop();
    }
  }
}

void HeapLeakChecker::LibCPreallocate() {
  // force various C library static allocations before we start leak-checking
  strerror(errno);
  struct in_addr addr;
  addr.s_addr = INADDR_ANY;
  inet_ntoa(addr);
  const time_t now = time(NULL);
  ctime(&now);
  void *stack[1];
  backtrace(stack, 0);
}

// Called from main() immediately after setting any requisite parameters
// from HeapChecker and HeapProfiler.
void HeapLeakChecker::StartFromMain(const string& heap_check_type) {
  if (heap_check_type != "") {
    if (!constructor_heap_profiling) {
      HeapProfiler::MESSAGE(-1, "HeapChecker: Can not start so late. "
                            "You have to enable heap checking with\n"
                            "             --heapcheck=..."
                            " or a dependency on //base:heapcheck\n");
      abort();
    }
    // make an indestructible copy for heap leak checking
    // happening after global variable destruction
    flags_heap_profile_pprof = new string(FLAGS_heap_profile_pprof);
    // Set all flags
    if (heap_check_type == "minimal") {
      // The least we can check.
      FLAGS_heap_check_before_constructors = false;  // (ignore more)
      FLAGS_heap_check_after_destructors = false;  // to after cleanup
                                                   // (most data is live)
      FLAGS_heap_check_strict_check = false;  // < profile check (ignore more)
      FLAGS_heap_check_ignore_told_live = true;  // ignore all live
      FLAGS_heap_check_ignore_thread_live = true;  // ignore all live
      FLAGS_heap_check_ignore_global_live = true;  // ignore all live
    } else if (heap_check_type == "normal") {
      // Faster than 'minimal' and not much stricter.
      FLAGS_heap_check_before_constructors = true;  // from no profile (fast)
      FLAGS_heap_check_after_destructors = false;  // to after cleanup
                                                   // (most data is live)
      FLAGS_heap_check_strict_check = true;  // == profile check (fast)
      FLAGS_heap_check_ignore_told_live = true;  // ignore all live
      FLAGS_heap_check_ignore_thread_live = true;  // ignore all live
      FLAGS_heap_check_ignore_global_live = true;  // ignore all live
    } else if (heap_check_type == "strict") {
      // A bit stricter than 'normal': global destructors must fully clean up
      // after themselves if they are present.
      FLAGS_heap_check_before_constructors = true;  // from no profile (fast)
      FLAGS_heap_check_after_destructors = true;  // to after destructors
                                                  // (less data live)
      FLAGS_heap_check_strict_check = true;  // == profile check (fast)
      FLAGS_heap_check_ignore_told_live = true;  // ignore all live
      FLAGS_heap_check_ignore_thread_live = true;  // ignore all live
      FLAGS_heap_check_ignore_global_live = true;  // ignore all live
    } else if (heap_check_type == "draconian") {
      // Drop not very portable and not very exact live heap flooding.
      FLAGS_heap_check_before_constructors = true;  // from no profile (fast)
      FLAGS_heap_check_after_destructors = true;  // to after destructors
                                                  // (need them)
      FLAGS_heap_check_strict_check = true;  // == profile check (fast)
      FLAGS_heap_check_ignore_told_live = false;  // no live flood (stricter)
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
    assert(HeapProfiler::is_on_);
    UseProcMaps(DISABLE_LIBRARY_ALLOCS);
    if (heap_check_type != "local") {
      // Schedule registered heap cleanup
      atexit(RunHeapCleanups);
      assert(main_heap_checker == NULL);
      main_heap_checker = new HeapLeakChecker(MAIN);
      // make sure new/delete hooks are installed properly:
      IgnoreObject(main_heap_checker);
      UnIgnoreObject(main_heap_checker);
      // **
      // ** If we crash here, it's probably because the binary is not
      // ** linked with an instrumented malloc, such as tcmalloc.
      // ** "nm <this_binary> | grep new" to verify.  An instrumented
      // ** malloc is necessary for using heap-checker.
      // **
    }
  } else {
    heap_checker_on = false;
  }
  if (!heap_checker_on  &&  constructor_heap_profiling) {
    // turns out do not need checking in the end; stop profiling
    HeapProfiler::MESSAGE(0, "HeapChecker: Turning itself off\n");
    HeapProfilerStop();
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
    HeapProfiler::MESSAGE(0, "HeapChecker: Checking for memory leaks\n");
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
  if (constructor_heap_profiling)  abort();
  constructor_heap_profiling = true;
  LibCPreallocate();
  HeapProfiler::Lock();
  HeapProfiler::EarlyStartLocked();  // fire-up HeapProfiler hooks
  heap_checker_on = true;
  assert(HeapProfiler::is_on_);
  HeapProfiler::Unlock();
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
        // Otherwise tcmalloc or debugallocation find errors
        // on a free() call from pthreads.
    }
    if (main_heap_checker)  abort();
  }
}

//----------------------------------------------------------------------
// HeapLeakChecker disabling helpers
//----------------------------------------------------------------------

// These functions are at the end of the file to prevent their inlining:

void HeapLeakChecker::DisableChecksInLocked(const char* pattern) {
  // disable our leaks below for growing disabled_regexp
  void* stack[1];
  if (GetStackTrace(stack, 1, 1) != 1)  abort();
  DisableChecksAtLocked(stack[0]);
  // make disabled_regexp
  if (disabled_regexp == NULL)  disabled_regexp = new string;
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
  // disable our leaks for constructing disabled_ranges_
  DisableChecksUp(1);
  if (pthread_mutex_lock(&hc_lock) != 0)  abort();
  if (HeapProfiler::disabled_ranges_ == NULL) {
    HeapProfiler::disabled_ranges_ = new HeapProfiler::DisabledRangeMap;
  }
  HeapProfiler::RangeValue value;
  value.start_address = reinterpret_cast<uintptr_t>(start_address);
  value.max_depth = max_depth;
  if (HeapProfiler::disabled_ranges_->
        insert(make_pair(reinterpret_cast<uintptr_t>(end_address),
                         value)).second) {
    HeapProfiler::MESSAGE(1, "HeapChecker: "
                          "Disabling leaks checking in stack traces "
                          "under frame addresses between %p..%p\n",
                          start_address, end_address);
  }
  if (pthread_mutex_unlock(&hc_lock) != 0)  abort();
}

void HeapLeakChecker::DisableChecksAtLocked(void* address) {
  if (HeapProfiler::disabled_addresses_ == NULL) {
    HeapProfiler::disabled_addresses_ = new HeapProfiler::DisabledAddressesSet;
  }
  // disable our leaks for constructing disabled_addresses_
  void* stack[1];
  if (GetStackTrace(stack, 1, 1) != 1)  abort();
  HeapProfiler::disabled_addresses_->
    insert(reinterpret_cast<uintptr_t>(stack[0]));
  // disable the requested address
  if (HeapProfiler::disabled_addresses_->
      insert(reinterpret_cast<uintptr_t>(address)).second) {
    HeapProfiler::MESSAGE(1, "HeapChecker: "
                          "Disabling leaks checking in stack traces "
                          "under frame address %p\n",
                          address);
  }
}
