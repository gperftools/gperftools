// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
// Copyright (c) 2024, gperftools Contributors
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

#ifndef THREADING_H_
#define THREADING_H_

#include <config.h>
#include "base/basictypes.h"

// Also allow for printing of a PerftoolsThreadID.
#define GPRIuPTHREAD "lu"
#define GPRIxPTHREAD "lx"

#if defined(__CYGWIN__) || defined(__CYGWIN32__) || defined(__APPLE__) || defined(__FreeBSD__)
#  define PRINTABLE_PTHREAD(pthreadt) reinterpret_cast<uintptr_t>(pthreadt)
#elif defined(__QNXNTO__)
#  define PRINTABLE_PTHREAD(pthreadt) static_cast<intptr_t>(pthreadt)
#else
#  define PRINTABLE_PTHREAD(pthreadt) pthreadt
#endif

#ifdef _WIN32 // Should cover both toolchains on Windows - MSVC & MINGW

using PerftoolsThreadID = DWORD;
using PerftoolsTlsKey = DWORD;

inline PerftoolsThreadID PerftoolsGetThreadID() {
  return GetCurrentThreadId();
}
inline int PerftoolsThreadIDEquals(PerftoolsThreadID left, PerftoolsThreadID right) {
  return left == right;
}

extern "C" PerftoolsTlsKey PthreadKeyCreate(void (*destr_fn)(void*));  /* port.cc */

inline int PerftoolsCreateTlsKey(PerftoolsTlsKey *pkey, void (*destructor)(void*)) {
  PerftoolsTlsKey key = PthreadKeyCreate(destructor);

  if (key != TLS_OUT_OF_INDEXES) {
    *(pkey) = key;
    return 0;
  }
  else {
    return GetLastError();
  }
}
inline void* PerftoolsGetTlsValue(PerftoolsTlsKey key) {
  DWORD err = GetLastError();
  void* rv = TlsGetValue(key);

  if (err) SetLastError(err);
  return rv;
}
inline int PerftoolsSetTlsValue(PerftoolsTlsKey key, const void* value) {
  if (TlsSetValue(key, (LPVOID)value)) {
    return 0;
  }
  else {
    return GetLastError();
  }
}

inline void PerftoolsYield() {
  Sleep(0);
}

#elif defined(HAVE_PTHREAD)

#  include <pthread.h>
#  include <sched.h>

using PerftoolsThreadID = pthread_t;
using PerftoolsTlsKey = pthread_key_t;

inline PerftoolsThreadID PerftoolsGetThreadID() {
  return pthread_self();
}
inline int PerftoolsThreadIDEquals(PerftoolsThreadID left, PerftoolsThreadID right) {
  return pthread_equal(left, right);
}

inline int PerftoolsCreateTlsKey(PerftoolsTlsKey *pkey, void (*destructor)(void*)) {
  return pthread_key_create(pkey, destructor);
}
inline void* PerftoolsGetTlsValue(PerftoolsTlsKey key) {
  return pthread_getspecific(key);
}
inline int PerftoolsSetTlsValue(PerftoolsTlsKey key, const void* value) {
  return pthread_setspecific(key, value);
}

inline ATTRIBUTE_ALWAYS_INLINE void PerftoolsYield() {
  sched_yield();
}

#else
#  error "Threading support is now mandatory"
#endif

#endif // THREADING_H_