// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
// Copyright (c) 2026, gperftools Contributors
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
#include "config.h"

#include "tracing/malloc_tracer.h"

#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <atomic>

#include "base/spinlock.h"
#include "internal_logging.h"
#include "page_heap_allocator.h"
#include "thread_cache.h"
#include "tracing/tracer_buffer.h"

namespace tcmalloc {

__thread MallocTracer* MallocTracer::instance_ ATTR_INITIAL_EXEC;
MallocTracer* MallocTracer::all_tracers_;
bool MallocTracer::active_;

// See comment next to buf_storage_. On 32-bit machines the size is
// smaller, but still not 4k-multiple, which is what matters.
static_assert(sizeof(void*) != 8 || sizeof(MallocTracer) == 4096 + 64);

namespace {

constexpr int kDumperPeriodMicros = 3000;

constexpr int kTokenSize = 1 << 10;

SpinLock lock;

std::atomic<uint64_t> token_counter;
// We don't want to depend on libatomic (and its potential locking
// inside malloc). So tracing is only supported where 64-bit atomics
// are native.
static_assert(std::atomic<uint64_t>::is_always_lock_free,
              "malloc tracing requires lock-free 64-bit atomics (configure with --disable-tracing)");
uint64_t thread_id_counter;
uint64_t thread_dump_written;

uint64_t base_ts;

__thread bool had_tracer ATTR_INITIAL_EXEC;
// See MallocTracer::MarkCurrentThreadUntraced.
__thread bool untraced_thread ATTR_INITIAL_EXEC;
// in_setup guards cases of malloc calls during SetupGlobalState
// (e.g. pthread_key_create calling malloc). Those get fallback
// tracer. Note, it is plain global (not thread-local). It flips to
// true and back to false as part of the very first traced malloc
// call, which we (reasonably) assume to be single-threaded.
bool in_setup;

pthread_key_t instance_key;
pthread_once_t setup_once = PTHREAD_ONCE_INIT;

// Protects the fallback tracer (see MallocTracer::WithInstance). Lock
// ordering: fallback_lock is taken before lock.
SpinLock fallback_lock;

PageHeapAllocator<MallocTracer> malloc_tracer_allocator;

TracerBuffer* tracer_buffer;

bool no_more_writes;

// Set in forked child (see TracerAfterForkChild). Child has no saver
// and no dumper threads, so it cannot write the trace. And parent
// keeps writing the trace anyways.
bool in_fork_child;

StaticStorage<MallocTracer> fallback_tracer_storage;

uint64_t GetNanos() {
  struct timespec tss;
  clock_gettime(CLOCK_MONOTONIC, &tss);
  return static_cast<uint64_t>(tss.tv_sec) * 1000000000 + tss.tv_nsec - base_ts;
}

void AppendBufLocked(const char* buf, size_t size) {
  if (no_more_writes) {
    return;
  }
  tracer_buffer->AppendData(buf, size);
}

}  // namespace

void MallocTracer::OnThreadExit(void* arg) {
  CHECK_CONDITION(!had_tracer);

  MallocTracer* tracer = static_cast<MallocTracer*>(arg);
  ASSERT(tracer == instance_);

  // have pthread call us again on next destruction iteration and give
  // rest of tls destructors chance to get traced properly
  if (tracer->destroy_count_++ < 3) {
    pthread_setspecific(instance_key, tracer);
    return;
  }

  had_tracer = true;
  instance_ = nullptr;
  UnlinkAndDestroy(tracer);
}

void MallocTracer::UnlinkAndDestroy(MallocTracer* tracer) {
  {
    SpinLockHolder l(&lock);
    MallocTracer* next = *tracer->pprev_ = tracer->next_;
    if (next) {
      next->pprev_ = tracer->pprev_;
    }
    tracer->pprev_ = reinterpret_cast<MallocTracer**>(0xababababababababULL);
    tracer->next_ = reinterpret_cast<MallocTracer*>(0xcdcdcdcdcdcdcdcdULL);
  }

  // Flushes remaining events and records thread's death.
  tracer->~MallocTracer();

  SpinLockHolder h(&lock);
  malloc_tracer_allocator.Delete(tracer);
}


void MallocTracer::WithInstanceSlow(FunctionRef<void(MallocTracer*)> body) {
  if (untraced_thread) {
    body(nullptr);
    return;
  }
  MallocTracer* instance = GetInstanceSlow();
  if (instance != nullptr) {
    body(instance);
    return;
  }
  SpinLockHolder h(&fallback_lock);
  body(fallback_tracer_storage.get());
}

void MallocTracer::SetupGlobalState() {
  // Fallback tracer must be set up first. Code below in_setup = true
  // may malloc, and those recursive mallocs are traced by fallback
  // tracer.
  base_ts = GetNanos() & MallocTraceEncoder::kTSMask;
  MallocTracer* fallback = fallback_tracer_storage.Construct(0);

  // Fallback tracer is on all_tracers_ list (so it gets dumped), and
  // it never dies. Note, all_tracers_ is empty at this point.
  {
    SpinLockHolder h(&lock);
    fallback->next_ = nullptr;
    fallback->pprev_ = &all_tracers_;
    all_tracers_ = fallback;
  }

  in_setup = true;

  tracer_buffer = TracerBuffer::Create();

  uint32_t magic = MallocTraceEncoder::kMagic;
  tracer_buffer->AppendData(reinterpret_cast<char*>(&magic), sizeof(magic));

  malloc_tracer_allocator.Init();
  int rv = pthread_key_create(&instance_key, &MallocTracer::OnThreadExit);
  CHECK_CONDITION(!rv);

  in_setup = false;
}

namespace {

void* DumperThread(void*) {
  MallocTracer::MarkCurrentThreadUntraced();
  while (true) {
    usleep(kDumperPeriodMicros);
    MallocTracer::DumpEverything();
  }
  return nullptr;
}

// Fork handlers. We make sure tracer's locks are not held by other
// threads at fork time, so that forked child can keep calling malloc
// (and thus tracer) safely. Child has no saver or dumper threads, so
// in child we just drop all further trace writes, and skip trace
// finalization at exit (which would otherwise wait for saver thread
// forever).
//
// Note, we register those handlers after tcmalloc's own (which is
// done during the very first malloc call). And prepare handlers are
// called in reverse order of registration. So we grab our locks
// before tcmalloc's, which matches lock ordering (we may call
// MetaDataAlloc with lock held).
void TracerPrepareFork() NO_THREAD_SAFETY_ANALYSIS {
  fallback_lock.Lock();
  lock.Lock();
}

void TracerAfterForkParent() NO_THREAD_SAFETY_ANALYSIS {
  lock.Unlock();
  fallback_lock.Unlock();
}

void TracerAfterForkChild() NO_THREAD_SAFETY_ANALYSIS {
  in_fork_child = true;
  no_more_writes = true;
  lock.Unlock();
  fallback_lock.Unlock();
}

void SetupTracer() {
  // Tracing gets activated (or not) during malloc initialization.
  ThreadCache::EnsureMallocInitialized();
  if (!MallocTracer::IsActive()) {
    return;
  }

  // Sets up the tracer of this thread (and tracing as a whole).
  MallocTracer::WithInstance([](MallocTracer*) {});

  // Opens trace output and starts saver thread. Note, we do it before
  // starting dumper.
  tracer_buffer->InitLate();

  pthread_atfork(TracerPrepareFork, TracerAfterForkParent, TracerAfterForkChild);

  CreateServiceThread(DumperThread, nullptr);
}

}  // namespace

MallocTracer* MallocTracer::GetInstanceSlow() {
  if (in_setup) {
    // We're called recursively from SetupGlobalState. Fallback
    // tracer is already set up by then.
    return nullptr;
  }

  pthread_once(&setup_once, &MallocTracer::SetupGlobalState);

  if (had_tracer) {
    // Our thread is exiting, and its tracer is already destroyed (by
    // OnThreadExit), but some other thread-exit code
    // still calls malloc or free.
    return nullptr;
  }

  MallocTracer* an_instance;
  {
    SpinLockHolder h(&lock);
    uint64_t thread_id = ++thread_id_counter;

    an_instance = malloc_tracer_allocator.New();
    new (an_instance) MallocTracer(thread_id);

    instance_ = an_instance;

    an_instance->next_ = all_tracers_;
    an_instance->pprev_ = &all_tracers_;

    if (an_instance->next_) {
      an_instance->next_->pprev_ = &an_instance->next_;
    }
    all_tracers_ = an_instance;
  }

  // Note, this may call malloc, which is fine since instance_ is
  // already set up.
  pthread_setspecific(instance_key, an_instance);

  return an_instance;
}

MallocTracer::MallocTracer(uint64_t thread_id) {
  // See comment next to fields.
  static_assert(offsetof(MallocTracer, next_) + sizeof(next_) <= 64);

  buf_ptr_ = buf_storage_;
  buf_end_ = buf_storage_ + sizeof(buf_storage_) - AltVarintCodec::kMaxSize;

  thread_id_ = thread_id;
  token_base_ = counter_ = 0;
  prev_size_ = 0;
  prev_token_ = 0;
  last_cpu_.store(-1, std::memory_order_relaxed);
  snapshot_buf_ptr_ = flushed_buf_ptr_ = buf_storage_;
  destroy_count_ = 0;

  RefreshToken();
}

uint64_t MallocTracer::UpdateTSAndCPU() {
  uint64_t ts = GetNanos();
  int cpu = sched_getcpu();
  last_cpu_.store(cpu, std::memory_order_relaxed);
  return MallocTraceEncoder::BundleTSAndCPU(ts, cpu);
}

void MallocTracer::EmitChunkLocked(uint64_t size, uint64_t ts_and_cpu) {
  char meta_buf[32];
  char* p = meta_buf;
  MallocTraceEncoder::triple enc = MallocTraceEncoder::EncodeBuffer(thread_id_, ts_and_cpu, size);
  p = AltVarintCodec::EncodeUnsigned(p, enc.first);
  p = AltVarintCodec::EncodeUnsigned(p, enc.second.first);
  p = AltVarintCodec::EncodeUnsigned(p, enc.second.second);

  AppendBufLocked(meta_buf, p - meta_buf);
  AppendBufLocked(flushed_buf_ptr_, size);
}

void MallocTracer::RefreshBuffer() {
  SpinLockHolder h(&lock);
  RefreshBufferLocked();
}

void MallocTracer::RefreshBufferLocked() {
  if (buf_ptr_ != flushed_buf_ptr_) {
    EmitChunkLocked(buf_ptr_ - flushed_buf_ptr_, UpdateTSAndCPU());
  }

  SetBufPtr(buf_storage_);
  flushed_buf_ptr_ = buf_storage_;
}

void MallocTracer::DumpSnapshotLocked() {
  uint64_t s = snapshot_buf_ptr_ - flushed_buf_ptr_;

  if (s == 0) {
    return;
  }

  uint64_t tscpu = MallocTraceEncoder::BundleTSAndCPU(GetNanos(), last_cpu_.load(std::memory_order_relaxed));
  EmitChunkLocked(s, tscpu);

  flushed_buf_ptr_ = snapshot_buf_ptr_;

  thread_dump_written += s;
}

void MallocTracer::RefreshTokenAndDec() {
  uint64_t base = token_counter.fetch_add(kTokenSize, std::memory_order_relaxed) + kTokenSize;

  token_base_ = base;
  counter_ = kTokenSize;

  MallocTraceEncoder::pair enc = MallocTraceEncoder::EncodeToken(base - kTokenSize, UpdateTSAndCPU());

  AppendWords(2, enc.first, enc.second);
}

void MallocTracer::RefreshToken() {
  RefreshTokenAndDec();
  counter_++;
}

void MallocTracer::DumpEverything() {
  SpinLockHolder h(&lock);
  DumpEverythingLocked();
}

void MallocTracer::DumpEverythingAndStopWrites() {
  SpinLockHolder h(&lock);
  DumpEverythingLocked();
  no_more_writes = true;
}

void MallocTracer::DumpEverythingLocked() {
  // We do 2 passes: first snapshot all tracers' positions, then
  // emit. This isn't needed for correctness (each tracer's acquire
  // load below is enough to see its data up to the snapshot). But it
  // keeps per-thread cutoff points of this window close together in
  // time. Doing it in one pass would have each next thread's cutoff
  // trail by the time spent emitting previous threads' chunks. And
  // tighter windows mean fewer cross-window reorderings for the
  // replayer to deal with.
  for (MallocTracer* t = all_tracers_; t != nullptr; t = t->next_) {
    // Acquire load ensures that we're able to see all the data
    // written up to snapshot_buf_ptr_.
    char* buf_ptr = t->BufPtrAsAtomic()->load(std::memory_order_acquire);
    t->snapshot_buf_ptr_ = buf_ptr;
  }

  for (MallocTracer* t = all_tracers_; t != nullptr; t = t->next_) {
    if (t->snapshot_buf_ptr_ == t->flushed_buf_ptr_) {
      continue;
    }
    t->DumpSnapshotLocked();
  }

  char sync_end_buf[24];
  char* p = sync_end_buf;

  uint64_t ts_and_cpu = MallocTraceEncoder::BundleTSAndCPU(GetNanos(), sched_getcpu());
  MallocTraceEncoder::pair enc = MallocTraceEncoder::EncodeSyncBarrier(ts_and_cpu);
  p = AltVarintCodec::EncodeUnsigned(p, enc.first);
  p = AltVarintCodec::EncodeUnsigned(p, enc.second);
  AppendBufLocked(sync_end_buf, p - sync_end_buf);
}

void MallocTracer::MarkCurrentThreadUntraced() {
  // Note, we set both before destroying the tracer, so that any
  // malloc calls from here on are untraced.
  untraced_thread = true;
  MallocTracer* tracer = instance_;
  if (tracer == nullptr) {
    return;
  }
  instance_ = nullptr;
  // Prevents OnThreadExit from running at thread exit.
  pthread_setspecific(instance_key, nullptr);
  UnlinkAndDestroy(tracer);
}

MallocTracer::~MallocTracer() {
  RefreshBuffer();

  char* p = buf_ptr_;
  MallocTraceEncoder::pair enc = MallocTraceEncoder::EncodeDeath(thread_id_, UpdateTSAndCPU());
  p = AltVarintCodec::EncodeUnsigned(p, enc.first);
  p = AltVarintCodec::EncodeUnsigned(p, enc.second);

  {
    SpinLockHolder h(&lock);
    AppendBufLocked(buf_storage_, p - buf_storage_);
  }

  memset(static_cast<void*>(this), 0xfe, sizeof(*this));
}

namespace {

void FinalizeTracing() {
  // Grab whatever is still sitting in per-thread buffers. Notably,
  // main thread's tracer is never destroyed (exit doesn't run pthread
  // key destructors for main thread), so without this we'd lose its
  // most recent events (and those of other still running threads).
  //
  // Saving rest of trace may still malloc, particularly if saver
  // thread compresses. So we need to drop lock soon. But we drop all
  // further buffer writes.
  MallocTracer::DumpEverythingAndStopWrites();

  char encoded_end[16];
  char* p = encoded_end;
  p = AltVarintCodec::EncodeUnsigned(p, MallocTraceEncoder::EncodeEnd());
  ASSERT(p <= encoded_end + sizeof(encoded_end));

  tracer_buffer->AppendData(encoded_end, p - encoded_end);
  tracer_buffer->Finalize();
}

// Sets up tracing (if activated) at module init time, and finalizes
// trace at exit.
class TracerInitDeinit {
 public:
  TracerInitDeinit() { SetupTracer(); }
  ~TracerInitDeinit() {
    if (!MallocTracer::IsActive() || in_fork_child) {
      return;
    }
    FinalizeTracing();
  }
};
TracerInitDeinit tracer_init_deinit;

}  // namespace

void MallocTracer::SPrintStats(char* start, char* end) {
  snprintf(start, end - start,
           "token_counter = %" PRIu64
           "\n"
           "thread_id_counter = %" PRIu64
           "\n"
           "thread_dump_written = %" PRIu64 "\n",
           token_counter.load(std::memory_order_relaxed), thread_id_counter, thread_dump_written);
}

void MallocTracer::TraceMisc(std::string_view msg) {
  SpinLockHolder h(&lock);

  // Misc events go directly into the trace. So we first emit whatever
  // events of this thread are still pending in its buffer, so that
  // misc event lands in trace after all prior events of this
  // thread. Note, we're the buffer's owner, so this is safe.
  RefreshBufferLocked();

  auto ts_and_cpu = UpdateTSAndCPU();

  char meta_buf[32];
  char* p = meta_buf;
  MallocTraceEncoder::triple enc = MallocTraceEncoder::EncodeMisc(thread_id_, ts_and_cpu, msg.size());
  p = AltVarintCodec::EncodeUnsigned(p, enc.first);
  p = AltVarintCodec::EncodeUnsigned(p, enc.second.first);
  p = AltVarintCodec::EncodeUnsigned(p, enc.second.second);

  AppendBufLocked(meta_buf, p - meta_buf);
  AppendBufLocked(msg.data(), msg.size());
}

}  // namespace tcmalloc
