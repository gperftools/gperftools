// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
/* Copyright (c) 2006, Google Inc.
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

#if !defined(_BASE_LOW_LEVEL_ALLOC_H_)
#define _BASE_LOW_LEVEL_ALLOC_H_

// A simple thread-safe memory allocator that does not depend on
// mutexes or thread-specific data.  It is intended to be used
// sparingly, and only when malloc() would introduce an unwanted
// dependency, such as inside the heap-checker.

#include "config.h"

#include <stddef.h>

#include <utility>

#include "base/basictypes.h"

namespace tcmalloc {

class LowLevelAlloc {
 public:
  class PagesAllocator {
  public:
    virtual ~PagesAllocator();
    virtual std::pair<void *,size_t> MapPages(size_t size) = 0;
    virtual void UnMapPages(void *addr, size_t size) = 0;
  };

  struct Arena;       // an arena from which memory may be allocated

  // Returns a pointer to a block of at least "request" bytes
  // that have been newly allocated from the specific arena.
  // for Alloc() call the DefaultArena() is used.
  // Returns 0 if passed request==0.
  // Does not return 0 under other circumstances; it crashes if memory
  // is not available.
  //
  // Passing nullptr arena implies use of DefaultArena
  static void *AllocWithArena(size_t request, Arena *arena);
  // Equivalent to AllocWithArena(request, nullptr)
  static void *Alloc(size_t request);

  static size_t UsableSize(const void *p);

  // Deallocates a region of memory that was previously allocated with
  // Alloc().   Does nothing if passed 0.   "s" must be either 0,
  // or must have been returned from a call to Alloc() and not yet passed to
  // Free() since that call to Alloc().  The space is returned to the arena
  // from which it was allocated.
  static void Free(void *s);

  static Arena *NewArena();

  // note: pages allocator will never be destroyed and allocated pages will never be freed
  // When allocator is nullptr, it's same as NewArena
  static Arena *NewArenaWithCustomAlloc(PagesAllocator *allocator);

  // Destroys an arena allocated by NewArena and returns true,
  // provided no allocated blocks remain in the arena.
  // If allocated blocks remain in the arena, does nothing and
  // returns false.
  // It is illegal to attempt to destroy the DefaultArena().
  static bool DeleteArena(Arena *arena);

  static PagesAllocator *GetDefaultPagesAllocator(void);

private:
  static Arena *DefaultArena();

  LowLevelAlloc();      // no instances
};

}  // namespace tcmalloc

#endif
