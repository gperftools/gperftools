/* Copyright (c) 2008, Google Inc.
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * ---
 * Author: Kostya Serebryany
 */

// This file defines dynamic annotations for use with dynamic analysis
// tool such as valgrind, PIN, etc.
//
// Dynamic annotation is a source code annotation that affects
// the generated code (that is, the annotation is not a comment).
// Each such annotation is attached to a particular
// instruction and/or to a particular object (address) in the program.
//
// The annotations that should be used by users are macros in all upper-case
// (e.g., ANNOTATE_NEW_MEMORY).
//
// Actual implementation of these macros may differ depending on the
// dynamic analysis tool being used.
//
// This file supports the following dynamic analysis tools:
// - None (NDEBUG is defined).
//    Macros are defined empty.
// - Helgrind (NDEBUG is not defined).
//    Macros are defined as calls to non-inlinable empty functions
//    that are intercepted by helgrind.
//
#ifndef BASE_DYNAMIC_ANNOTATIONS_H_
#define BASE_DYNAMIC_ANNOTATIONS_H_

#include "base/thread_annotations.h"

// All the annotation macros are in effect only in debug mode.
#ifndef NDEBUG

  // -------------------------------------------------------------
  // Annotations useful when implementing condition variables such as CondVar,
  // using conditional critical sections (Await/LockWhen) and when constructing
  // user-defined synchronization mechanisms.
  //
  // The annotations ANNOTATE_HAPPENS_BEFORE() and ANNOTATE_HAPPENS_AFTER() can
  // be used to define happens-before arcs in user-defined synchronization
  // mechanisms:  the race detector will infer an arc from the former to the
  // latter when they share the same argument pointer.
  //
  // Example 1 (reference counting):
  //
  // void Unref() {
  //   ANNOTATE_HAPPENS_BEFORE(&refcount_);
  //   if (AtomicDecrementByOne(&refcount_) == 0) {
  //     ANNOTATE_HAPPENS_AFTER(&refcount_);
  //     delete this;
  //   }
  // }
  //
  // Example 2 (message queue):
  //
  // void MyQueue::Put(Type *e) {
  //   MutexLock lock(&mu_);
  //   ANNOTATE_HAPPENS_BEFORE(e);
  //   PutElementIntoMyQueue(e);
  // }
  //
  // Type *MyQueue::Get() {
  //   MutexLock lock(&mu_);
  //   Type *e = GetElementFromMyQueue();
  //   ANNOTATE_HAPPENS_AFTER(e);
  //   return e;
  // }
  //
  // Note: when possible, please use the existing reference counting and message
  // queue implementations instead of inventing new ones.

  // Report that wait on the condition variable at address "cv" has succeeded
  // and the lock at address "lock" is held.
  #define ANNOTATE_CONDVAR_LOCK_WAIT(cv, lock) \
    AnnotateCondVarWait(__FILE__, __LINE__, cv, lock)

  // Report that wait on the condition variable at "cv" has succeeded.  Variant
  // w/o lock.
  #define ANNOTATE_CONDVAR_WAIT(cv) \
    AnnotateCondVarWait(__FILE__, __LINE__, cv, NULL)

  // Report that we are about to signal on the condition variable at address
  // "cv".
  #define ANNOTATE_CONDVAR_SIGNAL(cv) \
    AnnotateCondVarSignal(__FILE__, __LINE__, cv)

  // Report that we are about to signal_all on the condition variable at "cv".
  #define ANNOTATE_CONDVAR_SIGNAL_ALL(cv) \
    AnnotateCondVarSignalAll(__FILE__, __LINE__, cv)

  // Annotations for user-defined synchronization mechanisms.
  #define ANNOTATE_HAPPENS_BEFORE(obj) ANNOTATE_CONDVAR_SIGNAL(obj)
  #define ANNOTATE_HAPPENS_AFTER(obj)  ANNOTATE_CONDVAR_WAIT(obj)

  // Report that the bytes in the range [pointer, pointer+size) are about
  // to be published safely. The race checker will create a happens-before
  // arc from the call ANNOTATE_PUBLISH_MEMORY_RANGE(pointer, size) to
  // subsequent accesses to this memory.
  #define ANNOTATE_PUBLISH_MEMORY_RANGE(pointer, size) \
    AnnotatePublishMemoryRange(__FILE__, __LINE__, pointer, size)

  // Report that the bytes in the range [pointer, pointer+size) are not shared
  // between threads any more and can be safely used by the current thread w/o
  // synchronization. The race checker will create a happens-before arc from
  // all previous accesses to this memory to this call.
  //
  // This annotation could be applied to complex objects, such as STL
  // containers, with one condition: the accesses to the object itself
  // and its internal data should not be separated with any synchronization.
  //
  // Example that works:
  //
  // map<int, int> the_map;
  // void Thread1() {
  //   MutexLock lock(&mu);
  //   // Ok: accesses to the_map and its internal data is not separated by
  //   // synchronization.
  //   the_map[1]++;
  // }
  // void Thread2() {
  //   {
  //     MutexLock lock(&mu);
  //     ...
  //     // because of some reason we know that the_map will not be used by
  //     // other threads any more
  //     ANNOTATE_UNPUBLISH_MEMORY_RANGE(&the_map, sizeof(the_map));
  //   }
  //   the_map->DoSomething();
  // }
  //
  // Example that does not work (due to the way happens-before arcs are
  // represented in some race detectors):
  //
  // void Thread1() {
  //   MutexLock lock(&mu);
  //   int *guts_of_the_map = &(*the_map)[1];
  //   // we have some synchronization between access to 'c' and its guts.
  //   // This will make ANNOTATE_UNPUBLISH_MEMORY_RANGE in Thread2  useless.
  //   some_other_lock_or_other_synchronization_utility.Lock();
  //   (*guts_of_the_map)++;
  //    ...
  // }
  //
  // void Thread1() { // same as above...
  #define ANNOTATE_UNPUBLISH_MEMORY_RANGE(pointer, size) \
    AnnotateUnpublishMemoryRange(__FILE__, __LINE__, pointer, size)

  // This annotation should be used to annotate thread-safe swapping of
  // containers. Required only when using hybrid (i.e. not pure happens-before)
  // detectors.
  //
  // This annotation has the same limitation as ANNOTATE_UNPUBLISH_MEMORY_RANGE
  // (see above).
  //
  // Example:
  // map<int, int> the_map;
  // void Thread1() {
  //   MutexLock lock(&mu);
  //   the_map[1]++;
  // }
  // void Thread2() {
  //   map<int,int> tmp;
  //   {
  //     MutexLock lock(&mu);
  //     the_map.swap(tmp);
  //     ANNOTATE_SWAP_MEMORY_RANGE(&the_map, sizeof(the_map));
  //   }
  //   tmp->DoSomething();
  // }
  #define ANNOTATE_SWAP_MEMORY_RANGE(pointer, size)   \
    do {                                              \
      ANNOTATE_UNPUBLISH_MEMORY_RANGE(pointer, size); \
      ANNOTATE_PUBLISH_MEMORY_RANGE(pointer, size);   \
    } while (0)

  // Instruct the tool to create a happens-before arc between mu->Unlock() and
  // mu->Lock(). This annotation may slow down the race detector and hide real
  // races. Normally it is used only when it would be difficult to annotate each
  // of the mutex's critical sections individually using the annotations above.
  // This annotation makes sense only for hybrid race detectors. For pure
  // happens-before detectors this is a no-op. For more details see
  // http://code.google.com/p/data-race-test/wiki/PureHappensBeforeVsHybrid .
  #define ANNOTATE_PURE_HAPPENS_BEFORE_MUTEX(mu) \
    AnnotateMutexIsUsedAsCondVar(__FILE__, __LINE__, mu)

  // Deprecated. Use ANNOTATE_PURE_HAPPENS_BEFORE_MUTEX.
  #define ANNOTATE_MUTEX_IS_USED_AS_CONDVAR(mu) \
    AnnotateMutexIsUsedAsCondVar(__FILE__, __LINE__, mu)

  // -------------------------------------------------------------
  // Annotations useful when defining memory allocators, or when memory that
  // was protected in one way starts to be protected in another.

  // Report that a new memory at "address" of size "size" has been allocated.
  // This might be used when the memory has been retrieved from a free list and
  // is about to be reused, or when a the locking discipline for a variable
  // changes.
  #define ANNOTATE_NEW_MEMORY(address, size) \
    AnnotateNewMemory(__FILE__, __LINE__, address, size)

  // -------------------------------------------------------------
  // Annotations useful when defining FIFO queues that transfer data between
  // threads.

  // Report that the producer-consumer queue (such as ProducerConsumerQueue) at
  // address "pcq" has been created.  The ANNOTATE_PCQ_* annotations
  // should be used only for FIFO queues.  For non-FIFO queues use
  // ANNOTATE_HAPPENS_BEFORE (for put) and ANNOTATE_HAPPENS_AFTER (for get).
  #define ANNOTATE_PCQ_CREATE(pcq) \
    AnnotatePCQCreate(__FILE__, __LINE__, pcq)

  // Report that the queue at address "pcq" is about to be destroyed.
  #define ANNOTATE_PCQ_DESTROY(pcq) \
    AnnotatePCQDestroy(__FILE__, __LINE__, pcq)

  // Report that we are about to put an element into a FIFO queue at address
  // "pcq".
  #define ANNOTATE_PCQ_PUT(pcq) \
    AnnotatePCQPut(__FILE__, __LINE__, pcq)

  // Report that we've just got an element from a FIFO queue at address "pcq".
  #define ANNOTATE_PCQ_GET(pcq) \
    AnnotatePCQGet(__FILE__, __LINE__, pcq)

  // -------------------------------------------------------------
  // Annotations that suppress errors.  It is usually better to express the
  // program's synchronization using the other annotations, but these can
  // be used when all else fails.

  // Report that we may have a benign race on at "address".
  // Insert at the point where "address" has been allocated, preferably close
  // to the point where the race happens.
  // See also ANNOTATE_BENIGN_RACE_STATIC.
  #define ANNOTATE_BENIGN_RACE(address, description) \
    AnnotateBenignRace(__FILE__, __LINE__, address, description)

  // Request the analysis tool to ignore all reads in the current thread
  // until ANNOTATE_IGNORE_READS_END is called.
  // Useful to ignore intentional racey reads, while still checking
  // other reads and all writes.
  // See also ANNOTATE_UNPROTECTED_READ.
  #define ANNOTATE_IGNORE_READS_BEGIN() \
    AnnotateIgnoreReadsBegin(__FILE__, __LINE__)

  // Stop ignoring reads.
  #define ANNOTATE_IGNORE_READS_END() \
    AnnotateIgnoreReadsEnd(__FILE__, __LINE__)

  // Similar to ANNOTATE_IGNORE_READS_BEGIN, but ignore writes.
  #define ANNOTATE_IGNORE_WRITES_BEGIN() \
    AnnotateIgnoreWritesBegin(__FILE__, __LINE__)

  // Stop ignoring writes.
  #define ANNOTATE_IGNORE_WRITES_END() \
    AnnotateIgnoreWritesEnd(__FILE__, __LINE__)

  // Start ignoring all memory accesses (reads and writes).
  #define ANNOTATE_IGNORE_READS_AND_WRITES_BEGIN() \
    do {\
      ANNOTATE_IGNORE_READS_BEGIN();\
      ANNOTATE_IGNORE_WRITES_BEGIN();\
    }while(0)\

  // Stop ignoring all memory accesses.
  #define ANNOTATE_IGNORE_READS_AND_WRITES_END() \
    do {\
      ANNOTATE_IGNORE_WRITES_END();\
      ANNOTATE_IGNORE_READS_END();\
    }while(0)\

  // -------------------------------------------------------------
  // Annotations useful for debugging.

  // Request to trace every access to "address".
  #define ANNOTATE_TRACE_MEMORY(address) \
    AnnotateTraceMemory(__FILE__, __LINE__, address)

  // Report the current thread name to a race detector.
  #define ANNOTATE_THREAD_NAME(name) \
    AnnotateThreadName(__FILE__, __LINE__, name)

  // -------------------------------------------------------------
  // Annotations useful when implementing locks.  They are not
  // normally needed by modules that merely use locks.
  // The "lock" argument is a pointer to the lock object.

  // Report that a lock has been created at address "lock".
  #define ANNOTATE_RWLOCK_CREATE(lock) \
    AnnotateRWLockCreate(__FILE__, __LINE__, lock)

  // Report that the lock at address "lock" is about to be destroyed.
  #define ANNOTATE_RWLOCK_DESTROY(lock) \
    AnnotateRWLockDestroy(__FILE__, __LINE__, lock)

  // Report that the lock at address "lock" has been acquired.
  // is_w=1 for writer lock, is_w=0 for reader lock.
  #define ANNOTATE_RWLOCK_ACQUIRED(lock, is_w) \
    AnnotateRWLockAcquired(__FILE__, __LINE__, lock, is_w)

  // Report that the lock at address "lock" is about to be released.
  #define ANNOTATE_RWLOCK_RELEASED(lock, is_w) \
    AnnotateRWLockReleased(__FILE__, __LINE__, lock, is_w)

  // -------------------------------------------------------------
  // Annotations useful for testing race detectors.

  // Report that we expect a race on the variable at "address".
  // Use only in unit tests for a race detector.
  #define ANNOTATE_EXPECT_RACE(address, description) \
    AnnotateExpectRace(__FILE__, __LINE__, address, description)

  // A no-op. Insert where you like to test the interceptors.
  #define ANNOTATE_NO_OP(arg) \
    AnnotateNoOp(__FILE__, __LINE__, arg)

