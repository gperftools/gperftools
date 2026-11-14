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

// Tests for the heap profiler.
//
// Every test runs this binary again as a child process in "run"
// mode, which executes a short list of commands (allocate, free,
// start/stop the profiler, dump, ...) and exits. Tests then inspect
// the produced profiles with pprof. Running each scenario in a fresh
// process lets us exercise what only happens at process start and
// exit: HEAPPROFILE and the other environment variables, the dump at
// exit, and naming of profiles of child processes.
//
// pprof must be in PATH.

#include "config_for_unittests.h"

#include <gperftools/heap-profiler.h>

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <map>
#include <optional>
#include <ostream>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"

#include "base/basictypes.h"
#include "base/logging.h"
#include "tests/posix_testutil.h"

using tcmalloc::EnvOverrides;
using tcmalloc::ProcessResult;
using tcmalloc::RunProcess;
using tcmalloc::TempDir;

// Absolute path of this binary, for running it again from inside
// the per-test directory.
static std::string self_path;

/* alk: Note to whoever reads this, and more importantly makes further
 * changes. So this "commands" facility is somewhat odd and complex,
 * but seems to be the simplest approach for what is being done
 * here. If you're about to make any extensions of this, let alone
 * reusing it elsewhere, then you _must_ consider alternatives. For
 * single one-off use, this is okay, but growing it would be a
 * mistake, likely. */

// ---------------------------------------------------------------------------
// Child side: "heap-profiler_unittest run <command>..."
//
// Commands:
//   A+<n>x<size>    allocate n objects of size bytes in AllocateA
//   B+<n>x<size>    same in AllocateB
//   A-<n>, B-<n>    free the n most recent objects of AllocateA/AllocateB
//   start:<prefix>  HeapProfilerStart(prefix)
//   stop            HeapProfilerStop()
//   dump:<reason>   HeapProfilerDump(reason)
//   get:<path>      write the result of GetHeapProfile() to path
//   running:<0|1>   check the result of IsHeapProfilerRunning()
//   signal:<n>      raise signal n
//   sleep:<ms>      sleep for ms milliseconds
//   spawn           run the remaining commands in a child process
//                   (this binary again) and wait for it

namespace {

constexpr int kMaxObjects = 100000;

struct Objects {
  char* ptrs[kMaxObjects];
  int count;
};

Objects a_objects;
Objects b_objects;

}  // namespace

// The tests find these two by name in pprof output, which shows them
// as just "AllocateA"/"AllocateB". They are noinline so that they
// show up as their own frames. They are not extern "C", since some
// platforms (e.g. macOS) prefix C symbol names with '_', and not
// static, so that the compiler keeps them under their own name rather
// than a specialized copy like AllocateA.isra.0.
ATTRIBUTE_NOINLINE void AllocateA(int n, size_t size) {
  for (int i = 0; i < n; i++) {
    CHECK_LT(a_objects.count, kMaxObjects);
    a_objects.ptrs[a_objects.count++] = new char[size];
  }
}

ATTRIBUTE_NOINLINE void AllocateB(int n, size_t size) {
  for (int i = 0; i < n; i++) {
    CHECK_LT(b_objects.count, kMaxObjects);
    b_objects.ptrs[b_objects.count++] = new char[size];
  }
}

static void FreeObjects(Objects* objects, int n) {
  CHECK_LE(n, objects->count);
  for (int i = 0; i < n; i++) {
    delete[] objects->ptrs[--objects->count];
  }
}

static int64_t ParseInt(std::string_view str) {
  std::string s{str};
  char* end;
  errno = 0;
  long long v = strtoll(s.c_str(), &end, 10);
  if (s.empty() || *end != '\0' || errno != 0) {
    fprintf(stderr, "bad number: '%s'\n", s.c_str());
    abort();
  }
  return v;
}

