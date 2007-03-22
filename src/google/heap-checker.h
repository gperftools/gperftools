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
// Heap memory leak checker (utilizes heap-profiler and pprof).
//
// TODO(jandrews): rewrite this documentation
// HeapLeakChecker, a heap memory leak checking class.
//
// Verifies that there are no memory leaks between its
// construction and call to its *NoLeaks() or *SameHeap() member.
//
// It will dump two profiles at these two events
// (named <prefix>.<name>-beg.heap and <prefix>.<name>-end.heap
//  where <prefix> is determined automatically to some temporary location
//  and <name> is given in the HeapLeakChecker's constructor)
// and will return false in case the amount of in-use memory
// is more at the time of *NoLeaks() call than
// (or respectively differs at the time of *SameHeap())
// from what it was at the time of our construction.
// It will also in this case print a message on how to process the dumped
// profiles to locate leaks.
//
// GUIDELINE: In addition to the local heap leak checking between two arbitrary
// points in program's execution via an explicit HeapLeakChecker object,
// we provide a way for overall whole-program heap leak checking,
// which is WHAT ONE SHOULD NORMALLY USE.
//
// Currently supported heap-check types, from less strict to more
// strict, are:
//     "minimal", "normal", "strict", "draconian"
//
// There are also two more types: "as-is" and "local".
//
// GUIDELINE CONT.: Depending on the value of the HEAPCHECK variable
// -- as well as other flags of this module -- different modifications
// of leak checking between different points in program's execution
// take place.  The "as-is" value leaves control to the other flags of
// this module.  The "local" value does not start whole-program heap
// leak checking but activates all the machinery needed for local heap
// leak checking via explicitly created HeapLeakChecker objects.
//
// For the case of "normal" everything from before execution of all
// global variable constructors to normal program exit (namely after
// main() returns and after all REGISTER_HEAPCHECK_CLEANUP's are
// executed, but before any global variable destructors are executed)
// is checked for the absence of heap memory leaks.
//
// NOTE: For all but "draconian" whole-program leak check we also
// ignore all heap objects reachable (at the time of the check)
// from any global variable or any live thread stack variable
// or from any object identified by a HeapLeakChecker::IgnoreObject() call.
//
// CAVEAT: We do a liveness flood by traversing pointers to heap objects
// starting from some initial memory regions we know to potentially
// contain live pointer data.
// -- It might potentially not find some (global)
//    live data region to start the flood from,
//    but we consider such cases to be our bugs to fix.
// The liveness flood approach although not being very portable
// and 100% exact works in most cases (see below)
// and saves us from writing a lot of explicit clean up code
// and other hassles when dealing with thread data.
//
// The liveness flood simply attempts to treat any properly aligned
// byte sequences as pointers to heap objects and thinks that
// it found a good pointer simply when the current heap memory map
// contains an object with the address whose byte representation we found.
// As a result of this simple approach, it's unlikely but very possible
// for the flood to be inexact and occasionally result in leaked objects
// being erroneously determined to be live.
// Numerous reasons can lead to this, e.g.:
// - Random bit patters can happen to look
//   like pointers to leaked heap objects.
// - Stale pointer data not corresponding to any live program variables
//   can be still present in memory regions (e.g. thread stacks --see below)
//   that we consider to be live.
// - Stale pointer data that we did not clear can point
//   to a now leaked heap object simply because the heap object
//   address got reused by the memory allocator, e.g.
//     char* p = new char[1];
//     delete p;
//     new char[1];  // this is leaked but p might be pointing to it
//
// The implications of these imprecisions of the liveness flood
// are as follows:
// - For any heap leak check we might miss some memory leaks.
// - For a whole-program leak check, a leak report *does* always
//   correspond to a real leak (unless of course the heap-checker has a bug).
//   This is because in this case we start with an empty heap profile,
//   so there's never an issue of it saying that some heap objects
//   are live when they are not.
// - For local leak checks, a leak report can be a partial false positive
//   in the sense that the reported leaks might have actually occurred
//   before this local leak check was started.
//   Here's an example scenario: When we start a local check
//   heap profile snapshot mistakenly says that some previously
//   leaked objects are live.
//   When we end the local check the heap profile snapshot now correctly
//   determines that those objects are unreachable and reports them as leaks
//   for this leak check, whereas they had been already leaked before
//   this leak check has started.
//
// THREADS and heap leak checking: At the time of HeapLeakChecker's
// construction and during *NoLeaks()/*SameHeap() calls we grab a lock so that
// heap activity in other threads is paused for the time
// we are recording or analyzing the state of the heap.
// For any heap leak check it is possible to have
// other threads active and working with the heap
// when we make the HeapLeakChecker object or do its leak checking
// provided all these threads are discoverable with the implemetation
// of thread_lister.h (e.g. are linux pthreads).
// In this case leak checking should deterministically work fine.
//
// CAVEAT: Thread stack data ignoring (via thread_lister.h)
// does not work if the program is running under gdb, probably becauce the
// ptrace functionality needed for thread_lister is already hooked to by gdb.
//
// As mentioned above thread stack liveness determination
// might miss-classify objects that very recently became unreachable (leaked)
// as reachable in the cases when the values of the pointers
// to the now unreachable objects are still present in the active stack frames,
// while the pointers actually no longer correspond to any live program
// variables.
// For this reason trivial code like the following
// might not produce the expected leak checking outcome
// depending on how the compiled code works with the stack:
//
//   int* foo = new int [20];
//   HeapLeakChecker check("a_check");
//   foo = NULL;
//   CHECK(check.NoLeaks());  // this might succeed
//
// HINT: If you are debugging detected leaks, you can try different
// (e.g. less strict) values for HEAPCHECK to determine the cause of
// the reported leaks (see the code of
// HeapLeakChecker::InternalInitStart for details).
//
// GUIDELINE: Below are the preferred ways of making your (test)
// binary pass the above recommended overall heap leak check in the
// order of decreasing preference:
//
// 1. Fix the leaks if they are real leaks.
//
// 2. If you are sure that the reported leaks are not dangerous
//    and there is no good way to fix them, then you can use
//    HeapLeakChecker::DisableChecks(Up|In|At) calls (see below) in
//    the relevant modules to disable certain stack traces for the
//    purpose of leak checking.  You can also use
//    HeapLeakChecker::IgnoreObject() call to ignore certain leaked
//    heap objects and everythign reachable from them.
//
// 3. If the leaks are due to some initialization in a third-party
//    package, you might be able to force that initialization before
//    the heap checking starts.
//
//    I.e. if HEAPCHECK == "minimal" or less strict, if you put the
//    initialization in a global constructor the heap-checker will
//    ignore it.  If HEAPCHECK == "normal" or stricter, only
//    HeapLeakChecker::LibCPreallocate() happens before heap checking
//    starts.
//
// Making a binary pass at "strict" or "draconian" level is not
// necessary or even desirable in the numerous cases when it requires
// adding a lot of (otherwise unused) heap cleanup code to various
// core libraries.
//
// NOTE: the following should apply only if
//       HEAPCHECK == "strict" or stricter
//
// 4. If the found leaks are due to incomplete cleanup in destructors
//    of global variables, extend or add those destructors or use a
//    REGISTER_HEAPCHECK_CLEANUP to do the deallocations instead to
//    avoid cleanup overhead during normal execution.  This type of
//    leaks get reported when one goes from "normal" to "strict"
//    checking.
//
// NOTE: the following should apply only if
//       HEAPCHECK == "draconian" or stricter
//
// 5. If the found leaks are for global static pointers whose values are
//    allocated/grown (e.g on-demand) and never deallocated,
//    then you should be able to add REGISTER_HEAPCHECK_CLEANUP's
//    or appropriate destructors into these modules
//    to free those objects.
//
// Example of local usage (anywhere in the program):
//
//   HeapLeakChecker heap_checker("test_foo");
//
//   { <code that exercises some foo functionality
//      that should preserve memory allocation state> }
//
//   CHECK(heap_checker.SameHeap());
//
// NOTE: One should set HEAPCHECK to a non-empty value e.g. "local"
// to help suppress some false leaks for these local checks.


