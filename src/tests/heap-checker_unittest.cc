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

#include "google/perftools/config.h"
#include "base/logging.h"
#include "base/googleinit.h"

#include <google/heap-profiler.h>
#include <google/heap-checker.h>

#include <stdlib.h>
#include <vector>
#include <string>

using namespace std;

// Use an int* variable so that the compiler does not complain.
static void Use(int* foo) { CHECK(foo == foo); }

// not deallocates
static void TestHeapLeakCheckerDeathSimple() {
  HeapLeakChecker check("death_simple");
  int* foo = new int[100];
  void* bar = malloc(300);
  Use(foo);
  CHECK_EQ(check.BriefSameHeap(), false);
  delete [] foo;
  free(bar);
}

// deallocates more than allocates
static void TestHeapLeakCheckerDeathInverse() {
  int* bar = new int[250];
  Use(bar);
  HeapLeakChecker check("death_inverse");
  int* foo = new int[100];
  Use(foo);
  delete [] bar;
  CHECK_EQ(check.BriefSameHeap(), false);
  delete [] foo;
}

// deallocates more than allocates
static void TestHeapLeakCheckerDeathNoLeaks() {
  int* foo = new int[100];
  int* bar = new int[250];
  Use(foo);
  Use(bar);
  HeapLeakChecker check("death_noleaks");
  delete [] bar;
  CHECK_EQ(check.NoLeaks(), true);
  delete [] foo;
}

// have less objecs
static void TestHeapLeakCheckerDeathCountLess() {
  int* bar1 = new int[50];
  int* bar2 = new int[50];
  Use(bar1);
  Use(bar2);
  HeapLeakChecker check("death_count_less");
  int* foo = new int[100];
  Use(foo);
  delete [] bar1;
  delete [] bar2;
  CHECK_EQ(check.BriefSameHeap(), false);
  delete [] foo;
}

// have more objecs
static void TestHeapLeakCheckerDeathCountMore() {
  int* foo = new int[100];
  Use(foo);
  HeapLeakChecker check("death_count_more");
  int* bar1 = new int[50];
  int* bar2 = new int[50];
  Use(bar1);
  Use(bar2);
  delete [] foo;
  CHECK_EQ(check.BriefSameHeap(), false);
  delete [] bar1;
  delete [] bar2;
}

static void TestHeapLeakChecker() {
  { HeapLeakChecker check("trivial");
    int foo = 5;
    Use(&foo);
    CHECK(check.BriefSameHeap());
  }
  { HeapLeakChecker check("simple");
    int* foo = new int[100];
    int* bar = new int[200];
    Use(foo);
    Use(bar);
    delete [] foo;
    delete [] bar;
    CHECK(check.BriefSameHeap());
  }
}

// no false positives from pprof
static void TestHeapLeakCheckerPProf() {
  { HeapLeakChecker check("trivial_p");
    int foo = 5;
    Use(&foo);
    CHECK(check.SameHeap());
  }
  { HeapLeakChecker check("simple_p");
    int* foo = new int[100];
    int* bar = new int[200];
    Use(foo);
    Use(bar);
    delete [] foo;
    delete [] bar;
    CHECK(check.SameHeap());
  }
}

static void TestHeapLeakCheckerTrick() {
  int* bar1 = new int[60];
  int* bar2 = new int[40];
  Use(bar1);
  Use(bar2);
  HeapLeakChecker check("trick");
  int* foo1 = new int[70];
  int* foo2 = new int[30];
  Use(foo1);
  Use(foo2);
  delete [] bar1;
  delete [] bar2;
  CHECK(check.BriefSameHeap());
  delete [] foo1;
  delete [] foo2;
}

// no false negatives from pprof
static void TestHeapLeakCheckerDeathTrick() {
  int* bar1 = new int[60];
  int* bar2 = new int[40];
  Use(bar1);
  Use(bar2);
  HeapLeakChecker check("death_trick");
  int* foo1 = new int[70];
  int* foo2 = new int[30];
  Use(foo1);
  Use(foo2);
  delete [] bar1;
  delete [] bar2;
  // If this check fails, you are probably running a stripped binary
  CHECK_EQ(check.SameHeap(), false);  // pprof checking should catch it
  delete [] foo1;
  delete [] foo2;
}

static void TransLeaks() {
  new char;
}

static void DisabledLeaks() {
  HeapLeakChecker::DisableChecksUp(1);
  TransLeaks();
  new int[3];
}

static void RangeDisabledLeaks() {
  void* start_address = HeapLeakChecker::GetDisableChecksStart();
  new int[3];
  TransLeaks();
  HeapLeakChecker::DisableChecksToHereFrom(start_address);
}

