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
// Author: Maxim Lifantsev (with design ideas by Sanjay Ghemawat)
//
//
// Module for detecing heap (memory) leaks.
//
// For full(er) information, see doc/heap-checker.html
//
// This module can be linked into programs with
// no slowdown caused by this unless you activate the leak-checker:
//
//    1. Set the environment variable HEAPCHEK to _type_ before
//       running the program.
//
// _type_ is usually "normal" but can also be "minimal", "strict", or
// "draconian".  (See the html file for other options, like 'local'.)
//
// After that, just run your binary.  If the heap-checker detects
// a memory leak at program-exit, it will print instructions on how
// to track down the leak.


#ifndef BASE_HEAP_CHECKER_H__
#define BASE_HEAP_CHECKER_H__

#include <sys/types.h>  // for size_t
#include <stdint.h>     // for uintptr_t
#include <vector>


class HeapLeakChecker {
 public:  // Static functions for working with (whole-program) leak checking.

  // If heap leak checking is currently active in some mode
  // e.g. if leak checking was started (and is still active now)
  // due to any valid non-empty --heap_check flag value
  // (including "local") on the command-line
  // or via a dependency on //base:heapcheck.
  // The return value reflects iff HeapLeakChecker objects manually
  // constructed right now will be doing leak checking or nothing.
  // Note that we can go from active to inactive state during InitGoogle()
  // if FLAGS_heap_check gets set to "" by some code before/during InitGoogle().
  static bool IsActive();

  // Return pointer to the whole-program checker if (still) active
  // and NULL otherwise.
  // This is mainly to access BytesLeaked() and ObjectsLeaked() (see below)
  // for the whole-program checker after one calls NoGlobalLeaks()
  // or similar and gets false.
  static HeapLeakChecker* GlobalChecker();

  // Do whole-program leak check now (if it was activated for this binary);
  // return false only if it was activated and has failed.
  // The mode of the check is controlled by the command-line flags.
  // This method can be called repeatedly.
  // Things like GlobalChecker()->SameHeap() can also be called explicitly
  // to do the desired flavor of the check.
  static bool NoGlobalLeaks();

  // If whole-program checker if active,
  // cancel its automatic execution after main() exits.
  // This requires that some leak check (e.g. NoGlobalLeaks())
  // has been called at least once on the whole-program checker.
  static void CancelGlobalCheck();

 public:  // Non-static functions for starting and doing leak checking.

  // Start checking and name the leak check performed.
  // The name is used in naming dumped profiles
  // and needs to be unique only within your binary.
  // It must also be a string that can be a part of a file name,
  // in particular not contain path expressions.
  explicit HeapLeakChecker(const char *name);

  // Return true iff the heap does not have more objects allocated
  // w.r.t. its state at the time of our construction.
  // This does full pprof heap change checking and reporting.
  // To detect tricky leaks it depends on correct working pprof implementation
  // referred by FLAGS_heap_profile_pprof.
  // (By 'tricky leaks' we mean a change of heap state that e.g. for SameHeap
  //  preserves the number of allocated objects and bytes
  //  -- see TestHeapLeakCheckerTrick in heap-checker_unittest.cc --
  //  and thus is not detected by BriefNoLeaks.)
  // CAVEAT: pprof will do no checking over stripped binaries
  // (our automatic test binaries are stripped)
  // NOTE: All *NoLeaks() and *SameHeap() methods can be called many times
  // to check for leaks at different end-points in program's execution.
  bool NoLeaks() { return DoNoLeaks(NO_LEAKS, USE_PPROF, PPROF_REPORT); }

  // Return true iff the heap does not seem to have more objects allocated
  // w.r.t. its state at the time of our construction
  // by looking at the number of objects & bytes allocated.
  // This also tries to do pprof reporting of detected leaks.
  bool QuickNoLeaks() { return DoNoLeaks(NO_LEAKS, USE_COUNTS, PPROF_REPORT); }

  // Return true iff the heap does not seem to have more objects allocated
  // w.r.t. its state at the time of our construction
  // by looking at the number of objects & bytes allocated.
  // This does not try to use pprof at all.
  bool BriefNoLeaks() { return DoNoLeaks(NO_LEAKS, USE_COUNTS, NO_REPORT); }

