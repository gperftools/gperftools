// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
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

#ifndef _BASICTYPES_H_
#define _BASICTYPES_H_

#include <config.h>
#include <string.h>       // for memcpy()
#include <inttypes.h>     // gets us PRId64, etc

#include <stdint.h>
#include <sys/types.h>

// Define the "portable" printf and scanf macros, if they're not
// already there (via the inttypes.h we #included above, hopefully).
// Mostly it's old systems that don't support inttypes.h, so we assume
// they're 32 bit.
#ifndef PRIx64
#define PRIx64 "llx"
#endif
#ifndef SCNx64
#define SCNx64 "llx"
#endif
#ifndef PRId64
#define PRId64 "lld"
#endif
#ifndef SCNd64
#define SCNd64 "lld"
#endif
#ifndef PRIu64
#define PRIu64 "llu"
#endif
#ifndef PRIxPTR
#define PRIxPTR "lx"
#endif

#if defined(__GNUC__)
#define PREDICT_TRUE(x) __builtin_expect(!!(x), 1)
#define PREDICT_FALSE(x) __builtin_expect(!!(x), 0)
#else
#define PREDICT_TRUE(x) (x)
#define PREDICT_FALSE(x) (x)
#endif

// A macro to disallow the evil copy constructor and operator= functions
// This should be used in the private: declarations for a class
#define DISALLOW_EVIL_CONSTRUCTORS(TypeName)    \
  TypeName(const TypeName&);                    \
  void operator=(const TypeName&)

// An alternate name that leaves out the moral judgment... :-)
#define DISALLOW_COPY_AND_ASSIGN(TypeName) DISALLOW_EVIL_CONSTRUCTORS(TypeName)

#ifdef HAVE___ATTRIBUTE__
# define ATTRIBUTE_UNUSED __attribute__((unused))
#else
# define ATTRIBUTE_UNUSED
#endif

#if defined(HAVE___ATTRIBUTE__)
#define ATTR_INITIAL_EXEC __attribute__ ((tls_model ("initial-exec")))
#else
#define ATTR_INITIAL_EXEC
#endif

#define arraysize(a)  (sizeof(a) / sizeof(*(a)))

#define OFFSETOF_MEMBER(strct, field)                                   \
   (reinterpret_cast<char*>(&reinterpret_cast<strct*>(16)->field) -     \
    reinterpret_cast<char*>(16))

// bit_cast<Dest,Source> implements the equivalent of
// "*reinterpret_cast<Dest*>(&source)".
//
// The reinterpret_cast method would produce undefined behavior
// according to ISO C++ specification section 3.10 -15 -.
// bit_cast<> calls memcpy() which is blessed by the standard,
// especially by the example in section 3.9.
//
// Fortunately memcpy() is very fast.  In optimized mode, with a
// constant size, gcc 2.95.3, gcc 4.0.1, and msvc 7.1 produce inline
// code with the minimal amount of data movement.  On a 32-bit system,
// memcpy(d,s,4) compiles to one load and one store, and memcpy(d,s,8)
// compiles to two loads and two stores.

template <class Dest, class Source>
inline Dest bit_cast(const Source& source) {
  static_assert(sizeof(Dest) == sizeof(Source), "bitcasting unequal sizes");
  Dest dest;
  memcpy(&dest, &source, sizeof(dest));
  return dest;
}

// bit_store<Dest,Source> implements the equivalent of
// "dest = *reinterpret_cast<Dest*>(&source)".
//
// This prevents undefined behavior when the dest pointer is unaligned.
template <class Dest, class Source>
inline void bit_store(Dest *dest, const Source *source) {
  static_assert(sizeof(Dest) == sizeof(Source), "bitcasting unequal sizes");
  memcpy(dest, source, sizeof(Dest));
}

#ifdef HAVE___ATTRIBUTE__
# define ATTRIBUTE_WEAK      __attribute__((weak))
# define ATTRIBUTE_NOINLINE  __attribute__((noinline))
#else
# define ATTRIBUTE_WEAK
# define ATTRIBUTE_NOINLINE
#endif

#ifdef _MSC_VER
#undef ATTRIBUTE_NOINLINE
#define ATTRIBUTE_NOINLINE __declspec(noinline)
#endif

#if defined(HAVE___ATTRIBUTE__) && defined(__ELF__) && !defined(TCMALLOC_DISABLE_HIDDEN_VISIBILITY)
# define ATTRIBUTE_VISIBILITY_HIDDEN __attribute__((visibility("hidden")))
#else
# define ATTRIBUTE_VISIBILITY_HIDDEN
#endif

#if defined(HAVE___ATTRIBUTE__)
# if (defined(__i386__) || defined(__x86_64__))
#   define CACHELINE_ALIGNED __attribute__((aligned(64)))
# elif (defined(__PPC__) || defined(__PPC64__) || defined(__ppc__) || defined(__ppc64__))
#   define CACHELINE_ALIGNED __attribute__((aligned(16)))
# elif (defined(__arm__))
#   define CACHELINE_ALIGNED __attribute__((aligned(64)))
    // some ARMs have shorter cache lines (ARM1176JZF-S is 32 bytes for example) but obviously 64-byte aligned implies 32-byte aligned
# elif (defined(__mips__))
#   define CACHELINE_ALIGNED __attribute__((aligned(128)))
# elif (defined(__aarch64__))
#   define CACHELINE_ALIGNED __attribute__((aligned(64)))
    // implementation specific, Cortex-A53 and 57 should have 64 bytes
# elif (defined(__s390__))
#   define CACHELINE_ALIGNED __attribute__((aligned(256)))
# elif (defined(__riscv) && __riscv_xlen == 64)
#   define CACHELINE_ALIGNED __attribute__((aligned(64)))
# elif defined(__loongarch64)
#   define CACHELINE_ALIGNED __attribute__((aligned(64)))
# else
#   error Could not determine cache line length - unknown architecture
# endif
#else
# define CACHELINE_ALIGNED
#endif  // defined(HAVE___ATTRIBUTE__)

#if defined(HAVE___ATTRIBUTE__ALIGNED_FN)
#  define CACHELINE_ALIGNED_FN CACHELINE_ALIGNED
#else
#  define CACHELINE_ALIGNED_FN
#endif

// Structure for discovering alignment
union MemoryAligner {
  void*  p;
  double d;
  size_t s;
} CACHELINE_ALIGNED;

#if defined(__GNUC__)
#define ALWAYS_INLINE inline __attribute__((always_inline))
#elif defined(_MSC_VER)
#define ALWAYS_INLINE __forceinline
#else
#define ALWAYS_INLINE inline
#endif

#endif  // _BASICTYPES_H_
