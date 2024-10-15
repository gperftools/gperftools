/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
 * Copyright (c) 2024, gperftools Contributors
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
 */
#ifndef THREAD_CACHE_PTR_H_
#define THREAD_CACHE_PTR_H_
#include "config.h"

#include "base/basictypes.h"
#include "base/function_ref.h"
#include "base/spinlock.h"
#include "base/threading.h"
#include "thread_cache.h"

// This module encapsulates tcmalloc's thread cache access. Including
// fast-path access, early access (when process is too young and TLS
// facility isn't set up yet) and emergency malloc mode signaling.

namespace tcmalloc {

// Those platforms are known to do emutls or similar for TLS
// implementation. And so, we have to be more careful especially early
// in process lifetime.
#if __QNXNTO__ || __APPLE__ || __MINGW32__ || _AIX || TCMALLOC_FORCE_BAD_TLS
inline constexpr bool kHaveGoodTLS = false;
#else
// All other platforms are assumed to be great. Known great are
// GNU/Linux (musl too, and android's bionic too, but only most recent
// versions), FreeBSD, NetBSD, Solaris, Windows (but, sadly, not with
// mingw)
inline constexpr bool kHaveGoodTLS = true;
#endif

#if defined(ENABLE_EMERGENCY_MALLOC)
inline constexpr bool kUseEmergencyMalloc = true;
#else
inline constexpr bool kUseEmergencyMalloc = false;
#endif


class ThreadCachePtr {
public:
  static bool ThreadCacheKeyIsReady() {
    return (tls_key_ != kInvalidTLSKey);
  }

  static ThreadCache* GetIfPresent() {
    if constexpr (kHaveGoodTLS) {
      return tls_data_.fast_path_cache;
    }

    if (PREDICT_FALSE(!ThreadCacheKeyIsReady())) {
      return nullptr;
    }
    return static_cast<ThreadCache*>(GetTlsValue(tls_key_));
  }

  static void InitThreadCachePtrLate();

  static ThreadCachePtr Grab() {
    ThreadCache* cache = GetIfPresent();
    if (cache) {
      return {cache, false};
    }

    return GetSlow();
  }

  bool IsEmergencyMallocEnabled() const {
    return kUseEmergencyMalloc && is_emergency_malloc_;
  }

  ThreadCache* get() const { return ptr_; }

  ThreadCache& operator*() const { return *ptr_; }
  ThreadCache* operator->() const { return ptr_; }

  // Cleans up thread's cache pointer and returns what it was. Used by
  // TCMallocImplementation::MarkThreadIdle.
  static ThreadCache* ReleaseAndClear();

  // WithStacktraceScope runs passed function with given arg enabling
  // emergency malloc around that call. If emergency malloc for
  // current thread is already in effect it passes false to
  // stacktrace_allowed argument of `fn'. See malloc_backtrace.cc for
  // it's usage.
  static void WithStacktraceScope(void (*fn)(bool stacktrace_allowed, void* arg), void* arg);

  static void WithStacktraceScope(tcmalloc::FunctionRef<void(bool)> body) {
    WithStacktraceScope(body.fn, body.data);
  }

  // For pthread_atfork handler
  static SpinLock* GetSlowTLSLock();

private:
  friend class SlowTLS;

  static ThreadCachePtr GetSlow();
  static ThreadCachePtr GetReallySlow();

  static void ClearCacheTLS();

  ThreadCachePtr(ThreadCache* ptr, bool is_emergency_malloc)
    : ptr_(ptr), is_emergency_malloc_(is_emergency_malloc) {
  }

  struct TLSData {
    ThreadCache* fast_path_cache;
  };

  static inline thread_local TLSData tls_data_ ATTR_INITIAL_EXEC;
  static TlsKey tls_key_;

  ThreadCache* const ptr_;
  const bool is_emergency_malloc_;
};


#if !defined(ENABLE_EMERGENCY_MALLOC)
// Note, the "real" implementation for ENABLE_EMERGENCY_MALLOC case is in .cc
inline ATTRIBUTE_NOINLINE
void ThreadCachePtr::WithStacktraceScope(void (*fn)(bool stacktrace_allowed, void* arg), void* arg) {
  fn(true, arg);
  // prevent tail-calling fn.
  (void)*const_cast<volatile char*>(reinterpret_cast<char *>(arg));
}
#endif // !ENABLE_EMERGENCY_MALLOC

}  // namespace tcmalloc

#endif  // THREAD_CACHE_PTR_H_
