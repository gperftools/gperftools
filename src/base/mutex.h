/* Copyright (c) 2007, Google Inc.
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
 * Author: Craig Silverstein.
 *
 * A simple mutex wrapper, supporting locks and read-write locks.
 *
 * To use: you should define the following macros in your configure.ac:
 *   ACX_PTHREAD
 *   AC_RWLOCK
 * The latter is defined in ../autoconf.
 *
 * This class is meant to be internal-only, so it's defined in the
 * global namespace.  If you want to expose it, you'll want to move
 * it to the Google namespace.
 */

#include "config.h"    // to figure out pthreads support

#if defined(NO_THREADS)
  typedef int MutexType;   // some dummy type; it won't be used
#elif defined(HAVE_PTHREAD) && defined(HAVE_RWLOCK)
  // Needed for pthread_rwlock_*.  If it causes problems, you could take
  // it out, but then you'd have to unset HAVE_RWLOCK (at least on linux).
# define _XOPEN_SOURCE 500   // needed to get the rwlock calls
# include <pthread.h>
  typedef pthread_rwlock_t MutexType;
#elif defined(HAVE_PTHREAD)
# include <pthread.h>
  typedef pthread_mutex_t MutexType;
#else
# error Need to implement mutex.h/cc for your architecture, or #define NO_THREADS
#endif

class Mutex {
 public:
  // Create a Mutex that is not held by anybody.
  Mutex();

  // Destructor
  ~Mutex();

  void Lock();     // Block if necessary until free, then acquire exclusively
  void Unlock();   // Release.  Caller must hold it exclusively (via Lock())

  // Note that on systems that don't support read-write locks, these may
  // be implemented as synonyms to Lock() and Unlock().  So you can use
  // these for efficiency, but don't use them anyplace where being able
  // to do shared reads is necessary to avoid deadlock.
  void ReaderLock();    // Block until free or shared, then acquire a share
  void ReaderUnlock();  // Release a read share of this Mutex
  void WriterLock() { Lock(); }   // Block until free, then acquire exclusively
  void WriterUnlock() { Unlock(); } // Release the exclusive lock of this Mutex

 private:
  MutexType mutex_;

  // Catch the error of writing Mutex when intending MutexLock.
  Mutex(Mutex *ignored) {}
  // Disallow "evil" constructors
  Mutex(const Mutex&);
  void operator=(const Mutex&);
};


// MutexLock(mu) acquires mu when constructed and releases it when destroyed.
class MutexLock {
 public:
  explicit MutexLock(Mutex *mu) : mu_(mu) { mu_->Lock(); }
  ~MutexLock() { mu_->Unlock(); }
 private:
  Mutex * const mu_;
  // Disallow "evil" constructors
  MutexLock(const MutexLock&);
  void operator=(const MutexLock&);
};

// ReaderMutexLock and WriterMutexLock do the same, for rwlocks
class ReaderMutexLock {
 public:
  explicit ReaderMutexLock(Mutex *mu) : mu_(mu) { mu_->ReaderLock(); }
  ~ReaderMutexLock() { mu_->ReaderUnlock(); }
 private:
  Mutex * const mu_;
  // Disallow "evil" constructors
  ReaderMutexLock(const ReaderMutexLock&);
  void operator=(const ReaderMutexLock&);
};

class WriterMutexLock {
 public:
  explicit WriterMutexLock(Mutex *mu) : mu_(mu) { mu_->WriterLock(); }
  ~WriterMutexLock() { mu_->WriterUnlock(); }
 private:
  Mutex * const mu_;
  // Disallow "evil" constructors
  WriterMutexLock(const WriterMutexLock&);
  void operator=(const WriterMutexLock&);
};

// Catch bug where variable name is omitted, e.g. MutexLock (&mu);
#define MutexLock(x) COMPILE_ASSERT(0, mutex_lock_decl_missing_var_name)
#define ReaderMutexLock(x) COMPILE_ASSERT(0, rmutex_lock_decl_missing_var_name)
#define WriterMutexLock(x) COMPILE_ASSERT(0, wmutex_lock_decl_missing_var_name)
