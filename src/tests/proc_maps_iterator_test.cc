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
#include "config_for_unittests.h"

#include "base/proc_maps_iterator.h"

#include <stdio.h>

#include <string>

#include "base/generic_writer.h"

int variable;

// There is not much we can thoroughly test. But it is easy to test
// that we're seeing at least .bss bits. We can also check that we saw
// at least one executable mapping.
void TestForEachMapping() {
  bool seen_variable = false;
  bool seen_executable = false;
  bool ok = tcmalloc::ForEachProcMapping([&] (const tcmalloc::ProcMapping& mapping) {
    const uintptr_t variable_addr = reinterpret_cast<uintptr_t>(&variable);
    if (mapping.start <= variable_addr && variable_addr <= mapping.end) {
      seen_variable = true;
    }
    if (std::string(mapping.flags).find_first_of('x') != std::string::npos) {
      seen_executable = true;
    }
  });
  RAW_CHECK(ok, "failed to open proc/self/maps");
  RAW_CHECK(seen_variable, "");
  RAW_CHECK(seen_executable, "");
}

void TestSaveMappingsNonEmpty() {
  std::string s;
  {
    tcmalloc::StringGenericWriter writer(&s);
    tcmalloc::SaveProcSelfMaps(&writer);
  }
  // Lets at least ensure we got something
  CHECK_NE(s.size(), 0);
  printf("Got the following:\n%s\n---\n", s.c_str());
}

int main() {
  TestForEachMapping();
  TestSaveMappingsNonEmpty();
  printf("PASS\n");
}
