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
#include "base/spinlock.h"
#include "base/threading.h"
#include "thread_cache.h"

// This module encapsulates tcmalloc's thread cache access. Including
// fast-path access, early access (when process is too young and TLS
// facility isn't set up yet) and emergency malloc mode signalling.

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
inline constexpr bool kUseEmergencyMalloc = kHaveGoodTLS;
#else
inline constexpr bool kUseEmergencyMalloc = false;
#endif

class ThreadCachePtr {
public:
  static ThreadCache* GetFast() {
    if constexpr (kHaveGoodTLS) {
      return tls_data_.fast_path_cache;
    } else {
      if (!tls_ready_) {
        return nullptr;
      }
      return static_cast<ThreadCache*>(GetTlsValue(slow_thread_cache_key_));
    }
  }

  static void InitThreadCachePtrLate();

  static ThreadCachePtr GetSlow() {
    if constexpr (kHaveGoodTLS) {
      ThreadCache* cache = tls_data_.slow_path_cache;
      if (PREDICT_TRUE(cache != nullptr)) {
        return {cache, false};
      }
    }
    return DoGetSlow();
  }

  ThreadCachePtr(const ThreadCachePtr& other) = delete;
  ThreadCachePtr& operator=(const ThreadCachePtr& other) = delete;

  ~ThreadCachePtr() {
    if (locked_) {
      PutLocked();
    }
  }

  ThreadCache* get() const { return ptr_; }

  ThreadCache& operator*() const { return *ptr_; }
  ThreadCache* operator->() const { return ptr_; }

  // Cleans up thread's cache pointer and returns what it was. Used by
  // TCMallocImplementation::MarkThreadIdle.
  static ThreadCache* ReleaseAndClear();

private:
  static ThreadCachePtr DoGetSlow();
  static void ClearCacheTLS();
  static void PutLocked();

  ThreadCachePtr(ThreadCache* ptr, bool locked);

  struct TLSData {
    ThreadCache* fast_path_cache;
    ThreadCache* slow_path_cache;
    bool use_emergency_malloc;
  };

  static inline thread_local TLSData tls_data_;
  static bool tls_ready_;
  static TlsKey slow_thread_cache_key_;

  friend void SetUseEmergencyMalloc();
  friend void ResetUseEmergencyMalloc();
  friend bool IsUseEmergencyMalloc();

  ThreadCache* const ptr_;
  bool locked_;
};

inline void SetUseEmergencyMalloc() {
  if constexpr (kUseEmergencyMalloc) {
    auto& tls_data = ThreadCachePtr::tls_data_;
    tls_data.fast_path_cache = nullptr;
    tls_data.use_emergency_malloc = true;
  }
}

inline void ResetUseEmergencyMalloc() {
  if constexpr (kUseEmergencyMalloc) {
    auto& tls_data = ThreadCachePtr::tls_data_;
    tls_data.fast_path_cache = tls_data.slow_path_cache;
    tls_data.use_emergency_malloc = false;
  }
}

inline bool IsUseEmergencyMalloc() {
  if constexpr (kUseEmergencyMalloc) {
    return PREDICT_FALSE(ThreadCachePtr::tls_data_.use_emergency_malloc);
  } else {
    return false;
  }
}

}  // namespace tcmalloc

#endif  // THREAD_CACHE_PTR_H_
