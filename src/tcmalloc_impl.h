/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
 * Copyright (c) 2026, gperftools Contributors
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
#ifndef TCMALLOC_IMPL_H_
#define TCMALLOC_IMPL_H_

// Declarations of tcmalloc's own MallocExtension and TestingPortal
// implementations (defined in tcmalloc.cc). Overlays, such as
// the debug allocator, extend those.

#include "config.h"

#include <stddef.h>
#include <stdint.h>

#include <vector>

#include <gperftools/malloc_extension.h>

#include "base/basictypes.h"
#include "base/function_ref.h"
#include "testing_portal.h"

namespace tcmalloc {

class ATTRIBUTE_VISIBILITY_HIDDEN TestingPortalImpl : public TestingPortal {
 public:
  ~TestingPortalImpl() override = default;

  bool HaveSystemRelease() override;
  bool IsDebuggingMalloc() override;
  size_t GetMinSpanSize() override;
  size_t GetMinAlign() override;
  size_t GetMaxSize() override;
  int64_t& GetSampleParameter() override;
  double& GetReleaseRate() override;
  int32_t& GetMaxFreeQueueSize() override;

  bool HasEmergencyMalloc() override;
  bool IsOverlayed() override;

  bool IsEmergencyPtr(void* ptr) override;

  void WithEmergencyMallocEnabled(FunctionRef<void()> body) override;

  uint32_t GetSizeClass(void* p) override;

  void* RunReallocWithCallback(void* old_ptr, size_t new_size, void (*invalid_free_fn)(void*),
                               size_t (*invalid_get_size_fn)(const void*)) override;

  void WithOverlay(const AllocOps* ops, const AllocOps** next, FunctionRef<void()> body) override;

  static TestingPortalImpl* Get();
};

}  // namespace tcmalloc

// TCMalloc's support for extra malloc interfaces
class TCMallocImplementation : public MallocExtension {
 private:
  // ReleaseToSystem() might release more than the requested bytes because
  // the page heap releases at the span granularity, and spans are of wildly
  // different sizes.  This member keeps track of the extra bytes bytes
  // released so that the app can periodically call ReleaseToSystem() to
  // release memory at a constant rate.
  // NOTE: Protected by Static::pageheap_lock().
  size_t extra_bytes_released_;

 public:
  TCMallocImplementation() : extra_bytes_released_(0) {}

  void GetStats(char* buffer, int buffer_length) override;

  // We may print an extra, tcmalloc-specific warning message here.
  void GetHeapSample(MallocExtensionWriter* writer) override;

  void** ReadStackTraces(int* sample_period) override;

  void** ReadHeapGrowthStackTraces() override;

  size_t GetThreadCacheSize() override;

  void Ranges(void* arg, RangeFunction func) override;

  bool GetNumericProperty(const char* name, size_t* value) override;

  bool SetNumericProperty(const char* name, size_t value) override;

  // Note, in gperftools 2.5 we introduced this 'ligher-weight'
  // equivalent of MarkThreadIdle, but as of now mongo folk tells us
  // they don't use it anymore. And there was indeed possible issue
  // with this approach since it didn't 'return' thread's share of
  // total thread cache back to common pool. But that was almost
  // exactly the difference between regular mark idle and mark
  // "temporarily" idle. So we now go back to original mark idle, but
  // keep API for ABI and API compat sake.
  void MarkThreadTemporarilyIdle() override;

  void MarkThreadIdle() override;

  void MarkThreadBusy() override;

  SysAllocator* GetSystemAllocator() override;

  void SetSystemAllocator(SysAllocator* alloc) override;

  void ReleaseToSystem(size_t num_bytes) override;

  void SetMemoryReleaseRate(double rate) override;

  double GetMemoryReleaseRate() override;

  size_t GetEstimatedAllocatedSize(size_t size) override;

  // Those two go to the top of overlays (see alloc_overlays.h).
  size_t GetAllocatedSize(const void* ptr) override;

  // This duplicates some of the logic in GetSizeWithCallback, but is
  // faster.  This is important on OS X, where this function is called
  // on every allocation operation.
  Ownership GetOwnership(const void* ptr) override;

  void GetFreeListSizes(std::vector<MallocExtension::FreeListInfo>* v) override;
};

#endif  // TCMALLOC_IMPL_H_
