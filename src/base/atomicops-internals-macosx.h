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
 * Author: Mike Burrows
 */

// Implementation of atomic operations for Mac OS X.  This file should not
// be included directly.  Clients should instead include
// "base/atomicops.h".

#ifndef BASE_ATOMICOPS_INTERNALS_MACOSX_H__
#define BASE_ATOMICOPS_INTERNALS_MACOSX_H__

typedef int32_t Atomic32;
typedef intptr_t AtomicWord;

#include <libkern/OSAtomic.h>

#ifdef __LP64__   // Indicates 64-bit pointers under OS 
#define OSAtomicCastIntPtr(p) \
               reinterpret_cast<int64_t *>(const_cast<AtomicWord *>(p))
#define OSAtomicCompareAndSwapIntPtr OSAtomicCompareAndSwap64
#define OSAtomicAddIntPtr OSAtomicAdd64
#define OSAtomicCompareAndSwapIntPtrBarrier OSAtomicCompareAndSwap64Barrier
#else
#define OSAtomicCastIntPtr(p) \
               reinterpret_cast<int32_t *>(const_cast<AtomicWord *>(p))
#define OSAtomicCompareAndSwapIntPtr OSAtomicCompareAndSwap32
#define OSAtomicAddIntPtr OSAtomicAdd32
#define OSAtomicCompareAndSwapIntPtrBarrier OSAtomicCompareAndSwap32Barrier
#endif

inline void MemoryBarrier() {
  OSMemoryBarrier();
}

inline AtomicWord CompareAndSwap(volatile AtomicWord *ptr,
                                 AtomicWord old_value,
                                 AtomicWord new_value) {
  AtomicWord prev_value;
  do {
    if (OSAtomicCompareAndSwapIntPtr(old_value, new_value,
                                     OSAtomicCastIntPtr(ptr))) {
      return old_value;
    }
    prev_value = *ptr;
  } while (prev_value == old_value);
  return prev_value;
}

inline AtomicWord AtomicExchange(volatile AtomicWord *ptr,
                                 AtomicWord new_value) {
  AtomicWord old_value;
  do {
    old_value = *ptr;
  } while (!OSAtomicCompareAndSwapIntPtr(old_value, new_value,
                                         OSAtomicCastIntPtr(ptr)));
  return old_value;
}


inline AtomicWord AtomicIncrement(volatile AtomicWord *ptr, AtomicWord increment) {
  return OSAtomicAddIntPtr(increment, OSAtomicCastIntPtr(ptr));
}

inline AtomicWord Acquire_CompareAndSwap(volatile AtomicWord *ptr,
                                         AtomicWord old_value,
                                         AtomicWord new_value) {
  AtomicWord prev_value;
  do {
    if (OSAtomicCompareAndSwapIntPtrBarrier(old_value, new_value,
                                        OSAtomicCastIntPtr(ptr))) {
      return old_value;
    }
    prev_value = *ptr;
  } while (prev_value == old_value);
  return prev_value;
}

inline AtomicWord Release_CompareAndSwap(volatile AtomicWord *ptr,
                                         AtomicWord old_value,
                                         AtomicWord new_value) {
  // The lib kern interface does not distinguish between 
  // Acquire and Release memory barriers; they are equivalent.
  return Acquire_CompareAndSwap(ptr, old_value, new_value);
}


inline void Acquire_Store(volatile AtomicWord *ptr, AtomicWord value) {
  *ptr = value;
  MemoryBarrier();
}

inline void Release_Store(volatile AtomicWord *ptr, AtomicWord value) {
  MemoryBarrier();
  *ptr = value;
}

inline AtomicWord Acquire_Load(volatile const AtomicWord *ptr) {
  AtomicWord value = *ptr;
  MemoryBarrier();
  return value;
}

inline AtomicWord Release_Load(volatile const AtomicWord *ptr) {
  MemoryBarrier();
  return *ptr;
}


// MacOS uses long for intptr_t, AtomicWord and Atomic32 are always different
// on the Mac, even when they are the same size.  Thus, we always provide 
// Atomic32 versions.

inline Atomic32 CompareAndSwap(volatile Atomic32 *ptr,
                               Atomic32 old_value,
                               Atomic32 new_value) {
  Atomic32 prev_value;
  do {
    if (OSAtomicCompareAndSwap32(old_value, new_value,
                                 const_cast<Atomic32*>(ptr))) {
      return old_value;
    }
    prev_value = *ptr;
  } while (prev_value == old_value);
  return prev_value;
}

inline Atomic32 AtomicExchange(volatile Atomic32 *ptr,
                               Atomic32 new_value) {
  Atomic32 old_value;
  do {
    old_value = *ptr;
  } while (!OSAtomicCompareAndSwap32(old_value, new_value,
                                     const_cast<Atomic32*>(ptr)));
  return old_value;
}

inline Atomic32 AtomicIncrement(volatile Atomic32 *ptr, Atomic32 increment) {
  return OSAtomicAdd32(increment, const_cast<Atomic32*>(ptr));
}

inline Atomic32 Acquire_CompareAndSwap(volatile Atomic32 *ptr,
                                       Atomic32 old_value,
                                       Atomic32 new_value) {
  Atomic32 prev_value;
  do {
    if (OSAtomicCompareAndSwap32Barrier(old_value, new_value,
                                        const_cast<Atomic32*>(ptr))) {
      return old_value;
    }
    prev_value = *ptr;
  } while (prev_value == old_value);
  return prev_value;
}

inline Atomic32 Release_CompareAndSwap(volatile Atomic32 *ptr,
                                       Atomic32 old_value,
                                       Atomic32 new_value) {
  return Acquire_CompareAndSwap(ptr, old_value, new_value);
}


inline void Acquire_Store(volatile Atomic32 *ptr, Atomic32 value) {
  *ptr = value;
  MemoryBarrier();
}

inline void Release_Store(volatile Atomic32 *ptr, Atomic32 value) {
  MemoryBarrier();
  *ptr = value;
}

inline Atomic32 Acquire_Load(volatile const Atomic32 *ptr) {
  Atomic32 value = *ptr;
  MemoryBarrier();
  return value;
}

inline Atomic32 Release_Load(volatile const Atomic32 *ptr) {
  MemoryBarrier();
  return *ptr;
}

#endif  // BASE_ATOMICOPS_INTERNALS_MACOSX_H__
