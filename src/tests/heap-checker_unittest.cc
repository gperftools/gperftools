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
// Author: Maxim Lifantsev
//
// Running:
// ./heap-checker_unittest
//
// If the unittest crashes because it can't find pprof, try:
// PPROF_PATH=/usr/local/someplace/bin/pprof ./heap-checker_unittest
//
// To test that the whole-program heap checker will actually cause a leak, try:
// HEAPCHECK_TEST_LEAK= ./heap-checker_unittest
// HEAPCHECK_TEST_LOOP_LEAK= ./heap-checker_unittest
//
// Note: Both of the above commands *should* abort with an error message.

// CAVEAT: Do not use vector<> and string on-heap objects in this test,
// otherwise the test can sometimes fail for tricky leak checks
// when we want some allocated object not to be found live by the heap checker.
// This can happen with memory allocators like tcmalloc that can allocate
// heap objects back to back without any book-keeping data in between.
// What happens is that end-of-storage pointers of a live vector
// (or a string depending on the STL implementation used)
// can happen to point to that other heap-allocated
// object that is not reachable otherwise and that
// we don't want to be reachable.
//
// The implication of this for real leak checking
// is just one more chance for the liveness flood to be inexact
// (see the comment in our .h file).

#include "config_for_unittests.h"
#include <sys/poll.h>
#if defined HAVE_STDINT_H
#include <stdint.h>             // to get uint16_t (ISO naming madness)
#elif defined HAVE_INTTYPES_H
#include <inttypes.h>           // another place uint16_t might be defined
#endif
#include <sys/types.h>
#include <stdlib.h>
#include <errno.h>              // errno
#ifdef HAVE_UNISTD_H
#include <unistd.h>             // for sleep(), geteuid()
#endif
#ifdef HAVE_MMAP
#include <sys/mman.h>
#endif
#include <fcntl.h>              // for open(), close()
// FreeBSD has malloc.h, but complains if you use it
#if defined(HAVE_MALLOC_H) && !defined(__FreeBSD__)
#include <malloc.h>
#endif

#include <netinet/in.h>         // inet_ntoa
#include <arpa/inet.h>          // inet_ntoa
#ifdef HAVE_EXECINFO_H
#include <execinfo.h>           // backtrace
#endif
#ifdef HAVE_GRP_H
#include <grp.h>                // getgrent, getgrnam
#endif
#ifdef HAVE_PWD_H
#include <pwd.h>
#endif

#include <iostream>             // for cout
#include <iomanip>              // for hex
#include <set>
#include <map>
#include <vector>
#include <string>

#include "base/googleinit.h"
#include "base/logging.h"
#include "base/commandlineflags.h"
#include <google/heap-checker.h>
#include "memory_region_map.h"
#include <google/malloc_extension.h>
#include <google/stacktrace.h>

using namespace std;


// TODO(maxim): write a shell script to test that these indeed crash us
//              (i.e. we do detect leaks)
//              Maybe add more such crash tests.

DEFINE_bool(test_leak,
            EnvToBool("HEAP_CHECKER_TEST_TEST_LEAK", false),
            "If should cause a leak crash");
DEFINE_bool(test_loop_leak,
            EnvToBool("HEAP_CHECKER_TEST_TEST_LOOP_LEAK", false),
            "If should cause a looped leak crash");
DEFINE_bool(test_register_leak,
            EnvToBool("HEAP_CHECKER_TEST_TEST_REGISTER_LEAK", false),
            "If should cause a leak crash by hiding a pointer "
            "that is only in a register");
DEFINE_bool(test_cancel_global_check,
            EnvToBool("HEAP_CHECKER_TEST_TEST_CANCEL_GLOBAL_CHECK", false),
            "If should test HeapLeakChecker::CancelGlobalCheck "
            "when --test_leak or --test_loop_leak are given; "
            "the test should not fail then");
DEFINE_bool(maybe_stripped,
            EnvToBool("HEAP_CHECKER_TEST_MAYBE_STRIPPED", true),
            "If we think we can be a stripped binary");
DEFINE_bool(interfering_threads,
            EnvToBool("HEAP_CHECKER_TEST_INTERFERING_THREADS", true),
            "If we should use threads trying "
            "to interfere with leak checking");
DEFINE_bool(hoarding_threads,
            EnvToBool("HEAP_CHECKER_TEST_HOARDING_THREADS", true),
            "If threads (usually the manager thread) are known "
            "to retain some old state in their global buffers, "
            "so that it's hard to force leaks when threads are around");
            // TODO(maxim): Chage the default to false
            // when the standard environment used NTPL threads:
            // they do not seem to have this problem.
DEFINE_bool(no_threads,
            EnvToBool("HEAP_CHECKER_TEST_NO_THREADS", false),
            "If we should not use any threads");
            // This is used so we can make can_create_leaks_reliably true
            // for any pthread implementation and test with that.

#define WARN_IF(cond, msg)   LOG_IF(WARNING, cond, msg)

// This is an evil macro!  Be very careful using it...
#undef VLOG          // and we start by evilling overriding logging.h VLOG
#define VLOG(lvl)    if (FLAGS_verbose >= (lvl))  cout << "\n"
// This is, likewise, evil
#define LOGF         VLOG(INFO)


class Closure {
 public:
  virtual ~Closure() { }
  virtual void Run() = 0;
};

class Callback0 : public Closure {
 public:
  typedef void (*FunctionSignature)();

  inline Callback0(FunctionSignature f) : f_(f) {}
  virtual void Run() { (*f_)(); delete this; }

 private:
  FunctionSignature f_;
};

template <class P1> class Callback1 : public Closure {
 public:
  typedef void (*FunctionSignature)(P1);

  inline Callback1<P1>(FunctionSignature f, P1 p1) : f_(f), p1_(p1) {}
  virtual void Run() { (*f_)(p1_); delete this; }

 private:
  FunctionSignature f_;
  P1 p1_;
};

template <class P1, class P2> class Callback2 : public Closure {
 public:
  typedef void (*FunctionSignature)(P1,P2);

  inline Callback2<P1,P2>(FunctionSignature f, P1 p1, P2 p2) : f_(f), p1_(p1), p2_(p2) {}
  virtual void Run() { (*f_)(p1_, p2_); delete this; }

 private:
  FunctionSignature f_;
  P1 p1_;
  P2 p2_;
};

