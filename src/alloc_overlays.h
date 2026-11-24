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
#ifndef TCMALLOC_ALLOC_OVERLAYS_H_
#define TCMALLOC_ALLOC_OVERLAYS_H_

// Internal (not public, not removable) allocator overlays.
//
// An overlay is a table of operations (AllocOps) that sits on
// top of the actual tcmalloc ("base", base_ops) and implements
// allocation by calling down into the next one. Overlays are chained
// from the top overlay down to the base. The debug allocator
// (debugallocation.cc) is such an overlay. MallocHook new/delete hooks
// are run by an observe-only overlay (hooks_overlay) that is put on top of
// everything while any new/delete hook is registered.
//
// Above the overlays sits the dispatcher (in tcmalloc.cc). It owns the
// policy common to all overlays: OOM handling (errno, new_handler,
// bad_alloc, tc_new_mode), calloc, memalign family argument
// validation and the realloc algorithm. Overlays never do OOM handling
// or retries. Realloc is done by the dispatcher in terms of
// ResizeInPlace, Malloc, UsableSize and Free.
//
// Allocation fast paths check one global pointer (active_overlay) and
// tail-call the dispatcher when it is non-null.
//
// Overlays are installed only from ThreadCache::InitModule
// (i.e. during very first malloc, when there are no other threads
// yet), before any pointer is handed out, and are never removed. A
// overlay stores the "next" pointer returned by InstallOverlay in a
// plain global.
//
// Rules for overlays:
//
// * Overlays don't touch errno and don't call new_handler.

#include "config.h"

#include <stddef.h>
#include <stdint.h>

#include <atomic>

namespace tcmalloc {

enum : uint32_t {
  kKindMalloc = 0,
  kKindNew = 1,
  kKindNewArray = 2,
  kKindMask = 3,
  kSized = 1u << 2,  // size_hint is meaningful (even when 0)
  kAligned = 1u << 3,
};

constexpr bool AllocFlagsHave(uint32_t flags, uint32_t mask) {
  return (flags & mask) != 0;
}
constexpr bool AllocFlagsHasSized(uint32_t flags) { return AllocFlagsHave(flags, kSized); }
constexpr bool AllocFlagsHasAligned(uint32_t flags) {
  return AllocFlagsHave(flags, kAligned);
}

struct AllocOps {
  // align: 0 = default alignment; otherwise a power of 2 the caller asked for.
  // Returns nullptr on failure; never does OOM handling.
  void* (*Malloc)(size_t size, size_t align, uint32_t flags);
  // Free, ResizeInPlace and UsableSize only get pointers that were
  // returned by this overlay's Malloc (dispatcher handles emergency
  // malloc's chunks itself). Overlays may treat anything else as a bug.
  //
  // ptr != nullptr. size_hint valid iff (flags & kSized). align = 0 unless aligned delete.
  void (*Free)(void* ptr, size_t size_hint, size_t align, uint32_t flags);
  // ptr != nullptr, new_size != 0. true = resized in place (ptr stays valid with new size).
  bool (*ResizeInPlace)(void* ptr, size_t new_size);
  // NAllocX and UsableSize must be consistent: for every p returned
  // by Malloc(size, align, ...), UsableSize(p) == NAllocX(size,
  // align). Dispatcher checks that in debug builds. (Emergency
  // malloc's chunks, which never come from overlays, are exempt.)
  size_t (*NAllocX)(size_t size, size_t align);
  size_t (*UsableSize)(const void* ptr);  // ptr != nullptr
  // Unlike the rest, Owns may get any pointer (except nullptr and
  // emergency malloc's chunks): foreign, interior, already freed or
  // garbage. It must not crash or complain on those. Returns true if
  // ptr is (as far as the overlay can tell) a pointer it handed
  // out. It backs MallocExtension::GetOwnership, which OSX's malloc
  // zones use to find which zone a pointer belongs to.
  bool (*Owns)(const void* ptr);
};

// The actual tcmalloc. Defined in tcmalloc.cc.
extern const AllocOps base_ops;

// Defined in tcmalloc.cc. We start with those ops "installed". Their
// only job is to ensure that we're initialized and then pass the
// first call to the right ops (normally base_ops).
extern const AllocOps initial_ops;

// Only from ThreadCache::InitModule (see above). Returns the previous
// top (initially &base_ops). Overlay stores it in a plain global. Not
// supported with windows patching. Defined in tcmalloc.cc.
const AllocOps* InstallOverlay(const AllocOps* ops);

// ---- Dispatch state. Everything below is defined in tcmalloc.cc. ----

// The overlay that runs MallocHook new/delete hooks. Calls down to top_ops.
extern const AllocOps hooks_overlay;

// Top of the overlay chain. Normally &base_ops. Never nullptr.
extern const AllocOps* top_ops;

// Read by fast paths. nullptr means no overlays: fast paths go straight
// to base. Otherwise it is &hooks_overlay while any new/delete hook is
// registered, or else top_ops (when it isn't &base_ops).
// Starts as &initial_ops, which is reset on the first malloc call.
// Recomputed under hooklist_spinlock after every new/delete hook list
// change and at install time. Relaxed: ordering
// vs hook list contents is best effort for concurrently added hooks
// (same as the old new_hooks_/delete_hooks_ emptiness checks).
//
// Note, this is really std::atomic but declare it "plain" so we can
// have no doubts about initializing statically (no runtime
// constructors) regardless of early compiler bugs.
extern const AllocOps* active_overlay;

static inline const AllocOps* ActiveOverlay() {
  return reinterpret_cast<std::atomic<const AllocOps*>*>(&active_overlay)->load(std::memory_order_relaxed);
}

// implemented in tcmalloc.cc and malloc_hook_test.cc
void UpdateActiveOverlayLocked();

// Same, but grabs hooklist_spinlock. Defined in malloc_hook.cc.
void UpdateActiveOverlay();

}  // namespace tcmalloc

#endif  // TCMALLOC_ALLOC_OVERLAYS_H_