static void RunCommands(const std::vector<std::string>& commands) {
  for (size_t i = 0; i < commands.size(); i++) {
    std::string_view cmd = commands[i];
    std::string_view name = cmd.substr(0, cmd.find(':'));
    std::string arg{cmd.substr(std::min(cmd.size(), name.size() + 1))};

    if (cmd.size() >= 2 && (cmd[0] == 'A' || cmd[0] == 'B') && (cmd[1] == '+' || cmd[1] == '-')) {
      Objects* objects = cmd[0] == 'A' ? &a_objects : &b_objects;
      std::string_view rest = cmd.substr(2);
      if (cmd[1] == '-') {
        FreeObjects(objects, ParseInt(rest));
        continue;
      }
      size_t x = rest.find('x');
      CHECK_NE(x, std::string_view::npos);
      int n = ParseInt(rest.substr(0, x));
      size_t size = ParseInt(rest.substr(x + 1));
      if (cmd[0] == 'A') {
        AllocateA(n, size);
      } else {
        AllocateB(n, size);
      }
    } else if (name == "start") {
      HeapProfilerStart(arg.c_str());
    } else if (name == "stop") {
      HeapProfilerStop();
    } else if (name == "dump") {
      HeapProfilerDump(arg.c_str());
    } else if (name == "get") {
      char* profile = GetHeapProfile();
      FILE* f = fopen(arg.c_str(), "w");
      CHECK(f != nullptr);
      fputs(profile, f);
      fclose(f);
      free(profile);
    } else if (name == "running") {
      CHECK_EQ(IsHeapProfilerRunning(), ParseInt(arg));
    } else if (name == "signal") {
      raise(ParseInt(arg));
    } else if (name == "sleep") {
      int ms = ParseInt(arg);
      struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
      while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {
      }
    } else if (name == "spawn") {
      std::vector<std::string> argv{self_path, "run"};
      argv.insert(argv.end(), commands.begin() + i + 1, commands.end());
      ProcessResult result = RunProcess(argv);
      fputs(result.output.c_str(), stdout);
      fflush(stdout);
      if (!result.Succeeded()) {
        fprintf(stderr, "spawned child %s\n", result.Describe().c_str());
        abort();
      }
      return;
    } else {
      fprintf(stderr, "unknown command: '%s'\n", commands[i].c_str());
      abort();
    }
  }
}

// ---------------------------------------------------------------------------
// Test side.

// What pprof reports for one function, in its four sample types.
struct Usage {
  int64_t inuse_objects = 0;
  int64_t inuse_space = 0;
  int64_t alloc_objects = 0;
  int64_t alloc_space = 0;

  bool operator==(const Usage& o) const {
    return inuse_objects == o.inuse_objects && inuse_space == o.inuse_space && alloc_objects == o.alloc_objects &&
           alloc_space == o.alloc_space;
  }
};

void PrintTo(const Usage& u, std::ostream* os) {
  *os << "{inuse: " << u.inuse_objects << " objects, " << u.inuse_space << " bytes; alloc: " << u.alloc_objects
      << " objects, " << u.alloc_space << " bytes}";
}

using Files = std::vector<std::string>;

// Runs pprof on the profile and returns the "cum" value of every
// function it lists. Returns nullopt if the profile doesn't have this
// sample index.
static std::optional<std::map<std::string, int64_t>> PProfCumValues(const std::string& profile,
                                                                    const std::string& sample_index) {
  // -unit=B keeps pprof from scaling values to kB/MB. On object
  // counts it just adds a B suffix, which we strip.
  std::vector<std::string> argv{"pprof",   "-text",           "-sample_index=" + sample_index,
                                "-unit=B", "-nodefraction=0", "-nodecount=1000000", profile};

  std::map<std::string, int64_t> values;
  ProcessResult result = RunProcess(argv);
  if (!result.Succeeded()) {
    if (result.output.find("sample_index \"" + sample_index + "\" must be one of") != std::string::npos) {
      return std::nullopt;
    }
    ADD_FAILURE() << "pprof " << result.Describe()
                  << ". It needs to be in PATH (go install github.com/google/pprof@latest).\n"
                  << result.output;
    return values;
  }

  // After the header line, each line is: flat flat% sum% cum cum% name
  std::istringstream lines{result.output};
  std::string line;
  bool in_table = false;
  while (std::getline(lines, line)) {
    if (!in_table) {
      in_table = line.find("flat%") != std::string::npos;
      continue;
    }
    std::istringstream fields{line};
    std::string flat, flat_pct, sum_pct, cum, cum_pct, name;
    fields >> flat >> flat_pct >> sum_pct >> cum >> cum_pct >> std::ws;
    std::getline(fields, name);
    char* end;
    long long v = strtoll(cum.c_str(), &end, 10);
    if (name.empty() || end == cum.c_str() || !(*end == '\0' || strcmp(end, "B") == 0)) {
      ADD_FAILURE() << "can't parse pprof output line: " << line << "\nFull output:\n" << result.output;
      continue;
    }
    values[name] = v;
  }
  return values;
}