inline Callback0* NewCallback(void (*function)()) {
  return new Callback0(function);
}

template <class P1>
inline Callback1<P1>* NewCallback(void (*function)(P1), P1 p1) {
  return new Callback1<P1>(function, p1);
}

template <class P1, class P2>
inline Callback2<P1,P2>* NewCallback(void (*function)(P1,P2), P1 p1, P2 p2) {
  return new Callback2<P1,P2>(function, p1, p2);
}


// Set to true at end of main, so threads know.  Not entirely thread-safe!,
// but probably good enough.
static bool g_have_exited_main = false;

// If we can reliably create leaks (i.e. make leaked object
// really unreachable from any global data).
static bool can_create_leaks_reliably = false;

// We use a simple allocation wrapper
// to make sure we wipe out the newly allocated objects
// in case they still happened to contain some pointer data
// accidently left by the memory allocator.
struct Initialized { };
static Initialized initialized;
void* operator new(size_t size, const Initialized&) {
  // Below we use "p = new(initialized) Foo[1];" and  "delete[] p;"
  // instead of "p = new(initialized) Foo;"
  // when we need to delete an allocated object.
  void* p = malloc(size);
  memset(p, 0, size);
  return p;
}
void* operator new[](size_t size, const Initialized&) {
  char* p = new char[size];
  memset(p, 0, size);
  return p;
}

static void Pause() {
  poll(NULL, 0, 77);  // time for thread activity in HeapBusyThreadBody

  // Indirectly test malloc_extension.*:
  CHECK(MallocExtension::instance()->VerifyAllMemory());
  int blocks;
  size_t total;
  int histogram[kMallocHistogramSize];
  if (MallocExtension::instance()
       ->MallocMemoryStats(&blocks, &total, histogram)  &&  total != 0) {
    VLOG(3) << "Malloc stats: " << blocks << " blocks of "
            << total << " bytes";
    for (int i = 0; i < kMallocHistogramSize; ++i) {
      if (histogram[i]) {
        VLOG(3) << "  Malloc histogram at " << i << " : " << histogram[i];
      }
    }
  }
}

// Make gcc think a pointer is "used"
template <class T>
static void Use(T** foo) {
  VLOG(2) << "Dummy-using " << static_cast<void*>(*foo) << " at " << foo;
}

// Arbitrary value, but not such that xor'ing with it is likely
// to map one valid pointer to another valid pointer:
static const uintptr_t kHideMask = 0xF03A5F7B;

// Helpers to hide a pointer from live data traversal.
// We just xor the pointer so that (with high probability)
// it's not a valid address of a heap object anymore.
// Both Hide and UnHide must be executed within RunHidden() below
// to prevent leaving stale data on active stack that can be a pointer
// to a heap object that is not actually reachable via live variables.
// (UnHide might leave heap pointer value for an object
//  that will be deallocated but later another object
//  can be allocated at the same heap address.)
template <class T>
static void Hide(T** ptr) {
  // we cast values, not dereferenced pointers, so no aliasing issues:
  *ptr = reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(*ptr) ^ kHideMask);
  VLOG(2) << "hid: " << static_cast<void*>(*ptr);
}

template <class T>
static void UnHide(T** ptr) {
  VLOG(2) << "unhiding: " << static_cast<void*>(*ptr);
  // we cast values, not dereferenced pointers, so no aliasing issues:
  *ptr = reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(*ptr) ^ kHideMask);
}

static void LogHidden(const char* message, const void* ptr) {
  LOGF << message << " : "
       << ptr << " ^ " << reinterpret_cast<void*>(kHideMask) << endl;
}

// non-static to fool the compiler against inlining
extern void (*run_hidden_ptr)(Closure* c, int n);
void (*run_hidden_ptr)(Closure* c, int n);
extern void (*wipe_stack_ptr)(int n);
void (*wipe_stack_ptr)(int n);

static void DoRunHidden(Closure* c, int n) {
  if (n) {
    VLOG(10) << "Level " << n << " at " << &n;
    run_hidden_ptr(c, n-1);
    wipe_stack_ptr(n);
    sleep(0);  // undo -foptimize-sibling-calls
  } else {
    c->Run();
  }
}

static void DoWipeStack(int n) {
  VLOG(10) << "Wipe level " << n << " at " << &n;
  if (n) {
    const int sz = 30;
    volatile int arr[sz];
    for (int i = 0; i < sz; ++i)  arr[i] = 0;
    wipe_stack_ptr(n-1);
    sleep(0);  // undo -foptimize-sibling-calls
  }
}

// This executes closure c several stack frames down from the current one
// and then makes an effort to also wipe out the stack data that was used by
// the closure.
// This way we prevent leak checker from finding any temporary pointers
// of the closure execution on the stack and deciding that
// these pointers (and the pointed objects) are still live.
static void RunHidden(Closure* c) {
  DoRunHidden(c, 15);
  DoWipeStack(20);
}

static void DoAllocHidden(size_t size, void** ptr) {
  void* p = new(initialized) char[size];
  Hide(&p);
  Use(&p);  // use only hidden versions
  VLOG(2) << "Allocated hidden " << p << " at " << &p;
  *ptr = p;  // assign the hidden versions
}

static void* AllocHidden(size_t size) {
  void* r;
  RunHidden(NewCallback(DoAllocHidden, size, &r));
  return r;
}

static void DoDeAllocHidden(void** ptr) {
  Use(ptr);  // use only hidden versions
  void* p = *ptr;
  VLOG(2) << "Deallocating hidden " << p;
  UnHide(&p);
  delete [] (char*)p;
}

static void DeAllocHidden(void** ptr) {
  RunHidden(NewCallback(DoDeAllocHidden, ptr));
  *ptr = NULL;
  Use(ptr);
}

static bool RunSilent(HeapLeakChecker* check,
                      bool (HeapLeakChecker::* func)()) {
  // By default, don't print the 'we detected a leak' message in the
  // cases we're expecting a leak (we still print when --v is >= 1).
  // This way, the logging output is less confusing: we only print
  // "we detected a leak", and how to diagnose it, for *unexpected* leaks.
  int32 old_FLAGS_verbose = FLAGS_verbose;
  if (!VLOG_IS_ON(1))             // not on a verbose setting
    FLAGS_verbose = FATAL;        // only log fatal errors
  const bool retval = (check->*func)();
  FLAGS_verbose = old_FLAGS_verbose;
  return retval;
}