static void* RunDisabledLeaks(void* a) {
  DisabledLeaks();
  RangeDisabledLeaks();
  return a;
}

static void ThreadDisabledLeaks() {
  pthread_t tid;
  pthread_attr_t attr;
  CHECK(pthread_attr_init(&attr) == 0);
  CHECK(pthread_create(&tid, &attr, RunDisabledLeaks, NULL) == 0);
  void* res;
  CHECK(pthread_join(tid, &res) == 0);
}

static void TestHeapLeakCheckerDisabling() {
  HeapLeakChecker check("disabling");

  RunDisabledLeaks(NULL);
  RunDisabledLeaks(NULL);
  ThreadDisabledLeaks();
  RunDisabledLeaks(NULL);
  ThreadDisabledLeaks();
  ThreadDisabledLeaks();

  CHECK_EQ(check.SameHeap(), true);
}


REGISTER_MODULE_INITIALIZER(heap_checker_unittest, {
  HeapLeakChecker::DisableChecksIn("NamedDisabledLeaks");
});

static void NamedDisabledLeaks() {
  // We are testing two cases in this function: calling new[] directly and
  // calling it at one level deep (inside TransLeaks).  We want to always call
  // TransLeaks() first, because otherwise the compiler may turn this into a
  // tail recursion when compiling in optimized mode.  This messes up the stack
  // trace.
  // TODO: Is there any way to prevent this from happening in the general case
  // (i.e. user code)?
  TransLeaks();
  new float[5];
}

static void NamedTwoDisabledLeaks() {
  static bool first = true;
  if (first) {
    HeapLeakChecker::DisableChecksIn("NamedTwoDisabledLeaks");
    first = false;
  }
  TransLeaks();
  new double[5];
}

static void NamedThreeDisabledLeaks() {
  TransLeaks();
  new float[5];
}

static void* RunNamedDisabledLeaks(void* a) {
  void* start_address = NULL;
  if (a)  start_address = HeapLeakChecker::GetDisableChecksStart();

  NamedDisabledLeaks();
  NamedTwoDisabledLeaks();
  NamedThreeDisabledLeaks();

  // TODO(maxim): do not need this if we make pprof work in automated test runs
  if (a)  HeapLeakChecker::DisableChecksToHereFrom(start_address);

  return a;
}

static void ThreadNamedDisabledLeaks(void* a = NULL) {
  pthread_t tid;
  pthread_attr_t attr;
  CHECK(pthread_attr_init(&attr) == 0);
  CHECK(pthread_create(&tid, &attr, RunNamedDisabledLeaks, a) == 0);
  void* res;
  CHECK(pthread_join(tid, &res) == 0);
}

static void TestHeapLeakCheckerNamedDisabling() {
  HeapLeakChecker::DisableChecksIn("NamedThreeDisabledLeaks");

  HeapLeakChecker check("named_disabling");

  RunNamedDisabledLeaks(NULL);
  RunNamedDisabledLeaks(NULL);
  ThreadNamedDisabledLeaks();
  RunNamedDisabledLeaks(NULL);
  ThreadNamedDisabledLeaks();
  ThreadNamedDisabledLeaks();

  // If this check fails, you are probably be running a stripped binary.
  CHECK_EQ(check.SameHeap(), true);  // pprof checking should allow it
}

// The code from here to main()
// is to test that objects that are reachable from global
// variables are not reported as leaks,
// with the few exceptions like multiple-inherited objects.

string* live_leak = NULL;
string* live_leak2 = new string("ss");
vector<int>* live_leak3 = new vector<int>(10,10);
const char* live_leak4 = new char[5];
vector<int> live_leak5(20,10);
const vector<int> live_leak6(30,10);
const string* live_leak_arr1 = new string[5];

class ClassA {
 public:
  ClassA(int a) : ptr(NULL) { }
  mutable char* ptr;
};

const ClassA live_leak7(1);

template<class C>
class TClass {
 public:
  TClass(int a) : ptr(NULL) { }
  mutable C val;
  mutable C* ptr;
};

const TClass<string> live_leak8(1);

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

ClassB* live_leak_b;
ClassD1* live_leak_d1;
ClassD2* live_leak_d2;
ClassD* live_leak_d;

ClassB* live_leak_b_d1;
ClassB2* live_leak_b2_d2;
ClassB* live_leak_b_d;
ClassB2* live_leak_b2_d;

ClassD1* live_leak_d1_d;
ClassD2* live_leak_d2_d;