  // These are similar to their *NoLeaks counterparts,
  // but they in addition require no negative leaks,
  // i.e. the state of the heap must be exactly the same
  // as at the time of our construction.
  bool SameHeap() { return DoNoLeaks(SAME_HEAP, USE_PPROF, PPROF_REPORT); }
  bool QuickSameHeap()
    { return DoNoLeaks(SAME_HEAP, USE_COUNTS, PPROF_REPORT); }
  bool BriefSameHeap() { return DoNoLeaks(SAME_HEAP, USE_COUNTS, NO_REPORT); }

  // Detailed information about the number of leaked bytes and objects
  // (both of these can be negative as well).
  // These are available only after a *SameHeap or *NoLeaks
  // method has been called.
  // Note that it's possible for both of these to be zero
  // while SameHeap() or NoLeaks() returned false in case
  // of a heap state change that is significant
  // but preserves the byte and object counts.
  ssize_t BytesLeaked() const;
  ssize_t ObjectsLeaked() const;

  // Destructor (verifies that some *NoLeaks or *SameHeap method
  // has been called at least once).
  ~HeapLeakChecker();

 private:  // data

  char* name_;  // our remembered name
  size_t start_inuse_bytes_;  // bytes in use at our construction
  size_t start_inuse_allocs_;  // allocations in use at our construction
  bool has_checked_;  // if we have done the leak check, so these are ready:
  ssize_t inuse_bytes_increase_;  // bytes-in-use increase for this checker
  ssize_t inuse_allocs_increase_;  // allocations-in-use increase for this checker

 public:  // Static helpers to make us ignore certain leaks.

  // NOTE: All calls to DisableChecks* affect all later heap profile generation
  // that happens in our construction and inside of *NoLeaks().
  // They do nothing when heap leak checking is turned off.

  // CAVEAT: Disabling via all the DisableChecks* functions happens only
  // up to kMaxStackTrace stack frames (see heap-profile-table.h)
  // down from the stack frame identified by the function.
  // Hence, this disabling will stop working for very deep call stacks
  // and you might see quite wierd leak profile dumps in such cases.

  // CAVEAT: Disabling via DisableChecksIn works only with non-strip'ped
  // binaries.  It's better not to use this function if at all possible.
  //
  // Register 'pattern' as another variant of a regular expression to match
  // function_name, file_name:line_number, or function_address
  // of function call/return points for which allocations below them should be
  // ignored during heap leak checking.
  // (This becomes a part of pprof's '--ignore' argument.)
  // Usually this should be caled from a REGISTER_HEAPCHECK_CLEANUP
  // in the source file that is causing the leaks being ignored.
  static void DisableChecksIn(const char* pattern);

  // A pair of functions to disable heap checking between them.
  // For example
  //    ...
  //    void* start_address = HeapLeakChecker::GetDisableChecksStart();
  //    <do things>
  //    HeapLeakChecker::DisableChecksToHereFrom(start_address);
  //    ...
  // will disable heap leak checking for everything that happens
  // during any execution of <do things> (including any calls from it),
  // i.e. all objects allocated from there
  // and everything reachable from them will not be considered a leak.
  // Each such pair of function calls must be from the same function,
  // because this disabling works by remembering the range of
  // return program counter addresses for the two calls.
  static void* GetDisableChecksStart();
  static void DisableChecksToHereFrom(void* start_address);

  // ADVICE: Use GetDisableChecksStart, DisableChecksToHereFrom
  //         instead of DisableChecksUp|At whenever possible
  //         to make the code less fragile under different degrees of inlining.
  // Register the function call point (return program counter address)
  // 'stack_frames' above us for which allocations
  // (everything reachable from them) below it should be
  // ignored during heap leak checking.
  // 'stack_frames' must be >= 1 (in most cases one would use the value of 1).
  // For example
  //    void Foo() {  // Foo() should not get inlined
  //      HeapLeakChecker::DisableChecksUp(1);
  //      <do things>
  //    }
  // will disable heap leak checking for everything that happens
  // during any execution of <do things> (including any calls from it),
  // i.e. all objects allocated from there
  // and everything reachable from them will not be considered a leak.
  // CAVEAT: If Foo() is inlined this will disable heap leak checking
  // under all processing of all functions Foo() is inlined into.
  // Hence, for potentially inlined functions, use the GetDisableChecksStart,
  // DisableChecksToHereFrom calls instead.
  // (In the above example we store and use the return program counter
  //  addresses from Foo to do the disabling.)
  static void DisableChecksUp(int stack_frames);

