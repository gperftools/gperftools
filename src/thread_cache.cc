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

#include "thread_cache.h"
#include "maybe_threads.h"

// Twice the approximate gap between sampling actions.
// I.e., we take one sample approximately once every
//      tcmalloc_sample_parameter/2
// bytes of allocation, i.e., ~ once every 128KB.
// Must be a prime number.
#ifdef NO_TCMALLOC_SAMPLES
DEFINE_int64(tcmalloc_sample_parameter, 0,
             "Unused: code is compiled with NO_TCMALLOC_SAMPLES");
static size_t sample_period = 0;
#else
DEFINE_int64(tcmalloc_sample_parameter,
             EnvToInt("TCMALLOC_SAMPLE_PARAMETER", 262147),
	     "Twice the approximate gap between sampling actions."
	     " Must be a prime number. Otherwise will be rounded up to a "
	     " larger prime number");
static size_t sample_period = EnvToInt("TCMALLOC_SAMPLE_PARAMETER", 262147);
#endif
// Protects sample_period above
static SpinLock sample_period_lock(SpinLock::LINKER_INITIALIZED);

namespace tcmalloc {

/* The smallest prime > 2^n */
static unsigned int primes_list[] = {
	// Small values might cause high rates of sampling
	// and hence commented out.
        // 2, 5, 11, 17, 37, 67, 131, 257,
	// 521, 1031, 2053, 4099, 8209, 16411,
	32771, 65537, 131101, 262147, 524309, 1048583,
	2097169, 4194319, 8388617, 16777259, 33554467 };

static bool phinited = false;

volatile size_t ThreadCache::per_thread_cache_size_ = kMaxThreadCacheSize;
size_t ThreadCache::overall_thread_cache_size_ = kDefaultOverallThreadCacheSize;
PageHeapAllocator<ThreadCache> threadcache_allocator;
ThreadCache* ThreadCache::thread_heaps_ = NULL;
int ThreadCache::thread_heap_count_ = 0;
#ifdef HAVE_TLS
__thread ThreadCache* ThreadCache::threadlocal_heap_
# ifdef HAVE___ATTRIBUTE__
   __attribute__ ((tls_model ("initial-exec")))
# endif
   ;
#endif
bool ThreadCache::tsd_inited_ = false;
pthread_key_t ThreadCache::heap_key_;

#if defined(HAVE_TLS)
bool kernel_supports_tls = false;      // be conservative
# if !HAVE_DECL_UNAME   // if too old for uname, probably too old for TLS
    void CheckIfKernelSupportsTLS() {
      kernel_supports_tls = false;
    }
# else
#   include <sys/utsname.h>    // DECL_UNAME checked for <sys/utsname.h> too
    void CheckIfKernelSupportsTLS() {
      struct utsname buf;
      if (uname(&buf) != 0) {   // should be impossible
        MESSAGE("uname failed assuming no TLS support (errno=%d)\n", errno);
        kernel_supports_tls = false;
      } else if (strcasecmp(buf.sysname, "linux") == 0) {
        // The linux case: the first kernel to support TLS was 2.6.0
        if (buf.release[0] < '2' && buf.release[1] == '.')    // 0.x or 1.x
          kernel_supports_tls = false;
        else if (buf.release[0] == '2' && buf.release[1] == '.' &&
                 buf.release[2] >= '0' && buf.release[2] < '6' &&
                 buf.release[3] == '.')                       // 2.0 - 2.5
          kernel_supports_tls = false;
        else
          kernel_supports_tls = true;
      } else {        // some other kernel, we'll be optimisitic
        kernel_supports_tls = true;
      }
      // TODO(csilvers): VLOG(1) the tls status once we support RAW_VLOG
    }
#  endif  // HAVE_DECL_UNAME
#endif    // HAVE_TLS

void ThreadCache::Init(pthread_t tid) {
  size_ = 0;
  next_ = NULL;
  prev_ = NULL;
  tid_  = tid;
  in_setspecific_ = false;
  for (size_t cl = 0; cl < kNumClasses; ++cl) {
    list_[cl].Init();
  }

  // Initialize RNG -- run it for a bit to get to good values
  bytes_until_sample_ = 0;
  rnd_ = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(this));
  for (int i = 0; i < 100; i++) {
    PickNextSample(FLAGS_tcmalloc_sample_parameter * 2);
  }
}

void ThreadCache::Cleanup() {
  // Put unused memory back into central cache
  for (int cl = 0; cl < kNumClasses; ++cl) {
    if (list_[cl].length() > 0) {
      ReleaseToCentralCache(&list_[cl], cl, list_[cl].length());
    }
  }
}

