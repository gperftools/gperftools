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

constexpr inline TlsKey kInvalidTLSKey = TLS_OUT_OF_INDEXES;

ATTRIBUTE_VISIBILITY_HIDDEN TlsKey WinTlsKeyCreate(void (*destr_fn)(void*));  /* windows/port.cc */

ATTRIBUTE_VISIBILITY_HIDDEN inline int CreateTlsKey(TlsKey *pkey, void (*destructor)(void*)) {
  TlsKey key = WinTlsKeyCreate(destructor);

  if (key == TLS_OUT_OF_INDEXES) {
    return GetLastError();
  }

  *(pkey) = key;
  return 0;
}

ATTRIBUTE_VISIBILITY_HIDDEN inline void* GetTlsValue(TlsKey key) {
  return TlsGetValue(key);
}

ATTRIBUTE_VISIBILITY_HIDDEN inline int SetTlsValue(TlsKey key, const void* value) {
  return !TlsSetValue(key, (LPVOID)value);
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

// I've checked several implementations and they're all implementing
// pthread_key_t as some kind of integer type. Sometimes signed,
// sometimes unsigned, and occasionally of different width
// (basically, int or long).
//
// Notably, however, POSIX is explicitly _not_ requiring
// pthread_key_t to be of some integer type. So we're somewhat into
// non-portable territory here. But in practice we should be okay,
// not just given current practice, but also keeping in mind how to
// "reasonably" implement thread-specific values. Some applies, sadly, to
// C11 tss_t type.
//
// Another potentially tricky aspect is what values to consider
// invalid. POSIX also says nothing about this, sadly. Solaris has
// nonportable PTHREAD_ONCE_KEY_NP, which would be sensible to have
// in standard (even without pthread_key_create_once_np), but we
// have what we have. It's value is (int)(-1). And, indeed, -1 seems
// like most sensible value.
constexpr inline TlsKey kInvalidTLSKey = static_cast<TlsKey>(~uintptr_t{0});
static_assert(sizeof(pthread_key_t) <= sizeof(uintptr_t));

ATTRIBUTE_VISIBILITY_HIDDEN inline int CreateTlsKey(TlsKey *pkey, void (*destructor)(void*)) {
  int err = pthread_key_create(pkey, destructor);
  if (err != 0) {
    return err;
  }
  // It is super-implausible that we'll be able to create "invalid"
  // tls key value, but just in case, we check and re-create if we end
  // up getting one.
  if (*pkey != kInvalidTLSKey) {
    return 0;
  }

  return CreateTlsKey(pkey, destructor);
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