static void IgnoredLeaks() {
  int* p = new int;
  HeapLeakChecker::IgnoreObject(p);
  int** leak = new int*;
  HeapLeakChecker::IgnoreObject(leak);
  *leak = new int;
  HeapLeakChecker::UnIgnoreObject(p);
  delete p;
}

static void TestHeapLeakCheckerLiveness() {
  live_leak_b = new ClassB;
  live_leak_d1 = new ClassD1;
  live_leak_d2 = new ClassD2;
  live_leak_d = new ClassD;

  live_leak_b_d1 = new ClassD1;
  live_leak_b2_d2 = new ClassD2;
  live_leak_b_d = new ClassD;
  live_leak_b2_d = new ClassD;

  live_leak_d1_d = new ClassD;
  live_leak_d2_d = new ClassD;


#ifndef NDEBUG
  HeapLeakChecker::IgnoreObject((ClassD*)live_leak_b2_d);
  HeapLeakChecker::IgnoreObject((ClassD*)live_leak_d2_d);
    // These two do not get deleted with liveness flood
    // because the base class pointer points inside of the objects
    // in such cases of multiple inheritance.
    // Luckily google code does not use multiple inheritance almost at all.
    // Somehow this does not happen in optimized mode.
#endif

  live_leak = new string("live_leak");
  live_leak3->insert(live_leak3->begin(), 20, 20);
  live_leak2->append(*live_leak);
  live_leak7.ptr = new char [77];
  live_leak8.ptr = new string("aaa");
  live_leak8.val = string("bbbbbb");

  IgnoredLeaks();
  IgnoredLeaks();
  IgnoredLeaks();
}

// Check that we don't give false negatives or positives on leaks from the STL
// allocator.
void TestHeapLeakCheckerSTL() {
  HeapLeakChecker stl_check("stl");
  {
    string x = "banana";
    for (int i = 0; i < 10000; i++)
      x += "na";
  }
  CHECK(stl_check.SameHeap());
}

void TestHeapLeakCheckerSTLInverse() {
  HeapLeakChecker inverse_stl_checker("inverse_stl");
  string x = "queue";
  for (int i = 0; i < 1000; i++)
    x += "ue";
  CHECK_EQ(inverse_stl_checker.SameHeap(), false);
}

int main(int argc, char** argv) {
  // This needs to be set before InternalInitStart(), which makes a local copy
  if (getenv("PPROF_PATH"))
    HeapLeakChecker::set_pprof_path(getenv("PPROF_PATH"));

  // This needs to be set early because it determines the behaviour of
  // InternalInitStart().
  string heap_check_type;
  if (getenv("HEAPCHECK_MODE"))
    heap_check_type = getenv("HEAPCHECK_MODE");
  else
    heap_check_type = "strict";

  HeapLeakChecker::StartFromMain(heap_check_type);

  LogPrintf(INFO, "In main()");

  // The following two modes test whether the whole-program leak checker
  // appropriately detects leaks on exit.
  if (getenv("HEAPCHECK_TEST_LEAK")) {
    void* arr = new vector<int>(10, 10);
    LogPrintf(INFO, "Leaking %p", arr);
    fprintf(stdout, "PASS\n");
    return 0;
  }

  if (getenv("HEAPCHECK_TEST_LOOP_LEAK")) {
    void** arr1 = new void*[2];
    void** arr2 = new void*[2];
    arr1[1] = (void*)arr2;
    arr2[1] = (void*)arr1;
    LogPrintf(INFO, "Loop leaking %p and %p", arr1, arr2);
    fprintf(stdout, "PASS\n");
    return 0;
  }

  TestHeapLeakCheckerLiveness();

  HeapProfilerStart("/tmp/leaks");
  HeapLeakChecker heap_check("all");

  TestHeapLeakChecker();
  TestHeapLeakCheckerTrick();

  TestHeapLeakCheckerDeathSimple();
  TestHeapLeakCheckerDeathInverse();
  TestHeapLeakCheckerDeathNoLeaks();
  TestHeapLeakCheckerDeathCountLess();
  TestHeapLeakCheckerDeathCountMore();

  TestHeapLeakCheckerDeathTrick();
  TestHeapLeakCheckerPProf();

  TestHeapLeakCheckerDisabling();
  TestHeapLeakCheckerNamedDisabling();

  TestHeapLeakCheckerSTL();
  TestHeapLeakCheckerSTLInverse();

  int a;
  ThreadNamedDisabledLeaks(&a);

  CHECK(heap_check.SameHeap());

  HeapLeakChecker::IgnoreObject(new vector<int>(10, 10));
    // This checks both that IgnoreObject works, and
    // and the fact that we don't drop such leaks as live for some reason.

  fprintf(stdout, "PASS\n");
  return 0;
}