#define RUN_SILENT(check, func)  RunSilent(&(check), &HeapLeakChecker::func)

enum CheckType { SAME_HEAP, NO_LEAKS };

static void VerifyLeaks(HeapLeakChecker* check, CheckType type,
                        int leaked_bytes, int leaked_objects) {
  const bool no_leaks =
    type == NO_LEAKS ? RUN_SILENT(*check, BriefNoLeaks)
                     : RUN_SILENT(*check, BriefSameHeap);
  if (can_create_leaks_reliably) {
    // these might still fail occasionally, but it should be very rare
    CHECK_EQ(no_leaks, false);
    CHECK_EQ(check->BytesLeaked(), leaked_bytes);
    CHECK_EQ(check->ObjectsLeaked(), leaked_objects);
  } else {
    WARN_IF(no_leaks != false,
            "Expected leaks not found: "
            "Some liveness flood must be too optimistic");
  }
}

// not deallocates
static void TestHeapLeakCheckerDeathSimple() {
  HeapLeakChecker check("death_simple");
  void* foo = AllocHidden(100 * sizeof(int));
  Use(&foo);
  void* bar = AllocHidden(300);
  Use(&bar);
  LogHidden("Leaking", foo);
  LogHidden("Leaking", bar);
  Pause();
  VerifyLeaks(&check, NO_LEAKS, 300 + 100 * sizeof(int), 2);
  DeAllocHidden(&foo);
  DeAllocHidden(&bar);
}

static void MakeDeathLoop(void** arr1, void** arr2) {
  void** a1 = new(initialized) void*[2];
  void** a2 = new(initialized) void*[2];
  a1[1] = (void*)a2;
  a2[1] = (void*)a1;
  Hide(&a1);
  Hide(&a2);
  Use(&a1);
  Use(&a2);
  VLOG(2) << "Made hidden loop at " << &a1 << " to " << arr1;
  *arr1 = a1;
  *arr2 = a2;
}

// not deallocates two objects linked together
static void TestHeapLeakCheckerDeathLoop() {
  HeapLeakChecker check("death_loop");
  void* arr1;
  void* arr2;
  RunHidden(NewCallback(MakeDeathLoop, &arr1, &arr2));
  Use(&arr1);
  Use(&arr2);
  LogHidden("Leaking", arr1);
  LogHidden("Leaking", arr2);
  Pause();
  VerifyLeaks(&check, NO_LEAKS, 4 * sizeof(void*), 2);
  DeAllocHidden(&arr1);
  DeAllocHidden(&arr2);
}

// deallocates more than allocates
static void TestHeapLeakCheckerDeathInverse() {
  void* bar = AllocHidden(250 * sizeof(int));
  Use(&bar);
  LogHidden("Pre leaking", bar);
  Pause();
  HeapLeakChecker check("death_inverse");
  void* foo = AllocHidden(100 * sizeof(int));
  Use(&foo);
  LogHidden("Leaking", foo);
  DeAllocHidden(&bar);
  Pause();
  VerifyLeaks(&check, SAME_HEAP, -150 * int64(sizeof(int)), 0);
  DeAllocHidden(&foo);
}

// deallocates more than allocates
static void TestHeapLeakCheckerDeathNoLeaks() {
  void* foo = AllocHidden(100 * sizeof(int));
  Use(&foo);
  void* bar = AllocHidden(250 * sizeof(int));
  Use(&bar);
  HeapLeakChecker check("death_noleaks");
  DeAllocHidden(&bar);
  CHECK_EQ(check.BriefNoLeaks(), true);
  DeAllocHidden(&foo);
}

// have less objecs
static void TestHeapLeakCheckerDeathCountLess() {
  void* bar1 = AllocHidden(50 * sizeof(int));
  Use(&bar1);
  void* bar2 = AllocHidden(50 * sizeof(int));
  Use(&bar2);
  LogHidden("Pre leaking", bar1);
  LogHidden("Pre leaking", bar2);
  Pause();
  HeapLeakChecker check("death_count_less");
  void* foo = AllocHidden(100 * sizeof(int));
  Use(&foo);
  LogHidden("Leaking", foo);
  DeAllocHidden(&bar1);
  DeAllocHidden(&bar2);
  Pause();
  VerifyLeaks(&check, SAME_HEAP, 0, -1);
  DeAllocHidden(&foo);
}

// have more objecs
static void TestHeapLeakCheckerDeathCountMore() {
  void* foo = AllocHidden(100 * sizeof(int));
  Use(&foo);
  LogHidden("Pre leaking", foo);
  Pause();
  HeapLeakChecker check("death_count_more");
  void* bar1 = AllocHidden(50 * sizeof(int));
  Use(&bar1);
  void* bar2 = AllocHidden(50 * sizeof(int));
  Use(&bar2);
  LogHidden("Leaking", bar1);
  LogHidden("Leaking", bar2);
  DeAllocHidden(&foo);
  Pause();
  VerifyLeaks(&check, SAME_HEAP, 0, 1);
  DeAllocHidden(&bar1);
  DeAllocHidden(&bar2);
}

// simple tests that deallocate what they allocated
static void TestHeapLeakChecker() {
  { HeapLeakChecker check("trivial");
    int foo = 5;
    int* p = &foo;
    Use(&p);
    Pause();
    CHECK(check.BriefSameHeap());
  }
  Pause();
  { HeapLeakChecker check("simple");
    void* foo = AllocHidden(100 * sizeof(int));
    Use(&foo);
    void* bar = AllocHidden(200 * sizeof(int));
    Use(&bar);
    DeAllocHidden(&foo);
    DeAllocHidden(&bar);
    Pause();
    CHECK(check.BriefSameHeap());
  }
}

// no false positives from pprof
static void TestHeapLeakCheckerPProf() {
  { HeapLeakChecker check("trivial_p");
    int foo = 5;
    int* p = &foo;
    Use(&p);
    Pause();
    CHECK(check.BriefSameHeap());
  }
  Pause();
  { HeapLeakChecker check("simple_p");
    void* foo = AllocHidden(100 * sizeof(int));
    Use(&foo);
    void* bar = AllocHidden(200 * sizeof(int));
    Use(&bar);
    DeAllocHidden(&foo);
    DeAllocHidden(&bar);
    Pause();
    CHECK(check.SameHeap());
  }
}