#else  // NDEBUG is defined

  #define ANNOTATE_RWLOCK_CREATE(lock) // empty
  #define ANNOTATE_RWLOCK_DESTROY(lock) // empty
  #define ANNOTATE_RWLOCK_ACQUIRED(lock, is_w) // empty
  #define ANNOTATE_RWLOCK_RELEASED(lock, is_w) // empty
  #define ANNOTATE_CONDVAR_LOCK_WAIT(cv, lock) // empty
  #define ANNOTATE_CONDVAR_WAIT(cv) // empty
  #define ANNOTATE_CONDVAR_SIGNAL(cv) // empty
  #define ANNOTATE_CONDVAR_SIGNAL_ALL(cv) // empty
  #define ANNOTATE_HAPPENS_BEFORE(obj) // empty
  #define ANNOTATE_HAPPENS_AFTER(obj) // empty
  #define ANNOTATE_PUBLISH_MEMORY_RANGE(address, size) // empty
  #define ANNOTATE_UNPUBLISH_MEMORY_RANGE(address, size)  // empty
  #define ANNOTATE_SWAP_MEMORY_RANGE(address, size)  // empty
  #define ANNOTATE_PCQ_CREATE(pcq) // empty
  #define ANNOTATE_PCQ_DESTROY(pcq) // empty
  #define ANNOTATE_PCQ_PUT(pcq) // empty
  #define ANNOTATE_PCQ_GET(pcq) // empty
  #define ANNOTATE_NEW_MEMORY(address, size) // empty
  #define ANNOTATE_EXPECT_RACE(address, description) // empty
  #define ANNOTATE_BENIGN_RACE(address, description) // empty
  #define ANNOTATE_PURE_HAPPENS_BEFORE_MUTEX(mu) // empty
  #define ANNOTATE_MUTEX_IS_USED_AS_CONDVAR(mu) // empty
  #define ANNOTATE_TRACE_MEMORY(arg) // empty
  #define ANNOTATE_THREAD_NAME(name) // empty
  #define ANNOTATE_IGNORE_READS_BEGIN() // empty
  #define ANNOTATE_IGNORE_READS_END() // empty
  #define ANNOTATE_IGNORE_WRITES_BEGIN() // empty
  #define ANNOTATE_IGNORE_WRITES_END() // empty
  #define ANNOTATE_IGNORE_READS_AND_WRITES_BEGIN() // empty
  #define ANNOTATE_IGNORE_READS_AND_WRITES_END() // empty
  #define ANNOTATE_NO_OP(arg) // empty

