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

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>

#include "base/environ.h"

namespace tcmalloc {

namespace {

[[noreturn]] void DieWithErrno(const char* what, int err) {
  fprintf(stderr, "%s: %s\n", what, strerror(err));
  abort();
}

void SetCloexec(int fd) {
  int flags = fcntl(fd, F_GETFD);
  if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
    DieWithErrno("fcntl", errno);
  }
}

std::vector<std::string> BuildEnv(const EnvOverrides& overrides) {
  std::vector<std::string> env;
  for (char** p = environ; *p != nullptr; p++) {
    std::string_view entry{*p};
    std::string_view name = entry.substr(0, entry.find('='));
    bool overridden =
        std::any_of(overrides.begin(), overrides.end(), [name](const auto& kv) { return kv.first == name; });
    if (!overridden) {
      env.emplace_back(entry);
    }
  }
  for (auto it = overrides.begin(); it != overrides.end(); ++it) {
    const auto& [name, value] = *it;
    // When a name appears several times, the last entry wins.
    bool overridden_later =
        std::any_of(it + 1, overrides.end(), [&name = name](const auto& kv) { return kv.first == name; });
    if (value.has_value() && !overridden_later) {
      env.push_back(name + "=" + *value);
    }
  }
  return env;
}

bool KeepTempDirs() {
  const char* keep = getenv("GPERFTOOLS_KEEP_TEST_TMPDIR");
  return keep != nullptr && *keep != '\0' && strcmp(keep, "0") != 0;
}

std::vector<char*> ToCharPtrs(const std::vector<std::string>& strings) {
  std::vector<char*> retval;
  for (const std::string& s : strings) {
    retval.push_back(const_cast<char*>(s.c_str()));
  }
  retval.push_back(nullptr);
  return retval;
}

}  // namespace

bool ProcessResult::Succeeded() const { return status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0; }

std::string ProcessResult::Describe() const {
  if (status == -1) {
    return "failed to start";
  }
  if (WIFEXITED(status)) {
    return "exited with code " + std::to_string(WEXITSTATUS(status));
  }
  if (WIFSIGNALED(status)) {
    return "killed by signal " + std::to_string(WTERMSIG(status)) + " (" + strsignal(WTERMSIG(status)) + ")";
  }
  return "ended with wait status " + std::to_string(status);
}

ProcessResult RunProcess(const std::vector<std::string>& argv, const EnvOverrides& env) {
  ProcessResult result;

  int fds[2];
  if (pipe(fds) < 0) {
    DieWithErrno("pipe", errno);
  }
  // Keep both ends out of this and any other child; the child only
  // gets the dup2-ed copies below.
  SetCloexec(fds[0]);
  SetCloexec(fds[1]);

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_adddup2(&actions, fds[1], 1);
  posix_spawn_file_actions_adddup2(&actions, fds[1], 2);

  std::vector<std::string> env_strings = BuildEnv(env);
  std::vector<char*> c_argv = ToCharPtrs(argv);
  std::vector<char*> c_env = ToCharPtrs(env_strings);

  pid_t pid;
  int rv = posix_spawnp(&pid, c_argv[0], &actions, nullptr, c_argv.data(), c_env.data());
  posix_spawn_file_actions_destroy(&actions);
  close(fds[1]);

  if (rv != 0) {
    close(fds[0]);
    result.output = "posix_spawnp(" + argv[0] + ") failed: " + strerror(rv) + "\n";
    return result;
  }

  char buf[4096];
  for (;;) {
    ssize_t amt = read(fds[0], buf, sizeof(buf));
    if (amt < 0 && errno == EINTR) {
      continue;
    }
    if (amt < 0) {
      DieWithErrno("read", errno);
    }
    if (amt == 0) {
      break;
    }
    result.output.append(buf, amt);
  }
  close(fds[0]);

  int status;
  pid_t wait_rv;
  do {
    wait_rv = waitpid(pid, &status, 0);
  } while (wait_rv < 0 && errno == EINTR);
  if (wait_rv < 0) {
    DieWithErrno("waitpid", errno);
  }
  result.status = status;
  return result;
}

TempDir::TempDir(std::string_view name) {
  // Same lookup as gtest's testing::TempDir(): bazel sets
  // TEST_TMPDIR, everyone else uses TMPDIR.
  const char* tmpdir = nullptr;
  for (const char* var : {"TEST_TMPDIR", "TMPDIR"}) {
    tmpdir = getenv(var);
    if (tmpdir != nullptr && *tmpdir != '\0') {
      break;
    }
  }
  if (tmpdir == nullptr || *tmpdir == '\0') {
    tmpdir = "/tmp";
  }
  std::string templ = std::string(tmpdir) + "/" + std::string(name) + ".XXXXXX";
  if (mkdtemp(templ.data()) == nullptr) {
    DieWithErrno(("mkdtemp(" + templ + ")").c_str(), errno);
  }
  path_ = std::move(templ);

  // stdout, so that it is ordered with gtest's own output.
  printf("Created temporary directory %s (%s)\n", path_.c_str(),
         KeepTempDirs() ? "kept since GPERFTOOLS_KEEP_TEST_TMPDIR is set"
                        : "removed at the end; set GPERFTOOLS_KEEP_TEST_TMPDIR=1 to keep it");
  fflush(stdout);
}

TempDir::~TempDir() {
  if (KeepTempDirs()) {
    printf("Keeping temporary directory %s\n", path_.c_str());
    fflush(stdout);
    return;
  }

  auto remove_entry = [](const char* path, const struct stat*, int, struct FTW*) -> int {
    if (remove(path) < 0) {
      perror(path);
    }
    return 0;
  };
  (void)nftw(path_.c_str(), remove_entry, 16, FTW_DEPTH | FTW_PHYS);
}

std::string TempDir::Path(std::string_view name) const { return path_ + "/" + std::string(name); }

std::vector<std::string> TempDir::List() const {
  std::vector<std::string> names;
  DIR* dir = opendir(path_.c_str());
  if (dir == nullptr) {
    DieWithErrno(("opendir(" + path_ + ")").c_str(), errno);
  }
  while (struct dirent* entry = readdir(dir)) {
    if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
      names.emplace_back(entry->d_name);
    }
  }
  closedir(dir);
  std::sort(names.begin(), names.end());
  return names;
}

}  // namespace tcmalloc