// trick heap change: same total # of bytes and objects, but
// different individual object sizes
static void TestHeapLeakCheckerTrick() {
  void* bar1 = AllocHidden(240 * sizeof(int));
  Use(&bar1);
  void* bar2 = AllocHidden(160 * sizeof(int));
  Use(&bar2);
  LogHidden("Pre leaking", bar1);
  LogHidden("Pre leaking", bar2);
  Pause();
  HeapLeakChecker check("trick");
  void* foo1 = AllocHidden(280 * sizeof(int));
  Use(&foo1);
  void* foo2 = AllocHidden(120 * sizeof(int));
  Use(&foo2);
  LogHidden("Leaking", foo1);
  LogHidden("Leaking", foo2);
  DeAllocHidden(&bar1);
  DeAllocHidden(&bar2);
  Pause();
  if (can_create_leaks_reliably) {
    // this might still fail occasionally, but it should be very rare:
    CHECK(check.BriefSameHeap());
  } else {
    // we expect it to usually misbehave, so we silence it:
    WARN_IF(RUN_SILENT(check, BriefSameHeap) != true,
            "Tricky leaks unexpectedly found: "
            "Some liveness flood must be too optimistic");
  }
  DeAllocHidden(&foo1);
  DeAllocHidden(&foo2);
}

// no false negatives from pprof
static void TestHeapLeakCheckerDeathTrick() {
  void* bar1 = AllocHidden(240 * sizeof(int));
  Use(&bar1);
  void* bar2 = AllocHidden(160 * sizeof(int));
  Use(&bar2);
  HeapLeakChecker check("death_trick");
  DeAllocHidden(&bar1);
  DeAllocHidden(&bar2);
  void* foo1 = AllocHidden(280 * sizeof(int));
  Use(&foo1);
  void* foo2 = AllocHidden(120 * sizeof(int));
  Use(&foo2);
  // TODO(maxim): use the above if we make pprof work in automated test runs
  if (!FLAGS_maybe_stripped) {
    CHECK_EQ(RUN_SILENT(check, SameHeap), false);
      // pprof checking should catch the leak
  } else {
    WARN_IF(RUN_SILENT(check, SameHeap) != false,
            "death_trick leak is not caught; "
            "we must be using a stripped binary");
  }
  DeAllocHidden(&foo1);
  DeAllocHidden(&foo2);
}

// simple leak
static void TransLeaks() {
  AllocHidden(1 * sizeof(char));
}

// have leaks but disable them
static void DisabledLeaks() {
  HeapLeakChecker::DisableChecksUp(1);
  AllocHidden(3 * sizeof(int));
  TransLeaks();
}

// have leaks but range-disable them
static void RangeDisabledLeaks() {
  void* start_address = HeapLeakChecker::GetDisableChecksStart();
  AllocHidden(3 * sizeof(int));
  TransLeaks();
  HeapLeakChecker::DisableChecksToHereFrom(start_address);
}

// We need this function pointer trickery to fool an aggressive
// optimizing compiler such as icc into not inlining DisabledLeaks().
// Otherwise the stack-frame-address-based disabling in it
// will wrongly disable allocation tracking in
// the functions into which it's inlined.
static void (*disabled_leaks_addr)() = &DisabledLeaks;

// have different disabled leaks
static void* RunDisabledLeaks(void* a) {
  disabled_leaks_addr();
  RangeDisabledLeaks();
  return a;
}

// have different disabled leaks inside of a thread
static void ThreadDisabledLeaks() {
  if (FLAGS_no_threads)  return;
  pthread_t tid;
  pthread_attr_t attr;
  CHECK(pthread_attr_init(&attr) == 0);
  CHECK(pthread_create(&tid, &attr, RunDisabledLeaks, NULL) == 0);
  void* res;
  CHECK(pthread_join(tid, &res) == 0);
}

// different disabled leaks (some in threads)
static void TestHeapLeakCheckerDisabling() {
  HeapLeakChecker check("disabling");

  RunDisabledLeaks(NULL);
  RunDisabledLeaks(NULL);
  ThreadDisabledLeaks();
  RunDisabledLeaks(NULL);
  ThreadDisabledLeaks();
  ThreadDisabledLeaks();

  LOG_IF(FATAL, HeapLeakChecker::HaveDisabledChecksUp(1),
        "Some code with DisableChecksUp() got inlined into here; "
        "need to add more tricks to prevent this inlining.");

  Pause();

  CHECK(check.SameHeap());
}

typedef set<int> IntSet;

static int some_ints[] = { 1, 2, 3, 21, 22, 23, 24, 25 };

static void DoTestSTLAlloc() {
  IntSet* x = new(initialized) IntSet[1];
  *x = IntSet(some_ints, some_ints + 6);
  for (int i = 0; i < 1000; i++) {
    x->insert(i*3);
  }
  delete [] x;
}

// Check that normal STL usage does not result in a leak report.
// (In particular we test that there's no complex STL's own allocator
// running on top of our allocator with hooks to heap profiler
// that can result in false leak report in this case.)
static void TestSTLAlloc() {
  HeapLeakChecker check("stl");
  RunHidden(NewCallback(DoTestSTLAlloc));
  CHECK_EQ(check.BriefSameHeap(), true);
}

static void DoTestSTLAllocInverse(IntSet** setx) {
  IntSet* x = new(initialized) IntSet[1];
  *x = IntSet(some_ints, some_ints + 3);
  for (int i = 0; i < 100; i++) {
    x->insert(i*2);
  }
  Hide(&x);
  *setx = x;
}

static void FreeTestSTLAllocInverse(IntSet** setx) {
  IntSet* x = *setx;
  UnHide(&x);
  delete [] x;
}

