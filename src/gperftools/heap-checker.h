// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
// Copyright (c) 2005, Google Inc.
// Copyright (c) 2024, gperftools Contributors
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

// Note, this file contains stub of HeapLeakChecker API that we used
// to implement. Below is merely a stub, empty no-op
// implementation. It only exists to maintain some degree of backwards
// API and ABI compatibility. If you're using this file, please
// consider switching to sanitizers.
#ifndef BASE_HEAP_CHECKER_H_
#define BASE_HEAP_CHECKER_H_

#include <stddef.h>
#include <stdint.h>

#include <vector>

#ifndef PERFTOOLS_DLL_DECL
# ifdef _WIN32
#   define PERFTOOLS_DLL_DECL  __declspec(dllimport)
# else
#   define PERFTOOLS_DLL_DECL
# endif
#endif

// The class is thread-safe with respect to all the provided static methods,
// as well as HeapLeakChecker objects: they can be accessed by multiple threads.
class PERFTOOLS_DLL_DECL HeapLeakChecker {
 public:
  static bool IsActive();
  static HeapLeakChecker* GlobalChecker();
  static bool NoGlobalLeaks();
  static void CancelGlobalCheck();

  explicit HeapLeakChecker(const char *name);

  ~HeapLeakChecker();

  bool NoLeaks() { return DoNoLeaks(DO_NOT_SYMBOLIZE); }

  bool QuickNoLeaks()  { return NoLeaks(); }
  bool BriefNoLeaks()  { return NoLeaks(); }
  bool SameHeap()      { return NoLeaks(); }
  bool QuickSameHeap() { return NoLeaks(); }
  bool BriefSameHeap() { return NoLeaks(); }

  // Note, original code had ssize_t, but windows lacks it. So to keep
  // things easier and more portable we're converting to ptrdiff_t.
  // It is same in practice.
  ptrdiff_t BytesLeaked() const;
  ptrdiff_t ObjectsLeaked() const;

  class Disabler {
   public:
    Disabler();
    ~Disabler();
   private:
    Disabler(const Disabler&);        // disallow copy
    void operator=(const Disabler&);  // and assign
  };

  template <typename T>
  static T* IgnoreObject(T* ptr) {
    DoIgnoreObject(static_cast<const void*>(const_cast<const T*>(ptr)));
    return ptr;
  }

  static void UnIgnoreObject(const void* ptr);

private:
  enum ShouldSymbolize { SYMBOLIZE, DO_NOT_SYMBOLIZE };

  bool DoNoLeaks(ShouldSymbolize should_symbolize);
  static void DoIgnoreObject(const void* ptr);

  void* _ex_lock_;
  const char* name_;

  void* start_snapshot_;

  bool has_checked_;
  ptrdiff_t inuse_bytes_increase_;
  ptrdiff_t inuse_allocs_increase_;
                                   // for this checker
  bool keep_profiles_;

  HeapLeakChecker(const HeapLeakChecker&);
  void operator=(const HeapLeakChecker&);
};


// Holds a pointer that will not be traversed by the heap checker.
// Contrast with HeapLeakChecker::IgnoreObject(o), in which o and
// all objects reachable from o are ignored by the heap checker.
template <class T>
class HiddenPointer {
 public:
  explicit HiddenPointer(T* t)
      : masked_t_(reinterpret_cast<uintptr_t>(t) ^ kHideMask) {
  }
  // Returns unhidden pointer.  Be careful where you save the result.
  T* get() const { return reinterpret_cast<T*>(masked_t_ ^ kHideMask); }

 private:
  // Arbitrary value, but not such that xor'ing with it is likely
  // to map one valid pointer to another valid pointer:
  static const uintptr_t kHideMask =
      static_cast<uintptr_t>(0xF03A5F7BF03A5F7Bll);
  uintptr_t masked_t_;
};

// A class that exists solely to run its destructor.  This class should not be
// used directly, but instead by the REGISTER_HEAPCHECK_CLEANUP macro below.
class PERFTOOLS_DLL_DECL HeapCleaner {
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

#endif  // BASE_HEAP_CHECKER_H_
