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

// CAVEAT: Do not use vector<>s and string-s in this test,
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

#include "config.h"
#include "base/logging.h"
#include "base/googleinit.h"
#include <google/malloc_extension.h>
#include <google/heap-profiler.h>
#include <google/heap-checker.h>

#include <stdlib.h>
#include <sys/poll.h>
#if defined HAVE_STDINT_H
#include <stdint.h>             // to get uint16_t (ISO naming madness)
#elif defined HAVE_INTTYPES_H
#include <inttypes.h>           // another place uint16_t might be defined
#else
#include <sys/types.h>          // our last best hope
#endif
#include <unistd.h>             // for sleep()
#include <iostream>             // for cout
#include <vector>
#include <set>
#include <string>

#include <errno.h>              // errno
#include <netinet/in.h>         // inet_ntoa
#include <arpa/inet.h>          // inet_ntoa
#ifdef HAVE_EXECINFO_H
#include <execinfo.h>           // backtrace
#endif
#ifdef HAVE_GRP_H
#include <grp.h>                // getgrent, getgrnam
#endif

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


using namespace std;

static bool FLAGS_maybe_stripped = false;   // TODO(csilvers): use this?
static bool FLAGS_interfering_threads = true;
static bool FLAGS_test_register_leak = false;  // TODO(csilvers): this as well?

// Set to true at end of main, so threads know.  Not entirely thread-safe!,
// but probably good enough.
static bool g_have_exited_main = false;

// If our allocator guarantees that heap object addresses are never reused.
// We need this property so that stale uncleared pointer data
// does not accidentaly lead to heap-checker wrongly believing that
// some data is live.
static bool unique_heap_addresses = false;

