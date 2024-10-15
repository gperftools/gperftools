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

#include "common.h"
#include "malloc_backtrace.h"
#include "static_vars.h"

namespace tcmalloc {

/* static */
TlsKey ThreadCachePtr::tls_key_ = kInvalidTLSKey;

// SlowTLS implements slow-but-safe thread-local facility. It maps
// threads to pairs of ThreadCache and emergency malloc mode flag.
//
// We use it in places where we cannot safely use "normal" TLS
// facility (due to recursion-into-malloc concerns).  Strictly
// speaking, it is only necessary for !kHaveGoodTLS systems. But since
// we want to avoid too much divergency between those 2 classes of
// systems, we also have even good-tls systems use this facility.
//
// We use it for early stage of process lifetime (before we're sure it
// is safe to initialize pthread_{set,get}specific key). We also use
// early in thread's thread cache initialization around the call to
// pthread_setspecific (which in some implementations occasionally
// recurses back to malloc). And we use it for StacktraceScope
// lifetimes to signal emergency malloc mode.
//
// The implementation uses small fixed-size hash table keyed by
// SelfThreadId into Entry structs which contain pointer and bool.
class SlowTLS {
public:
  struct Entry {
    ThreadCache* const cache;
    bool emergency_malloc{};
    bool was_allocated{};

    explicit Entry(ThreadCache* cache) : cache(cache) {}

    Entry(const Entry&) = delete;
    Entry& operator=(const Entry&) = delete;

    void DebugDirty() {
#ifndef NDEBUG
      memset(this, 0xff, sizeof(*this));
#endif
    }

  private:
    friend class SlowTLS;
    uintptr_t thread_id;
    Entry* next;
    Entry** prev;
  };

  class Result {
  public:
    bool Found() const {
      return entry_ != nullptr;
    }
    bool IsEmergencyMalloc() const {
      return entry_->emergency_malloc;
    }
    ThreadCache* GetCache() const {
      return entry_->cache;
    }
    Entry* GetEntry() const {
      return entry_;
    }
  private:
    Result(uintptr_t thread_id, Entry** ht_place, Entry* entry)
      : thread_id_(thread_id), ht_place_(ht_place), entry_(entry) {}

    friend class SlowTLS;

    const uintptr_t thread_id_;
    Entry** const ht_place_;
    Entry* const entry_;
  };

  static Result Lookup() {
    uintptr_t thread_id;
#if defined(__FreeBSD__) || defined(__NetBSD__)
    const bool kIsBSD = true;
#else
    const bool kIsBSD = false;
#endif
    if constexpr (kHaveGoodTLS || kIsBSD) {
      // SelfThreadId is a working, mostly portable and recursion-free
      // thread identifier.
      //
      // However, on FreeBSD and NetBSD thread's errno location for
      // initial thread changes early during process
      // initialization. As runtime facility "switches" from single
      // threaded "mode" to multi-threaded. IMHO a tiny mistake on
      // their part, it adds small overhead too. Outcome is, if we use
      // errno location, we then "leak" very first thread cache
      // instance. Not a disaster, but not great. And
      // thread_dealloc_unittest catches this too. So lets fix it and
      // have tests pass. Other OSes might do the same, but I checked
      // opensolaris, osex, all Linux libc-s, they're all good.
      //
      // Both those BSDs have great Elf-based TLS which also covers
      // this early usage case too. And since it is faster too (no
      // need to call __error or __errno_location), lets use it on all
      // "good-TLS" platforms. We already have tls_data_, so lets use
      // it's address.
      thread_id = reinterpret_cast<uintptr_t>(&ThreadCachePtr::tls_data_);
    } else {
      thread_id = SelfThreadId();
    }
    Entry** ht_place = &hash_table_[std::hash<uintptr_t>{}(thread_id) % kTableSize];

    SpinLockHolder h(&lock_);

    for (Entry* entry = *ht_place; entry != nullptr; entry = entry->next) {
      if (entry->thread_id == thread_id) {
        return Result{thread_id, ht_place, entry};
      }
    }

    return Result{thread_id, ht_place, nullptr};
  }

