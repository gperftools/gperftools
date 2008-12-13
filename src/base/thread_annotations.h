// Copyright (c) 2008, Google Inc.
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
// Author: Le-Chun Wu
//
// This header file contains the macro definitions for thread safety
// annotations that allow the developers to document the locking policies
// of their multi-threaded code. The annotations can also help program
// analysis tools to identify potential thread safety issues.
//
// The annotations are implemented using GCC's "attributes" extension.
// Using the macros defined here instead of the raw GCC attributes allows
// for portability and future compatibility.
//
// This functionality is not yet fully implemented in perftools,
// but may be one day.

#ifndef BASE_THREAD_ANNOTATIONS_H_
#define BASE_THREAD_ANNOTATIONS_H_

#if defined(__GNUC__) && defined(__SUPPORT_TS_ANNOTATION__) && (!defined(SWIG))

// Document if a shared variable/field needs to be protected by a lock.
// GUARDED_BY allows the user to specify a particular lock that should be
// held when accessing the annotated variable, while GUARDED_VAR only
// indicates a shared variable should be guarded (by any lock). GUARDED_VAR
// is primarily used when the client cannot express the name of the lock.
#define GUARDED_BY(x)          __attribute__ ((guarded_by(x)))
#define GUARDED_VAR            __attribute__ ((guarded))

// Document if the memory location pointed to by a pointer should be guarded
// by a lock when dereferencing the pointer. Similar to GUARDED_VAR,
// PT_GUARDED_VAR is primarily used when the client cannot express the name
// of the lock. Note that a pointer variable to a shared memory location
// could itself be a shared variable. For example, if a shared global pointer
// q, which is guarded by mu1, points to a shared memory location that is
// guarded by mu2, q should be annotated as follows:
//     int *q GUARDED_BY(mu1) PT_GUARDED_BY(mu2);
#define PT_GUARDED_BY(x)       __attribute__ ((point_to_guarded_by(x)))
#define PT_GUARDED_VAR         __attribute__ ((point_to_guarded))

// Document the acquisition order between locks that can be held
// simultaneously by a thread. For any two locks that need to be annotated
// to establish an acquisition order, only one of them needs the annotation.
// (i.e. You don't have to annotate both locks with both ACQUIRED_AFTER
// and ACQUIRED_BEFORE.)
#define ACQUIRED_AFTER(...)    __attribute__ ((acquired_after(__VA_ARGS__)))
#define ACQUIRED_BEFORE(...)   __attribute__ ((acquired_before(__VA_ARGS__)))

// The following three annotations document the lock requirements for
// functions/methods.

// Document if a function expects certain locks to be held before it is called
#define EXCLUSIVE_LOCKS_REQUIRED(...) \
  __attribute__ ((exclusive_locks_required(__VA_ARGS__)))

#define SHARED_LOCKS_REQUIRED(...) \
  __attribute__ ((shared_locks_required(__VA_ARGS__)))

// Document the locks acquired in the body of the function. These locks
// cannot be held when calling this function (as google3's Mutex locks are
// non-reentrant).
#define LOCKS_EXCLUDED(...)    __attribute__ ((locks_excluded(__VA_ARGS__)))

// Document the lock the annotated function returns without acquiring it.
#define LOCK_RETURNED(x)       __attribute__ ((lock_returned(x)))

// Document if a class/type is a lockable type (such as the Mutex class).
#define LOCKABLE               __attribute__ ((lockable))

// Document if a class is a scoped lockable type (such as the MutexLock class).
#define SCOPED_LOCKABLE        __attribute__ ((scoped_lockable))

// The following annotations specify lock and unlock primitives.
#define EXCLUSIVE_LOCK_FUNCTION(...) \
  __attribute__ ((exclusive_lock(__VA_ARGS__)))

#define SHARED_LOCK_FUNCTION(...) \
  __attribute__ ((shared_lock(__VA_ARGS__)))

#define EXCLUSIVE_TRYLOCK_FUNCTION(...) \
  __attribute__ ((exclusive_trylock(__VA_ARGS__)))

#define SHARED_TRYLOCK_FUNCTION(...) \
  __attribute__ ((shared_trylock(__VA_ARGS__)))

#define UNLOCK_FUNCTION(...)   __attribute__ ((unlock(__VA_ARGS__)))

// An escape hatch for thread safety analysis to ignore the annotated function.
#define NO_THREAD_SAFETY_ANALYSIS  __attribute__ ((no_thread_safety_analysis))


#else

// When the compiler is not GCC, these annotations are simply no-ops.

#define GUARDED_BY(x)                   // no-op
#define GUARDED_VAR                     // no-op
#define PT_GUARDED_BY(x)                // no-op
#define PT_GUARDED_VAR                  // no-op
#define ACQUIRED_AFTER(...)             // no-op
#define ACQUIRED_BEFORE(...)            // no-op
#define EXCLUSIVE_LOCKS_REQUIRED(...)   // no-op
#define SHARED_LOCKS_REQUIRED(...)      // no-op
#define LOCKS_EXCLUDED(...)             // no-op
#define LOCK_RETURNED(x)                // no-op
#define LOCKABLE                        // no-op
#define SCOPED_LOCKABLE                 // no-op
#define EXCLUSIVE_LOCK_FUNCTION(...)    // no-op
#define SHARED_LOCK_FUNCTION(...)       // no-op
#define EXCLUSIVE_TRYLOCK_FUNCTION(...) // no-op
#define SHARED_TRYLOCK_FUNCTION(...)    // no-op
#define UNLOCK_FUNCTION(...)            // no-op
#define NO_THREAD_SAFETY_ANALYSIS       // no-op

#endif // defined(__GNUC__) && defined(__SUPPORT_TS_ANNOTATION__)
       // && !defined(SWIG)

#endif  // BASE_THREAD_ANNOTATIONS_H_