// Check that normal leaked STL usage *does* result in a leak report.
// (In particular we test that there's no complex STL's own allocator
// running on top of our allocator with hooks to heap profiler
// that can result in false absence of leak report in this case.)
static void TestSTLAllocInverse() {
  HeapLeakChecker check("death_inverse_stl");
  IntSet* x;
  RunHidden(NewCallback(DoTestSTLAllocInverse, &x));
  LogHidden("Leaking", x);
  if (can_create_leaks_reliably) {
    // these might still fail occasionally, but it should be very rare
    CHECK_EQ(RUN_SILENT(check, BriefNoLeaks), false);
    CHECK_GE(check.BytesLeaked(), 100 * sizeof(int));
    CHECK_GE(check.ObjectsLeaked(), 100);
      // assumes set<>s are represented by some kind of binary tree
      // or something else allocating >=1 heap object per set object
  } else {
    WARN_IF(RUN_SILENT(check, BriefNoLeaks) != false,
            "Expected leaks not found: "
            "Some liveness flood must be too optimistic");
  }
  RunHidden(NewCallback(FreeTestSTLAllocInverse, &x));
}

template<class Alloc>
static void DirectTestSTLAlloc(Alloc allocator, const char* name) {
  HeapLeakChecker check((string("direct_stl-") + name).c_str());
  const int size = 1000;
  typename Alloc::pointer ptrs[size];
  for (int i = 0; i < size; ++i) {
    typename Alloc::pointer p = allocator.allocate(i*3+1);
    HeapLeakChecker::IgnoreObject(p);
    // This will crash if p is not known to heap profiler:
    // (i.e. STL's "allocator" does not have a direct hook to heap profiler)
    HeapLeakChecker::UnIgnoreObject(p);
    ptrs[i] = p;
  }
  for (int i = 0; i < size; ++i) {
    allocator.deallocate(ptrs[i], i*3+1);
    ptrs[i] = NULL;
  }
  CHECK(check.BriefSameHeap());  // just in case
}

static struct group* grp = NULL;
static const int kKeys = 50;
static pthread_key_t key[kKeys];

static void KeyFree(void* ptr) {
  delete [] (char*)ptr;
}

static bool key_init_has_run = false;

static void KeyInit() {
  for (int i = 0; i < kKeys; ++i) {
    CHECK_EQ(pthread_key_create(&key[i], KeyFree), 0);
    VLOG(2) << "pthread key " << i << " : " << key[i];
  }
  key_init_has_run = true;   // needed for a sanity-check
}

// force various C library static and thread-specific allocations
static void TestLibCAllocate() {
  CHECK(key_init_has_run);
  for (int i = 0; i < kKeys; ++i) {
    void* p = pthread_getspecific(key[i]);
    if (NULL == p) {
      if (i == 0) {
        // Test-logging inside threads which (potentially) creates and uses
        // thread-local data inside standard C++ library:
        VLOG(0) << "Adding pthread-specifics for thread " << pthread_self()
                << " pid " << getpid();
      }
      p = new(initialized) char[77 + i];
      VLOG(2) << "pthread specific " << i << " : " << p;
      pthread_setspecific(key[i], p);
    }
  }

  strerror(errno);
  struct in_addr addr;
  addr.s_addr = INADDR_ANY;
  inet_ntoa(addr);
  const time_t now = time(NULL);
  ctime(&now);
  void *stack[1];
#ifdef HAVE_EXECINFO_H
  backtrace(stack, 1);
#endif
#ifdef HAVE_GRP_H
  gid_t gid = getgid();
  getgrgid(gid);
  if (grp == NULL)  grp = getgrent();  // a race condition here is okay
  getgrnam(grp->gr_name);
#endif
#ifdef HAVE_PWD_H
  getpwuid(geteuid());
#endif
}

// Continuous random heap memory activity to try to disrupt heap checking.
static void* HeapBusyThreadBody(void* a) {
  TestLibCAllocate();

  int user = 0;
  // Try to hide ptr from heap checker in a CPU register:
  // Here we are just making a best effort to put the only pointer
  // to a heap object into a thread register to test
  // the thread-register finding machinery in the heap checker.
#if defined(__i386__) && defined(__GNUC__)
  register int** ptr asm("esi");
#elif defined(__x86_64__) && defined(__GNUC__)
  register int** ptr asm("r15");
#else
  register int** ptr;
#endif
  ptr = NULL;
  typedef set<int> Set;
  Set s1;
  while (1) {
    // TestLibCAllocate() calls libc functions that don't work so well
    // after main() has exited.  So we just don't do the test then.
    if (!g_have_exited_main)
      TestLibCAllocate();

    if (ptr == NULL) {
      ptr = new(initialized) int*[1];
      *ptr = new(initialized) int[1];
    }
    set<int>* s2 = new(initialized) set<int>[1];
    s1.insert(random());
    s2->insert(*s1.begin());
    user += *s2->begin();
    **ptr += user;
    if (random() % 51 == 0) {
      s1.clear();
      if (random() % 2 == 0) {
        s1.~Set();
        new(&s1) Set;
      }
    }
    VLOG(3) << pthread_self() << " (" << getpid() << "): in wait: "
            << ptr << ", " << *ptr << "; " << s1.size();
    VLOG(2) << pthread_self() << " (" << getpid() << "): in wait, ptr = "
            << reinterpret_cast<void*>(
                 reinterpret_cast<uintptr_t>(ptr) ^ kHideMask)
            << "^" << reinterpret_cast<void*>(kHideMask);
    if (FLAGS_test_register_leak) {
      // Hide the register "ptr" value with an xor mask.
      // If one provides --test_register_leak flag, the test should
      // (with very high probability) crash on some leak check
      // with a leak report (of some x * sizeof(int) + y * sizeof(int*) bytes)
      // pointing at the two lines above in this function
      // with "new(initialized) int" in them as the allocators
      // of the leaked objects.
      // CAVEAT: We can't really prevent a compiler to save some
      // temporary values of "ptr" on the stack and thus let us find
      // the heap objects not via the register.
      // Hence it's normal if for certain compilers or optimization modes
      // --test_register_leak does not cause a leak crash of the above form
      // (this happens e.g. for gcc 4.0.1 in opt mode).
      ptr = reinterpret_cast<int **>(
          reinterpret_cast<uintptr_t>(ptr) ^ kHideMask);
      // busy loop to get the thread interrupted at:
      for (int i = 1; i < 1000000; ++i)  user += (1 + user * user * 5) / i;
      ptr = reinterpret_cast<int **>(
          reinterpret_cast<uintptr_t>(ptr) ^ kHideMask);
    } else {
      poll(NULL, 0, random() % 100);
    }
    VLOG(2) << pthread_self() << ": continuing";
    if (random() % 3 == 0) {
      delete [] *ptr;
      delete [] ptr;
      ptr = NULL;
    }
    delete [] s2;
  }
  return a;
}