// Remove some objects of class "cl" from central cache and add to thread heap.
// On success, return the first object for immediate use; otherwise return NULL.
void* ThreadCache::FetchFromCentralCache(size_t cl, size_t byte_size) {
  void *start, *end;
  int fetch_count = Static::central_cache()[cl].RemoveRange(
      &start, &end,
      Static::sizemap()->num_objects_to_move(cl));
  ASSERT((start == NULL) == (fetch_count == 0));
  if (--fetch_count >= 0) {
    size_ += byte_size * fetch_count;
    list_[cl].PushRange(fetch_count, SLL_Next(start), end);
  }
  return start;
}

// Remove some objects of class "cl" from thread heap and add to central cache
size_t ThreadCache::ReleaseToCentralCache(FreeList* src,
                                                   size_t cl, int N) {
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
  return size_ -= delta_bytes;
}

// Release idle memory to the central cache
void ThreadCache::Scavenge() {
  // If the low-water mark for the free list is L, it means we would
  // not have had to allocate anything from the central cache even if
  // we had reduced the free list size by L.  We aim to get closer to
  // that situation by dropping L/2 nodes from the free list.  This
  // may not release much memory, but if so we will call scavenge again
  // pretty soon and the low-water marks will be high on that call.
  //int64 start = CycleClock::Now();

  for (int cl = 0; cl < kNumClasses; cl++) {
    FreeList* list = &list_[cl];
    const int lowmark = list->lowwatermark();
    if (lowmark > 0) {
      const int drop = (lowmark > 1) ? lowmark/2 : 1;
      ReleaseToCentralCache(list, cl, drop);
    }
    list->clear_lowwatermark();
  }

  //int64 finish = CycleClock::Now();
  //CycleTimer ct;
  //MESSAGE("GC: %.0f ns\n", ct.CyclesToUsec(finish-start)*1000.0);
}

void ThreadCache::PickNextSample(size_t k) {
  // Copied from "base/synchronization.cc" (written by Mike Burrows)
  // Make next "random" number
  // x^32+x^22+x^2+x^1+1 is a primitive polynomial for random numbers
  static const uint32_t kPoly = (1 << 22) | (1 << 2) | (1 << 1) | (1 << 0);
  uint32_t r = rnd_;
  rnd_ = (r << 1) ^ ((static_cast<int32_t>(r) >> 31) & kPoly);

  // Next point is "rnd_ % (sample_period)".  I.e., average
  // increment is "sample_period/2".
  const int flag_value = FLAGS_tcmalloc_sample_parameter;
  static int last_flag_value = -1;

  if (flag_value != last_flag_value) {
    SpinLockHolder h(&sample_period_lock);
    int i;
    for (i = 0; i < (sizeof(primes_list)/sizeof(primes_list[0]) - 1); i++) {
      if (primes_list[i] >= flag_value) {
        break;
      }
    }
    sample_period = primes_list[i];
    last_flag_value = flag_value;
  }

  bytes_until_sample_ += rnd_ % sample_period;

  if (k > (static_cast<size_t>(-1) >> 2)) {
    // If the user has asked for a huge allocation then it is possible
    // for the code below to loop infinitely.  Just return (note that
    // this throws off the sampling accuracy somewhat, but a user who
    // is allocating more than 1G of memory at a time can live with a
    // minor inaccuracy in profiling of small allocations, and also
    // would rather not wait for the loop below to terminate).
    return;
  }

  while (bytes_until_sample_ < k) {
    // Increase bytes_until_sample_ by enough average sampling periods
    // (sample_period >> 1) to allow us to sample past the current
    // allocation.
    bytes_until_sample_ += (sample_period >> 1);
  }

  bytes_until_sample_ -= k;
}

void ThreadCache::InitModule() {
  // There is a slight potential race here because of double-checked
  // locking idiom.  However, as long as the program does a small
  // allocation before switching to multi-threaded mode, we will be
  // fine.  We increase the chances of doing such a small allocation
  // by doing one in the constructor of the module_enter_exit_hook
  // object declared below.
  SpinLockHolder h(Static::pageheap_lock());
  if (!phinited) {
    Static::InitStaticVars();
    threadcache_allocator.Init();
    phinited = 1;
  }
}

void ThreadCache::InitTSD() {
  ASSERT(!tsd_inited_);
  perftools_pthread_key_create(&heap_key_, DestroyThreadCache);
  tsd_inited_ = true;

  // We may have used a fake pthread_t for the main thread.  Fix it.
  pthread_t zero;
  memset(&zero, 0, sizeof(zero));
  SpinLockHolder h(Static::pageheap_lock());
  for (ThreadCache* h = thread_heaps_; h != NULL; h = h->next_) {
    if (h->tid_ == zero) {
      h->tid_ = pthread_self();
    }
  }
}