// Usage of every function in the profile, as pprof sees it.
// Functions not in the profile get zeros.
static std::map<std::string, Usage> ReadProfile(const std::string& profile) {
  std::map<std::string, Usage> usage;
  auto read = [&](const std::string& sample_index, int64_t Usage::* field) {
    auto values = PProfCumValues(profile, sample_index);
    for (const auto& [name, value] : values.value_or(std::map<std::string, int64_t>{})) {
      usage[name].*field = value;
    }
    return values.has_value();
  };

  EXPECT_TRUE(read("inuse_objects", &Usage::inuse_objects));
  EXPECT_TRUE(read("inuse_space", &Usage::inuse_space));
  if (!read("alloc_objects", &Usage::alloc_objects)) {
    // When nothing was freed since the profiler started, pprof's
    // reader of this format offers only in-use values: allocated
    // values are the same then.
    for (auto& [name, u] : usage) {
      u.alloc_objects = u.inuse_objects;
      u.alloc_space = u.inuse_space;
    }
  } else {
    EXPECT_TRUE(read("alloc_space", &Usage::alloc_space));
  }
  return usage;
}

static std::string ReadFile(const std::string& path) {
  FILE* f = fopen(path.c_str(), "r");
  if (f == nullptr) {
    ADD_FAILURE() << "can't open " << path;
    return "";
  }
  std::string contents;
  char buf[4096];
  size_t amt;
  while ((amt = fread(buf, 1, sizeof(buf), f)) > 0) {
    contents.append(buf, amt);
  }
  fclose(f);
  return contents;
}

static int CountMatches(const std::string& text, const char* regex) {
  std::regex re{regex};
  return std::distance(std::sregex_iterator(text.begin(), text.end(), re), std::sregex_iterator());
}

// Each test runs inside its own fresh temporary directory, so file
// names below are relative to it.
class HeapProfilerTest : public testing::Test {
 protected:
  TempDir dir_{"heap-profiler_unittest"};

  void SetUp() override {
    char cwd[PATH_MAX];
    ASSERT_NE(getcwd(cwd, sizeof(cwd)), nullptr) << strerror(errno);
    original_dir_ = cwd;
    ChangeDir(dir_.path());
  }

  void TearDown() override { ChangeDir(original_dir_); }

  // Runs the commands in a child process and checks that it
  // succeeded. The child starts with the heap profiler off and all
  // periodic dumps disabled; env is applied on top of that.
  ProcessResult Run(const std::vector<std::string>& commands, const EnvOverrides& env = {}) {
    EnvOverrides full_env{
        {"HEAPPROFILE", std::nullopt},
        {"HEAPPROFILE_USE_PID", std::nullopt},
        {"HEAPPROFILESIGNAL", std::nullopt},
        {"HEAP_PROFILE_CLEANUP", std::nullopt},
        {"HEAP_PROFILE_ALLOCATION_INTERVAL", "0"},
        {"HEAP_PROFILE_DEALLOCATION_INTERVAL", "0"},
        {"HEAP_PROFILE_INUSE_INTERVAL", "0"},
        {"HEAP_PROFILE_TIME_INTERVAL", "0"},
    };
    full_env.insert(full_env.end(), env.begin(), env.end());

    std::vector<std::string> argv{self_path, "run"};
    argv.insert(argv.end(), commands.begin(), commands.end());
    ProcessResult result = RunProcess(argv, full_env);
    EXPECT_TRUE(result.Succeeded()) << "child " << result.Describe() << ". Output:\n" << result.output;
    return result;
  }

