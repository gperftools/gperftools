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

#ifndef TCMALLOC_TRACING_MALLOC_TRACER_H_
#define TCMALLOC_TRACING_MALLOC_TRACER_H_
#include "config.h"

#include <atomic>
#include <string_view>
#include <type_traits>

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>  // for ssize_t

#include "tracing/altvarint_codec.h"
#include "base/basictypes.h"
#include "base/function_ref.h"
#include "base/static_storage.h"
#include "tracing/malloc_trace_encoder.h"

namespace tcmalloc {

class MallocTracer {
 public:
  // Runs body(tracer) with the current thread's tracer. In rare
  // cases when the thread has no tracer (during tracer setup, or
  // after thread's tracer was destroyed by thread exit), body is
  // passed the shared "fallback" tracer instead, which is locked for
  // the duration of the body.
  //
  // The fallback tracer has thread id 0, and since it is only used
  // under lock, it produces a single consistent stream of events.
  //
  // On untraced threads (see MarkCurrentThreadUntraced), body is
  // passed nullptr. Bodies must then record nothing, and use
  // kUntracedToken as the allocation's token. Note, on the fast path
  // compiler knows tracer is not nullptr, so those checks are free
  // there.
  //
  // Returns whatever body returns. Prefer capturing by value and
  // returning results (as opposed to capturing by reference), so that
  // fast path can keep everything in registers.
  //
  // Note, body is instantiated twice: inlined into the fast path and
  // as a FunctionRef thunk for the slow path. So keep it small.
  template <typename Body>
  static auto WithInstance(const Body& body) -> decltype(body(static_cast<MallocTracer*>(nullptr)));

  // Token of allocations made by untraced threads. We never record
  // any events about such allocations (including when they are freed
  // or reallocated by traced threads). Real tokens never get anywhere
  // close to this value.
  static constexpr uint64_t kUntracedToken = ~uint64_t{0};

  uint64_t TraceMalloc(size_t size);
  void TraceFree(uint64_t token);
  void TraceFreeSized(uint64_t token);
  uint64_t TraceRealloc(uint64_t old_token, size_t new_size);
  uint64_t TraceMemalign(size_t size, size_t alignment);

  // Tracing is activated (once, during malloc initialization, see
  // SetupTracingMalloc in tracing/tracing_overlay.cc) when
  // TCMALLOC_TRACE_OUTPUT environment variable is set. Nothing in
  // here must be used when it is not active.
  static bool IsActive() { return active_; }
  static void Activate() { active_ = true; }

  static void DumpEverything();
  // Final round of DumpEverything at trace finalization. Also stops
  // any further trace writes (atomically with the dump).
  static void DumpEverythingAndStopWrites();
  // Makes current thread untraced. For tracer's own threads (saver
  // and dumper). Notably, saver must never trace anything, since
  // flushing its events may need to wait for buffer space, which
  // only saver itself frees. Destroys thread's tracer, if any.
  static void MarkCurrentThreadUntraced();
  static void SPrintStats(char* start, char* end);

  // appends misc event with the given ~arbitrary text message. Must
  // be called by the tracer's owner (i.e. via WithInstance).
  void TraceMisc(std::string_view msg);

 private:
  friend class StaticStorage<MallocTracer>;

  explicit MallocTracer(uint64_t thread_id);
  ~MallocTracer();

  void AppendWords(int count, uint64_t first, uint64_t second);

  std::atomic<char*>* BufPtrAsAtomic() { return reinterpret_cast<std::atomic<char*>*>(&buf_ptr_); }
  void SetBufPtr(char* new_value) { BufPtrAsAtomic()->store(new_value, std::memory_order_release); }

  bool HasSpaceFor(int varints) {
    // note, buf_end is set up such that varints = 1 case doesn't need any
    // extra computation
    return (buf_ptr_ + AltVarintCodec::kMaxSize * (varints - 1) < buf_end_);
  }

  void RefreshToken();
  void RefreshTokenAndDec();
  void EmitChunkLocked(uint64_t amount, uint64_t ts_and_cpu);
  void RefreshBuffer();
  void RefreshBufferLocked();

  // Returns the current thread's tracer, creating it if
  // needed. Returns nullptr if the fallback tracer must be used.
  static MallocTracer* GetInstanceSlow();
  static void WithInstanceSlow(FunctionRef<void(MallocTracer*)> body);
  template <typename Body>
  static auto WithInstanceSlowBody(Body body) -> decltype(body(static_cast<MallocTracer*>(nullptr)));

  static void SetupGlobalState();
  static void OnThreadExit(void* arg);
  static void UnlinkAndDestroy(MallocTracer* tracer);

  uint64_t UpdateTSAndCPU();

  void DumpSnapshotLocked();
  static void DumpEverythingLocked();

  // Note, fields below are ordered such that fast-path fields, next_
  // (which dumper uses to walk all_tracers_) and last_cpu_ are in the
  // first cache line. And the rest of dumper-visible fields are in
  // the next one.
  char* buf_ptr_;
  char* buf_end_;