static void RunHeapBusyThreads() {
  if (FLAGS_no_threads)  return;

  const int n = 17;  // make many threads

  pthread_t tid;
  pthread_attr_t attr;
  CHECK(pthread_attr_init(&attr) == 0);
  // make them and let them run
  for (int i = 0; i < n; ++i) {
    VLOG(0) << "Creating extra thread " << i + 1;
    CHECK(pthread_create(&tid, &attr, HeapBusyThreadBody, NULL) == 0);
  }

  Pause();
  Pause();
}

// tests disabling via function name
REGISTER_MODULE_INITIALIZER(heap_checker_unittest, {
  // TODO(csilvers): figure out when this might be true
  can_create_leaks_reliably = false;
  HeapLeakChecker::DisableChecksIn("NamedDisabledLeaks");
});

// NOTE: For NamedDisabledLeaks, NamedTwoDisabledLeaks
// and NamedThreeDisabledLeaks for the name-based disabling to work in opt mode
// we need to undo the tail-recursion optimization effect
// of -foptimize-sibling-calls that is enabled by -O2 in gcc 3.* and 4.*
// so that for the leaking calls we can find the stack frame
// that resolves to a Named*DisabledLeaks function.
// We do this by adding a fake last statement to these functions
// so that tail-recursion optimization is done with it.

// have leaks that we disable via our function name in MODULE_INITIALIZER
static void NamedDisabledLeaks() {
  AllocHidden(5 * sizeof(float));
  TransLeaks();
  sleep(0);  // undo -foptimize-sibling-calls
}

// to trick complier into preventing inlining
static void (*named_disabled_leaks)() = &NamedDisabledLeaks;

// have leaks that we disable via our function name ourselves
static void NamedTwoDisabledLeaks() {
  static bool first = true;
  if (first) {
    HeapLeakChecker::DisableChecksIn("NamedTwoDisabledLeaks");
    first = false;
  }
  AllocHidden(5 * sizeof(double));
  TransLeaks();
  sleep(0);  // undo -foptimize-sibling-calls
}

// to trick complier into preventing inlining
static void (*named_two_disabled_leaks)() = &NamedTwoDisabledLeaks;

// have leaks that we disable via our function name in our caller
static void NamedThreeDisabledLeaks() {
  AllocHidden(5 * sizeof(float));
  TransLeaks();
  sleep(0);  // undo -foptimize-sibling-calls
}

// to trick complier into preventing inlining
static void (*named_three_disabled_leaks)() = &NamedThreeDisabledLeaks;

static bool range_disable_named = false;

// have leaks that we disable via function names
static void* RunNamedDisabledLeaks(void* a) {
  // We get the address unconditionally here to fool gcc 4.1.0 in opt mode:
  // else it reorders the binary code so that our return address bracketing
  // does not work here.
  void* start_address = HeapLeakChecker::GetDisableChecksStart();

  named_disabled_leaks();
  named_two_disabled_leaks();
  named_three_disabled_leaks();

  // TODO(maxim): do not need this if we make pprof work in automated test runs
  if (range_disable_named) {
    HeapLeakChecker::DisableChecksToHereFrom(start_address);
  }
  sleep(0);  // undo -foptimize-sibling-calls
  return a;
}

// have leaks inside of threads that we disable via function names
static void ThreadNamedDisabledLeaks() {
  if (FLAGS_no_threads)  return;
  pthread_t tid;
  pthread_attr_t attr;
  CHECK(pthread_attr_init(&attr) == 0);
  CHECK(pthread_create(&tid, &attr, RunNamedDisabledLeaks, NULL) == 0);
  void* res;
  CHECK(pthread_join(tid, &res) == 0);
}

// test leak disabling via function names
static void TestHeapLeakCheckerNamedDisabling() {
  HeapLeakChecker::DisableChecksIn("NamedThreeDisabledLeaks");

  HeapLeakChecker check("named_disabling");

  RunNamedDisabledLeaks(NULL);
  RunNamedDisabledLeaks(NULL);
  ThreadNamedDisabledLeaks();
  RunNamedDisabledLeaks(NULL);
  ThreadNamedDisabledLeaks();
  ThreadNamedDisabledLeaks();

  Pause();

  if (!FLAGS_maybe_stripped) {
    CHECK_EQ(check.SameHeap(), true);
      // pprof checking should allow it
  } else {
    WARN_IF(check.SameHeap() != true,
            "named_disabling leaks are caught; "
            "we must be using a stripped binary");
  }
}

// The code from here to main()
// is to test that objects that are reachable from global
// variables are not reported as leaks,
// with the few exceptions like multiple-inherited objects.

// A dummy class to mimic allocation behavior of string-s.
template<class T>
struct Array {
  Array() {
    size = 3 + random() % 30;
    ptr = new(initialized) T[size];
  }
  ~Array() { delete [] ptr; }
  Array(const Array& x) {
    size = x.size;
    ptr = new(initialized) T[size];
    for (size_t i = 0; i < size; ++i) {
      ptr[i] = x.ptr[i];
    }
  }
  void operator=(const Array& x) {
    delete [] ptr;
    size = x.size;
    ptr = new(initialized) T[size];
    for (size_t i = 0; i < size; ++i) {
      ptr[i] = x.ptr[i];
    }
  }
  void append(const Array& x) {
    T* p = new(initialized) T[size + x.size];
    for (size_t i = 0; i < size; ++i) {
      p[i] = ptr[i];
    }
    for (size_t i = 0; i < x.size; ++i) {
      p[size+i] = x.ptr[i];
    }
    size += x.size;
    delete [] ptr;
    ptr = p;
  }
 private:
  size_t size;
  T* ptr;
};

static Array<char>* live_leak = NULL;
static Array<char>* live_leak2 = new(initialized) Array<char>();
static int* live_leak3 = new(initialized) int[10];
static const char* live_leak4 = new(initialized) char[5];
static int data[] = { 1, 2, 3, 4, 5, 6, 7, 21, 22, 23, 24, 25, 26, 27 };
static set<int> live_leak5(data, data+7);
static const set<int> live_leak6(data, data+14);
static const Array<char>* live_leak_arr1 = new(initialized) Array<char>[5];

class ClassA {
 public:
  ClassA(int a) : ptr(NULL) { }
  mutable char* ptr;
};

