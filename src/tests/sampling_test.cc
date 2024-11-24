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
// This tests ReadStackTraces and ReadGrowthStackTraces.  It does this
// by doing a bunch of allocations and then calling those functions.
// A driver shell-script can call this, and then call pprof, and
// verify the expected output.  The output is written to
// argv[1].heap and argv[1].growth

#include "config_for_unittests.h"

#include "gperftools/malloc_extension.h"

#include <stdio.h>
#include <stdlib.h>
#include <regex.h>

#include <memory>
#include <string>
#include <string_view>

#include "tests/testutil.h"

#include "base/logging.h"
#include "base/cleanup.h"
#include "testing_portal.h"

static std::string NaiveShellQuote(std::string_view arg) {
  // We're naive, so don't support paths with quote char. With that
  // we're able to quote by simply wrapping things with quotes.
  CHECK_EQ(arg.find('"'), std::string_view::npos);
  std::string retval({'"'});
  retval.append(arg);
  retval.append({'"'});
  return retval;
}

extern "C" ATTRIBUTE_NOINLINE
void* AllocateAllocate() {
  auto local_noopt = [] (void* ptr) ATTRIBUTE_NOINLINE {
    return noopt(ptr);
  };
  return local_noopt(malloc(10000));
}

#ifndef PPROF_PATH
#define PPROF_PATH pprof
#endif

#define XSTR(x) #x
#define STR(x) XSTR(x)

const char kPProfPath[] = STR(PPROF_PATH);

static void VerifyWithPProf(std::string_view argv0, std::string_view path) {
  std::string cmdline = NaiveShellQuote(kPProfPath) + " --text " + NaiveShellQuote(argv0) + " " + NaiveShellQuote(path);
  printf("pprof cmdline: %s\n", cmdline.c_str());
  FILE* p = popen(cmdline.c_str(), "r");
  if (!p) {
    perror("popen");
    abort();
  }
  tcmalloc::Cleanup close_pipe([p] () { (void)pclose(p); });

  constexpr int kBufSize = 1024;
  std::string contents;
  char buf[kBufSize];
  while (!feof(p)) {
    size_t amt = fread(buf, 1, kBufSize, p);
    contents.append(buf, buf + amt);
  }

  fprintf(stderr, "pprof output:\n%s\n\n", contents.c_str());

  regmatch_t pmatch[3];
  regex_t regex;
  CHECK_EQ(regcomp(&regex, "([0-9.]+)(MB)? *([0-9.]+)% *_*AllocateAllocate", REG_NEWLINE | REG_EXTENDED), 0);
  CHECK_EQ(regexec(&regex, contents.c_str(), 3, pmatch, 0), 0);

  fprintf(stderr,"AllocateAllocate regex match: %.*s\n",
          int(pmatch[0].rm_eo - pmatch[0].rm_so),
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

struct TempFile {
  FILE* f;
  std::string const path;

  TempFile(FILE* f, std::string_view path) : f(f), path(path) {}

  ~TempFile() {
    if (f) {
      (void)fclose(f);
    }
  }

  FILE* ReleaseFile() {
    FILE* retval = f;
    f = nullptr;
    return retval;
  }

  static TempFile Create(std::string_view base_template) {
    CHECK_EQ(base_template.substr(base_template.size() - 6), "XXXXXX");

    const char* raw_tmpdir = getenv("TMPDIR");
    if (raw_tmpdir == nullptr) {
      raw_tmpdir = "/tmp";
    }
    std::string_view tmpdir{raw_tmpdir};
    size_t len = tmpdir.size() + 1 + base_template.size() + 1;
    std::unique_ptr<char[]> path_template{new char[len]};
    auto it = std::copy(tmpdir.begin(), tmpdir.end(), path_template.get());
    *it++ = '/';
    it = std::copy(base_template.begin(), base_template.end(), it);
    *it++ = '\0';
    CHECK_EQ(it, path_template.get() + len);

    int fd = mkstemp(path_template.get());
    if (fd < 0) {
      perror("mkstemp");
    }
    CHECK_GE(fd, 0);

    return TempFile{fdopen(fd, "r+"), std::string(path_template.get(), len-1)};
  }
};

int main(int argc, char** argv) {
  tcmalloc::TestingPortal::Get()->GetSampleParameter() = 512 << 10;
  // Make sure allocations we sample are done on fresh thread cache, so that
  // sampling parameter update is taken into account.
  MallocExtension::instance()->MarkThreadIdle();

  for (int i = 0; i < 8000; i++) {
    AllocateAllocate();
  }

  TempFile heap_tmp = TempFile::Create("sampling_test.heap.XXXXXX");
  TempFile growth_tmp = TempFile::Create("sampling_test.growth.XXXXXX");
  tcmalloc::Cleanup unlink_temps{[&] () {
    (void)unlink(heap_tmp.path.c_str());
    (void)unlink(growth_tmp.path.c_str());
  }};

  std::string s;
  MallocExtension::instance()->GetHeapSample(&s);
  fwrite(s.data(), 1, s.size(), heap_tmp.f);
  fclose(heap_tmp.ReleaseFile());

  s.clear();
  MallocExtension::instance()->GetHeapGrowthStacks(&s);
  fwrite(s.data(), 1, s.size(), growth_tmp.f);
  fclose(growth_tmp.ReleaseFile());

  VerifyWithPProf(argv[0], heap_tmp.path);
  VerifyWithPProf(argv[0], growth_tmp.path);
}
