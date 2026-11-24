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

// Malloc tracing overlay (see alloc_overlays.h). Every chunk we hand
// out is prefixed by 16 bytes of metadata: offset from the start of
// the underlying chunk to the metadata (non-zero only for aligned
// allocations with alignment > 16) and the trace token of the
// allocation. Tokens are produced by MallocTracer which records all
// malloc/free/realloc events into the trace.
//
// With --enable-tracing (ENABLE_TRACING), it is built into every
// tcmalloc variant. ThreadCache::InitModule calls SetupTracingMalloc
// during very first malloc call, which installs the overlay when
// TCMALLOC_TRACE_OUTPUT environment variable is set.

#include "config.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <string_view>
#include <vector>

#include <gperftools/malloc_extension.h>

#include "alloc_overlays.h"
#include "base/basictypes.h"
#include "base/static_storage.h"
#include "internal_logging.h"
#include "getenv_safe.h"
#include "tracing/malloc_tracer.h"
#include "tcmalloc_internal.h"

namespace tcmalloc {
namespace {

// The next overlay down. Set once by SetupTracingMalloc before any
// pointer is handed out.
const AllocOps* tracing_next;

constexpr size_t kMetaSize = 2 * sizeof(uint64_t);
static_assert(kMetaSize == 16);

struct TracingMeta {
  uint64_t offset;  // from the start of the underlying chunk to this header
  uint64_t token;
};

ALWAYS_INLINE TracingMeta* MetaOf(const void* ptr) {
  return reinterpret_cast<TracingMeta*>(const_cast<char*>(static_cast<const char*>(ptr)) - kMetaSize);
}

// Total overhead (metadata plus alignment padding) of the chunk with
// given meta.
ALWAYS_INLINE size_t ExtraOf(const TracingMeta* meta) { return meta->offset + kMetaSize; }

ALWAYS_INLINE char* RawOf(const void* ptr, const TracingMeta* meta) {
  return reinterpret_cast<char*>(const_cast<TracingMeta*>(meta)) - meta->offset;
}

// How much extra space we ask the next layer for, given (possibly 0)
// alignment. For alignments up to 16 we simply prepend metadata
// (and ask the next layer for same alignment). For larger alignments
// we ask for "align" extra bytes, so that user's chunk starts at
// aligned boundary and metadata is placed right before it.
ALWAYS_INLINE size_t ExtraFor(size_t align) { return align <= kMetaSize ? kMetaSize : align; }

// Memalign family (and aligned new). Note, align can be 0 here
// (e.g. memalign(0, size)), and we still trace it as memalign.
ATTRIBUTE_NOINLINE void* TracingMemalign(size_t size, size_t align, uint32_t flags) {
  size_t extra = ExtraFor(align);
  if (PREDICT_FALSE(size + extra < size)) {
    return nullptr;  // overflow
  }
  // Note, we deliberately drop "kind fidelity" by passing kKindMalloc
  // down, so that everything is cheaper and simpler.
  char* raw = static_cast<char*>(tracing_next->Malloc(size + extra, align, kKindMalloc | kAligned));
  if (PREDICT_FALSE(raw == nullptr)) {
    return nullptr;
  }

  return MallocTracer::WithInstance([size, align, raw, extra](MallocTracer* tracer) -> void* {
    uint64_t token = tracer ? tracer->TraceMemalign(size, align) : MallocTracer::kUntracedToken;
    char* ptr = raw + extra;
    TracingMeta* meta = MetaOf(ptr);
    meta->offset = extra - kMetaSize;
    meta->token = token;
    return ptr;
  });
}

void* TracingMalloc(size_t size, size_t align, uint32_t flags) {
  if (PREDICT_FALSE(AllocFlagsHasAligned(flags))) {
    return TracingMemalign(size, align, flags);
  }

  // Regular (not aligned) allocation. Which is by far the most
  // common case. Here, metadata is always right at the start of
  // underlying chunk.
  ASSERT(align == 0);
  if (PREDICT_FALSE(size + kMetaSize < size)) {
    return nullptr;  // overflow
  }
  // Note, we deliberately drop "kind fidelity" by passing kKindMalloc
  // down, so that everything is cheaper and simpler.
  char* raw = static_cast<char*>(tracing_next->Malloc(size + kMetaSize, 0, kKindMalloc));
  if (PREDICT_FALSE(raw == nullptr)) {
    return nullptr;
  }

  return MallocTracer::WithInstance([size, raw](MallocTracer* tracer) -> void* {
    uint64_t token = tracer ? tracer->TraceMalloc(size) : MallocTracer::kUntracedToken;
    TracingMeta* meta = reinterpret_cast<TracingMeta*>(raw);
    meta->offset = 0;
    meta->token = token;
    return raw + kMetaSize;
  });
}

// Why this is needed given tracing_next->Free is tail call anyways?
// Because otherwise gcc somehow tries some odd tricks, perhaps
// "devirtualization", guessing that tracing_next->Free might be
// TracingFree.
void InvokeNextFree(void* ptr) {
  auto free_ptr = tracing_next->Free;
#ifdef __GNUC__
  asm("" : "+r"(free_ptr));
#endif
  free_ptr(ptr, 0, 0, 0);
}

void TracingFree(void* ptr, size_t size_hint, size_t align, uint32_t flags) {
  // TODO: decide if we want this runtime branch. In practice
  // recording whether free was sized or not, seems not super
  // important.
  //
  // Also note, we don't really encode size in the free sized
  // event. So those branches only differ by 3 lower bits of encoded
  // word, so this can be optimized and streamlined with a bit of
  // work, likely branchless too.
  if (false && AllocFlagsHasSized(flags)) {
    void* raw = MallocTracer::WithInstance([ptr](MallocTracer* tracer) -> void* {
      TracingMeta* meta = MetaOf(ptr);
      uint64_t token = meta->token;
      if (tracer && token != MallocTracer::kUntracedToken) {
        tracer->TraceFreeSized(token);
      }
      return RawOf(ptr, meta);
    });
    InvokeNextFree(raw);
  } else {
    void* raw = MallocTracer::WithInstance([ptr](MallocTracer* tracer) -> void* {
      TracingMeta* meta = MetaOf(ptr);
      uint64_t token = meta->token;
      // Note, allocations of untraced threads have kUntracedToken, and
      // we never record their frees (even on traced threads).
      if (tracer && token != MallocTracer::kUntracedToken) {
        tracer->TraceFree(token);
      }
      return RawOf(ptr, meta);
    });
    InvokeNextFree(raw);
  }
}

bool TracingResizeInPlace(void* ptr, size_t new_size) {
  TracingMeta* meta = MetaOf(ptr);
  size_t extra = ExtraOf(meta);
  if (PREDICT_FALSE(new_size + extra < new_size)) {
    return false;
  }
  if (!tracing_next->ResizeInPlace(RawOf(ptr, meta), new_size + extra)) {
    // Dispatcher will do malloc + free, which we'll trace as such.
    return false;
  }
  uint64_t old_token = meta->token;
  meta->token = MallocTracer::WithInstance([old_token, new_size](MallocTracer* tracer) {
    if (old_token == MallocTracer::kUntracedToken) {
      return old_token;  // allocation of untraced thread stays untraced
    }
    if (!tracer) {
      // Untraced thread resizing traced allocation. We keep the old
      // token, so that the eventual free still matches the traced
      // malloc (just with stale size).
      return old_token;
    }
    return tracer->TraceRealloc(old_token, new_size);
  });
  return true;
}

size_t TracingNAllocX(size_t size, size_t align) {
  size_t extra = ExtraFor(align);
  if (PREDICT_FALSE(size + extra < size)) {
    // We never allocate this, so there is no UsableSize to be
    // consistent with.
    return size;
  }
  return tracing_next->NAllocX(size + extra, align) - extra;
}

size_t TracingUsableSize(const void* ptr) {
  const TracingMeta* meta = MetaOf(ptr);
  return tracing_next->UsableSize(RawOf(ptr, meta)) - ExtraOf(meta);
}

bool TracingOwns(const void* ptr) {
  // Note, ptr can be anything, so we must make sure memory is safe
  // to read before looking at our metadata.
  const char* p = static_cast<const char*>(ptr);
  uintptr_t addr = reinterpret_cast<uintptr_t>(p);

  if (tracing_next == &base_ops) {
    // Base tcmalloc owns interior pointers too. So if the place
    // where our metadata would be is owned, it is safe to read
    // it. Then we sanity-check the offset and verify that the raw
    // chunk is owned too.
    if (addr < kMetaSize || !tracing_next->Owns(p - kMetaSize)) {
      return false;
    }
    const TracingMeta* meta = MetaOf(ptr);
    uint64_t extra = meta->offset + kMetaSize;
    if (meta->offset != 0 && (extra & (extra - 1)) != 0) {
      return false;  // our offsets are always 0 or (power of 2) - 16
    }
    if (extra > addr) {
      return false;
    }
    return tracing_next->Owns(p - extra);
  }

  // Otherwise, the next layer is the debug allocation overlay, which
  // owns exactly the pointers it handed out. So we try every possible
  // raw chunk start (our extra space is always a power of 2 >= 16),
  // and only read metadata when the raw chunk is owned and large
  // enough to hold it.
  for (uintptr_t extra = kMetaSize; extra != 0 && extra <= addr; extra <<= 1) {
    const char* raw = p - extra;
    if (!tracing_next->Owns(raw) || tracing_next->UsableSize(raw) < extra) {
      continue;
    }
    return MetaOf(ptr)->offset == extra - kMetaSize;
  }
  return false;
}

const AllocOps tracing_overlay = {
    TracingMalloc, TracingFree, TracingResizeInPlace, TracingNAllocX, TracingUsableSize, TracingOwns,
};

// MallocExtension instance which wraps the previous one (i.e. like
// overlays do for allocation ops) and records some notable calls
// into the trace as "misc" events.
class TracingMallocExtension : public MallocExtension {
 public:
  explicit TracingMallocExtension(MallocExtension* next) : next_(next) {}