  // Same as Run, with HEAPPROFILE=prof, so profiles are named
  // prof.0001.heap, prof.0002.heap, ...
  ProcessResult RunProfiled(const std::vector<std::string>& commands, EnvOverrides env = {}) {
    env.insert(env.begin(), {"HEAPPROFILE", "prof"});
    return Run(commands, env);
  }

 private:
  static void ChangeDir(const std::string& path) {
    printf("Changing directory to %s\n", path.c_str());
    fflush(stdout);
    ASSERT_EQ(chdir(path.c_str()), 0) << path << ": " << strerror(errno);
  }

  std::string original_dir_;
};

TEST_F(HeapProfilerTest, DumpsAtExit) {
  ProcessResult result = RunProfiled({"A+10x1000", "B+5x2000", "B-5"});

  EXPECT_EQ(dir_.List(), Files{"prof.0001.heap"});
  EXPECT_EQ(CountMatches(result.output, R"(prof\.0001\.heap \(Exiting, )"), 1) << result.output;

  auto usage = ReadProfile("prof.0001.heap");
  EXPECT_EQ(usage["AllocateA"], (Usage{10, 10000, 10, 10000}));
  EXPECT_EQ(usage["AllocateB"], (Usage{0, 0, 5, 10000}));
}

// Runs a sequence of steps several times, each time stopping after
// one more step, so each run leaves a profile of the state after its
// last step.
TEST_F(HeapProfilerTest, StepByStep) {
  struct Step {
    const char* command;
    Usage a;  // expected usage of AllocateA after this step
    Usage b;  // same for AllocateB
  };
  const Step steps[] = {
      {"A+100x1000", {100, 100000, 100, 100000}, {}},
      {"B+200x10000", {100, 100000, 100, 100000}, {200, 2000000, 200, 2000000}},
      {"A-100", {0, 0, 100, 100000}, {200, 2000000, 200, 2000000}},
      {"B-100", {0, 0, 100, 100000}, {100, 1000000, 200, 2000000}},
      {"A+50x100000", {50, 5000000, 150, 5100000}, {100, 1000000, 200, 2000000}},
  };

  std::vector<std::string> commands;
  for (size_t i = 0; i < std::size(steps); i++) {
    SCOPED_TRACE(testing::Message() << "after step " << i << " (" << steps[i].command << ")");
    commands.push_back(steps[i].command);

    std::string prefix = "step" + std::to_string(i);
    Run(commands, {{"HEAPPROFILE", prefix}});

    auto usage = ReadProfile(prefix + ".0001.heap");
    EXPECT_EQ(usage["AllocateA"], steps[i].a);
    EXPECT_EQ(usage["AllocateB"], steps[i].b);
  }
}

TEST_F(HeapProfilerTest, StartStopDumpGet) {
  ProcessResult result = Run({
      "running:0",
      "get:off.txt",
      "start:first",
      "running:1",
      "A+10x1000",
      "dump:one",
      "stop",
      "running:0",
      // Not profiled, and freeing them later while profiling must
      // not count.
      "B+20x1000",
      "start:second",
      "A+3x1000",
      "B-20",
      "dump:two",
      "get:get.heap",
      "stop",
  });

  // Dump numbering continues after a restart. Nothing is dumped at
  // exit, since the profiler was stopped.
  EXPECT_EQ(dir_.List(), (Files{"first.0001.heap", "get.heap", "off.txt", "second.0002.heap"}));
  EXPECT_EQ(CountMatches(result.output, R"(first\.0001\.heap \(one\))"), 1) << result.output;
  EXPECT_EQ(CountMatches(result.output, R"(second\.0002\.heap \(two\))"), 1) << result.output;

  EXPECT_EQ(ReadFile("off.txt"), "");

  auto first = ReadProfile("first.0001.heap");
  EXPECT_EQ(first["AllocateA"], (Usage{10, 10000, 10, 10000}));
  EXPECT_EQ(first["AllocateB"], Usage{});

  // Stop discards everything recorded so far.
  for (const char* profile : {"second.0002.heap", "get.heap"}) {
    SCOPED_TRACE(profile);
    auto second = ReadProfile(profile);
    EXPECT_EQ(second["AllocateA"], (Usage{3, 3000, 3, 3000}));
    EXPECT_EQ(second["AllocateB"], Usage{});
  }
}

