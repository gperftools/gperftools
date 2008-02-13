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
 */

// Implementation of atomic operations for ppc-linux.  This file should not
// be included directly.  Clients should instead include
// "base/atomicops.h".

#ifndef BASE_ATOMICOPS_INTERNALS_LINUXPPC_H__
#define BASE_ATOMICOPS_INTERNALS_LINUXPPC_H__

#define LWSYNC_ON_SMP
#define PPC405_ERR77(a, b)
#define ISYNC_ON_SMP


/* Adapted from atomic_add in asm-powerpc/atomic.h */
inline int32_t OSAtomicAdd32(int32_t amount, int32_t *value) {
  int t;
  __asm__ __volatile__(
"1:		lwarx   %0,0,%3         # atomic_add\n\
		add     %0,%2,%0\n"
		PPC405_ERR77(0,%3)
"		stwcx.  %0,0,%3 \n\
		bne-    1b"
		: "=&r" (t), "+m" (*value)
		: "r" (amount), "r" (value)
		: "cc");
  return *value;
}

/* Adapted from __cmpxchg_u32 in asm-powerpc/atomic.h */
inline bool OSAtomicCompareAndSwap32(int32_t old_value, int32_t new_value,
                                     int32_t *value) {
  unsigned int prev;
  __asm__ __volatile__ (
		LWSYNC_ON_SMP
"1:		lwarx   %0,0,%2         # __cmpxchg_u32\n\
		cmpw    0,%0,%3\n\
		bne-    2f\n"
		PPC405_ERR77(0,%2)
"		stwcx.  %4,0,%2\n\
		bne-    1b"
		ISYNC_ON_SMP
		"\n\
2:"
                : "=&r" (prev), "+m" (*value)
                : "r" (value), "r" (old_value), "r" (new_value)
                : "cc", "memory");
  return true;
}

/* Adapted from __cmpxchg_u32 in asm-powerpc/atomic.h */
inline int32_t OSAtomicCompareAndSwap32Barrier(int32_t old_value,
                                               int32_t new_value,
                                               int32_t *value) {
  unsigned int prev;
  __asm__ __volatile__ (
		LWSYNC_ON_SMP
"1:		lwarx   %0,0,%2         # __cmpxchg_u32\n\
		cmpw    0,%0,%3\n\
		bne-    2f\n"
		PPC405_ERR77(0,%2)
"		stwcx.  %4,0,%2\n\
		bne-    1b"
		ISYNC_ON_SMP
		"\n\
2:"
                : "=&r" (prev), "+m" (*value)
                : "r" (value), "r" (old_value), "r" (new_value)
                : "cc", "memory");
  return true;
}

inline void MemoryBarrier() {
  // TODO
}

// int32_t and intptr_t seems to be equal on ppc-linux
// therefore we have no extra Atomic32 function versions.
typedef int32_t Atomic32;
typedef intptr_t AtomicWord;

#define OSAtomicCastIntPtr(p) \
               reinterpret_cast<int32_t *>(const_cast<AtomicWord *>(p))
#define OSAtomicCompareAndSwapIntPtr OSAtomicCompareAndSwap32
#define OSAtomicAddIntPtr OSAtomicAdd32
#define OSAtomicCompareAndSwapIntPtrBarrier OSAtomicCompareAndSwap32Barrier


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
  // The ppc interface does not distinguish between Acquire and
  // Release memory barriers; they are equivalent.
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


#endif  // BASE_ATOMICOPS_INTERNALS_LINUXPPC_H__