#endif  // NDEBUG

// Use the macros above rather than using these functions directly.
extern "C" void AnnotateRWLockCreate(const char *file, int line,
                                     const volatile void *lock);
extern "C" void AnnotateRWLockDestroy(const char *file, int line,
                                      const volatile void *lock);
extern "C" void AnnotateRWLockAcquired(const char *file, int line,
                                       const volatile void *lock, long is_w);
extern "C" void AnnotateRWLockReleased(const char *file, int line,
                                       const volatile void *lock, long is_w);
extern "C" void AnnotateCondVarWait(const char *file, int line,
                                    const volatile void *cv,
                                    const volatile void *lock);
extern "C" void AnnotateCondVarSignal(const char *file, int line,
                                      const volatile void *cv);
extern "C" void AnnotateCondVarSignalAll(const char *file, int line,
                                         const volatile void *cv);
extern "C" void AnnotatePublishMemoryRange(const char *file, int line,
                                           const volatile void *address,
                                           long size);
extern "C" void AnnotateUnpublishMemoryRange(const char *file, int line,
                                           const volatile void *address,
                                           long size);
extern "C" void AnnotatePCQCreate(const char *file, int line,
                                  const volatile void *pcq);
extern "C" void AnnotatePCQDestroy(const char *file, int line,
                                   const volatile void *pcq);