// Allocates and frees 10 * 1 MiB. With objects as big as the 1 MiB
// intervals below, each allocation (or free) crosses the next
// threshold by itself, whatever else the process allocated before
// (as long as that is under 1 MiB).
static const std::vector<std::string> kAllocThenFree10MiB{"A+10x1048576", "A-10"};

TEST_F(HeapProfilerTest, AllocationInterval) {
  ProcessResult result = RunProfiled(kAllocThenFree10MiB, {{"HEAP_PROFILE_ALLOCATION_INTERVAL", "1048576"}});

  EXPECT_EQ(CountMatches(result.output, R"(\(\d+ MB allocated cumulatively)"), 10) << result.output;
  EXPECT_EQ(CountMatches(result.output, R"(\(10 MB allocated cumulatively, 10 MB currently in use\))"), 1)
      << result.output;
  // 10 periodic dumps and one at exit.
  EXPECT_EQ(dir_.List().size(), 11);
}

TEST_F(HeapProfilerTest, DeallocationInterval) {
  ProcessResult result = RunProfiled(kAllocThenFree10MiB, {{"HEAP_PROFILE_DEALLOCATION_INTERVAL", "1048576"}});

  EXPECT_EQ(CountMatches(result.output, R"(\(\d+ MB freed cumulatively)"), 10) << result.output;
  EXPECT_EQ(CountMatches(result.output, R"(\(10 MB freed cumulatively, 0 MB currently in use\))"), 1) << result.output;
  EXPECT_EQ(dir_.List().size(), 11);
}

TEST_F(HeapProfilerTest, InuseInterval) {
  // Dumps happen when in-use memory exceeds the previous high-water
  // mark by more than 1 MiB. With 64 KiB objects that is after about
  // 16, 33 and 50 objects (one more each if nothing else is in use),
  // so going up to 60 dumps 3 times. Coming back to 60 objects after
  // freeing everything stays below the mark, so no more dumps.
  ProcessResult result =
      RunProfiled({"A+60x65536", "A-60", "A+60x65536"}, {{"HEAP_PROFILE_INUSE_INTERVAL", "1048576"}});

  EXPECT_EQ(CountMatches(result.output, R"(\(\d+ MB currently in use\))"), 3) << result.output;
  EXPECT_EQ(dir_.List().size(), 4);
}

TEST_F(HeapProfilerTest, TimeInterval) {
  // The first allocation after start always dumps. Then the next
  // allocation that is at least a second later. Since the clock has
  // 1 second resolution, an extra dump can happen before the sleep if
  // a second boundary passes early on.
  ProcessResult result = RunProfiled({"A+1x1000", "sleep:1100", "A+1x1000"}, {{"HEAP_PROFILE_TIME_INTERVAL", "1"}});

  int dumps = CountMatches(result.output, R"(sec since the last dump\))");
  EXPECT_GE(dumps, 2) << result.output;
  EXPECT_LE(dumps, 3) << result.output;
}