#ifndef BASE_HEAP_CHECKER_H__
#define BASE_HEAP_CHECKER_H__

#include <sys/types.h>    // for size_t
#include <vector>

// A macro to declare module heap check cleanup tasks
// (they run only if we are doing heap leak checking.)
// Use
//  public:
//   void Class::HeapCleanup();
// if you need to do heap check cleanup on private members of a class.
#define REGISTER_HEAPCHECK_CLEANUP(name, body)  \
  namespace { \
  void heapcheck_cleanup_##name() { body; } \
  static HeapCleaner heapcheck_cleaner_##name(&heapcheck_cleanup_##name); \
  }

// A class that exists solely to run its destructor.  This class should not be
// used directly, but instead by the REGISTER_HEAPCHECK_CLEANUP macro above.
class HeapCleaner {
 public:
  typedef void (*void_function)(void);
  HeapCleaner(void_function f);
  static void RunHeapCleanups();
 private:
  static std::vector<void_function>* heap_cleanups_;
};

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
  bool NoLeaks() { return DoNoLeaks(false, true, true); }

  // Return true iff the heap does not seem to have more objects allocated
  // w.r.t. its state at the time of our construction
  // by looking at the number of objects & bytes allocated.
  // This also tries to do pprof reporting of detected leaks.
  bool QuickNoLeaks() { return DoNoLeaks(false, false, true); }

  // Return true iff the heap does not seem to have more objects allocated
  // w.r.t. its state at the time of our construction
  // by looking at the number of objects & bytes allocated.
  // This does not try to use pprof at all.
  bool BriefNoLeaks() { return DoNoLeaks(false, false, false); }

  // These are similar to their *NoLeaks counterparts,
  // but they in addition require no negative leaks,
  // i.e. the state of the heap must be exactly the same
  // as at the time of our construction.
  bool SameHeap() { return DoNoLeaks(true, true, true); }
  bool QuickSameHeap() { return DoNoLeaks(true, false, true); }
  bool BriefSameHeap() { return DoNoLeaks(true, false, false); }

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

  // Destructor
  // (verifies that some *SameHeap or *NoLeaks method has been called).
  ~HeapLeakChecker();

  // Accessors to determine various internal parameters.  These should
  // be set as early as possible.

  // If overall heap check reports found leaks via pprof.  Default: true
  static void set_heap_check_report(bool);
  // Location of pprof script.  Default: $prefix/bin/pprof
  static void set_pprof_path(const char*);
  // Location to write profile dumps.  Default: /tmp
  static void set_dump_directory(const char*);

  static bool heap_check_report();
  static const char* pprof_path();
  static const char* dump_directory();

 private:  // data

  char* name_;  // our remembered name
  size_t start_inuse_bytes_;  // bytes in use at our construction
  size_t start_inuse_allocs_;  // allocations in use at our construction
  bool has_checked_;  // if we have done the leak check, so these are ready:
  ssize_t inuse_bytes_increase_;  // bytes-in-use increase for this checker
  ssize_t inuse_allocs_increase_;  // allocations-in-use increase for this checker

  static pid_t main_thread_pid_;  // For naming output files
  static std::string* dump_directory_; // Location to write profile dumps

 public:  // Static helpers to make us ignore certain leaks.

  // NOTE: All calls to DisableChecks* affect all later heap profile generation
  // that happens in our construction and inside of *NoLeaks().
  // They do nothing when heap leak checking is turned off.

  // CAVEAT: Disabling via all the DisableChecks* functions happens only
  // up to kMaxStackTrace stack frames (see heap-profiler.cc)
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

  // Full starting of recommended whole-program checking.  This runs after
  // HeapChecker::BeforeConstructors and can do initializations which may
  // depend on configuration parameters set by initialization code.
  // Valid values of heap_check type are:
  //  - "minimal"
  //  - "normal"
  //  - "strict"
  //  - "draconian"
  //  - "local"
  static void InternalInitStart(const std::string& heap_check_type);

  struct RangeValue;
  struct StackExtent;

 private:  // Various helpers

  // Helper for dumping start/end heap leak checking profiles.
  void DumpProfileLocked(bool start, const StackExtent& self_stack);
  // Helper for constructors
  void Create(const char *name);
  // Helper for *NoLeaks and *SameHeap
  bool DoNoLeaks(bool same_heap, bool do_full, bool do_report);
  // Helper for IgnoreObject
  static void IgnoreObjectLocked(void* ptr);
  // Helper for DisableChecksAt
  static void DisableChecksAtLocked(void* address);
  // Helper for DisableChecksIn
  static void DisableChecksInLocked(const char* pattern);
  // Helper for DisableChecksToHereFrom
  static void DisableChecksFromTo(void* start_address,
                                  void* end_address,
                                  int max_depth);
  // Helper for DoNoLeaks to ignore all objects reachable from all live data
  static void IgnoreAllLiveObjectsLocked(const StackExtent& self_stack);
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
  // Runs REGISTER_HEAPCHECK_CLEANUP cleanups and potentially
  // calls DoMainHeapCheck
  static void RunHeapCleanups();
  // Do the overall whole-program heap leak check
  static void DoMainHeapCheck();

  // Type of task for UseProcMaps
  enum ProcMapsTask { 
    RECORD_GLOBAL_DATA_LOCKED, 
    DISABLE_LIBRARY_ALLOCS 
  };
  // Success/Error Return codes for UseProcMaps.
  enum ProcMapsResult { PROC_MAPS_USED, CANT_OPEN_PROC_MAPS,
                        NO_SHARED_LIBS_IN_PROC_MAPS };
  // Read /proc/self/maps, parse it, and do the 'proc_maps_task' for each line.
  static ProcMapsResult UseProcMaps(ProcMapsTask proc_maps_task);
  // A ProcMapsTask to disable allocations from 'library'
  // that is mapped to [start_address..end_address)
  // (only if library is a certain system library).
  static void DisableLibraryAllocs(const char* library,
                                   void* start_address,
                                   void* end_address);

 private:

  // This gets to execute before constructors for all global objects
  static void BeforeConstructors();
  friend void HeapLeakChecker_BeforeConstructors();
  // This gets to execute after destructors for all global objects
  friend void HeapLeakChecker_AfterDestructors();

 private:
  // Start whole-executable checking.
  HeapLeakChecker();

  // Don't allow copy constructors -- these are declared but not defined
  HeapLeakChecker(const HeapLeakChecker&);
  void operator=(const HeapLeakChecker&);
};

#endif  // BASE_HEAP_CHECKER_H__