static const ClassA live_leak7(1);

template<class C>
class TClass {
 public:
  TClass(int a) : ptr(NULL) { }
  mutable C val;
  mutable C* ptr;
};

static const TClass<Array<char> > live_leak8(1);

class ClassB {
 public:
  ClassB() { }
  int b[10];
  virtual void f() { }
  virtual ~ClassB() { }
};

class ClassB2 {
 public:
  ClassB2() { }
  int b2[10];
  virtual void f2() { }
  virtual ~ClassB2() { }
};

class ClassD1 : public ClassB {
  int d1[10];
  virtual void f() { }
};

class ClassD2 : public ClassB2 {
  int d2[10];
  virtual void f2() { }
};

class ClassD : public ClassD1, public ClassD2 {
  int d[10];
  virtual void f() { }
  virtual void f2() { }
};

static ClassB* live_leak_b;
static ClassD1* live_leak_d1;
static ClassD2* live_leak_d2;
static ClassD* live_leak_d;

static ClassB* live_leak_b_d1;
static ClassB2* live_leak_b2_d2;
static ClassB* live_leak_b_d;
static ClassB2* live_leak_b2_d;

static ClassD1* live_leak_d1_d;
static ClassD2* live_leak_d2_d;

// A dummy string class mimics certain third party string
// implementations, which store a refcount in the first
// few bytes and keeps a pointer pointing behind the refcount.
template <typename R>
class RefcountStr {
 public:
  RefcountStr() {
    ptr = new char[sizeof(R) * 10];
    ptr = ptr + sizeof(R);
  }
  char* ptr;
};
// E.g., mimics UnicodeString defined in third_party/icu.
static const RefcountStr<uint32> live_leak_refcountstr_4;

// have leaks but ignore the leaked objects
static void IgnoredLeaks() {
  int* p = new(initialized) int[1];
  HeapLeakChecker::IgnoreObject(p);
  int** leak = new(initialized) int*;
  HeapLeakChecker::IgnoreObject(leak);
  *leak = new(initialized) int;
  HeapLeakChecker::UnIgnoreObject(p);
  delete [] p;
}

// allocate many objects reachable from global data
static void TestHeapLeakCheckerLiveness() {
  live_leak_b = new(initialized) ClassB;
  live_leak_d1 = new(initialized) ClassD1;
  live_leak_d2 = new(initialized) ClassD2;
  live_leak_d = new(initialized) ClassD;

  live_leak_b_d1 = new(initialized) ClassD1;
  live_leak_b2_d2 = new(initialized) ClassD2;
  live_leak_b_d = new(initialized) ClassD;
  live_leak_b2_d = new(initialized) ClassD;

  live_leak_d1_d = new(initialized) ClassD;
  live_leak_d2_d = new(initialized) ClassD;

  HeapLeakChecker::IgnoreObject((ClassD*)live_leak_b2_d);
  HeapLeakChecker::IgnoreObject((ClassD*)live_leak_d2_d);
    // These two do not get deleted with liveness flood
    // because the base class pointer points inside of the objects
    // in such cases of multiple inheritance.
    // Luckily google code does not use multiple inheritance almost at all.

  live_leak = new(initialized) Array<char>();
  delete [] live_leak3;
  live_leak3 = new(initialized) int[33];
  live_leak2->append(*live_leak);
  live_leak7.ptr = new(initialized) char[77];
  live_leak8.ptr = new(initialized) Array<char>();
  live_leak8.val = Array<char>();

  IgnoredLeaks();
  IgnoredLeaks();
  IgnoredLeaks();
}

DECLARE_string(heap_check);

