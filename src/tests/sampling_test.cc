// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
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
// This tests GetHeapSample and GetHeapGrowthStacks. It does a bunch
// of allocations, saves both profiles to temporary files and checks
// with pprof that the allocations are accounted for.
//
// pprof must be in PATH.

#include "config_for_unittests.h"

#include "gperftools/malloc_extension.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <regex.h>

#include <string>
#include <string_view>

#include "tests/posix_testutil.h"
#include "tests/testutil.h"

#include "base/logging.h"
#include "testing_portal.h"

extern "C" ATTRIBUTE_NOINLINE void* AllocateAllocate() {
  auto local_noopt = [](void* ptr) ATTRIBUTE_NOINLINE { return noopt(ptr); };
  return local_noopt(malloc(10000));
}

static void VerifyWithPProf(std::string_view argv0, const std::string& path) {
  tcmalloc::ProcessResult result = tcmalloc::RunProcess({"pprof", "--text", std::string(argv0), path});
  fprintf(stderr, "pprof output:\n%s\n\n", result.output.c_str());
  if (!result.Succeeded()) {
    fprintf(stderr, "pprof %s. It needs to be in PATH (go install github.com/google/pprof@latest).\n",
            result.Describe().c_str());
    abort();
  }
  const std::string& contents = result.output;

  regmatch_t pmatch[3];
  regex_t regex;
  CHECK_EQ(regcomp(&regex, "([0-9.]+)(MB)? *([0-9.]+)% *_*AllocateAllocate", REG_NEWLINE | REG_EXTENDED), 0);
  CHECK_EQ(regexec(&regex, contents.c_str(), 3, pmatch, 0), 0);

  fprintf(stderr, "AllocateAllocate regex match: %.*s\n", int(pmatch[0].rm_eo - pmatch[0].rm_so),
          contents.data() + pmatch[0].rm_so);

  std::string number{contents.data() + pmatch[1].rm_so, contents.data() + pmatch[1].rm_eo};

  errno = 0;
  char* endptr;
  double megs = strtod(number.c_str(), &endptr);
  CHECK(endptr && *endptr == '\0');
  CHECK_EQ(errno, 0);

  // We allocate 8*10^7 bytes of memory, which is 76M.  Because we
  // sample, the estimate may be a bit high or a bit low: we accept
  // anything from 50M to 109M.
  if (!(50 <= megs && megs < 110)) {
    fprintf(stderr, "expected megs to be between 50 and 110. Got: %f\n", megs);
    abort();
  }
}

static void WriteFile(const std::string& path, const std::string& contents) {
  FILE* f = fopen(path.c_str(), "w");
  if (f == nullptr) {
    perror(path.c_str());
    abort();
  }
  fwrite(contents.data(), 1, contents.size(), f);
  fclose(f);
}

int main(int argc, char** argv) {
  tcmalloc::TestingPortal::Get()->GetSampleParameter() = 512 << 10;
  // Make sure allocations we sample are done on fresh thread cache, so that
  // sampling parameter update is taken into account.
  MallocExtension::instance()->MarkThreadIdle();

  for (int i = 0; i < 8000; i++) {
    AllocateAllocate();
  }

  tcmalloc::TempDir dir("sampling_test");

  std::string s;
  MallocExtension::instance()->GetHeapSample(&s);
  WriteFile(dir.Path("heap"), s);

  s.clear();
  MallocExtension::instance()->GetHeapGrowthStacks(&s);
  WriteFile(dir.Path("growth"), s);

  VerifyWithPProf(argv[0], dir.Path("heap"));
  VerifyWithPProf(argv[0], dir.Path("growth"));
}
