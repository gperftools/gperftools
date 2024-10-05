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

// This file "re-implements" allocation functions in
// libbacktrace/alloc.c. We add "feature" that tracks allocations on
// per-backtrace_state instances. Allowing us to dispose
// backtrace_state instances. (which libbacktrace normally doesn't support)

#include "config.h"

#include <stdlib.h>

#include <algorithm>
#include <atomic>
#include <tuple>

#include "base/low_level_alloc.h"
#include "libbacktrace_api.h"

namespace tcmalloc {

// This is PagesAllocator allocator that tracks all mmap-ed chunks it
// produced and just mass-unmaps them all when destroyed.
//
// It's purpose is to let us clean libbacktrace state after
// symbolization. Which is not "natively" supported by libbacktrace,
// but we're able to take advantage of our memory allocation
// integration. And lack of usage of mmapio.c.
class BTPagesAllocator : public LowLevelAlloc::PagesAllocator {
private:
  struct Header {
    const size_t size;
    Header* next{};

    explicit Header(size_t size) : size(size) {}
  };

public:
  // Allocate() mmap-s a chunk of memory then constructs
  // BTPagesAllocator instance inside and returns it. Rest of the
  // chunk's memory is initial_chunk_ that we're giving out with first
  // MapPages call.
  static BTPagesAllocator* Allocate() {
    static constexpr size_t kSize = 8 << 20;
    Header* initial_header = AllocateAsHeader(kSize - sizeof(Header));
    BTPagesAllocator* place = reinterpret_cast<BTPagesAllocator*>(initial_header + 1);
    size_t initial_chunk_size = initial_header->size - sizeof(Header) - sizeof(BTPagesAllocator);

    BTPagesAllocator* result = new (place) BTPagesAllocator({place + 1, initial_chunk_size});
    *result->tail_ = initial_header;
    result->tail_ = &initial_header->next;
    return result;
  }

  std::pair<void *,size_t> MapPages(size_t size) override {
    if (initial_chunk_.second >= size) {
      std::pair<void*, size_t> replacement{};
      std::swap(replacement, initial_chunk_);
      return replacement;
    }

    Header* hdr = AllocateAsHeader(size);

    *tail_ = hdr;
    tail_ = &hdr->next;

    return {hdr + 1, hdr->size - sizeof(Header)};
  }

  void UnMapPages(void *addr, size_t size) override {
    abort();
  }

  void Destroy() {
    LowLevelAlloc::PagesAllocator* parent_allocator = LowLevelAlloc::GetDefaultPagesAllocator();
    Header* hdr = head_;

    this->~BTPagesAllocator();

    while (hdr) {
      Header* next = hdr->next;
      parent_allocator->UnMapPages(hdr, hdr->size);
      hdr = next;
    }
  }

private:
  explicit BTPagesAllocator(std::pair<void*, size_t> initial_chunk) : initial_chunk_(initial_chunk) {}
  ~BTPagesAllocator() override = default;

  static Header* AllocateAsHeader(size_t size) {
    void* memory;
    size_t actual_size;
    LowLevelAlloc::PagesAllocator* parent_allocator = LowLevelAlloc::GetDefaultPagesAllocator();
    std::tie(memory, actual_size) = parent_allocator->MapPages(size + sizeof(Header));
    return new (memory) Header(actual_size);
  }

  Header* head_{};
  // tail_ is pointer to head_ or to Header::next of last header. A
  // place we append to.
  Header** tail_{&head_};

  std::pair<void*, size_t> initial_chunk_;
};

// We prepend this to each backtrace_state that we use which lets us
// pull our allocator bits into memory allocation routines below.
struct StatePrefix {
  BTPagesAllocator* const allocator;
  LowLevelAlloc::Arena* const arena;

  explicit StatePrefix(BTPagesAllocator* allocator, LowLevelAlloc::Arena* arena)
    : allocator(allocator), arena(arena) {}
};

}  // namespace tcmalloc

using tcmalloc::BTPagesAllocator;
using tcmalloc::StatePrefix;
using tcmalloc::LowLevelAlloc;

void *tcmalloc_backtrace_alloc(struct backtrace_state *state,
                               size_t size, backtrace_error_callback error_callback,
                               void *data) {
  static std::atomic<backtrace_state*> initial_state_ptr;

  backtrace_state* initial_state = initial_state_ptr.load(std::memory_order_relaxed);
  if (initial_state == nullptr) {
    initial_state = state;
    initial_state_ptr.store(initial_state);
  }

  if (state == initial_state) {
    // We're asked to allocate backtrace_state instance (see state.c
    // for how it is done, to understand how we're detecting it)
    BTPagesAllocator* allocator = BTPagesAllocator::Allocate();
    LowLevelAlloc::Arena* arena = LowLevelAlloc::NewArenaWithCustomAlloc(allocator);
    void* memory = LowLevelAlloc::AllocWithArena(size + sizeof(StatePrefix), arena);
    auto prefix = new (memory) StatePrefix(allocator, arena);
    void* result = prefix + 1;
    return result;
  }

  StatePrefix* prefix = reinterpret_cast<StatePrefix*>(state) - 1;
  return LowLevelAlloc::AllocWithArena(size, prefix->arena);
}

void tcmalloc_backtrace_free(struct backtrace_state *state,
                             void *p, size_t size,
                             backtrace_error_callback error_callback,
                             void *data) {
  if (p == nullptr) { return; }
  LowLevelAlloc::Free(p);
}

static void resize_to(struct backtrace_state *state,
                      size_t new_size,
                      struct backtrace_vector *vec) {
  void *base = tcmalloc_backtrace_alloc(state, new_size, nullptr, nullptr);
  memcpy(base, vec->base, vec->size);
  tcmalloc_backtrace_free(state, vec->base, vec->alc, nullptr, nullptr);

  vec->base = base;
  vec->alc = new_size - vec->size;
}

void *tcmalloc_backtrace_vector_grow(struct backtrace_state *state,
                                     size_t size, backtrace_error_callback error_callback,
                                     void *data, struct backtrace_vector *vec)
{
  void *ret;

  if (size > vec->alc) {
    size_t new_size = std::max(size * 32, vec->size * 2);
    new_size = std::max(new_size, vec->size + size);

    resize_to(state, new_size, vec);
  }

  ret = static_cast<char *>(vec->base) + vec->size;
  vec->size += size;
  vec->alc -= size;
  return ret;
}

void *tcmalloc_backtrace_vector_finish(struct backtrace_state *state,
                                       struct backtrace_vector *vec,
                                       backtrace_error_callback error_callback,
                                       void *data) {
  void *ret = vec->base;
  vec->base = nullptr;
  vec->size = 0;
  vec->alc = 0;
  return ret;
}

int tcmalloc_backtrace_vector_release(struct backtrace_state *state,
                                      struct backtrace_vector *vec,
                                      backtrace_error_callback error_callback,
                                      void *data) {
  resize_to(state, vec->size, vec);
  return 1;
}

void tcmalloc_backtrace_dispose_state(struct backtrace_state* state) {
  StatePrefix* prefix = reinterpret_cast<StatePrefix*>(state) - 1;
  prefix->allocator->Destroy(); // frees all memory
}