// Get address (PC value) following the mmap call into addr_after_mmap_call
static void* Mmapper(uintptr_t* addr_after_mmap_call) {
  void* r = mmap(NULL, 100, PROT_READ|PROT_WRITE,
                 MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  // Get current PC value into addr_after_mmap_call
  void* stack[1];
  CHECK_EQ(GetStackTrace(stack, 1, 0), 1);
  *addr_after_mmap_call = reinterpret_cast<uintptr_t>(stack[0]);
  sleep(0);  // undo -foptimize-sibling-calls
  return r;
}

// to trick complier into preventing inlining
static void* (*mmapper_addr)(uintptr_t*) = &Mmapper;

// TODO(maxim): copy/move this to memory_region_map_unittest
// TODO(maxim): expand this test to include mmap64, mremap and sbrk calls.
static void VerifyMemoryRegionMapStackGet() {
  uintptr_t caller_addr_limit;
  void* addr = (*mmapper_addr)(&caller_addr_limit);
  uintptr_t caller = 0;
  MemoryRegionMap::Lock();
  for (MemoryRegionMap::RegionIterator
         i = MemoryRegionMap::BeginRegionLocked();
         i != MemoryRegionMap::EndRegionLocked(); ++i) {
    if (i->start_addr == reinterpret_cast<uintptr_t>(addr)) {
      CHECK(caller == 0);
      caller = i->caller;
    }
  }
  MemoryRegionMap::Unlock();
  // caller must point into Mmapper function:
  if (!(reinterpret_cast<uintptr_t>(mmapper_addr) <= caller  &&
        caller < caller_addr_limit)) {
    LOGF << std::hex << "0x" << caller
         << " does not seem to point into code of function Mmapper at "
         << "0x" << reinterpret_cast<uintptr_t>(mmapper_addr)
         << "! Stack frame collection must be off in MemoryRegionMap!";
    LOG(FATAL, "\n");
  }
  munmap(addr, 100);
}

static void* Mallocer(uintptr_t* addr_after_malloc_call) {
  void* r = malloc(100);
  sleep(0);  // undo -foptimize-sibling-calls
  // Get current PC value into addr_after_malloc_call
  void* stack[1];
  CHECK_EQ(GetStackTrace(stack, 1, 0), 1);
  *addr_after_malloc_call = reinterpret_cast<uintptr_t>(stack[0]);
  return r;
}

// to trick complier into preventing inlining
static void* (*mallocer_addr)(uintptr_t*) = &Mallocer;

// non-static for friendship with HeapProfiler
// TODO(maxim): expand this test to include
// realloc, calloc, memalign, valloc, pvalloc, new, and new[].
extern void VerifyHeapProfileTableStackGet() {
  uintptr_t caller_addr_limit;
  void* addr = (*mallocer_addr)(&caller_addr_limit);
  uintptr_t caller =
    reinterpret_cast<uintptr_t>(HeapLeakChecker::GetAllocCaller(addr));
  // caller must point into Mallocer function:
  if (!(reinterpret_cast<uintptr_t>(mallocer_addr) <= caller  &&
        caller < caller_addr_limit)) {
    LOGF << std::hex << "0x" << caller
         << " does not seem to point into code of function Mallocer at "
         << "0x" << reinterpret_cast<uintptr_t>(mallocer_addr)
         << "! Stack frame collection must be off in heap profiler!";
    LOG(FATAL, "\n");
  }
  free(addr);
}

// Helper to do 'return 0;' inside main(): insted we do 'return Pass();'
static int Pass() {
  fprintf(stdout, "PASS\n");
  g_have_exited_main = true;
  return 0;
}

int main(int argc, char** argv) {
  run_hidden_ptr = DoRunHidden;
  wipe_stack_ptr = DoWipeStack;
  
  if (!HeapLeakChecker::IsActive()) {
    CHECK_EQ(FLAGS_heap_check, "");
    LOG(WARNING, "HeapLeakChecker got turned off; we won't test much...");
  } else {
    VerifyMemoryRegionMapStackGet();
    VerifyHeapProfileTableStackGet();
  }

  KeyInit();

  // glibc 2.4, on x86_64 at least, has a lock-ordering bug, which
  // means deadlock is possible when one thread calls dl_open at the
  // same time another thread is calling dl_iterate_phdr.  libunwind
  // calls dl_iterate_phdr, and TestLibCAllocate calls dl_open (or the
  // various syscalls in it do), at least the first time it's run.
  // To avoid the deadlock, we run TestLibCAllocate once before getting
  // multi-threaded.
  // TODO(csilvers): once libc is fixed, or libunwind can work around it,
  //                 get rid of this early call.  We *want* our test to
  //                 find potential problems like this one!
  TestLibCAllocate();

  if (FLAGS_interfering_threads) {
    RunHeapBusyThreads();  // add interference early
  }
  TestLibCAllocate();

  LOGF << "In main(): heap_check=" << FLAGS_heap_check << endl;

  CHECK(HeapLeakChecker::NoGlobalLeaks());  // so far, so good

  if (FLAGS_test_leak) {
    void* arr = AllocHidden(10 * sizeof(int));
    Use(&arr);
    LogHidden("Leaking", arr);
    if (FLAGS_test_cancel_global_check)  HeapLeakChecker::CancelGlobalCheck();
    return Pass();
      // whole-program leak-check should (with very high probability)
      // catch the leak of arr (10 * sizeof(int) bytes)
      // (when !FLAGS_test_cancel_global_check)
  }

  if (FLAGS_test_loop_leak) {
    void* arr1;
    void* arr2;
    RunHidden(NewCallback(MakeDeathLoop, &arr1, &arr2));
    Use(&arr1);
    Use(&arr2);
    LogHidden("Loop leaking", arr1);
    LogHidden("Loop leaking", arr2);
    if (FLAGS_test_cancel_global_check)  HeapLeakChecker::CancelGlobalCheck();
    return Pass();
      // whole-program leak-check should (with very high probability)
      // catch the leak of arr1 and arr2 (4 * sizeof(void*) bytes)
      // (when !FLAGS_test_cancel_global_check)
  }

  TestHeapLeakCheckerLiveness();

  HeapLeakChecker heap_check("all");

  TestHeapLeakChecker();
  Pause();
  TestHeapLeakCheckerTrick();
  Pause();

  TestHeapLeakCheckerDeathSimple();
  Pause();
  TestHeapLeakCheckerDeathLoop();
  Pause();
  TestHeapLeakCheckerDeathInverse();
  Pause();
  TestHeapLeakCheckerDeathNoLeaks();
  Pause();
  TestHeapLeakCheckerDeathCountLess();
  Pause();
  TestHeapLeakCheckerDeathCountMore();
  Pause();

  TestHeapLeakCheckerDeathTrick();
  Pause();

  CHECK(HeapLeakChecker::NoGlobalLeaks());  // so far, so good

  TestHeapLeakCheckerPProf();
  Pause();

  TestHeapLeakCheckerDisabling();
  Pause();

  TestSTLAlloc();
  Pause();
  TestSTLAllocInverse();
  Pause();

  // Test that various STL allocators work.  Some of these are redundant, but
  // we don't know how STL might change in the future.  For example,
  // http://wiki/Main/StringNeStdString.
#define DTSL(a) { DirectTestSTLAlloc(a, #a); Pause(); }
  DTSL(std::allocator<char>());
  DTSL(std::allocator<int>());
  DTSL(std::string().get_allocator());
  DTSL(string().get_allocator());
  DTSL(vector<int>().get_allocator());
  DTSL(vector<double>().get_allocator());
  DTSL(vector<vector<int> >().get_allocator());
  DTSL(vector<string>().get_allocator());
  DTSL((map<string, string>().get_allocator()));
  DTSL((map<string, int>().get_allocator()));
  DTSL(set<char>().get_allocator());
#undef DTSL

  TestLibCAllocate();
  Pause();

  CHECK(HeapLeakChecker::NoGlobalLeaks());  // so far, so good

  void* start_address = HeapLeakChecker::GetDisableChecksStart();

  TestHeapLeakCheckerNamedDisabling();
  Pause();

  if (!FLAGS_maybe_stripped) {
    CHECK(heap_check.SameHeap());
  } else {
    WARN_IF(heap_check.SameHeap() != true,
            "overall leaks are caught; we must be using a stripped binary");
  }

  // TODO(maxim): do not need these if we make pprof work in automated test runs
  HeapLeakChecker::DisableChecksToHereFrom(start_address);
  // This will also disable (w/o relying on pprof anymore)
  // all leaks that earlier occured inside of ThreadNamedDisabledLeaks:
  range_disable_named = true;
  ThreadNamedDisabledLeaks();

  HeapLeakChecker::IgnoreObject(new(initialized) set<int>(data, data + 13));
    // This checks both that IgnoreObject works, and
    // and the fact that we don't drop such leaks as live for some reason.

  CHECK(HeapLeakChecker::NoGlobalLeaks());  // so far, so good

  return Pass();
}
