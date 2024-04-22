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
#include <thread>

#ifdef _WIN32 // Should cover both toolchains on Windows - MSVC & MINGW

namespace tcmalloc {

using TlsKey = DWORD;

ATTRIBUTE_VISIBILITY_HIDDEN TlsKey WinTlsKeyCreate(void (*destr_fn)(void*));  /* windows/port.cc */

ATTRIBUTE_VISIBILITY_HIDDEN inline int CreateTlsKey(TlsKey *pkey, void (*destructor)(void*)) {
  TlsKey key = WinTlsKeyCreate(destructor);

  if (key != TLS_OUT_OF_INDEXES) {
    *(pkey) = key;
    return 0;
  }
  else {
    return GetLastError();
  }
}
ATTRIBUTE_VISIBILITY_HIDDEN inline void* GetTlsValue(TlsKey key) {
  DWORD err = GetLastError();
  void* rv = TlsGetValue(key);

  if (err) SetLastError(err);
  return rv;
}
ATTRIBUTE_VISIBILITY_HIDDEN inline int SetTlsValue(TlsKey key, const void* value) {
  if (TlsSetValue(key, (LPVOID)value)) {
    return 0;
  }
  else {
    return GetLastError();
  }
}

ATTRIBUTE_VISIBILITY_HIDDEN inline uintptr_t SelfThreadId() {
  // Notably, windows/ms crt errno access recurses into malloc (!), so
  // we have to do this. But this is fast and good enough.
  return static_cast<uintptr_t>(GetCurrentThreadId());
}


} // namespace tcmalloc

#else

#include <errno.h>
#include <pthread.h>

namespace tcmalloc {

using TlsKey = pthread_key_t;

ATTRIBUTE_VISIBILITY_HIDDEN inline int CreateTlsKey(TlsKey *pkey, void (*destructor)(void*)) {
  return pthread_key_create(pkey, destructor);
}
ATTRIBUTE_VISIBILITY_HIDDEN inline void* GetTlsValue(TlsKey key) {
  return pthread_getspecific(key);
}
ATTRIBUTE_VISIBILITY_HIDDEN inline int SetTlsValue(TlsKey key, const void* value) {
  return pthread_setspecific(key, value);
}
ATTRIBUTE_VISIBILITY_HIDDEN inline uintptr_t SelfThreadId() {
  // Most platforms (with notable exception of windows C runtime) can
  // use address of errno as a quick and portable and recursion-free
  // thread identifier.
  return reinterpret_cast<uintptr_t>(&errno);
}

} // namespace tcmalloc

#endif

#endif // THREADING_H_
