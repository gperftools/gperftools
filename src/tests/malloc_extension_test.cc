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
// Author: Craig Silverstein
//
// Simple test of malloc_extension.  Includes test of C shims.

#include "config_for_unittests.h"
#include <stdio.h>
#include <sys/types.h>
#include "base/logging.h"
#include <google/malloc_extension.h>
#include <google/malloc_extension_c.h>

using STL_NAMESPACE::vector;

int main(int argc, char** argv) {
  void* a = malloc(1000);

  size_t cxx_bytes_used, c_bytes_used;
  ASSERT_TRUE(MallocExtension::instance()->GetNumericProperty(
      "generic.current_allocated_bytes", &cxx_bytes_used));
  ASSERT_TRUE(MallocExtension_GetNumericProperty(
      "generic.current_allocated_bytes", &c_bytes_used));
  ASSERT_GT(cxx_bytes_used, 1000);
  ASSERT_EQ(cxx_bytes_used, c_bytes_used);

  ASSERT_TRUE(MallocExtension::instance()->VerifyAllMemory());
  ASSERT_TRUE(MallocExtension_VerifyAllMemory());

  ASSERT_GE(MallocExtension::instance()->GetAllocatedSize(a), 1000);
  // This is just a sanity check.  If we allocated too much, tcmalloc is broken
  ASSERT_LE(MallocExtension::instance()->GetAllocatedSize(a), 5000);
  ASSERT_GE(MallocExtension::instance()->GetEstimatedAllocatedSize(1000), 1000);

  for (int i = 0; i < 10; ++i) {
    void *p = malloc(i);
    ASSERT_GE(MallocExtension::instance()->GetAllocatedSize(p),
             MallocExtension::instance()->GetEstimatedAllocatedSize(i));
    free(p);
  }

  // Check the c-shim version too.
  ASSERT_GE(MallocExtension_GetAllocatedSize(a), 1000);
  ASSERT_LE(MallocExtension_GetAllocatedSize(a), 5000);
  ASSERT_GE(MallocExtension_GetEstimatedAllocatedSize(1000), 1000);

  // test invariant: size of freelist = heap_size - allocated_bytes
  free(malloc(32000));
  size_t heap_size = 0;
  size_t allocated = 0;
  ASSERT_TRUE(MallocExtension::instance()->GetNumericProperty(
      "generic.current_allocated_bytes", &allocated));
  ASSERT_TRUE(MallocExtension::instance()->GetNumericProperty(
      "generic.heap_size", &heap_size));
  vector<MallocExtension::FreeListInfo> info;
  MallocExtension::instance()->GetFreeListSizes(&info);

  ASSERT_GE(info.size(), 0);
  int64 free_bytes = 0;
  for (vector<MallocExtension::FreeListInfo>::const_iterator it = info.begin();
       it != info.end();
       ++it) {
    free_bytes += it->total_bytes_free;
  }

  // don't expect an exact equality since the calls to query the heap
  // themselves free and allocate memory
  size_t error = abs((heap_size - allocated) - free_bytes);
  ASSERT_LT(error, 0.15 * heap_size);

  free(a);

  printf("DONE\n");
  return 0;
}