  static ThreadCache* TryToReleaseCacheFromAllocation(Result* result) {
    Entry* entry = result->entry_;

    // GetSlow deals with emergency_malloc case before it calling us.
    ASSERT(!entry->emergency_malloc);

    if (PREDICT_FALSE(entry->was_allocated) && ThreadCachePtr::ThreadCacheKeyIsReady()) {
      ThreadCache* cache = entry->cache;

      SlowTLS::UnregisterEntry(entry);

      return cache;
    }

    return nullptr;
  }

  static void RegisterEntry(Result* result, Entry* entry) {
    entry->thread_id = result->thread_id_;
    entry->prev = result->ht_place_;

    SpinLockHolder h(&lock_);

    Entry* next = entry->next = *result->ht_place_;
    if (next) {
      ASSERT(next->prev == result->ht_place_);
      next->prev = &entry->next;
    }
    *result->ht_place_ = entry;
  }

  static void UnregisterEntry(Entry* entry) {
    SpinLockHolder h(&lock_);
    ASSERT(*entry->prev == entry);
    Entry* next = *entry->prev = entry->next;
    if (next) {
      ASSERT(next->prev == &entry->next);
      next->prev = entry->prev;
    }
    entry->DebugDirty();
  }

  static SpinLock* GetLock() { return &lock_; }

private:
  static constexpr inline int kTableSize = 257;
  static inline Entry* hash_table_[kTableSize];
  static inline SpinLock lock_;
};

/* static */
ThreadCachePtr ThreadCachePtr::GetSlow() {
  // We're being called after GetIfPresent found no cache in normal
  // TLS storage.
  ASSERT(GetIfPresent() == nullptr);

  SlowTLS::Result tr = SlowTLS::Lookup();

  ThreadCache* cache;

  if (tr.Found()) {
    if (tr.IsEmergencyMalloc()) {
      return {nullptr, true};
    }

    // We found TLS entry with our cache. Lets check if we want try
    // convert this cache from pre-tls-ready mode to proper one.
    cache = SlowTLS::TryToReleaseCacheFromAllocation(&tr);
    if (cache == nullptr) {
      // If not, then we return the cache we got in the entry. This
      // must be thread cache instance being set inside ongoing
      // SetTlsValue.
      return {tr.GetCache(), false};
    }
  } else {
    if (!ThreadCacheKeyIsReady()) {
      return GetReallySlow();
    }
    // We're sure that everything is initialized enough to not just
    // create new ThreadCache instance, but to set it into TLS
    // storage.
    cache = ThreadCache::NewHeap();
  }

  SlowTLS::Entry registration{cache};

  // Register our newly created (or extracted from
  // TryToReleaseCacheFromAllocation) cache instance in slow
  // storage. So that if SetTlsValue below recurses back into malloc,
  // we're able to find it and avoid more SetTlsValue
  // recursion.
  SlowTLS::RegisterEntry(&tr, &registration);

  SetTlsValue(tls_key_, cache);

  SlowTLS::UnregisterEntry(&registration);

  // Note, we could set it before SetTlsValue above and actually
  // prevent any risk of SetTlsValue recursion. But since we want to
  // ensure test coverage for somewhat less common !kHaveGoodTLS
  // systems, lets have "good" systems run the "bad systems'" logic
  // too. For test coverage. Very slight performance hit for of the
  // SlowTLS registration for newly created threads we can afford.
  if constexpr (kHaveGoodTLS) {
    tls_data_.fast_path_cache = cache;
  }

  return {cache, false};
}

/* static */ ATTRIBUTE_NOINLINE
ThreadCachePtr ThreadCachePtr::GetReallySlow() {
  // This is called after we found no cache in regular TLS storage and
  // that TLS storage key isn't set up yet. I.e. process is running,
  // but not all C++ initializers (in this specific case,
  // InitThreadCachePtrLate) ran yet.
  //
  // Not just only that, but we might be dealing with entirely
  // uninitialized malloc. So we handle that first.
  ThreadCache::InitModule();

  // InitModule does some locking (and super-unlikely, but not
  // impossibly, some sleeping). Also it runs some malloc as well
  // (e.g. for pthread_atfork). So here we might actually find
  // thread's cache to be present.

  SlowTLS::Result tr = SlowTLS::Lookup();

  if (tr.Found()) {
    return {tr.GetCache(), tr.IsEmergencyMalloc()};
  }

  ThreadCache* cache = ThreadCache::NewHeap();

  // Note, we allocate slow tls registration and "leak" it. We expect
  // just single thread (initial thread) in most common cases. And
  // maybe, very rarely several. So leaking a little memory is totally
  // harmless. After all, it is our general approach to never free
  // metadata allocations. Plus, those threads that are either initial
  // thread or are allocated before program's main() tend to live
  // forever anyways.
  void* memory = MetaDataAlloc(sizeof(SlowTLS::Entry));
  SlowTLS::Entry* allocated_registration = new (memory) SlowTLS::Entry{cache};
  allocated_registration->was_allocated = true;

  SlowTLS::RegisterEntry(&tr, allocated_registration);

  return {cache, false};
}

/* static */
void ThreadCachePtr::ClearCacheTLS() {
  if constexpr (kHaveGoodTLS) {
    tls_data_.fast_path_cache = nullptr;
  }
}

/* static */
ThreadCache* ThreadCachePtr::ReleaseAndClear() {
  ThreadCache* cache = GetIfPresent();

  if (cache) {
    ClearCacheTLS();
    SetTlsValue(tls_key_, nullptr);
  }
  return cache;
}

/* static */
void ThreadCachePtr::InitThreadCachePtrLate() {
  ASSERT(tls_key_ == kInvalidTLSKey);

  ThreadCache::InitModule();

#if !defined(NDEBUG) && defined(__GLIBC__)
  if constexpr (!kHaveGoodTLS) {
    // Lets force glibc to exercise SetTlsValue recursion case (in
    // GetSlow) in debug mode. For test coverage.
    TlsKey leaked;
    for (int i = 32; i > 0; i--) {
      CreateTlsKey(&leaked, nullptr);
    }
  }
#endif // !NDEBUG

  // NOTE: creating tls key is likely to recurse into malloc. So this
  // is "late" initialization. And we must not mark tls initialized
  // until this is complete.
  int err = CreateTlsKey(&tls_key_, +[] (void *ptr) -> void {
    ClearCacheTLS();

    ThreadCache::DeleteCache(static_cast<ThreadCache*>(ptr));
  });
  CHECK(err == 0);
}

SpinLock* ThreadCachePtr::GetSlowTLSLock() {
  return SlowTLS::GetLock();
}

#if defined(ENABLE_EMERGENCY_MALLOC)

/* static */ ATTRIBUTE_NOINLINE
void ThreadCachePtr::WithStacktraceScope(void (*fn)(bool stacktrace_allowed, void* arg), void* arg) {
  SlowTLS::Result tr = SlowTLS::Lookup();

  SlowTLS::Entry* entry = tr.GetEntry();
  if (entry) {
    if (entry->emergency_malloc) {
      return fn(false, arg);
    }

    ASSERT(GetIfPresent() == nullptr);

    // We have existing entry. Likely "was_allocated" entry. We just
    // mark emergency_malloc in the entry for the duration of the
    // call.
    //
    // Also note, that emergency_malloc Entry cannot be "released" by
    // GetSlow logic (we check emergency malloc mode first).
    entry->emergency_malloc = true;
    fn(true, arg);
    entry->emergency_malloc = false;

    return;
  }

  // If there is currently active ThreadCache for this thread, lets
  // make sure we capture it in our registration.
  SlowTLS::Entry registration{GetIfPresent()};
  registration.emergency_malloc = true;

  SlowTLS::RegisterEntry(&tr, &registration);

  if (registration.cache != nullptr) {
    // Holds iff we don't touch fast_path_cache until tls is ready as
    // currently written.
    ASSERT(ThreadCacheKeyIsReady());
    if constexpr (kHaveGoodTLS) {
      tls_data_.fast_path_cache = nullptr;
    }
    SetTlsValue(tls_key_, nullptr);
  }

  fn(true, arg);

  if (registration.cache != nullptr) {
    SetTlsValue(tls_key_, registration.cache);
    if constexpr (kHaveGoodTLS) {
      tls_data_.fast_path_cache = registration.cache;
    }
  }
  SlowTLS::UnregisterEntry(&registration);
}

#endif  // ENABLE_EMERGENCY_MALLOC

}  // namespace tcmalloc
