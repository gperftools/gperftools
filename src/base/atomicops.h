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

// Some fast atomic operations -- typically with machine-dependent
// implementations.  This file may need editing as Google code is
// ported to different architectures.

#ifndef THREAD_ATOMICOPS_H__
#define THREAD_ATOMICOPS_H__

#include "config.h"
#include <stdint.h>

// ------------------------------------------------------------------------
// Include the platform specific implementations of the types
// and operations listed below.
// TODO(csilvers): figure out ARCH_PIII/ARCH_K8 (perhaps via ./configure?)
// ------------------------------------------------------------------------

// macosx.h should work correctly for Darwin/x86 as well, but the
// x86.h version works fine as well, so we'll go with that.
// TODO(csilvers): match piii, not just __i386.  Also, match k8
#if defined(__MACH__) && defined(__APPLE__) && defined(__ppc__)
#include "base/atomicops-internals-macosx.h"
#elif defined(__GNUC__) && (defined(__i386) || defined(ARCH_K8))
#include "base/atomicops-internals-x86.h"
#elif defined(__i386) && defined(MSVC)
#include "base/atomicops-internals-x86-msvc.h"
#elif defined(__linux__) && defined(__PPC__)
#include "base/atomicops-internals-linuxppc.h"
#else
// Assume x86 for now.  If you need to support a new architecture and
// don't know how to implement atomic ops, you can probably get away
// with using pthreads, since atomicops is only used by spinlock.h/cc
//#error You need to implement atomic operations for this architecture
#include "base/atomicops-internals-x86.h"
#endif

// ------------------------------------------------------------------------
// Commented out type definitions and method declarations for documentation
// of the interface provided by this module.
// ------------------------------------------------------------------------

#if 0

// Signed type that can hold a pointer and supports the atomic ops below, as
// well as atomic loads and stores.  Instances must be naturally-aligned.
typedef intptr_t AtomicWord;

// Signed 32-bit type that supports the atomic ops below, as well as atomic
// loads and stores.  Instances must be naturally aligned.  This type differs
// from AtomicWord in 64-bit binaries where AtomicWord is 64-bits.
typedef int32_t Atomic32;

// Atomically execute:
//      result = *ptr;
//      if (*ptr == old_value)
//        *ptr = new_value;
//      return result;
//
// I.e., replace "*ptr" with "new_value" if "*ptr" used to be "old_value".
// Always return the old value of "*ptr"
//
// This routine implies no memory barriers.
AtomicWord CompareAndSwap(volatile AtomicWord* ptr,
                          AtomicWord old_value,
                          AtomicWord new_value);

// Atomically store new_value into *ptr, returning the previous value held in
// *ptr.  This routine implies no memory barriers.
AtomicWord AtomicExchange(volatile AtomicWord* ptr, AtomicWord new_value);

// Atomically increment *ptr by "increment".  Returns the new value of
// *ptr with the increment applied.  This routine implies no memory
// barriers.
AtomicWord AtomicIncrement(volatile AtomicWord* ptr, AtomicWord increment);

// ------------------------------------------------------------------------
// These following lower-level operations are typically useful only to people
// implementing higher-level synchronization operations like spinlocks,
// mutexes, and condition-variables.  They combine CompareAndSwap(), a load, or
// a store with appropriate memory-ordering instructions.  "Acquire" operations
// ensure that no later memory access can be reordered ahead of the operation.
// "Release" operations ensure that no previous memory access can be reordered
// after the operation.
// ------------------------------------------------------------------------
AtomicWord Acquire_CompareAndSwap(volatile AtomicWord* ptr,
                                  AtomicWord old_value,
                                  AtomicWord new_value);
AtomicWord Release_CompareAndSwap(volatile AtomicWord* ptr,
                                  AtomicWord old_value,
                                  AtomicWord new_value);
void Acquire_Store(volatile AtomicWord* ptr, AtomicWord value);
void Release_Store(volatile AtomicWord* ptr, AtomicWord value);
AtomicWord Acquire_Load(volatile const AtomicWord* ptr);
AtomicWord Release_Load(volatile const AtomicWord* ptr);

// Corresponding operations on Atomic32
Atomic32 CompareAndSwap(volatile Atomic32* ptr,
                        Atomic32 old_value,
                        Atomic32 new_value);
Atomic32 AtomicExchange(volatile Atomic32* ptr, Atomic32 new_value);
Atomic32 AtomicIncrement(volatile Atomic32* ptr, Atomic32 increment);
Atomic32 Acquire_CompareAndSwap(volatile Atomic32* ptr,
                                Atomic32 old_value,
                                Atomic32 new_value);
Atomic32 Release_CompareAndSwap(volatile Atomic32* ptr,
                                Atomic32 old_value,
                                Atomic32 new_value);
void Acquire_Store(volatile Atomic32* ptr, Atomic32 value);
void Release_Store(volatile Atomic32* ptr, Atomic32 value);
Atomic32 Acquire_Load(volatile const Atomic32* ptr);
Atomic32 Release_Load(volatile const Atomic32* ptr);

void MemoryBarrier();

#endif

#endif  // THREAD_ATOMICOPS_H__
