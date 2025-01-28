// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
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
#include "config.h"

#include "run_benchmark.h"

#include "trivialre.h"

#include <functional>
#include <sstream>
#include <string>
#include <string_view>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sys/time.h>
#endif

static constexpr double kTrialNSec = 0.3e9;

static double benchmark_duration_nsec = 3e9;
static int benchmark_repetitions = 3;
static std::function<bool(std::string_view bench)> benchmark_filter;
bool benchmark_list_only;

static
std::function<bool(std::string_view bench)> parse_filter_or_die(std::string_view filter) {
  struct Policy {
    bool failed{};
    std::string_view msg{};
    std::string_view at{};

    void NoteError(std::string_view msg, std::string_view at) {
      failed = true;
      this->msg = msg;
      this->at = at;
    }
    void StartedParsing(std::string_view str) {}
  };

  trivialre::re_compiler::C<trivialre::matchers::MatcherBuilder, Policy> compiler({});
  trivialre::Matcher matcher = compiler.CompileOrDie(filter);
  if (compiler.failed) {
    fprintf(stderr, "failed to parse benchmark filter: '%.*s'.\nParse error: %.*s at %.*s\n",
            (int)filter.size(), filter.data(),
            (int)compiler.msg.size(), compiler.msg.data(),
            (int)compiler.at.size(), compiler.at.data());
    exit(1);
  }

  return [matcher] (std::string_view candidate) {
    return trivialre::MatchSubstring(matcher, candidate);
  };
}

void init_benchmark(int *argc, char ***argv) {
  int n = *argc;
  char **args = *argv;
  for (int i = 1; i < n; i++) {
    std::string_view a{args[i]};
    if (a.substr(0, 2) != "--") {
      fprintf(stderr, "benchmark only understands --long-form=<value> flags. got: %.*s\n",
              (int)a.size(), a.data());
      exit(1);
    }
    a.remove_prefix(2);
    std::string_view rest;
    auto has_prefix = [&] (std::string_view s) {
      bool rv = (a.substr(0, s.size()) == s);
      if (rv) {
        rest = a.substr(s.size());
      }
      return rv;
    };
    if (a == "help") {
      printf("%s --help\n"
             "  --benchmark_filter=<regex>\n"
             "  --benchmark_list\n"
             "  --benchmark_min_time=<seconds>\n"
             "  --benchmark_repetitions=<count>\n"
             "\n", (*argv)[0]);
      benchmark_list_only = true;
    } else if (has_prefix("benchmark_min_time=")) {
      char *end = nullptr;
      double value = strtod(rest.data(), &end);
      if (!end || *end) {
        fprintf(stderr, "failed to parse benchmark_min_time argument: %s\n", args[i]);
        exit(1);
      }
      benchmark_duration_nsec = value * 1e9;
      // printf("benchmark_duration_nsec = %f\n", benchmark_duration_nsec);
    } else if (has_prefix("benchmark_repetitions=")) {
      char *end = nullptr;
      long value = strtol(rest.data(), &end, 0);
      if (!end || *end || value < 1 || static_cast<long>(static_cast<int>(value)) != value) {
        fprintf(stderr, "failed to parse benchmark_repetitions argument: %s\n", args[i]);
        exit(1);
      }
      benchmark_repetitions = value;
    } else if (has_prefix("benchmark_filter=")) {
      benchmark_filter = parse_filter_or_die(rest);
    } else if (a == "benchmark_list") {
      benchmark_list_only = true;
    } else {
      fprintf(stderr, "unknown flag: %.*s\n", (int)a.size(), a.data());
      exit(1);
    }
  }
}

struct internal_bench {
  bench_body body;
  uintptr_t param;
};

static void run_body(struct internal_bench *b, long iterations)
{
  b->body(iterations, b->param);
}

#ifdef _WIN32
static uint64_t get_fs_time_ticks()
{
  FILETIME ft;
  GetSystemTimePreciseAsFileTime(&ft);
  return (uint64_t{ft.dwHighDateTime} << 32) + ft.dwLowDateTime;
}

static double measure_once(struct internal_bench *b, long iterations)
{
  uint64_t ticks_before, ticks_after;

  ticks_before = get_fs_time_ticks();

  run_body(b, iterations);

  ticks_after = get_fs_time_ticks();

  // Windows FILETIME ticks are 100 nanos per tick.
  return (ticks_after - ticks_before) * 100e0;
}
#else
static double measure_once(struct internal_bench *b, long iterations)
{
  struct timeval tv_before, tv_after;
  int rv;
  double time;

  rv = gettimeofday(&tv_before, nullptr);
  if (rv) {
    perror("gettimeofday");
    abort();
  }

  run_body(b, iterations);

  rv = gettimeofday(&tv_after, nullptr);
  if (rv) {
    perror("gettimeofday");
    abort();
  }
  tv_after.tv_sec -= tv_before.tv_sec;
  time = tv_after.tv_sec * 1E6 + tv_after.tv_usec;
  time -= tv_before.tv_usec;
  time *= 1000;
  return time;
}
#endif

static double run_benchmark(struct internal_bench *b)
{
  long iterations = 128;
  double nsec;
  while (1) {
    nsec = measure_once(b, iterations);
    if (nsec > kTrialNSec) {
      break;
    }
    iterations = ((unsigned long)iterations) << 1;
    if (iterations <= 0) { // overflow
      abort();
    }
  }
  while (nsec < benchmark_duration_nsec) {
    double target_iterations = iterations * benchmark_duration_nsec * 1.1 / nsec;
    if (target_iterations > (double)LONG_MAX) {
      abort();
    }
    iterations = target_iterations;
    nsec = measure_once(b, iterations);
  }
  return nsec / iterations;
}

void report_benchmark(const char *name, bench_body body, uintptr_t param)
{
  std::ostringstream full_name_stream(name, std::ios_base::ate);
  if (param) {
    full_name_stream << "(" << param << ")";
  }
  std::string full_name = full_name_stream.str();

  if (benchmark_list_only) {
    printf("known benchmark: %s\n", full_name.c_str());
    return;
  }

  if (benchmark_filter && !benchmark_filter(std::string_view{full_name})) {
    return;
  }

  internal_bench b;
  b.body = body;
  b.param = param;
  for (int i = 0; i < benchmark_repetitions; i++) {
    int slen = printf("Benchmark: %s", full_name.c_str());
    fflush(stdout);

    double nsec = run_benchmark(&b);
    int padding_size;

    padding_size = 60 - slen;
    if (padding_size < 1) {
      padding_size = 1;
    }
    printf("%*c%f nsec (rate: %f Mops/sec)\n", padding_size, ' ', nsec, 1e9/nsec/1e6);
    fflush(stdout);
  }
}