// We use a simple allocation wrapper
// to make sure we wipe out the newly allocated objects
// in case they still happened to contain some pointer data
// accidently left by the memory allocator.
struct Initialized { };
static Initialized initialized;
void* operator new(size_t size, const Initialized&) {
  // Below we use "p = new (initialized) Foo[1];" and  "delete[] p;"
  // instead of "p = new (initialized) Foo;"
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

static void CheckLeak(HeapLeakChecker* check, 
                      size_t bytes_leaked, size_t objects_leaked) {
  if (unique_heap_addresses) {
    if (getenv("HEAPCHECK")) {
      // these might still fail occasionally, but it should be very rare
      CHECK_EQ(check->BriefNoLeaks(), false);
      CHECK_EQ(check->BytesLeaked(), bytes_leaked);
      CHECK_EQ(check->ObjectsLeaked(), objects_leaked);
    }
  } else if (check->BriefNoLeaks() != false) {
    cout << "Some liveness flood must be too optimistic\n";
  }
}

static void Pause() {
  poll(NULL, 0, 77);  // time for thread activity in HeapBusyThreadBody

  // Indirectly test debugallocation.* and malloc_interface.*:

  CHECK(MallocExtension::instance()->VerifyAllMemory());
  // Comment the printing of malloc-stats out for now.  It seems a bit broken
#if 0
  int blocks;
  size_t total;
  int histogram[kMallocHistogramSize];
  if (MallocExtension::instance()
       ->MallocMemoryStats(&blocks, &total, histogram)  &&  total != 0) {
    cout << "Malloc stats: " << blocks << " blocks of "
         << total << " bytes\n";
    for (int i = 0; i < kMallocHistogramSize; ++i) {
      if (histogram[i]) {
        cout << "  Malloc histogram at " << i << " : " << histogram[i] << "\n";
      }
    }
  }
#endif
}

static bool noleak() {   // compare to this if you expect no leak
  return true;
}

static bool leak() {   // compare to this if you expect a leak
  // When we're not doing heap-checking, we can't tell if there's a leak
  if ( !getenv("HEAPCHECK") ) {
    return true;
  } else {
    return false;
  }
}

// Make gcc think a pointer is "used"
template <class T>
static void Use(T** foo) {
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
  reinterpret_cast<uintptr_t&>(*ptr) =
    (reinterpret_cast<uintptr_t&>(*ptr) ^ kHideMask);
}

template <class T>
static void UnHide(T** ptr) {
  reinterpret_cast<uintptr_t&>(*ptr) =
    (reinterpret_cast<uintptr_t&>(*ptr) ^ kHideMask);
}

// non-static to fool the compiler against inlining
extern void (*run_hidden_ptr)(Closure* c, int n);
void (*run_hidden_ptr)(Closure* c, int n);
extern void (*wipe_stack_ptr)(int n);
void (*wipe_stack_ptr)(int n);

static void DoRunHidden(Closure* c, int n) {
  if (n) {
    run_hidden_ptr(c, n-1);
    wipe_stack_ptr(n);
    sleep(0);  // undo -foptimize-sibling-calls
  } else {
    c->Run();
  }
}

static void DoWipeStack(int n) {
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
  void* p = new (initialized) char[size];
  Hide(&p);
  Use(&p);  // use only hidden versions
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
  UnHide(&p);
  delete [] (char*)p;
}

static void DeAllocHidden(void** ptr) {
  RunHidden(NewCallback(DoDeAllocHidden, ptr));
  *ptr = NULL;
  Use(ptr);
}

// not deallocates
static void TestHeapLeakCheckerDeathSimple() {
  HeapLeakChecker check("death_simple");
  void* foo = AllocHidden(100 * sizeof(int));
  Use(&foo);
  void* bar = AllocHidden(300);
  Use(&bar);
  CheckLeak(&check, 300 + 100 * sizeof(int), 2);
  DeAllocHidden(&foo);
  DeAllocHidden(&bar);
}

static void MakeDeathLoop(void** arr1, void** arr2) {
  void** a1 = new (initialized) void*[2];
  void** a2 = new (initialized) void*[2];
  a1[1] = (void*)a2;
  a2[1] = (void*)a1;
  Hide(&a1);
  Hide(&a2);
  Use(&a1);
  Use(&a2);
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
  CheckLeak(&check, 4 * sizeof(void*), 2);
  DeAllocHidden(&arr1);
  DeAllocHidden(&arr2);
}

// deallocates more than allocates
static void TestHeapLeakCheckerDeathInverse() {
  void* bar = AllocHidden(250 * sizeof(int));
  Use(&bar);
  HeapLeakChecker check("death_inverse");
  void* foo = AllocHidden(100 * sizeof(int));
  Use(&foo);
  DeAllocHidden(&bar);
  CheckLeak(&check, (size_t)(-150 * size_t(sizeof(int))), 0);
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
  CHECK_EQ(check.BriefNoLeaks(), noleak());
  DeAllocHidden(&foo);
}

// have less objecs
static void TestHeapLeakCheckerDeathCountLess() {
  void* bar1 = AllocHidden(50 * sizeof(int));
  Use(&bar1);
  void* bar2 = AllocHidden(50 * sizeof(int));
  Use(&bar2);
  HeapLeakChecker check("death_count_less");
  void* foo = AllocHidden(100 * sizeof(int));
  Use(&foo);
  DeAllocHidden(&bar1);
  DeAllocHidden(&bar2);
  CheckLeak(&check, 0, (size_t)-1);
  DeAllocHidden(&foo);
}

// have more objecs
static void TestHeapLeakCheckerDeathCountMore() {
  void* foo = AllocHidden(100 * sizeof(int));
  Use(&foo);
  HeapLeakChecker check("death_count_more");
  void* bar1 = AllocHidden(50 * sizeof(int));
  Use(&bar1);
  void* bar2 = AllocHidden(50 * sizeof(int));
  Use(&bar2);
  DeAllocHidden(&foo);
  CheckLeak(&check, 0, 1);
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
  HeapLeakChecker check("trick");
  void* foo1 = AllocHidden(280 * sizeof(int));
  Use(&foo1);
  void* foo2 = AllocHidden(120 * sizeof(int));
  Use(&foo2);
  DeAllocHidden(&bar1);
  DeAllocHidden(&bar2);
  Pause();
  CHECK(check.BriefSameHeap());
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
    CHECK_EQ(check.SameHeap(), leak());  // pprof checking should catch it
  } else if (check.SameHeap()) {
    cout << "death_trick leak is not caught; we must be using stripped binary\n";
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

  // if this fails, some code with DisableChecksUp() got inlined into here;
  // need to add more tricks to prevent this inlining.
  CHECK(!HeapLeakChecker::HaveDisabledChecksUp(1));

  Pause();

  CHECK(check.SameHeap());
}

typedef set<int> IntSet;

static int some_ints[] = { 1, 2, 3, 21, 22, 23, 24, 25 };

static void DoTestSTLAlloc() {
  IntSet* x = new (initialized) IntSet[1];
  *x  = IntSet(some_ints, some_ints + 6);
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
  IntSet* x = new (initialized) IntSet[1];
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
  HeapLeakChecker check("inverse_stl");
  IntSet* x;
  RunHidden(NewCallback(DoTestSTLAllocInverse, &x));
  if (unique_heap_addresses) {
    if (getenv("HEAPCHECK")) {
      // these might still fail occasionally, but it should be very rare
      CHECK_EQ(check.BriefNoLeaks(), false);
      CHECK_GE(check.BytesLeaked(), 100 * sizeof(int));
      CHECK_GE(check.ObjectsLeaked(), 100);
      // assumes set<>s are represented by some kind of binary tree
      // or something else allocating >=1 heap object per set object
    }
  } else if (check.BriefNoLeaks() != false) {
    cout << "Some liveness flood must be too optimistic";
  }
  RunHidden(NewCallback(FreeTestSTLAllocInverse, &x));
}

template<class Alloc>
static void DirectTestSTLAlloc(Alloc allocator, const char* name) {
  HeapLeakChecker check((string("direct_stl-") + name).c_str());
  const int size = 1000;
  char* ptrs[size];
  for (int i = 0; i < size; ++i) {
    char* p = allocator.allocate(i*3+1);
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
static pthread_once_t key_once = PTHREAD_ONCE_INIT;
static const int kKeys = 50;
static pthread_key_t key[kKeys];

static void KeyFree(void* ptr) {
  delete [] (char*)ptr;
}

static void KeyInit() {
  for (int i = 0; i < kKeys; ++i) {
    CHECK_EQ(pthread_key_create(&key[i], KeyFree), 0);
  }
}

// force various C library static and thread-specific allocations
static void TestLibCAllocate() {
  pthread_once(&key_once, KeyInit);
  for (int i = 0; i < kKeys; ++i) {
    void* p = pthread_getspecific(key[i]);
    if (NULL == p) {
      p = new (initialized) char[77 + i];
      pthread_setspecific(key[i], p);
    }
  }

  strerror(errno);
  struct in_addr addr;
  addr.s_addr = INADDR_ANY;
  inet_ntoa(addr);
  const time_t now = time(NULL);
  ctime(&now);
#ifdef HAVE_EXECINFO_H
  void *stack[1];
  backtrace(stack, 1);
#endif
#ifdef HAVE_GRP_H
  if (grp == NULL)  grp = getgrent();  // a race condition here is okay
  getgrnam(grp->gr_name);
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
#if defined(__i386__) && \
    (__GNUC__ == 2 || __GNUC__ == 3 || __GNUC__ == 4 || \
     defined(__INTEL_COMPILER))
  register int** ptr asm("esi");
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
      ptr = new (initialized) int*[1];
      *ptr = new (initialized) int[1];
    }
    set<int>* s2 = new (initialized) set<int>[1];
    s1.insert(random());
    s2->insert(*s1.begin());
    user += *s2->begin();
    **ptr += user;
    if (random() % 51 == 0) {
      s1.clear();
      if (random() % 2 == 0) {
        s1.~Set();
        new (&s1) Set;
      }
    }
    if (FLAGS_test_register_leak) {
      // Hide the register "ptr" value with an xor mask.
      // If one provides --test_register_leak flag, the test should
      // (with very high probability) crash on some leak check
      // with a leak report (of some x * sizeof(int) + y * sizeof(int*) bytes)
      // pointing at the two lines above in this function
      // with "new (initialized) int" in them as the allocators
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
  const int n = 17;  // make many threads

  pthread_t tid;
  pthread_attr_t attr;
  CHECK(pthread_attr_init(&attr) == 0);
  // make them and let them run
  for (int i = 0; i < n; ++i) {
    CHECK(pthread_create(&tid, &attr, HeapBusyThreadBody, NULL) == 0);
  }

  Pause();
  Pause();
}

// tests disabling via function name
REGISTER_MODULE_INITIALIZER(heap_checker_unittest, {
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
  sleep(0);  // undo -foptimize-sibling-calls
}

// to trick complier into preventing inlining
static void (*named_two_disabled_leaks)() = &NamedTwoDisabledLeaks;

// have leaks that we disable via our function name in our caller
static void NamedThreeDisabledLeaks() {
  AllocHidden(5 * sizeof(float));
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
  } else if (!check.SameHeap()) {
    cout << "named_disabling leaks are caught; we must be using stripped binary\n";
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
    ptr = new (initialized) T[size];
  }
  ~Array() { delete [] ptr; }
  Array(const Array& x) {
    size = x.size;
    ptr = new (initialized) T[size];
    for (size_t i = 0; i < size; ++i) {
      ptr[i] = x.ptr[i];
    }
  }
  void operator=(const Array& x) {
    delete [] ptr;
    size = x.size;
    ptr = new (initialized) T[size];
    for (size_t i = 0; i < size; ++i) {
      ptr[i] = x.ptr[i];
    }
  }
  void append(const Array& x) {
    T* p = new (initialized) T[size + x.size];
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
static Array<char>* live_leak2 = new (initialized) Array<char>();
static int* live_leak3 = new (initialized) int[10];
static const char* live_leak4 = new (initialized) char[5];
static int data[] = { 1, 2, 3, 4, 5, 6, 7, 21, 22, 23, 24, 25, 26, 27 };
static set<int> live_leak5(data, data+7);
static const set<int> live_leak6(data, data+14);
static const Array<char>* live_leak_arr1 = new (initialized) Array<char>[5];

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

// have leaks but ignore the leaked objects
static void IgnoredLeaks() {
  int* p = new (initialized) int[1];
  HeapLeakChecker::IgnoreObject(p);
  int** leak = new (initialized) int*;
  HeapLeakChecker::IgnoreObject(leak);
  *leak = new (initialized) int;
  HeapLeakChecker::UnIgnoreObject(p);
  delete [] p;
}

// allocate many objects reachable from global data
static void TestHeapLeakCheckerLiveness() {
  live_leak_b = new (initialized) ClassB;
  live_leak_d1 = new (initialized) ClassD1;
  live_leak_d2 = new (initialized) ClassD2;
  live_leak_d = new (initialized) ClassD;

  live_leak_b_d1 = new (initialized) ClassD1;
  live_leak_b2_d2 = new (initialized) ClassD2;
  live_leak_b_d = new (initialized) ClassD;
  live_leak_b2_d = new (initialized) ClassD;

  live_leak_d1_d = new (initialized) ClassD;
  live_leak_d2_d = new (initialized) ClassD;

  HeapLeakChecker::IgnoreObject((ClassD*)live_leak_b2_d);
  HeapLeakChecker::IgnoreObject((ClassD*)live_leak_d2_d);
    // These two do not get deleted with liveness flood
    // because the base class pointer points inside of the objects
    // in such cases of multiple inheritance.
    // Luckily google code does not use multiple inheritance almost at all.

  live_leak = new (initialized) Array<char>();
  delete [] live_leak3;
  live_leak3 = new (initialized) int[33];
  live_leak2->append(*live_leak);
  live_leak7.ptr = new (initialized) char[77];
  live_leak8.ptr = new (initialized) Array<char>();
  live_leak8.val = Array<char>();

  IgnoredLeaks();
  IgnoredLeaks();
  IgnoredLeaks();
}

int main(int argc, char** argv) {
  // This needs to be set before InternalInitStart(), which makes a local copy
  if (getenv("PPROF_PATH"))
    HeapLeakChecker::set_pprof_path(getenv("PPROF_PATH"));

  run_hidden_ptr = DoRunHidden;
  wipe_stack_ptr = DoWipeStack;

  if (FLAGS_interfering_threads) {
    RunHeapBusyThreads();  // add interference early
  }
  TestLibCAllocate();

  LogPrintf(INFO, "In main()");

  // The following two modes test whether the whole-program leak checker
  // appropriately detects leaks on exit.
  if (getenv("HEAPCHECK_TEST_LEAK")) {
    void* arr = AllocHidden(10 * sizeof(int));
    Use(&arr);
    LogPrintf(INFO, "Leaking %p", arr);
    return 0;  // whole-program leak-check should (with very high probability)
               // catch the leak of arr (10 * sizeof(int) bytes)
  }

  if (getenv("HEAPCHECK_TEST_LOOP_LEAK")) {
    void* arr1;
    void* arr2;
    RunHidden(NewCallback(MakeDeathLoop, &arr1, &arr2));
    Use(&arr1);
    Use(&arr2);
    LogPrintf(INFO, "Loop leaking %p and %p", arr1, arr2);
    return 0;  // whole-program leak-check should (with very high probability)
               // catch the leak of arr1 and arr2 (4 * sizeof(void*) bytes)
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

  TestHeapLeakCheckerPProf();
  Pause();

  TestHeapLeakCheckerDisabling();
  Pause();

  TestSTLAlloc();
  Pause();
  TestSTLAllocInverse();
  Pause();
  DirectTestSTLAlloc(set<char>().get_allocator(), "alloc");
    // default STL allocator
  Pause();
  // TODO: re-enable this test once we've change configure.ac to include
  // the right header file that defines pthread_allocator.
  //DirectTestSTLAlloc(pthread_allocator<char>(), "pthread_alloc");
  Pause();

  TestLibCAllocate();
  Pause();

  void* start_address = HeapLeakChecker::GetDisableChecksStart();

  TestHeapLeakCheckerNamedDisabling();
  Pause();

  if (!FLAGS_maybe_stripped) {
    CHECK(heap_check.SameHeap());
  } else if (!heap_check.SameHeap()) {
    cout << "overall leaks are caught; we must be using stripped binary\n";
  }

  // This will also disable (w/o relying on pprof anymore)
  // all leaks that earlier occured inside of ThreadNamedDisabledLeaks:
  range_disable_named = true;
  ThreadNamedDisabledLeaks();

  HeapLeakChecker::IgnoreObject(new (initialized) set<int>(data, data + 13));
    // This checks both that IgnoreObject works, and
    // and the fact that we don't drop such leaks as live for some reason.

  fprintf(stdout, "PASS\n");

  g_have_exited_main = true;
  return 0;
}
