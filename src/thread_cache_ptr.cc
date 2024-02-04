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
#include "thread_cache_ptr.h"

#include <type_traits>
#include <cstddef>

#include <errno.h>

#include "static_vars.h"

namespace tcmalloc {

/* static */
bool ThreadCachePtr::tls_ready_;
/* static */
TlsKey ThreadCachePtr::slow_thread_cache_key_;

namespace {

SpinLock init_cache_lock;
bool init_cache_ready;

ThreadCache* GetInitCache() {
  alignas(ThreadCache) static std::byte init_cache_storage[sizeof(ThreadCache)];

  return reinterpret_cast<ThreadCache*>(init_cache_storage);
}

}  // namespace

// This is only used for !kHaveGoodTLS. Extra special care is required
// to deal with ThreadCache while it is being assigned to TLS.
inline ThreadCache* CreateBadTLSCache(const TlsKey& slow_thread_cache_key) {
  struct HashEntry {
    HashEntry** prev;
    HashEntry* next;
    uintptr_t thread_id;
    ThreadCache* cache;
  };

  static constexpr int kTableSize = 257;
  static HashEntry* in_progress[kTableSize];
  static SpinLock lock;

  // We use errno address as thread id. Works on windows including
  // mingw without recursing back into malloc.
  uintptr_t thread_id = reinterpret_cast<uintptr_t>(&errno);

  HashEntry** parent = &in_progress[std::hash<uintptr_t>{}(thread_id) % kTableSize];

  {
    SpinLockHolder h(&lock);
    for (HashEntry* entry = *parent; entry != nullptr; entry = entry->next) {
      if (entry->thread_id == thread_id) {
        // We are recursing from ongoing TlsSetValue
        return entry->cache;
      }
    }
  }

  HashEntry my_entry;
  my_entry.prev = parent;
  my_entry.thread_id = thread_id;
  my_entry.cache = ThreadCache::NewHeap();

  {
    SpinLockHolder h(&lock);
    HashEntry* next = *parent;
    if (next) {
      next->prev = &my_entry.next;
    }
    my_entry.next = next;
    *parent = &my_entry;
  }

  tcmalloc::SetTlsValue(slow_thread_cache_key, my_entry.cache);

  {
    SpinLockHolder h(&lock);
    *my_entry.prev = my_entry.next;
  }

  return my_entry.cache;
}

ThreadCachePtr::ThreadCachePtr(ThreadCache* ptr, bool locked)
  : ptr_(ptr), locked_(locked) {}

/* static */
ThreadCachePtr ThreadCachePtr::DoGetSlow() NO_THREAD_SAFETY_ANALYSIS {
  if (PREDICT_TRUE(tls_ready_)) {
    ThreadCache* cache;

    if constexpr (kHaveGoodTLS) {
      cache = tls_data_.slow_path_cache;
    } else {
      cache = GetFast();
    }

    if (PREDICT_FALSE(!cache)) {
      if constexpr (kHaveGoodTLS) {
        cache = ThreadCache::NewHeap();

        // We do it first, in case SetTlsValue recurses back to malloc
        tls_data_.slow_path_cache = cache;
        tls_data_.fast_path_cache = cache;

        tcmalloc::SetTlsValue(slow_thread_cache_key_, cache);
      } else {
        cache = CreateBadTLSCache(slow_thread_cache_key_);
      }
    }

    return ThreadCachePtr{cache, false};
  }

  ThreadCache::InitModule();

  init_cache_lock.Lock();

  if (!init_cache_ready) {
    SpinLockHolder h(Static::pageheap_lock());

    new (GetInitCache()) ThreadCache();
    init_cache_ready = true;
  }

  return ThreadCachePtr{GetInitCache(), true};
}

/* static */
void ThreadCachePtr::PutLocked() NO_THREAD_SAFETY_ANALYSIS {
  init_cache_lock.Unlock();
}

/* static */
void ThreadCachePtr::ClearCacheTLS() {
  if constexpr (kHaveGoodTLS) {
    tls_data_.slow_path_cache = nullptr;
    tls_data_.fast_path_cache = nullptr;
  }
}

/* static */
ThreadCache* ThreadCachePtr::ReleaseAndClear() {
  if (!tls_ready_) {
    return nullptr;
  }

  ThreadCache* cache;
  if constexpr (kHaveGoodTLS) {
    cache = tls_data_.slow_path_cache;
  } else {
    cache = GetFast();
  }

  if (cache) {
    ClearCacheTLS();
    SetTlsValue(slow_thread_cache_key_, nullptr);
  }
  return cache;
}

/* static */
void ThreadCachePtr::InitThreadCachePtrLate() {
  ASSERT(!tls_ready_);

  ThreadCache::InitModule();

#if !defined(NDEBUG) && defined(__GLIBC__)
  if constexpr (!kHaveGoodTLS) {
    // Lets force glibc to exercise CreateBadTLSCache recursion case
    // in debug mode. For test coverage.
    TlsKey leaked;
    for (int i = 32; i > 0; i--) {
      CreateTlsKey(&leaked, nullptr);
    }
  }
#endif // !NDEBUG

  // NOTE: creating tls key is likely to recurse into malloc. So this
  // is "late" initialization. And we must not mark tls initialized
  // until this is complete.
  int err = CreateTlsKey(&slow_thread_cache_key_, +[] (void *ptr) -> void {
    ClearCacheTLS();

    ThreadCache::DeleteCache(static_cast<ThreadCache*>(ptr));
  });
  CHECK(err == 0);


  // Now everything is ready. Note, we do simple single-threaded flag
  // variable, because we assume that everything is single threaded
  // until now.
  tls_ready_ = true;

  // And lets cleanup init cache
  {
    SpinLockHolder h(&init_cache_lock);
    {
      SpinLockHolder h2(Static::pageheap_lock());
      init_cache_ready = false;
    }

    GetInitCache()->~ThreadCache();
  }
}

}  // namespace tcmalloc