  bool VerifyAllMemory() override { return next_->VerifyAllMemory(); }
  bool VerifyNewMemory(const void* p) override { return next_->VerifyNewMemory(p); }
  bool VerifyArrayNewMemory(const void* p) override { return next_->VerifyArrayNewMemory(p); }
  bool VerifyMallocMemory(const void* p) override { return next_->VerifyMallocMemory(p); }
  bool MallocMemoryStats(int* blocks, size_t* total, int histogram[kMallocHistogramSize]) override {
    return next_->MallocMemoryStats(blocks, total, histogram);
  }

  void GetStats(char* buffer, int buffer_length) override { next_->GetStats(buffer, buffer_length); }
  void GetHeapSample(MallocExtensionWriter* writer) override { next_->GetHeapSample(writer); }
  void GetHeapGrowthStacks(MallocExtensionWriter* writer) override { next_->GetHeapGrowthStacks(writer); }
  void Ranges(void* arg, RangeFunction func) override { next_->Ranges(arg, func); }

  bool GetNumericProperty(const char* property, size_t* value) override {
    return next_->GetNumericProperty(property, value);
  }
  bool SetNumericProperty(const char* property, size_t value) override {
    return next_->SetNumericProperty(property, value);
  }

  void MarkThreadIdle() override {
    TraceMisc("(mark-thread-idle)");
    next_->MarkThreadIdle();
  }
  void MarkThreadBusy() override {
    TraceMisc("(mark-thread-busy)");
    next_->MarkThreadBusy();
  }
  void MarkThreadTemporarilyIdle() override {
    TraceMisc("(mark-thread-temporarily-idle)");
    next_->MarkThreadTemporarilyIdle();
  }