  // Same as DisableChecksUp,
  // but the function return program counter address is given explicitly.
  static void DisableChecksAt(void* address);

  // Tests for checking that DisableChecksUp and DisableChecksAt
  // behaved as expected, for example
  //    void Foo() {
  //      HeapLeakChecker::DisableChecksUp(1);
  //      <do things>
  //    }
  //    void Bar() {
  //      Foo();
  //      CHECK(!HeapLeakChecker::HaveDisabledChecksUp(1));
  //        // This will fail if Foo() got inlined into Bar()
  //        // (due to more aggressive optimization in the (new) compiler)
  //        // which breaks the intended behavior of DisableChecksUp(1) in it.
  //      <do things>
  //    }
  // These return false when heap leak checking is turned off.
  static bool HaveDisabledChecksUp(int stack_frames);
  static bool HaveDisabledChecksAt(void* address);

  // Ignore an object located at 'ptr'
  // (as well as all heap objects (transitively) referenced from it)
  // for the purposes of heap leak checking.
  // If 'ptr' does not point to an active allocated object
  // at the time of this call, it is ignored;
  // but if it does, the object must not get deleted from the heap later on;
  // it must also be not already ignored at the time of this call.
  static void IgnoreObject(void* ptr);

  // Undo what an earlier IgnoreObject() call promised and asked to do.
  // At the time of this call 'ptr' must point to an active allocated object
  // which was previously registered with IgnoreObject().
  static void UnIgnoreObject(void* ptr);

 public:  // Initializations; to be called from main() only.

  // Full starting of recommended whole-program checking.
  static void InternalInitStart();

 public:  // Internal types defined in .cc

  struct Allocator;
  struct RangeValue;

 private:  // Various helpers

  // Type for DumpProfileLocked
  enum ProfileType { START_PROFILE, END_PROFILE };
  // Helper for dumping start/end heap leak checking profiles
  // and getting the byte/object counts.
  void DumpProfileLocked(ProfileType profile_type, void* self_stack_top,
                         size_t* alloc_bytes, size_t* alloc_objects);
  // Helper for constructors
  void Create(const char *name);
  // Types for DoNoLeaks and its helpers.
  enum CheckType { SAME_HEAP, NO_LEAKS };
  enum CheckFullness { USE_PPROF, USE_COUNTS };
  enum ReportMode { PPROF_REPORT, NO_REPORT };
  // Helpers for *NoLeaks and *SameHeap
  bool DoNoLeaks(CheckType check_type,
                 CheckFullness fullness,
                 ReportMode report_mode);
  bool DoNoLeaksOnce(CheckType check_type,
                     CheckFullness fullness,
                     ReportMode report_mode);
  // Helper for IgnoreObject
  static void IgnoreObjectLocked(void* ptr);
  // Helper for DisableChecksAt
  static void DisableChecksAtLocked(void* address);
  // Helper for DisableChecksIn
  static void DisableChecksInLocked(const char* pattern);
  // Helper for DisableChecksToHereFrom
  static void DisableChecksFromToLocked(void* start_address,
                                        void* end_address,
                                        int max_depth);
  // Helper for DoNoLeaks to ignore all objects reachable from all live data
  static void IgnoreAllLiveObjectsLocked(void* self_stack_top);
  // Callback we pass to ListAllProcessThreads (see thread_lister.h)
  // that is invoked when all threads of our process are found and stopped.
  // The call back does the things needed to ignore live data reachable from
  // thread stacks and registers for all our threads
  // as well as do other global-live-data ignoring
  // (via IgnoreNonThreadLiveObjectsLocked)
  // during the quiet state of all threads being stopped.
  // For the argument meaning see the comment by ListAllProcessThreads.
  // Here we only use num_threads and thread_pids, that ListAllProcessThreads
  // fills for us with the number and pids of all the threads of our process
  // it found and attached to.
  static int IgnoreLiveThreads(void* parameter,
                               int num_threads,
                               pid_t* thread_pids,
                               va_list ap);
  // Helper for IgnoreAllLiveObjectsLocked and IgnoreLiveThreads
  // that we prefer to execute from IgnoreLiveThreads
  // while all threads are stopped.
  // This helper does live object discovery and ignoring
  // for all objects that are reachable from everything
  // not related to thread stacks and registers.
  static void IgnoreNonThreadLiveObjectsLocked();
  // Helper for IgnoreNonThreadLiveObjectsLocked and IgnoreLiveThreads
  // to discover and ignore all heap objects
  // reachable from currently considered live objects
  // (live_objects static global variable in out .cc file).
  // "name", "name2" are two strings that we print one after another
  // in a debug message to describe what kind of live object sources
  // are being used.
  static void IgnoreLiveObjectsLocked(const char* name, const char* name2);
  // Heap profile object filtering callback to filter out live objects.
  static bool HeapProfileFilter(void* ptr, size_t size);
  // Runs REGISTER_HEAPCHECK_CLEANUP cleanups and potentially
  // calls DoMainHeapCheck
  static void RunHeapCleanups();
  // Do the overall whole-program heap leak check
  static void DoMainHeapCheck();