  uint64_t token_base_;
  uint64_t counter_;

  ssize_t prev_size_;
  uint64_t prev_token_;

  MallocTracer* next_;

  // Written by owner thread only, but also read by dumper thread
  // (see DumpSnapshotLocked). Thus relaxed atomic.
  std::atomic<int> last_cpu_;

  // Data between buf_storage_ and flushed_buf_ptr_ is already in the
  // trace (emitted by dumper). And snapshot_buf_ptr_ is dumper's
  // snapshot of buf_ptr_ (see DumpEverythingLocked). Both are
  // protected by lock.
  char* snapshot_buf_ptr_;
  char* flushed_buf_ptr_;

  uint64_t thread_id_;
  MallocTracer** pprev_;
  int destroy_count_;

  // sizeof(MallocTracer) is 4096 + 64 on 64-bit machines (see
  // static_assert in malloc_tracer.cc). Tracers are allocated back to back (by
  // PageHeapAllocator), so this makes their headers land at varying
  // offsets within 4k pages. With 4k size, all headers would map to
  // the same few cache sets, making dumper's walk over many
  // (mostly idle) tracers thrash the cache.
  char buf_storage_[4056];

  static __thread MallocTracer* instance_ ATTR_INITIAL_EXEC;
  static MallocTracer* all_tracers_;
  static bool active_;
};

// Note, flatten makes sure body is inlined into the fast path (even
// if inliner thinks body is too large).
template <typename Body>
ALWAYS_INLINE __attribute__((flatten)) auto MallocTracer::WithInstance(const Body& body)
    -> decltype(body(static_cast<MallocTracer*>(nullptr))) {
  MallocTracer* instance = instance_;
  if (PREDICT_TRUE(instance != nullptr)) {
    return body(instance);
  }
  return WithInstanceSlowBody(body);
}

// Note, we take body by value, so that (small) body is passed in
// registers, and fast path doesn't have to materialize it in memory.
template <typename Body>
ATTRIBUTE_NOINLINE auto MallocTracer::WithInstanceSlowBody(Body body)
    -> decltype(body(static_cast<MallocTracer*>(nullptr))) {
  using R = decltype(body(static_cast<MallocTracer*>(nullptr)));
  if constexpr (std::is_void_v<R>) {
    WithInstanceSlow(FunctionRef<void(MallocTracer*)>(body));
  } else {
    R result;
    WithInstanceSlow([&](MallocTracer* tracer) { result = body(tracer); });
    return result;
  }
}

ALWAYS_INLINE
void MallocTracer::AppendWords(int count, uint64_t first, uint64_t second) {
  if (PREDICT_FALSE(!HasSpaceFor(count))) {
    RefreshBuffer();
  }

  char* p = buf_ptr_;

  p = AltVarintCodec::EncodeUnsigned(p, first);
  if (count > 1) {
    p = AltVarintCodec::EncodeUnsigned(p, second);
  }

  SetBufPtr(p);
}

ALWAYS_INLINE
uint64_t MallocTracer::TraceMalloc(size_t size) {
  if (!--counter_) {
    RefreshTokenAndDec();
  }
  uint64_t token = token_base_ - counter_;

  uint64_t to_encode = MallocTraceEncoder::EncodeMalloc(size, &prev_size_);
  AppendWords(1, to_encode, to_encode);
  return token;
}

ALWAYS_INLINE
void MallocTracer::TraceFree(uint64_t token) {
  uint64_t to_encode = MallocTraceEncoder::EncodeFree(token, &prev_token_);
  AppendWords(1, to_encode, to_encode);
}

ALWAYS_INLINE
void MallocTracer::TraceFreeSized(uint64_t token) {
  uint64_t to_encode = MallocTraceEncoder::EncodeFreeSized(token, &prev_token_);
  AppendWords(1, to_encode, to_encode);
}

ALWAYS_INLINE
uint64_t MallocTracer::TraceRealloc(uint64_t old_token, size_t new_size) {
  if (!--counter_) {
    RefreshTokenAndDec();
  }
  uint64_t token = token_base_ - counter_;

  MallocTraceEncoder::pair p = MallocTraceEncoder::EncodeRealloc(old_token, new_size, &prev_size_, &prev_token_);
  AppendWords(2, p.first, p.second);
  return token;
}

ALWAYS_INLINE
uint64_t MallocTracer::TraceMemalign(size_t size, size_t alignment) {
  if (!--counter_) {
    RefreshTokenAndDec();
  }
  uint64_t token = token_base_ - counter_;

  MallocTraceEncoder::pair p = MallocTraceEncoder::EncodeMemalign(size, alignment, &prev_size_);
  AppendWords(2, p.first, p.second);
  return token;
}

}  // namespace tcmalloc

#endif  // TCMALLOC_TRACING_MALLOC_TRACER_H_
