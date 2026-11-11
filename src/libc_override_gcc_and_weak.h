// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
// Copyright (c) 2011, Google Inc.
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
// Author: Craig Silverstein <opensource@google.com>
//
// Used to override malloc routines on systems that define the
// memory allocation routines to be weak symbols in their libc
// (almost all unix-based systems are like this), on gcc, which
// suppports the 'alias' attribute.

#ifndef TCMALLOC_LIBC_OVERRIDE_GCC_AND_WEAK_INL_H_
#define TCMALLOC_LIBC_OVERRIDE_GCC_AND_WEAK_INL_H_

#ifdef HAVE_SYS_CDEFS_H
#include <sys/cdefs.h>  // for __THROW
#endif
#include <gperftools/tcmalloc.h>

#ifndef __THROW  // I guess we're not on a glibc-like system
#define __THROW  // __THROW is just an optimization, so ok to make it ""
#endif

#ifndef __GNUC__
#error libc_override_gcc_and_weak.h is for gcc distributions only.
#endif

#define ALIAS(tc_fn) __attribute__((alias(#tc_fn), used)) PERFTOOLS_DLL_DECL

void* operator new(size_t size) CPP_BADALLOC ALIAS(tc_new);
void operator delete(void* p) CPP_NOTHROW ALIAS(tc_delete);
void* operator new[](size_t size) CPP_BADALLOC ALIAS(tc_newarray);
void operator delete[](void* p) CPP_NOTHROW ALIAS(tc_deletearray);
void* operator new(size_t size, const std::nothrow_t& nt) CPP_NOTHROW ALIAS(tc_new_nothrow);
void* operator new[](size_t size, const std::nothrow_t& nt) CPP_NOTHROW ALIAS(tc_newarray_nothrow);
void operator delete(void* p, const std::nothrow_t& nt) CPP_NOTHROW ALIAS(tc_delete_nothrow);
void operator delete[](void* p, const std::nothrow_t& nt) CPP_NOTHROW ALIAS(tc_deletearray_nothrow);

void operator delete(void* p, size_t size) CPP_NOTHROW ALIAS(tc_delete_sized);
void operator delete[](void* p, size_t size) CPP_NOTHROW ALIAS(tc_deletearray_sized);

void* operator new(size_t size, std::align_val_t al) ALIAS(tc_new_aligned);
void operator delete(void* p, std::align_val_t al) CPP_NOTHROW ALIAS(tc_delete_aligned);
void* operator new[](size_t size, std::align_val_t al) ALIAS(tc_newarray_aligned);
void operator delete[](void* p, std::align_val_t al) CPP_NOTHROW ALIAS(tc_deletearray_aligned);
void* operator new(size_t size, std::align_val_t al, const std::nothrow_t& nt) CPP_NOTHROW
    ALIAS(tc_new_aligned_nothrow);
void* operator new[](size_t size, std::align_val_t al, const std::nothrow_t& nt) CPP_NOTHROW
    ALIAS(tc_newarray_aligned_nothrow);
void operator delete(void* p, std::align_val_t al, const std::nothrow_t& nt) CPP_NOTHROW
    ALIAS(tc_delete_aligned_nothrow);
void operator delete[](void* p, std::align_val_t al, const std::nothrow_t& nt) CPP_NOTHROW
    ALIAS(tc_deletearray_aligned_nothrow);

void operator delete(void* p, size_t size, std::align_val_t al) CPP_NOTHROW ALIAS(tc_delete_sized_aligned);
void operator delete[](void* p, size_t size, std::align_val_t al) CPP_NOTHROW ALIAS(tc_deletearray_sized_aligned);

extern "C" {
void* malloc(size_t size) __THROW ALIAS(tc_malloc);
void free(void* ptr) __THROW ALIAS(tc_free);
void* realloc(void* ptr, size_t size) __THROW ALIAS(tc_realloc);
void* calloc(size_t n, size_t size) __THROW ALIAS(tc_calloc);
#if __QNXNTO__
// QNX has crazy cfree declaration
int cfree(void* ptr) {
  tc_cfree(ptr);
  return 0;
}
#else
void cfree(void* ptr) __THROW ALIAS(tc_cfree);
#endif
void* memalign(size_t align, size_t s) __THROW ALIAS(tc_memalign);
void* aligned_alloc(size_t align, size_t s) __THROW ALIAS(tc_memalign);
void* valloc(size_t size) __THROW ALIAS(tc_valloc);
void* pvalloc(size_t size) __THROW ALIAS(tc_pvalloc);
int posix_memalign(void** r, size_t a, size_t s) __THROW ALIAS(tc_posix_memalign);
#ifndef __UCLIBC__
void malloc_stats(void) __THROW ALIAS(tc_malloc_stats);
#endif
#if __QNXNTO__
int mallopt(int, intptr_t) ALIAS(tc_mallopt);
#else
int mallopt(int cmd, int value) __THROW ALIAS(tc_mallopt);
#endif
#ifdef HAVE_STRUCT_MALLINFO
struct mallinfo mallinfo(void) __THROW ALIAS(tc_mallinfo);
#endif
#ifdef HAVE_STRUCT_MALLINFO2
struct mallinfo2 mallinfo2(void) __THROW ALIAS(tc_mallinfo2);
#endif
size_t malloc_size(void* p) __THROW ALIAS(tc_malloc_size);
#if defined(__ANDROID__)
size_t malloc_usable_size(const void* p) __THROW ALIAS(tc_malloc_size);
#else
size_t malloc_usable_size(void* p) __THROW ALIAS(tc_malloc_size);
#endif

// C23
void free_sized(void* p, size_t size) __THROW ALIAS(tc_free_sized);
void free_aligned_sized(void* p, size_t align, size_t size) __THROW ALIAS(tc_free_aligned_sized);
}  // extern "C"

/* AIX User-defined malloc replacement interface overrides */
#if defined(_AIX)
#include "libc_override_aix.h"
#endif

#undef ALIAS

// No need to do anything at tcmalloc-registration time: we do it all
// via overriding weak symbols (at link time).
static void ReplaceSystemAlloc() {}

#endif  // TCMALLOC_LIBC_OVERRIDE_GCC_AND_WEAK_INL_H_
