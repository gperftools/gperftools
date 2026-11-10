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

// Test helpers for running sub-processes and managing temporary
// directories. POSIX only.

#ifndef TESTS_POSIX_TESTUTIL_H_
#define TESTS_POSIX_TESTUTIL_H_

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tcmalloc {

// Environment changes for a child process. (name, value) sets the
// variable, (name, std::nullopt) removes it. When a name appears
// several times, the last entry wins.
using EnvOverrides = std::vector<std::pair<std::string, std::optional<std::string>>>;

struct ProcessResult {
  // Raw status as returned by waitpid(), or -1 if the process could
  // not be started.
  int status = -1;
  // Everything the process wrote to stdout and stderr, interleaved.
  std::string output;

  // True if the process exited normally with code 0.
  bool Succeeded() const;
  // How the process ended, e.g. "exited with code 1" or "killed by
  // signal 11".
  std::string Describe() const;
};

// Runs argv[0] with the given arguments and waits for it to
// finish. When argv[0] has no slash, it is searched in PATH. The
// child gets the current environment with env applied on top, and
// /dev/null as stdin.
ProcessResult RunProcess(const std::vector<std::string>& argv, const EnvOverrides& env = {});

// Creates a fresh directory under $TEST_TMPDIR, $TMPDIR or /tmp (the
// first one set, like gtest's testing::TempDir()) and deletes it
// with everything inside when destroyed. Set
// GPERFTOOLS_KEEP_TEST_TMPDIR=1 to keep it for inspection.
class TempDir {
 public:
  explicit TempDir(std::string_view name);
  ~TempDir();

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const std::string& path() const { return path_; }

  // Path of the given entry inside this directory.
  std::string Path(std::string_view name) const;

  // Sorted names of the entries in this directory (not recursive).
  std::vector<std::string> List() const;

 private:
  std::string path_;
};

}  // namespace tcmalloc

#endif  // TESTS_POSIX_TESTUTIL_H_