  // Type of task for UseProcMapsLocked
  enum ProcMapsTask {
    RECORD_GLOBAL_DATA,
    DISABLE_LIBRARY_ALLOCS
  };
  // Success/Error Return codes for UseProcMapsLocked.
  enum ProcMapsResult {
    PROC_MAPS_USED,
    CANT_OPEN_PROC_MAPS,
    NO_SHARED_LIBS_IN_PROC_MAPS
  };
  // Read /proc/self/maps, parse it, and do the 'proc_maps_task' for each line.
  static ProcMapsResult UseProcMapsLocked(ProcMapsTask proc_maps_task);
  // A ProcMapsTask to disable allocations from 'library'
  // that is mapped to [start_address..end_address)
  // (only if library is a certain system library).
  static void DisableLibraryAllocsLocked(const char* library,
                                         uintptr_t start_address,
                                         uintptr_t end_address);

  // Return true iff "*ptr" points to a heap object;
  // we also fill *object_size for this object then.
  // If yes, we might move "*ptr" to point to the very start of the object
  // (this needs to happen for C++ class array allocations
  // and for basic_string-s of C++ library that comes with gcc 3.4).
  static bool HaveOnHeapLocked(void** ptr, size_t* object_size);

 private:

  // Helper for VerifyHeapProfileTableStackGet in the unittest
  // to get the recorded allocation caller for ptr,
  // which must be a heap object.
  static void* GetAllocCaller(void* ptr);
  friend void VerifyHeapProfileTableStackGet();

  // This gets to execute before constructors for all global objects
  static void BeforeConstructors();
  friend void HeapLeakChecker_BeforeConstructors();
  // This gets to execute after destructors for all global objects
  friend void HeapLeakChecker_AfterDestructors();
  // Helper to shutdown heap leak checker when it's not needed
  // or can't function properly.
  static void TurnItselfOff();

 private:

  // Start whole-executable checking.
  HeapLeakChecker();

 private:
  // Disallow "evil" constructors.
  HeapLeakChecker(const HeapLeakChecker&);
  void operator=(const HeapLeakChecker&);
};


// A class that exists solely to run its destructor.  This class should not be
// used directly, but instead by the REGISTER_HEAPCHECK_CLEANUP macro below.
class HeapCleaner {
 public:
  typedef void (*void_function)(void);
  HeapCleaner(void_function f);
  static void RunHeapCleanups();
 private:
  static std::vector<void_function>* heap_cleanups_;
};

// A macro to declare module heap check cleanup tasks
// (they run only if we are doing heap leak checking.)
// 'body' should be the cleanup code to run.  'name' doesn't matter,
// but must be unique amongst all REGISTER_HEAPCHECK_CLEANUP calls.
#define REGISTER_HEAPCHECK_CLEANUP(name, body)  \
  namespace { \
  void heapcheck_cleanup_##name() { body; } \
  static HeapCleaner heapcheck_cleaner_##name(&heapcheck_cleanup_##name); \
  }

#endif  // BASE_HEAP_CHECKER_H__