ThreadCache* ThreadCache::CreateCacheIfNecessary() {
  // Initialize per-thread data if necessary
  ThreadCache* heap = NULL;
  {
    SpinLockHolder h(Static::pageheap_lock());

    // Early on in glibc's life, we cannot even call pthread_self()
    pthread_t me;
    if (!tsd_inited_) {
      memset(&me, 0, sizeof(me));
    } else {
      me = pthread_self();
    }

    // This may be a recursive malloc call from pthread_setspecific()
    // In that case, the heap for this thread has already been created
    // and added to the linked list.  So we search for that first.
    for (ThreadCache* h = thread_heaps_; h != NULL; h = h->next_) {
      if (h->tid_ == me) {
        heap = h;
        break;
      }
    }

    if (heap == NULL) heap = NewHeap(me);
  }

  // We call pthread_setspecific() outside the lock because it may
  // call malloc() recursively.  We check for the recursive call using
  // the "in_setspecific_" flag so that we can avoid calling
  // pthread_setspecific() if we are already inside pthread_setspecific().
  if (!heap->in_setspecific_ && tsd_inited_) {
    heap->in_setspecific_ = true;
    perftools_pthread_setspecific(heap_key_, heap);
#ifdef HAVE_TLS
    // Also keep a copy in __thread for faster retrieval
    threadlocal_heap_ = heap;
#endif
    heap->in_setspecific_ = false;
  }
  return heap;
}

void ThreadCache::BecomeIdle() {
  if (!tsd_inited_) return;              // No caches yet
  ThreadCache* heap = GetThreadHeap();
  if (heap == NULL) return;             // No thread cache to remove
  if (heap->in_setspecific_) return;    // Do not disturb the active caller

  heap->in_setspecific_ = true;
  perftools_pthread_setspecific(heap_key_, NULL);
#ifdef HAVE_TLS
  // Also update the copy in __thread
  threadlocal_heap_ = NULL;
#endif
  heap->in_setspecific_ = false;
  if (GetThreadHeap() == heap) {
    // Somehow heap got reinstated by a recursive call to malloc
    // from pthread_setspecific.  We give up in this case.
    return;
  }

  // We can now get rid of the heap
  DeleteCache(heap);
}

void ThreadCache::DestroyThreadCache(void* ptr) {
  // Note that "ptr" cannot be NULL since pthread promises not
  // to invoke the destructor on NULL values, but for safety,
  // we check anyway.
  if (ptr == NULL) return;
#ifdef HAVE_TLS
  // Prevent fast path of GetThreadHeap() from returning heap.
  threadlocal_heap_ = NULL;
#endif
  DeleteCache(reinterpret_cast<ThreadCache*>(ptr));
}

void ThreadCache::DeleteCache(ThreadCache* heap) {
  // Remove all memory from heap
  heap->Cleanup();

  // Remove from linked list
  SpinLockHolder h(Static::pageheap_lock());
  if (heap->next_ != NULL) heap->next_->prev_ = heap->prev_;
  if (heap->prev_ != NULL) heap->prev_->next_ = heap->next_;
  if (thread_heaps_ == heap) thread_heaps_ = heap->next_;
  thread_heap_count_--;
  RecomputeThreadCacheSize();

  threadcache_allocator.Delete(heap);
}

void ThreadCache::RecomputeThreadCacheSize() {
  // Divide available space across threads
  int n = thread_heap_count_ > 0 ? thread_heap_count_ : 1;
  size_t space = overall_thread_cache_size_ / n;

  // Limit to allowed range
  if (space < kMinThreadCacheSize) space = kMinThreadCacheSize;
  if (space > kMaxThreadCacheSize) space = kMaxThreadCacheSize;

  per_thread_cache_size_ = space;
  //MESSAGE("Threads %d => cache size %8d\n", n, int(space));
}

void ThreadCache::Print() const {
  for (int cl = 0; cl < kNumClasses; ++cl) {
    MESSAGE("      %5" PRIuS " : %4" PRIuS " len; %4d lo\n",
            Static::sizemap()->ByteSizeForClass(cl),
            list_[cl].length(),
            list_[cl].lowwatermark());
  }
}

void ThreadCache::GetThreadStats(uint64_t* total_bytes, uint64_t* class_count) {
  for (ThreadCache* h = thread_heaps_; h != NULL; h = h->next_) {
    *total_bytes += h->Size();
    if (class_count) {
      for (int cl = 0; cl < kNumClasses; ++cl) {
        class_count[cl] += h->freelist_length(cl);
      }
    }
  }
}

void ThreadCache::set_overall_thread_cache_size(size_t new_size) {
  // Clip the value to a reasonable range
  if (new_size < kMinThreadCacheSize) new_size = kMinThreadCacheSize;
  if (new_size > (1<<30)) new_size = (1<<30);     // Limit to 1GB

  overall_thread_cache_size_ = new_size;
  ThreadCache::RecomputeThreadCacheSize();
}

}  // namespace tcmalloc
