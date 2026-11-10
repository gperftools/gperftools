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

#include "config_for_unittests.h"

#include "tests/posix_testutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "gtest/gtest.h"

using tcmalloc::ProcessResult;
using tcmalloc::RunProcess;
using tcmalloc::TempDir;

static ProcessResult RunShell(const std::string& script, const tcmalloc::EnvOverrides& env = {}) {
  return RunProcess({"/bin/sh", "-c", script}, env);
}

TEST(RunProcessTest, CapturesStdoutAndStderr) {
  ProcessResult r = RunShell("echo out; echo err >&2");
  EXPECT_TRUE(r.Succeeded()) << r.Describe();
  EXPECT_EQ(r.output, "out\nerr\n");
}

TEST(RunProcessTest, ReportsExitCode) {
  ProcessResult r = RunShell("exit 3");
  EXPECT_FALSE(r.Succeeded());
  EXPECT_EQ(r.Describe(), "exited with code 3");
}

TEST(RunProcessTest, ReportsSignal) {
  ProcessResult r = RunShell("kill -9 $$");
  EXPECT_FALSE(r.Succeeded());
  EXPECT_EQ(r.Describe().rfind("killed by signal 9", 0), 0) << r.Describe();
}

TEST(RunProcessTest, SearchesPath) {
  ProcessResult r = RunProcess({"sh", "-c", "echo hi"});
  EXPECT_TRUE(r.Succeeded()) << r.Describe();
  EXPECT_EQ(r.output, "hi\n");
}

TEST(RunProcessTest, MissingProgram) {
  ProcessResult r = RunProcess({"gperftools-no-such-program"});
  EXPECT_FALSE(r.Succeeded());
  // Depending on the libc, the error is reported either by
  // posix_spawnp itself or by the child exiting with 127.
  EXPECT_TRUE(r.status == -1 || r.Describe() == "exited with code 127") << r.Describe();
}

TEST(RunProcessTest, EnvOverrides) {
  ProcessResult r = RunShell("echo \"$GPERFTOOLS_TEST_SET\"; echo \"${GPERFTOOLS_TEST_UNSET-unset}\"",
                             {{"GPERFTOOLS_TEST_SET", "value"}, {"GPERFTOOLS_TEST_UNSET", std::nullopt}});
  EXPECT_TRUE(r.Succeeded()) << r.Describe();
  EXPECT_EQ(r.output, "value\nunset\n");

  setenv("GPERFTOOLS_TEST_UNSET", "inherited", 1);
  r = RunShell("echo \"${GPERFTOOLS_TEST_UNSET-unset}\"");
  EXPECT_EQ(r.output, "inherited\n");
  r = RunShell("echo \"${GPERFTOOLS_TEST_UNSET-unset}\"", {{"GPERFTOOLS_TEST_UNSET", std::nullopt}});
  EXPECT_EQ(r.output, "unset\n");
  r = RunShell("echo \"${GPERFTOOLS_TEST_UNSET-unset}\"", {{"GPERFTOOLS_TEST_UNSET", "replaced"}});
  EXPECT_EQ(r.output, "replaced\n");
  unsetenv("GPERFTOOLS_TEST_UNSET");

  r = RunShell("echo \"${GPERFTOOLS_TEST_SET-unset}\"",
               {{"GPERFTOOLS_TEST_SET", "first"}, {"GPERFTOOLS_TEST_SET", "last"}});
  EXPECT_EQ(r.output, "last\n");
  r = RunShell("echo \"${GPERFTOOLS_TEST_SET-unset}\"",
               {{"GPERFTOOLS_TEST_SET", "first"}, {"GPERFTOOLS_TEST_SET", std::nullopt}});
  EXPECT_EQ(r.output, "unset\n");
}

TEST(TempDirTest, CreatesListsAndRemoves) {
  unsetenv("GPERFTOOLS_KEEP_TEST_TMPDIR");
  std::string path;
  {
    TempDir dir("posix_testutil_test");
    path = dir.path();

    struct stat st;
    ASSERT_EQ(stat(path.c_str(), &st), 0);
    EXPECT_TRUE(S_ISDIR(st.st_mode));
    EXPECT_TRUE(dir.List().empty());

    for (const char* name : {"b", "a"}) {
      FILE* f = fopen(dir.Path(name).c_str(), "w");
      ASSERT_NE(f, nullptr);
      fclose(f);
    }
    ASSERT_EQ(mkdir(dir.Path("sub").c_str(), 0755), 0);
    FILE* f = fopen(dir.Path("sub/c").c_str(), "w");
    ASSERT_NE(f, nullptr);
    fclose(f);

    EXPECT_EQ(dir.List(), (std::vector<std::string>{"a", "b", "sub"}));
  }
  EXPECT_NE(access(path.c_str(), F_OK), 0);
}
