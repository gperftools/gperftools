// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
// Copyright (c) 2008, Google Inc.
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
// Author: Ken Ashcraft <opensource@google.com>

#include <config.h>

#include "thread_cache.h"

#include <algorithm>                    // for max, min

#include <errno.h>
#include <string.h>                     // for memcpy

#include "base/commandlineflags.h"      // for SpinLockHolder
#include "base/spinlock.h"              // for SpinLockHolder
#include "central_freelist.h"
#include "getenv_safe.h"                // for TCMallocGetenvSafe
#include "tcmalloc_internal.h"
#include "thread_cache_ptr.h"

using std::min;
using std::max;

// Note: this is initialized manually in InitModule to ensure that
// it's configured at right time
//
// DEFINE_int64(tcmalloc_max_total_thread_cache_bytes,
//              EnvToInt64("TCMALLOC_MAX_TOTAL_THREAD_CACHE_BYTES",
//                         kDefaultOverallThreadCacheSize),
//              "Bound on the total amount of bytes allocated to "
//              "thread caches. This bound is not strict, so it is possible "
//              "for the cache to go over this bound in certain circumstances. "
//              "Maximum value of this flag is capped to 1 GB.");


namespace tcmalloc {

static bool phinited = false;

volatile size_t ThreadCache::per_thread_cache_size_ = kMaxThreadCacheSize;

std::atomic<size_t> ThreadCache::min_per_thread_cache_size_ = kMinThreadCacheSize;
size_t ThreadCache::overall_thread_cache_size_ = kDefaultOverallThreadCacheSize;
size_t ThreadCache::initial_thread_cache_size_ = kStealAmount;
bool ThreadCache::use_batch_size_from_start_ = false;
ssize_t ThreadCache::unclaimed_cache_space_ = kDefaultOverallThreadCacheSize;
PageHeapAllocator<ThreadCache> threadcache_allocator;
ThreadCache* ThreadCache::thread_heaps_ = NULL;
int ThreadCache::thread_heap_count_ = 0;
ThreadCache* ThreadCache::next_memory_steal_ = NULL;

ThreadCache::ThreadCache() {
  ASSERT(Static::pageheap_lock()->IsHeld());

  size_ = 0;

  max_size_ = 0;
  SetInitialLimitLocked();
  if (max_size_ == 0) {
    // There isn't enough memory to go around.  Just give the minimum to
    // this thread.
    size_t min_size = min_per_thread_cache_size_.load(std::memory_order_relaxed);
    SetMaxSize(min_size);

    // Take unclaimed_cache_space_ negative.
    unclaimed_cache_space_ -= min_size;
    ASSERT(unclaimed_cache_space_ < 0);
  }

  next_ = nullptr;
  prev_ = nullptr;
  if (!use_batch_size_from_start_)
    for (uint32_t cl = 0; cl < Static::num_size_classes(); ++cl) {
      list_[cl].Init(Static::sizemap()->class_to_size(cl));
    }
  else
    for (uint32_t cl = 0; cl < Static::num_size_classes(); ++cl) {
      list_[cl].Init(Static::sizemap()->class_to_size(cl));
      list_[cl].set_max_length(Static::sizemap()->num_objects_to_move(cl));
    }

  uintptr_t sampler_seed;
  uintptr_t addr = reinterpret_cast<uintptr_t>(&sampler_seed);
  sampler_seed = addr;

  sampler_.Init(uint64_t{sampler_seed});
}

ThreadCache::~ThreadCache() {
  // Put unused memory back into central cache
  for (uint32_t cl = 0; cl < Static::num_size_classes(); ++cl) {
    if (list_[cl].length() > 0) {
      ReleaseToCentralCache(&list_[cl], cl, list_[cl].length());
    }
  }
}

// Remove some objects of class "cl" from central cache and add to thread heap.
// On success, return the first object for immediate use; otherwise return NULL.
void* ThreadCache::FetchFromCentralCache(uint32_t cl, int32_t byte_size,
                                         void *(*oom_handler)(size_t size)) {
  FreeList* list = &list_[cl];
  ASSERT(list->empty());
  const int batch_size = Static::sizemap()->num_objects_to_move(cl);

  const int num_to_move = min<int>(list->max_length(), batch_size);
  void *start, *end;
  int fetch_count = Static::central_cache()[cl].RemoveRange(
      &start, &end, num_to_move);

  if (fetch_count == 0) {
    ASSERT(start == NULL);
    return oom_handler(byte_size);
  }
  ASSERT(start != NULL);

  if (--fetch_count >= 0) {
    size_ += byte_size * fetch_count;
    list->PushRange(fetch_count, SLL_Next(start), end);
  }

  // Increase max length slowly up to batch_size.  After that,
  // increase by batch_size in one shot so that the length is a
  // multiple of batch_size.
  if (list->max_length() < batch_size) {
    list->set_max_length(list->max_length() + 1);
  } else {
    // Don't let the list get too long.  In 32 bit builds, the length
    // is represented by a 16 bit int, so we need to watch out for
    // integer overflow.
    int new_length = min<int>(list->max_length() + batch_size,
                              kMaxDynamicFreeListLength);
    // The list's max_length must always be a multiple of batch_size,
    // and kMaxDynamicFreeListLength is not necessarily a multiple
    // of batch_size.
    new_length -= new_length % batch_size;
    ASSERT(new_length % batch_size == 0);
    list->set_max_length(new_length);
  }
  return start;
}

void ThreadCache::ListTooLong(FreeList* list, uint32_t cl) {
  size_ += list->object_size();

  const int batch_size = Static::sizemap()->num_objects_to_move(cl);
  ReleaseToCentralCache(list, cl, batch_size);

  // If the list is too long, we need to transfer some number of
  // objects to the central cache.  Ideally, we would transfer
  // num_objects_to_move, so the code below tries to make max_length
  // converge on num_objects_to_move.

  if (list->max_length() < batch_size) {
    // Slow start the max_length so we don't overreserve.
    list->set_max_length(list->max_length() + 1);
  } else if (list->max_length() > batch_size) {
    // If we consistently go over max_length, shrink max_length.  If we don't
    // shrink it, some amount of memory will always stay in this freelist.
    list->set_length_overages(list->length_overages() + 1);
    if (list->length_overages() > kMaxOverages) {
      ASSERT(list->max_length() > batch_size);
      list->set_max_length(list->max_length() - batch_size);
      list->set_length_overages(0);
    }
  }

  if (PREDICT_FALSE(size_ > max_size_)) {
    Scavenge();
  }
}

// Remove some objects of class "cl" from thread heap and add to central cache
void ThreadCache::ReleaseToCentralCache(FreeList* src, uint32_t cl, int N) {
  ASSERT(src == &list_[cl]);
  if (N > src->length()) N = src->length();
  size_t delta_bytes = N * Static::sizemap()->ByteSizeForClass(cl);

  // We return prepackaged chains of the correct size to the central cache.
  // TODO: Use the same format internally in the thread caches?
  int batch_size = Static::sizemap()->num_objects_to_move(cl);
  while (N > batch_size) {
    void *tail, *head;
    src->PopRange(batch_size, &head, &tail);
    Static::central_cache()[cl].InsertRange(head, tail, batch_size);
    N -= batch_size;
  }
  void *tail, *head;
  src->PopRange(N, &head, &tail);
  Static::central_cache()[cl].InsertRange(head, tail, N);
  size_ -= delta_bytes;
}

// Release idle memory to the central cache
void ThreadCache::Scavenge() {
  // If the low-water mark for the free list is L, it means we would
  // not have had to allocate anything from the central cache even if
  // we had reduced the free list size by L.  We aim to get closer to
  // that situation by dropping L/2 nodes from the free list.  This
  // may not release much memory, but if so we will call scavenge again
  // pretty soon and the low-water marks will be high on that call.
  for (int cl = 0; cl < Static::num_size_classes(); cl++) {
    FreeList* list = &list_[cl];
    const int lowmark = list->lowwatermark();
    if (lowmark > 0) {
      const int drop = (lowmark > 1) ? lowmark/2 : 1;
      ReleaseToCentralCache(list, cl, drop);

      // Shrink the max length if it isn't used.  Only shrink down to
      // batch_size -- if the thread was active enough to get the max_length
      // above batch_size, it will likely be that active again.  If
      // max_length shinks below batch_size, the thread will have to
      // go through the slow-start behavior again.  The slow-start is useful
      // mainly for threads that stay relatively idle for their entire
      // lifetime.
      const int batch_size = Static::sizemap()->num_objects_to_move(cl);
      if (list->max_length() > batch_size) {
        list->set_max_length(
            max<int>(list->max_length() - batch_size, batch_size));
      }
    }
    list->clear_lowwatermark();
  }

  IncreaseCacheLimit();
}

void ThreadCache::IncreaseCacheLimit() {
  SpinLockHolder h(Static::pageheap_lock());
  IncreaseCacheLimitLocked();
}

void ThreadCache::IncreaseCacheLimitLocked() {
  if (unclaimed_cache_space_ > 0) {
    // Possibly make unclaimed_cache_space_ negative.
    unclaimed_cache_space_ -= kStealAmount;
    SetMaxSize(max_size_ + kStealAmount);
    return;
  }
  // Don't hold pageheap_lock too long.  Try to steal from 10 other
  // threads before giving up.  The i < 10 condition also prevents an
  // infinite loop in case none of the existing thread heaps are
  // suitable places to steal from.
  for (int i = 0; i < 10;
       ++i, next_memory_steal_ = next_memory_steal_->next_) {
    // Reached the end of the linked list.  Start at the beginning.
    if (next_memory_steal_ == NULL) {
      ASSERT(thread_heaps_ != NULL);
      next_memory_steal_ = thread_heaps_;
    }
    if (next_memory_steal_ == this ||
        next_memory_steal_->max_size_
          <= min_per_thread_cache_size_.load(std::memory_order_relaxed)) {
      continue;
    }
    next_memory_steal_->SetMaxSize(next_memory_steal_->max_size_ - kStealAmount);
    SetMaxSize(max_size_ + kStealAmount);

    next_memory_steal_ = next_memory_steal_->next_;
    return;
  }
}

void ThreadCache::SetInitialLimitLocked()
{
  if (unclaimed_cache_space_ >= initial_thread_cache_size_) {
    unclaimed_cache_space_ -= initial_thread_cache_size_;
    SetMaxSize(initial_thread_cache_size_);
    return;
  }
  IncreaseCacheLimitLocked();
}

int ThreadCache::GetSamplePeriod() {
  return Sampler::GetSamplePeriod();
}

void ThreadCache::InitModule() {
  {
    SpinLockHolder h(Static::pageheap_lock());
    if (phinited) {
      return;
    }
    const char *tcb = TCMallocGetenvSafe("TCMALLOC_MAX_TOTAL_THREAD_CACHE_BYTES");
    if (tcb) {
      set_overall_thread_cache_size(strtoll(tcb, NULL, 10));
    }
    const char *tcib = TCMallocGetenvSafe("TCMALLOC_INITIAL_THREAD_CACHE_BYTES");
    if (tcib) {
      long long tcib_val = strtoll(tcib, NULL, 10);
      if (tcib_val > kStealAmount)
        initial_thread_cache_size_ = tcib_val;
    }
    use_batch_size_from_start_ = TCMallocGetenvSafe("TCMALLOC_BATCH_SIZE_FROM_START");
    Static::InitStaticVars();
    threadcache_allocator.Init();
    SetupMallocExtension();
    phinited = 1;
  }

  // We do "late" part of initialization without holding lock since
  // there is chance it'll recurse into malloc
  Static::InitLateMaybeRecursive();

#ifndef NDEBUG
  // pthread_atfork above may malloc sometimes. Lets ensure we test
  // that malloc works from here.
  (operator delete)((operator new)(1));
#endif
}

ThreadCache* ThreadCache::NewHeap() {
  SpinLockHolder h(Static::pageheap_lock());

  // Create the heap and add it to the linked list
  ThreadCache *heap = new (threadcache_allocator.New()) ThreadCache();

  heap->next_ = thread_heaps_;
  heap->prev_ = NULL;
  if (thread_heaps_ != NULL) {
    thread_heaps_->prev_ = heap;
  } else {
    // This is the only thread heap at the momment.
    ASSERT(next_memory_steal_ == NULL);
    next_memory_steal_ = heap;
  }
  thread_heaps_ = heap;
  thread_heap_count_++;
  return heap;
}

void ThreadCache::DeleteCache(ThreadCache* heap) {
  // Remove all memory from heap
  heap->~ThreadCache();

  // Remove from linked list
  SpinLockHolder h(Static::pageheap_lock());
  if (heap->next_ != NULL) heap->next_->prev_ = heap->prev_;
  if (heap->prev_ != NULL) heap->prev_->next_ = heap->next_;
  if (thread_heaps_ == heap) thread_heaps_ = heap->next_;
  thread_heap_count_--;

  if (next_memory_steal_ == heap) next_memory_steal_ = heap->next_;
  if (next_memory_steal_ == NULL) next_memory_steal_ = thread_heaps_;
  unclaimed_cache_space_ += heap->max_size_;

  threadcache_allocator.Delete(heap);
}

void ThreadCache::RecomputePerThreadCacheSize() {
  // Divide available space across threads
  int n = thread_heap_count_ > 0 ? thread_heap_count_ : 1;
  size_t space = overall_thread_cache_size_ / n;

  size_t min_size = min_per_thread_cache_size_.load(std::memory_order_relaxed);
  // Limit to allowed range
  if (space < min_size) space = min_size;
  if (space > kMaxThreadCacheSize) space = kMaxThreadCacheSize;

  double ratio = space / max<double>(1, per_thread_cache_size_);
  size_t claimed = 0;
  for (ThreadCache* h = thread_heaps_; h != NULL; h = h->next_) {
    // Increasing the total cache size should not circumvent the
    // slow-start growth of max_size_.
    if (ratio < 1.0) {
      h->SetMaxSize(h->max_size_ * ratio);
    }
    claimed += h->max_size_;
  }
  unclaimed_cache_space_ = overall_thread_cache_size_ - claimed;
  per_thread_cache_size_ = space;
}

void ThreadCache::GetThreadStats(uint64_t* total_bytes, uint64_t* class_count) {
  for (ThreadCache* h = thread_heaps_; h != NULL; h = h->next_) {
    *total_bytes += h->Size();
    if (class_count) {
      for (int cl = 0; cl < Static::num_size_classes(); ++cl) {
        class_count[cl] += h->freelist_length(cl);
      }
    }
  }
}

void ThreadCache::set_overall_thread_cache_size(size_t new_size) {
  // Clip the value to a reasonable range
  size_t min_size = min_per_thread_cache_size_.load(std::memory_order_relaxed);
  if (new_size < min_size) {
    new_size = min_size;
  }
  if (new_size > (1<<30)) new_size = (1<<30);     // Limit to 1GB
  overall_thread_cache_size_ = new_size;

  RecomputePerThreadCacheSize();
}

}  // namespace tcmalloc
