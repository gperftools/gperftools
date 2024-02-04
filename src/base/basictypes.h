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

#if defined(HAVE___ATTRIBUTE__) && defined(__ELF__)
# define ATTRIBUTE_VISIBILITY_HIDDEN __attribute__((visibility("hidden")))
#else
# define ATTRIBUTE_VISIBILITY_HIDDEN
#endif

// Section attributes are supported for both ELF and Mach-O, but in
// very different ways.  Here's the API we provide:
// 1) ATTRIBUTE_SECTION: put this with the declaration of all functions
//    you want to be in the same linker section
// 2) DEFINE_ATTRIBUTE_SECTION_VARS: must be called once per unique
//    name.  You want to make sure this is executed before any
//    DECLARE_ATTRIBUTE_SECTION_VARS; the easiest way is to put them
//    in the same .cc file.  Put this call at the global level.
// 3) INIT_ATTRIBUTE_SECTION_VARS: you can scatter calls to this in
//    multiple places to help ensure execution before any
//    DECLARE_ATTRIBUTE_SECTION_VARS.  You must have at least one
//    DEFINE, but you can have many INITs.  Put each in its own scope.
// 4) DECLARE_ATTRIBUTE_SECTION_VARS: must be called before using
//    ATTRIBUTE_SECTION_START or ATTRIBUTE_SECTION_STOP on a name.
//    Put this call at the global level.
// 5) ATTRIBUTE_SECTION_START/ATTRIBUTE_SECTION_STOP: call this to say
//    where in memory a given section is.  All functions declared with
//    ATTRIBUTE_SECTION are guaranteed to be between START and STOP.

#if defined(HAVE___ATTRIBUTE__) && defined(__ELF__)
# define ATTRIBUTE_SECTION(name) __attribute__ ((section (#name))) __attribute__((noinline))

  // Weak section declaration to be used as a global declaration
  // for ATTRIBUTE_SECTION_START|STOP(name) to compile and link
  // even without functions with ATTRIBUTE_SECTION(name).
# define DECLARE_ATTRIBUTE_SECTION_VARS(name) \
    extern char __start_##name[] ATTRIBUTE_WEAK; \
    extern char __stop_##name[] ATTRIBUTE_WEAK
# define INIT_ATTRIBUTE_SECTION_VARS(name)     // no-op for ELF
# define DEFINE_ATTRIBUTE_SECTION_VARS(name)   // no-op for ELF

  // Return void* pointers to start/end of a section of code with functions
  // having ATTRIBUTE_SECTION(name), or 0 if no such function exists.
  // One must DECLARE_ATTRIBUTE_SECTION(name) for this to compile and link.
# define ATTRIBUTE_SECTION_START(name) (reinterpret_cast<void*>(__start_##name))
# define ATTRIBUTE_SECTION_STOP(name) (reinterpret_cast<void*>(__stop_##name))
# define HAVE_ATTRIBUTE_SECTION_START 1

#elif defined(HAVE___ATTRIBUTE__) && defined(__MACH__)
# define ATTRIBUTE_SECTION(name) __attribute__ ((section ("__TEXT, " #name))) __attribute__((noinline))

#include <mach-o/getsect.h>
#include <mach-o/dyld.h>
class AssignAttributeStartEnd {
 public:
  AssignAttributeStartEnd(const char* name, char** pstart, char** pend) {
    // Find out what dynamic library name is defined in
    for (int i = _dyld_image_count() - 1; i >= 0; --i) {
      const mach_header* hdr = _dyld_get_image_header(i);
#ifdef MH_MAGIC_64
      if (hdr->magic == MH_MAGIC_64) {
        uint64_t len;
        *pstart = getsectdatafromheader_64((mach_header_64*)hdr,
                                           "__TEXT", name, &len);
        if (*pstart) {   // NULL if not defined in this dynamic library
          *pstart += _dyld_get_image_vmaddr_slide(i);   // correct for reloc
          *pend = *pstart + len;
          return;
        }
      }
#endif
      if (hdr->magic == MH_MAGIC) {
        uint32_t len;
        *pstart = getsectdatafromheader(hdr, "__TEXT", name, &len);
        if (*pstart) {   // NULL if not defined in this dynamic library
          *pstart += _dyld_get_image_vmaddr_slide(i);   // correct for reloc
          *pend = *pstart + len;
          return;
        }
      }
    }

    // If we get here, not defined in a dll at all.  See if defined statically.
    unsigned long len;    // don't ask me why this type isn't uint32_t too...
    *pstart = getsectdata("__TEXT", name, &len);
    *pend = *pstart + len;
  }
};

#define DECLARE_ATTRIBUTE_SECTION_VARS(name)    \
  extern char* __start_##name;                  \
  extern char* __stop_##name

#define INIT_ATTRIBUTE_SECTION_VARS(name)               \
  DECLARE_ATTRIBUTE_SECTION_VARS(name);                 \
  static const AssignAttributeStartEnd __assign_##name( \
    #name, &__start_##name, &__stop_##name)

#define DEFINE_ATTRIBUTE_SECTION_VARS(name)     \
  char* __start_##name, *__stop_##name;         \
  INIT_ATTRIBUTE_SECTION_VARS(name)

# define ATTRIBUTE_SECTION_START(name) (reinterpret_cast<void*>(__start_##name))
# define ATTRIBUTE_SECTION_STOP(name) (reinterpret_cast<void*>(__stop_##name))
# define HAVE_ATTRIBUTE_SECTION_START 1

#else  // not HAVE___ATTRIBUTE__ && __ELF__, nor HAVE___ATTRIBUTE__ && __MACH__
# define ATTRIBUTE_SECTION(name)
# define DECLARE_ATTRIBUTE_SECTION_VARS(name)
# define INIT_ATTRIBUTE_SECTION_VARS(name)
# define DEFINE_ATTRIBUTE_SECTION_VARS(name)
# define ATTRIBUTE_SECTION_START(name) (reinterpret_cast<void*>(0))
# define ATTRIBUTE_SECTION_STOP(name) (reinterpret_cast<void*>(0))

#endif  // HAVE___ATTRIBUTE__ and __ELF__ or __MACH__

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

#if defined(HAVE___ATTRIBUTE__) && defined(__ELF__)
#define ATTRIBUTE_HIDDEN __attribute__((visibility("hidden")))
#else
#define ATTRIBUTE_HIDDEN
#endif

#if defined(__GNUC__)
#define ALWAYS_INLINE inline __attribute__((always_inline))
#elif defined(_MSC_VER)
#define ALWAYS_INLINE __forceinline
#else
#define ALWAYS_INLINE inline
#endif

#endif  // _BASICTYPES_H_
