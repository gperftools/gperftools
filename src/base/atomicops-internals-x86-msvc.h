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

// Implementation of atomic operations for x86.  This file should not
// be included directly.  Clients should instead include
// "base/atomicops.h".

#ifndef BASE_ATOMICOPS_INTERNALS_X86_MSVC_H__
#define BASE_ATOMICOPS_INTERNALS_X86_MSVC_H__
#include "base/basictypes.h"  // For COMPILE_ASSERT

typedef intptr_t AtomicWord;
#ifdef _WIN64
typedef LONG Atomic32;
#else
typedef AtomicWord Atomic32;
#endif

COMPILE_ASSERT(sizeof(AtomicWord) == sizeof(PVOID), atomic_word_is_atomic);

inline AtomicWord CompareAndSwap(volatile AtomicWord* ptr,
                                 AtomicWord old_value,
                                 AtomicWord new_value) {
  PVOID result = InterlockedCompareExchangePointer(
    reinterpret_cast<volatile PVOID*>(ptr),
    reinterpret_cast<PVOID>(new_value), reinterpret_cast<PVOID>(old_value));
  return reinterpret_cast<AtomicWord>(result);
}

inline AtomicWord AtomicExchange(volatile AtomicWord* ptr,
                                 AtomicWord new_value) {
  PVOID result = InterlockedExchangePointer(
    const_cast<PVOID*>(reinterpret_cast<volatile PVOID*>(ptr)),
    reinterpret_cast<PVOID>(new_value));
  return reinterpret_cast<AtomicWord>(result);
}

#ifdef _WIN64
inline Atomic32 AtomicIncrement(volatile Atomic32* ptr, Atomic32 increment) {
  // InterlockedExchangeAdd returns *ptr before being incremented
  // and we must return nonzero iff *ptr is nonzero after being
  // incremented.
  return InterlockedExchangeAdd(ptr, increment) + increment;
}

inline AtomicWord AtomicIncrement(volatile AtomicWord* ptr, AtomicWord increment) {
    return InterlockedExchangeAdd64(
      reinterpret_cast<volatile LONGLONG*>(ptr),
      static_cast<LONGLONG>(increment)) + increment;
}
#else
inline AtomicWord AtomicIncrement(volatile AtomicWord* ptr, AtomicWord increment) {
    return InterlockedExchangeAdd(
      reinterpret_cast<volatile LONG*>(ptr),
      static_cast<LONG>(increment)) + increment;
}
#endif

inline AtomicWord Acquire_CompareAndSwap(volatile AtomicWord* ptr,
                                         AtomicWord old_value,
                                         AtomicWord new_value) {
  return CompareAndSwap(ptr, old_value, new_value);
}

inline AtomicWord Release_CompareAndSwap(volatile AtomicWord* ptr,
                                         AtomicWord old_value,
                                         AtomicWord new_value) {
  return CompareAndSwap(ptr, old_value, new_value);
}

// In msvc8/vs2005, winnt.h already contains a definition for MemoryBarrier.
#if !(defined(_MSC_VER) && _MSC_VER >= 1400)
inline void MemoryBarrier() {
  AtomicWord value = 0;
  AtomicExchange(&value, 0); // acts as a barrier
}
#endif

inline void Acquire_Store(volatile AtomicWord* ptr, AtomicWord value) {
  AtomicExchange(ptr, value);
}

inline void Release_Store(volatile AtomicWord* ptr, AtomicWord value) {
  *ptr = value; // works w/o barrier for current Intel chips as of June 2005

  // When new chips come out, check:
  //  IA-32 Intel Architecture Software Developer's Manual, Volume 3:
  //  System Programming Guide, Chatper 7: Multiple-processor management,
  //  Section 7.2, Memory Ordering.
  // Last seen at:
  //   http://developer.intel.com/design/pentium4/manuals/index_new.htm
}

inline AtomicWord Acquire_Load(volatile const AtomicWord* ptr) {
  AtomicWord value = *ptr;
  MemoryBarrier();
  return value;
}

inline AtomicWord Release_Load(volatile const AtomicWord* ptr) {
  MemoryBarrier();
  return *ptr;
}

#endif  // BASE_ATOMICOPS_INTERNALS_X86_MSVC_H__
