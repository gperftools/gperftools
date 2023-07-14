/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
 * Copyright (c) 2023, gperftools Contributors
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

// ---
// Author: Artem Y. Polyakov

#include <stdlib.h>   // for environment primitives
#include <unistd.h>   // for getpid()
#include <limits.h>   // for PATH_MAX
#include <string>
#include <utility>
#include "addressmap-inl.h"
#include "base/logging.h"
#include "base/sysinfo.h"
#include "base/commandlineflags.h"

#define TEST_VAR   "GPROF_TEST_PATH"
#define TEST_FORCE TEST_VAR "_USE_PID"
#define TEST_VAL   "/var/log/some_file_name"
#define HPC_RANK "5"

#define PID_SUFFIX TC_ENV_PID_SUFFIX
#define PMIX_RANK_ENV TC_ENV_PMIX_RANK
#define PMIX_SUFFIX TC_ENV_PMIX_SUFFIX
#define SLURM_SUFFIX TC_ENV_SLURM_SUFFIX
#define SLURM_JOBID_ENV TC_ENV_SLURM_JOBID
#define SLURM_PROCID_ENV TC_ENV_SLURM_PROCID


// Manage environment
void setEnvDefault() {
  setenv(TEST_VAR, TEST_VAL, 1);
}

void unsetEnvDefault() {
  unsetenv(TEST_VAR);
}

void setEnvForced() {
  setEnvDefault();
  setenv(TEST_FORCE, "1", 1);
}

void unsetEnvForced() {
  unsetEnvDefault();
  unsetenv(TEST_FORCE);
}

// Possible outcomes

void appendPID(std::string &str) {
  str += PID_SUFFIX;
  str += std::to_string(getpid());
}

const std::string genDefaultParent() {
  std::string expected;
  expected = TEST_VAL;
  return expected;
}

const std::string genDefaultChild() {
  std::string expected = genDefaultParent();
  appendPID(expected);
  return expected;
}

const std::string genForced() {
  return genDefaultChild();
}

const std::string genPMIxParent() {
  std::string expected = genDefaultParent();
  expected += PMIX_SUFFIX;
  expected += HPC_RANK;
  return expected;
}

const std::string genPMIxChild() {
  std::string expected = genPMIxParent();
  appendPID(expected);
  return expected;
}

const std::string genSlurmParent() {
  std::string expected = genDefaultParent();
  expected += SLURM_SUFFIX;
  expected += HPC_RANK;
  return expected;
}

const std::string genSlurmChild() {
  std::string expected = genSlurmParent();
  appendPID(expected);
  return expected;
}

// Test the default case
void testDefault() {
  char path[PATH_MAX];
  setEnvDefault();

  // Test parent case (will set the child flag)
  GetUniquePathFromEnv(TEST_VAR, path);
  EXPECT_TRUE(!genDefaultParent().compare(path));

  // Test child case
  GetUniquePathFromEnv(TEST_VAR, path);
  EXPECT_TRUE(!genDefaultChild().compare(path));

  unsetEnvDefault();

  setEnvForced();

  // Test parent case - must include PID (will set the child flag)
  GetUniquePathFromEnv(TEST_VAR, path);
  EXPECT_TRUE(!genDefaultChild().compare(path));

  // Test child case
  GetUniquePathFromEnv(TEST_VAR, path);
  EXPECT_TRUE(!genDefaultChild().compare(path));

  unsetEnvForced();
}

// Test the PMIx case
void testPMIx() {
  char path[PATH_MAX];

  // Set PMIx rank
  setenv(PMIX_RANK_ENV, HPC_RANK, 1);

  // Test non-forced case
  setEnvDefault();

  // Test parent case (will set the child flag)
  GetUniquePathFromEnv(TEST_VAR, path);
  EXPECT_TRUE(!genPMIxParent().compare(path));

  // Test child case
  GetUniquePathFromEnv(TEST_VAR, path);
  EXPECT_TRUE(!genPMIxChild().compare(path));

  unsetEnvDefault();

  // Test forced case - should generate same path
  setEnvForced();

  // Test parent case (will set the child flag)
  GetUniquePathFromEnv(TEST_VAR, path);
  EXPECT_TRUE(!genPMIxChild().compare(path));

  // Test child case
  GetUniquePathFromEnv(TEST_VAR, path);
  EXPECT_TRUE(!genPMIxChild().compare(path));

  unsetEnvForced();

  unsetenv(PMIX_RANK_ENV);
}


// Test the Slurm case
void testSlurm() {
  char path[PATH_MAX];

  // Set PMIx rank
  setenv(SLURM_JOBID_ENV, "1", 1);

  // Test non-forced case (no process ID found)
  setEnvDefault();

  // Test parent case (will set the child flag)
  GetUniquePathFromEnv(TEST_VAR, path);
  EXPECT_TRUE(!genForced().compare(path));

  // Test child case
  GetUniquePathFromEnv(TEST_VAR, path);
  EXPECT_TRUE(!genForced().compare(path));

  unsetEnvDefault();

  // Test non-forced case (has Slurm process ID)
  setenv(SLURM_PROCID_ENV, HPC_RANK, 1);

  setEnvDefault();

  // Test parent case (will set the child flag)
  GetUniquePathFromEnv(TEST_VAR, path);
  EXPECT_TRUE(!genSlurmParent().compare(path));

  // Test child case
  GetUniquePathFromEnv(TEST_VAR, path);
  EXPECT_TRUE(!genSlurmChild().compare(path));

  unsetEnvDefault();

  // Test forced case - should generate same path
  setEnvForced();

  // Test parent case (will set the child flag)
  GetUniquePathFromEnv(TEST_VAR, path);
  EXPECT_TRUE(!genSlurmChild().compare(path));

  // Test child case
  GetUniquePathFromEnv(TEST_VAR, path);
  EXPECT_TRUE(!genSlurmChild().compare(path));

  unsetEnvForced();

  unsetenv(SLURM_PROCID_ENV);
  unsetenv(SLURM_JOBID_ENV);
}

// Test the OMPI case
void testOMPI() {
  char path[PATH_MAX];

  // Set PMIx rank
  setenv("OMPI_HOME", "/some/path", 1);

  // Test non-forced case
  setEnvDefault();

  // Test parent case (will set the child flag)
  GetUniquePathFromEnv(TEST_VAR, path);
  EXPECT_TRUE(!genForced().compare(path));

  // Test child case
  GetUniquePathFromEnv(TEST_VAR, path);
  EXPECT_TRUE(!genForced().compare(path));

  unsetEnvDefault();

  // Test forced case - should generate same path
  setEnvForced();

  // Test parent case (will set the child flag)
  GetUniquePathFromEnv(TEST_VAR, path);
  EXPECT_TRUE(!genForced().compare(path));

  // Test child case
  GetUniquePathFromEnv(TEST_VAR, path);
  EXPECT_TRUE(!genForced().compare(path));

  unsetEnvForced();

  unsetenv("OMPI_HOME");
}

int main(int argc, char** argv) {

  testDefault();
  testPMIx();
  testSlurm();
  testOMPI();

  printf("PASS\n");
  return 0;
}
