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
// Some hooks into heap-profiler.cc
// that are needed by heap-checker.cc
//

#ifndef BASE_HEAP_PROFILER_INL_H__
#define BASE_HEAP_PROFILER_INL_H__

#include <google/perftools/config.h>

#if defined HAVE_STDINT_H
#include <stdint.h>             // to get uint16_t (ISO naming madness)
#elif defined HAVE_INTTYPES_H
#include <inttypes.h>           // another place uint16_t might be defined
#else
#include <sys/types.h>          // our last best hope
#endif
#include <pthread.h>
#include <google/perftools/basictypes.h>
#include <google/heap-profiler.h>
#include <map>
#include <google/perftools/hash_set.h>

template<class T> class AddressMap; // in addressmap-inl.h
class HeapLeakChecker;  // in heap-checker.h

// namespace for heap profiler components
class HeapProfiler {
 public:  // data types

  // Profile entry
  struct Bucket {
    uintptr_t hash_;      // Hash value
    int     depth_;       // Depth of stack trace
    void**  stack_;       // Stack trace
    int32   allocs_;      // Number of allocs
    int32   frees_;       // Number of frees
    int64   alloc_size_;  // Total size of all allocated objects
    int64   free_size_;   // Total size of all freed objects
    Bucket* next_;        // Next entry in hash-table
  };

  // Info stored in the address map
  struct AllocValue {
    Bucket* bucket;  // The stack-trace bucket
    size_t  bytes;   // Number of allocated bytes
  };
  typedef AddressMap<AllocValue> AllocationMap;

  // Value stored in the map of disabled address ranges;
  // its key is the end of the address range.
  // We'll ignore allocations with a return address in a disabled range
  // if the address occurs at 'max_depth' or less in the stack trace.
  struct RangeValue {
    uintptr_t start_address;  // the start of the range
    int       max_depth;      // the maximal stack depth to disable at
  };
  typedef STL_NAMESPACE::map<uintptr_t, RangeValue> DisabledRangeMap;
  typedef HASH_NAMESPACE::hash_set<uintptr_t> DisabledAddressesSet;

 private:  // state variables
           // NOTE: None of these have destructors that change their state.
           //       Keep it this way: heap-checker depends on it.

  // Is heap-profiling on as a subsytem
  static bool is_on_;
  // If we are disabling heap-profiling recording for incoming
  // (de)allocation calls from the thread specified by temp_disabled_tid_.
  // This is done for (de)allocations that are internal
  // to heap profiler or heap checker, so that we can hold the global
  // profiler's lock and pause heap activity from other threads.
  static bool temp_disable_;
  static pthread_t temp_disabled_tid_;
  // The disabled addresses registered
  // with HeapLeakChecker::DisableChecksUp
  static DisabledAddressesSet* disabled_addresses_;
  // The disabled address ranges registered
  // with HeapLeakChecker::DisableChecksFromTo.
  static DisabledRangeMap* disabled_ranges_;
  // Flag if we are doing heap dump for leaks checking vs.
  // for general memory profiling
  static bool dump_for_leaks_;
  // Prevents recursive dumping
  static bool dumping_;
  // Overall profile stats
  static Bucket total_;
  // Last dumped profile stats
  static Bucket profile_;
  // Stats for the disabled part of the last dumped profile
  static Bucket disabled_;
  // Prefix used for profile file names (NULL if not ready for dumping yet)
  static char* filename_prefix_;
  // Map of all currently allocated object we know about
  static AllocationMap* allocation_;
  // Number of frames to skip in stack traces.  This is the number of functions
  // that are called between malloc() and RecordAlloc().  This can differ
  // depending on the compiler and level of optimization under which we are
  // running.
  static int strip_frames_;
  // Whether we have recorded our first allocation.  This is used to
  // distinguish the magic first call of RecordAlloc that sets strip_frames_
  static bool done_first_alloc_;
  // Location of stack pointer in Init().  Also used to help determine
  // strip_frames_.
  static void* recordalloc_reference_stack_position_;

  // Global lock for profile structure
  static void Lock();
  static void Unlock();

 private:  // functions

  // Own heap profiler's internal allocation mechanism
  static void* Malloc(size_t bytes);
  static void Free(void* p);
  // Helper for HeapProfilerDump:
  // second_prefix is not NULL when the dumped profile
  // is to be named differently for leaks checking
  static void DumpLocked(const char *reason, const char* second_prefix);

 private:  // helpers of heap-checker.cc

  // If "ptr" points to a heap object;
  // we also fill alloc_value for this object then.
  // If yes, we might move "ptr" to point to the very start of the object
  // (this needs to happen for C++ class array allocations
  // and for basic_string-s of C++ library that comes with gcc 3.4).
  static bool HaveOnHeap(void** ptr, AllocValue* alloc_value);
  static bool HaveOnHeapLocked(void** ptr, AllocValue* alloc_value);

 private:  // helpers of heap-profiler.cc

  // Get bucket for current stack trace (skip "skip_count" most recent frames)
  static Bucket* GetBucket(int skip_count);
  static int UnparseBucket(char* buf, int buflen, int bufsize, Bucket* b);
  static void RecordAlloc(void* ptr, size_t bytes, int skip_count);
  static void RecordFree(void* ptr);
  static void RecordFreeLocked(void* ptr);
  // Activates profile collection before profile dumping.
  // Can be called before global object constructors.
  static void EarlyStartLocked();
  static void StartLocked(const char* prefix);
  static void StopLocked();
  static void NewHook(void* ptr, size_t size);
  static void DeleteHook(void* ptr);
  static void MmapHook(void* result,
                       void* start, size_t size,
                       int prot, int flags,
                       int fd, off_t offset);
  static void MunmapHook(void* ptr, size_t size);

 private:  // intended users

  friend class HeapLeakChecker;
  friend void HeapProfilerStart(const char* prefix);
  friend void HeapProfilerStop();
  friend void HeapProfilerDump(const char *reason);
  friend char* GetHeapProfile();

 public:

  // printing messages without using malloc
  // Message levels (levels <= 0 are printed by default):
  //    -1     Errors
  //    0      Normal informational reports
  //    1      Stuff users won't usually care about
  static void MESSAGE(int logging_level, const char* format, ...)
#ifdef _HAVE___ATTRIBUTE__
    __attribute__ ((__format__ (__printf__, 2, 3)))
#endif
;

  // Module initialization
  static void Init();

  // Are we running?
  static bool IsOn() { return is_on_; }
};

#endif  // BASE_HEAP_PROFILER_INL_H__