  SysAllocator* GetSystemAllocator() override { return next_->GetSystemAllocator(); }
  void SetSystemAllocator(SysAllocator* a) override { next_->SetSystemAllocator(a); }

  // Note, we don't override ReleaseFreeMemory. Default implementation
  // calls our ReleaseToSystem, which traces it.
  void ReleaseToSystem(size_t num_bytes) override {
    char buf[64];
    int written = snprintf(buf, sizeof(buf), "(release-to-system %zu)", num_bytes);
    TraceMisc(std::string_view(buf, std::min<size_t>(written, sizeof(buf) - 1)));
    next_->ReleaseToSystem(num_bytes);
  }
  void SetMemoryReleaseRate(double rate) override { next_->SetMemoryReleaseRate(rate); }
  double GetMemoryReleaseRate() override { return next_->GetMemoryReleaseRate(); }

  size_t GetEstimatedAllocatedSize(size_t size) override { return next_->GetEstimatedAllocatedSize(size); }
  size_t GetAllocatedSize(const void* p) override { return next_->GetAllocatedSize(p); }
  Ownership GetOwnership(const void* p) override { return next_->GetOwnership(p); }
  void GetFreeListSizes(std::vector<FreeListInfo>* v) override { next_->GetFreeListSizes(v); }

  void** ReadStackTraces(int* sample_period) override { return next_->ReadStackTraces(sample_period); }
  void** ReadHeapGrowthStackTraces() override { return next_->ReadHeapGrowthStackTraces(); }
  size_t GetThreadCacheSize() override { return next_->GetThreadCacheSize(); }

 private:
  static void TraceMisc(std::string_view msg) {
    MallocTracer::WithInstance([msg](MallocTracer* tracer) {
      if (tracer) {
        tracer->TraceMisc(msg);
      }
    });
  }

  MallocExtension* const next_;
};

StaticStorage<TracingMallocExtension> tracing_extension_storage;

}  // namespace

void SetupTracingMalloc() {
  // Note: this runs under threads_lock and pageheap_lock (during very
  // first malloc), so we must not malloc here. MallocTracer sets
  // itself up lazily on first traced event. And module initializers
  // in tracing/malloc_tracer.cc and tracing/tracer_buffer.cc start
  // the dumper and saver threads, and open the output file.
  if (TCMallocGetenvSafe("TCMALLOC_TRACE_OUTPUT") == nullptr) {
    return;
  }
  MallocTracer::Activate();
  tracing_next = InstallOverlay(&tracing_overlay);

  // Wrap whatever MallocExtension instance was set up before us. Note,
  // we're called during very first malloc, so there are no other
  // threads to observe the intermediate state.
  MallocExtension* prev = ReplaceMallocExtension(nullptr);
  CHECK_CONDITION(prev != nullptr);
  ReplaceMallocExtension(tracing_extension_storage.Construct(prev));
}

}  // namespace tcmalloc