TEST_F(HeapProfilerTest, DumpOnSignal) {
  ProcessResult result = RunProfiled({"A+10x1000", "signal:" + std::to_string(SIGUSR1), "A+5x1000"},
                                     {{"HEAPPROFILESIGNAL", std::to_string(SIGUSR1)}});

  EXPECT_EQ(dir_.List(), (Files{"prof.0001.heap", "prof.0002.heap"}));
  EXPECT_EQ(CountMatches(result.output, R"(prof\.0001\.heap \(signal\))"), 1) << result.output;
  EXPECT_EQ(ReadProfile("prof.0001.heap")["AllocateA"], (Usage{10, 10000, 10, 10000}));
  EXPECT_EQ(ReadProfile("prof.0002.heap")["AllocateA"], (Usage{15, 15000, 15, 15000}));
}

class HeapProfilerCleanupTest : public HeapProfilerTest {
 protected:
  void SetUp() override {
    HeapProfilerTest::SetUp();
    // A failed ASSERT_* in the parent's SetUp only returns from
    // that function, and we may not be in the temp dir then. Don't
    // create our files elsewhere.
    if (HasFatalFailure()) {
      return;
    }
    for (const char* name : kOldFiles) {
      FILE* f = fopen(name, "w");
      ASSERT_NE(f, nullptr);
      fclose(f);
    }
  }

  static constexpr const char* kOldFiles[] = {"prof.0001.heap", "prof.0042.heap",
                                              // These don't match prof.*.heap.
                                              "prof.0001.txt", "prof_123.0001.heap", "other.0001.heap"};
};

TEST_F(HeapProfilerCleanupTest, RemovesOldProfiles) {
  RunProfiled({"A+1x1000"});
  EXPECT_EQ(dir_.List(), (Files{"other.0001.heap", "prof.0001.heap", "prof.0001.txt", "prof_123.0001.heap"}));
  EXPECT_EQ(ReadProfile("prof.0001.heap")["AllocateA"], (Usage{1, 1000, 1, 1000}));
}

TEST_F(HeapProfilerCleanupTest, KeepsOldProfilesWhenDisabled) {
  RunProfiled({"A+1x1000"}, {{"HEAP_PROFILE_CLEANUP", "false"}});
  EXPECT_EQ(dir_.List(),
            (Files{"other.0001.heap", "prof.0001.heap", "prof.0001.txt", "prof.0042.heap", "prof_123.0001.heap"}));
}

TEST_F(HeapProfilerTest, ChildProcess) {
  // The parent dumps before starting the child, so we also check that
  // the child's cleanup of old profiles leaves the parent's alone.
  RunProfiled({"B+10x1000", "dump:before-spawn", "spawn", "A+20x1000"});

  Files files = dir_.List();
  ASSERT_EQ(files.size(), 3) << testing::PrintToString(files);
  EXPECT_EQ(files[0], "prof.0001.heap");
  EXPECT_EQ(files[1], "prof.0002.heap");
  EXPECT_TRUE(std::regex_match(files[2], std::regex{R"(prof_\d+\.0001\.heap)"})) << files[2];

  auto parent = ReadProfile(files[1]);
  EXPECT_EQ(parent["AllocateB"], (Usage{10, 10000, 10, 10000}));
  EXPECT_EQ(parent["AllocateA"], Usage{});

  auto child = ReadProfile(files[2]);
  EXPECT_EQ(child["AllocateA"], (Usage{20, 20000, 20, 20000}));
  EXPECT_EQ(child["AllocateB"], Usage{});
}

TEST_F(HeapProfilerTest, UnwritablePath) {
  ProcessResult result = Run({"A+1x1000"}, {{"HEAPPROFILE", "no-such-dir/prof"}});
  EXPECT_EQ(CountMatches(result.output, "Failed dumping heap profile"), 1) << result.output;
  EXPECT_TRUE(dir_.List().empty());
}

int main(int argc, char** argv) {
  char* path = realpath(argv[0], nullptr);
  CHECK(path != nullptr);
  self_path = path;
  free(path);

  if (argc >= 2 && strcmp(argv[1], "run") == 0) {
    RunCommands(std::vector<std::string>(argv + 2, argv + argc));
    return 0;
  }

  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