extern "C" void AnnotatePCQPut(const char *file, int line,
                               const volatile void *pcq);
extern "C" void AnnotatePCQGet(const char *file, int line,
                               const volatile void *pcq);
extern "C" void AnnotateNewMemory(const char *file, int line,
                                  const volatile void *address,
                                  long size);
extern "C" void AnnotateExpectRace(const char *file, int line,
                                   const volatile void *address,
                                   const char *description);
extern "C" void AnnotateBenignRace(const char *file, int line,
                                   const volatile void *address,
                                   const char *description);
extern "C" void AnnotateMutexIsUsedAsCondVar(const char *file, int line,
                                            const volatile void *mu);
extern "C" void AnnotateTraceMemory(const char *file, int line,
                                    const volatile void *arg);
extern "C" void AnnotateThreadName(const char *file, int line,
                                   const char *name);
extern "C" void AnnotateIgnoreReadsBegin(const char *file, int line);
extern "C" void AnnotateIgnoreReadsEnd(const char *file, int line);
extern "C" void AnnotateIgnoreWritesBegin(const char *file, int line);
extern "C" void AnnotateIgnoreWritesEnd(const char *file, int line);
extern "C" void AnnotateNoOp(const char *file, int line,
                             const volatile void *arg);

#ifndef NDEBUG

  // ANNOTATE_UNPROTECTED_READ is the preferred way to annotate racey reads.
  //
  // Instead of doing
  //    ANNOTATE_IGNORE_READS_BEGIN();
  //    ... = x;
  //    ANNOTATE_IGNORE_READS_END();
  // one can use
  //    ... = ANNOTATE_UNPROTECTED_READ(x);
  template <class T>
  inline T ANNOTATE_UNPROTECTED_READ(const volatile T &x)
       NO_THREAD_SAFETY_ANALYSIS {
    ANNOTATE_IGNORE_READS_BEGIN();
    T res = x;
    ANNOTATE_IGNORE_READS_END();
    return res;
  }

  // Apply ANNOTATE_BENIGN_RACE to a static variable.
  #define ANNOTATE_BENIGN_RACE_STATIC(static_var, description)        \
    namespace {                                                       \
      class static_var ## _annotator {                                \
       public:                                                        \
        static_var ## _annotator() {                                  \
          ANNOTATE_BENIGN_RACE(&static_var,                           \
            # static_var ": " description);                           \
        }                                                             \
      };                                                              \
      static static_var ## _annotator the ## static_var ## _annotator;\
    }
#else // !NDEBUG

  #define ANNOTATE_UNPROTECTED_READ(x) (x)
  #define ANNOTATE_BENIGN_RACE_STATIC(static_var, description)  // empty

#endif // !NDEBUG

// Return non-zero value if running under valgrind.
extern "C" int RunningOnValgrind();


#endif  // BASE_DYNAMIC_ANNOTATIONS_H_
