/* -*- Mode: C; c-basic-offset: 2; indent-tabs-mode: nil -*- */
/* Copyright (c) 2009, Google Inc.
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
 *
 * ---
 * Author: Craig Silverstein
 *
 * This tests the c shims: malloc_extension_c.h and malloc_hook_c.h.
 */

#include "config.h"

#include <gperftools/malloc_extension_c.h>
#include <gperftools/malloc_hook_c.h>
#include <gperftools/tcmalloc.h>

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>   /* for size_t */

#include "base/basictypes.h"
#include "gtest/gtest.h"

static int g_new_hook_calls = 0;
static int g_delete_hook_calls = 0;

void TestNewHook(const void* ptr, size_t size) {
  void* result[5];

  ASSERT_LE(MallocHook_GetCallerStackTrace(
              result,
              sizeof(result)/sizeof(*result), 0),
            2);

  g_new_hook_calls++;
}

void TestDeleteHook(const void* ptr) {
  g_delete_hook_calls++;
}

static
void *forced_malloc(size_t size)
{
  void *rv = tc_malloc(size);
  if (!rv) {
    abort();
  }
  return rv;
}

TEST(TestMalloc, Hook) {
  ASSERT_TRUE(MallocHook_AddNewHook(&TestNewHook));
  ASSERT_TRUE(MallocHook_AddDeleteHook(&TestDeleteHook));

  free(forced_malloc(10));
  free(forced_malloc(20));
  ASSERT_EQ(g_new_hook_calls, 2);
  ASSERT_EQ(g_delete_hook_calls, 2);
  ASSERT_TRUE(MallocHook_RemoveNewHook(&TestNewHook));

  ASSERT_TRUE(MallocHook_RemoveDeleteHook(&TestDeleteHook));

  free(forced_malloc(10));
  free(forced_malloc(20));
  ASSERT_EQ(g_new_hook_calls, 2);

  MallocHook_SetNewHook(&TestNewHook);
  MallocHook_SetDeleteHook(&TestDeleteHook);

  free(forced_malloc(10));
  free(forced_malloc(20));
  ASSERT_EQ(g_new_hook_calls, 4);

  ASSERT_NE(MallocHook_SetNewHook(nullptr), nullptr);
  ASSERT_NE(MallocHook_SetDeleteHook(nullptr), nullptr);
}

TEST(TestMalloc, Extension) {
  int blocks;
  size_t total;
  int hist[64];
  char buffer[200];
  char* x = (char*)forced_malloc(10);

  MallocExtension_VerifyAllMemory();
  MallocExtension_VerifyMallocMemory(x);
  MallocExtension_MallocMemoryStats(&blocks, &total, hist);
  MallocExtension_GetStats(buffer, sizeof(buffer));

  ASSERT_TRUE(
    MallocExtension_GetNumericProperty(
      "generic.current_allocated_bytes",
      &total));

  ASSERT_GE(total, 10) << "GetNumericProperty had bad return for generic.current_allocated_bytes";

  MallocExtension_MarkThreadIdle();
  MallocExtension_MarkThreadBusy();
  MallocExtension_ReleaseToSystem(1);
  MallocExtension_ReleaseFreeMemory();

  ASSERT_GE(MallocExtension_GetEstimatedAllocatedSize(10), 10);

  ASSERT_GE(MallocExtension_GetAllocatedSize(x), 10);
  ASSERT_EQ(MallocExtension_GetOwnership(x), MallocExtension_kOwned);
  ASSERT_EQ(MallocExtension_GetOwnership(hist), MallocExtension_kNotOwned);

  free(x);
}
