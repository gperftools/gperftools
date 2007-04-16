/* Copyright (c) 2006, Google Inc.
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
 * Author: Sanjay Ghemawat
 */

//
// Fast spinlocks (at least on x86, a lock/unlock pair is approximately
// half the cost of a Mutex because the unlock just does a store instead
// of a compare-and-swap which is expensive).

// Spinlock is async signal safe.
// If used within a signal handler, all lock holders 
// should block the signal even outside the signal handler.

#ifndef BASE_SPINLOCK_H__
#define BASE_SPINLOCK_H__

#include "base/basictypes.h"
#include "base/atomicops.h"

class SpinLock {
 public:
  SpinLock() : lockword_(0) { }

  // Special constructor for use with static SpinLock objects.  E.g.,
  //
  //    static SpinLock lock(SpinLock::LINKER_INITIALIZED);
  //
  // When intialized using this constructor, we depend on the fact
  // that the linker has already initialized the memory appropriately.
  // A SpinLock constructed like this can be freely used from global
  // initializers without worrying about the order in which global
  // initializers run.
  enum StaticInitializer { LINKER_INITIALIZED };
  explicit SpinLock(StaticInitializer x) {
    // Does nothing; lockword_ is already initialized
  }

  inline void Lock() {
    if (Acquire_CompareAndSwap(&lockword_, 0, 1) != 0) {
      SlowLock();
    }
  }

  inline void Unlock() {
    Release_Store(&lockword_, 0);
  }

  // Report if we think the lock can be held by this thread.
  // When the lock is truly held by the invoking thread
  // we will always return true.
  // Indended to be used as CHECK(lock.IsHeld());
  inline bool IsHeld() const {
    return lockword_ != 0;
  }

 private:
  // Lock-state: 0 means unlocked, 1 means locked
  volatile AtomicWord lockword_;

  void SlowLock();

  DISALLOW_EVIL_CONSTRUCTORS(SpinLock);
};

// Corresponding locker object that arranges to acquire a spinlock for
// the duration of a C++ scope.
class SpinLockHolder {
 private:
  SpinLock* lock_;
 public:
  inline explicit SpinLockHolder(SpinLock* l) : lock_(l) { l->Lock(); }
  inline ~SpinLockHolder() { lock_->Unlock(); }
};
// Catch bug where variable name is omitted, e.g. SpinLockHolder (&lock);
#define SpinLockHolder(x) COMPILE_ASSERT(0, spin_lock_decl_missing_var_name)


#endif  // BASE_SPINLOCK_H__
