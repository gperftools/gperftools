/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#include "symbolize-backtrace.h"

#include <assert.h>
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "check.h"

std::string DoDumpToString(int pipe_rd, int pipe_wr, void* const* stack, int stack_depth, bool want_symbolize,
                           std::string_view line_prefix) {
  std::string result;
  std::thread reader([&]() {
    char buf[1024];
    ssize_t n;
    while ((n = read(pipe_rd, buf, sizeof(buf))) > 0) {
      result.append(buf, n);
    }
    close(pipe_rd);
  });

  DumpStackTraceToFD(pipe_wr, stack, stack_depth, want_symbolize, line_prefix);
  close(pipe_wr);
  reader.join();
  return result;
}

std::string DumpStackTraceToString(void* const* stack, int stack_depth, bool want_symbolize,
                                   std::string_view line_prefix) {
  int pipefd[2];
  if (pipe(pipefd) != 0) {
    perror("pipe");
    abort();
  }
  return DoDumpToString(pipefd[0], pipefd[1], stack, stack_depth, want_symbolize, line_prefix);
}

// The first line of a dump, with its line prefix removed; empty if the dump
// does not start with that prefix.
std::string FirstLineAfterPrefix(const std::string& dump, std::string_view prefix) {
  if (dump.compare(0, prefix.size(), prefix) != 0) {
    return "";
  }
  size_t end = dump.find('\n');
  return dump.substr(prefix.size(), end == std::string::npos ? std::string::npos : end - prefix.size());
}

static int MoveOutOfTheWay(int fd) {
  // Anything comfortably above 2; the point is only to leave 0/1/2 free.
  static constexpr int kHighFdBase = 900;
  int moved = fcntl(fd, F_DUPFD_CLOEXEC, kHighFdBase);
  if (moved < 0) {
    perror("fcntl(F_DUPFD_CLOEXEC)");
    abort();
  }
  return moved;
}

// Symbolizes with the standard descriptors closed, which is the state an
// LD_PRELOAD target is normally in by the time it runs its atexit handlers
// (glibc's close_stdout closes stdout and stderr). Descriptors 0/1/2 being
// free is what makes the socketpair, memfd and pipe the symbolizer creates land
// in that range, and every descriptor it hands to the sym-helper daemon has to
// survive that.
std::string DumpStackTraceWithClosedStdio(void* const* stack, int stack_depth, bool close_stdin) {
  int pipefd[2];
  if (pipe(pipefd) != 0) {
    perror("pipe");
    abort();
  }
  assert(pipefd[0] > 2 && pipefd[1] > 2);

  int saved[3];
  for (int i = 0; i < 3; i++) {
    saved[i] = MoveOutOfTheWay(i);
  }

  close(STDOUT_FILENO);
  close(STDERR_FILENO);
  if (close_stdin) {
    close(STDIN_FILENO);
  }

  std::string result = DoDumpToString(pipefd[0], pipefd[1], stack, stack_depth, true, "CLOSED: ");

  for (int i = 0; i < 3; i++) {
    dup2(saved[i], i);
    close(saved[i]);
  }

  return result;
}

// Test function to be found in backtrace
__attribute__((noinline)) void test_func_1(void (*callback)()) {
  callback();
  asm volatile("");  // no tail call
}

__attribute__((noinline)) void test_func_2(void (*callback)()) {
  callback();
  asm volatile("");  // no tail call
}

struct ExpectedSymbol {
  std::string fn_name;
  bool found;
};

std::vector<ExpectedSymbol> expected_symbols;
bool test_failed = false;

void capture_and_verify() {
  void* stack[64];
  int depth = backtrace(stack, 64);

  printf("Captured backtrace of depth %d:\n", depth);

  // 1. Test WithSymbolizerFnRef with C++ lambdas
  WithSymbolizerFnRef(
      [&](Symbolizer* sym) {
        for (int i = 0; i < depth; ++i) {
          sym->Add((uintptr_t)stack[i] - 1);
        }
      },
      [](const SymbolizeOutcome& outcome) {
        printf("  %.*s (%.*s:%d) %s\n", (int)outcome.function.size(), outcome.function.data(),
               (int)outcome.filename.size(), outcome.filename.data(), outcome.lineno,
               outcome.inlined ? "(inline)" : "");

        // Check if this frame matches any expected symbol
        for (auto& expected : expected_symbols) {
          if (!expected.found && outcome.function.find(expected.fn_name) != std::string::npos) {
            expected.found = true;
            printf("    -> Found expected symbol: %s\n", expected.fn_name.c_str());
          }
        }
      });

  // 2. Test DumpStackTraceToFD with want_symbolize = true
  std::string symbolized_output = DumpStackTraceToString(stack, depth, true, "SYM: ");
  printf("DumpStackTrace (symbolized) output:\n%s\n", symbolized_output.c_str());

  if (symbolized_output.find("SYM: @ 0x") == std::string::npos) {
    printf("FAILED: DumpStackTrace symbolized output missing 'SYM: @ 0x'\n");
    test_failed = true;
  }
  for (const auto& expected : expected_symbols) {
    if (symbolized_output.find(expected.fn_name) == std::string::npos) {
      printf("FAILED: DumpStackTrace symbolized output missing expected function name: %s\n", expected.fn_name.c_str());
      test_failed = true;
    }
  }

  // 3. Test DumpStackTraceToStderr with want_symbolize = false
  std::string unsymbolized_output = DumpStackTraceToString(stack, depth, false, "UNSYM: ");
  printf("DumpStackTraceToStderr (unsymbolized) output:\n%s\n", unsymbolized_output.c_str());

  if (unsymbolized_output.find("UNSYM: @ 0x") == std::string::npos) {
    printf("FAILED: DumpStackTrace unsymbolized output missing 'UNSYM: @ 0x'\n");
    test_failed = true;
  }
}

int compare_ints(const void* a, const void* b) {
  capture_and_verify();
  return (*(int*)a - *(int*)b);
}

extern "C" {
void test_only_symbolize_backtrace_kill_daemon(void);
}

int main() {
  // A healthy sym-helper daemon never delivers SIGCHLD to its owner: its
  // addr2line children are its own, and it outlives every request here.
  signal(SIGCHLD, [](int) -> void {
    fprintf(stderr, "SIGCHLD!\n");
    abort();
  });

  // We expect main, test_func_1, test_func_2, and qsort (maybe) and compare_ints
  expected_symbols = {
      {"main", false},
      {"test_func_1", false},
      {"test_func_2", false},
      {"compare_ints", false},
  };

  test_func_1([]() {
    test_func_2([]() {
      // Invoke qsort to get a frame in libc
      int arr[] = {2, 1};
      qsort(arr, 2, sizeof(int), compare_ints);
    });
  });

  bool successes = !test_failed;
  for (const auto& expected : expected_symbols) {
    if (!expected.found) {
      printf("FAILED: Did not find symbol '%s' in backtrace.\n", expected.fn_name.c_str());
      successes = false;
    }
  }

  {
    // We had a bug where several identical return addresses in a row
    // (like in a naive recursion below) were incorrectly printed as inlining.
    struct T {
      void* stack[64];
      int depth;
      __attribute__((noinline)) int rec(int a, int b, int n) {
        if (n <= 0) {
          depth = backtrace(stack, 64);
          return a + b;
        }
        int ret = rec(b, a + b, n - 1);
        asm volatile("" : "=r"(ret) : "0"(ret));
        return ret;
      }
    };
    T t;
    int answer = t.rec(1, 0, 42);
    printf("answer = %d\n", answer);
    std::string output = DumpStackTraceToString(t.stack, t.depth, true, "| ");
    puts(R"(
expecting several entries like this:
| @ 0x55e2d5c4a471
|  main::T::rec(int, int, int) (/proc/self/cwd/symbolize-backtrace-test.cc:168)
| @ 0x55e2d5c4a45a
|  main::T::rec(int, int, int) (/proc/self/cwd/symbolize-backtrace-test.cc:172)
| @ 0x55e2d5c4a45a
|  main::T::rec(int, int, int) (/proc/self/cwd/symbolize-backtrace-test.cc:172)
| @ 0x55e2d5c4a45a
|  main::T::rec(int, int, int) (/proc/self/cwd/symbolize-backtrace-test.cc:172)
| @ 0x55e2d5c4a45a
|  main::T::rec(int, int, int) (/proc/self/cwd/symbolize-backtrace-test.cc:172)
)");
    auto next_occurence = [&](std::string_view what, size_t prev) -> size_t {
      if (prev == std::string::npos) {
        return prev;
      }
      return output.find(what, prev + 1);
    };
    static constexpr char kOccurrence[] = "|  main::T::rec(int";
    size_t first = output.find(kOccurrence);
    size_t second = next_occurence(kOccurrence, first);
    size_t third = next_occurence(kOccurrence, second);
    printf("first, second, third = %zu, %zu, %zu\n", first, second, third);
    if (std::max(first, std::max(second, third)) == std::string::npos) {
      printf("unable to find three occurences of '%s'\n", kOccurrence);
      printf("Backtrace:\n%s\n", output.c_str());
      successes = false;
    }
  }

  {
    // With 0/1/2 closed, the memfd and completion pipe each request creates
    // land in that range before being handed to the daemon. Getting
    // descriptors in that range wrong has historically been silent: the spawn
    // code the daemon replaced once made every dump come back empty here, with
    // nothing failing.
    //
    // Note the daemon is already running by now (the cases above symbolized),
    // so this does not cover its spawn with 0/1/2 closed, where its own end of
    // the mailbox is the descriptor at risk.
    void* stack[8];
    int depth = backtrace(stack, 8);
    if (depth > 4) {
      depth = 4;  // the top few frames prove the point; keep the log readable
    }
    // With stdin closed as well, all three standard slots are up for grabs.
    for (bool close_stdin : {false, true}) {
      std::string output = DumpStackTraceWithClosedStdio(stack, depth, close_stdin);
      printf("closed-stdio dump (stdin %s):\n%s\n", close_stdin ? "closed" : "open", output.c_str());
      if (output.find("CLOSED: @ 0x") == std::string::npos) {
        printf("FAILED: closed-stdio dump produced no output at all\n");
        successes = false;
      } else if (output.find("main") == std::string::npos) {
        printf("FAILED: closed-stdio dump was not symbolized (no 'main' in it)\n");
        successes = false;
      }
    }
  }

  {
    // A dead daemon must still leave a usable dump: every frame as its pc,
    // module and ELF vaddr, just without symbols. Last, because nothing
    // respawns the daemon. Frame 0 is in main, so its address line has to match
    // what the live daemon printed for it, vaddr included.
    void* stack[8];
    int depth = backtrace(stack, 8);
    if (depth > 4) {
      depth = 4;
    }
    std::string live_output = DumpStackTraceToString(stack, depth, true, "LIVE: ");

    // NOTE: stateful. Breaks symbolization. So keep it last test in
    // the file.
    signal(SIGCHLD, SIG_DFL);
    test_only_symbolize_backtrace_kill_daemon();

    std::string output = DumpStackTraceToString(stack, depth, true, "DEAD: ");
    printf("dead-daemon dump:\n%s\n", output.c_str());
    std::string live_line = FirstLineAfterPrefix(live_output, "LIVE: ");
    std::string dead_line = FirstLineAfterPrefix(output, "DEAD: ");
    unsigned long vaddr_dead, vaddr_live;
    unsigned long addr_dead, addr_live;
    char brace;
    CHECK(sscanf(live_line.c_str(), "@ 0x%lx (%*[^+]+%lx%c", &addr_live, &vaddr_live, &brace) == 3 && brace == ')');
    CHECK(sscanf(dead_line.c_str(), "@ 0x%lx (%*[^+]+%lx%c", &addr_dead, &vaddr_dead, &brace) == 3 && brace == ')');
    printf("addr_live = 0x%lx, vaddr_live = 0x%lx\n", addr_live, vaddr_live);
    if (addr_dead != addr_live || vaddr_dead != vaddr_live) {
      successes = false;
      printf("FAILED: non-matching live vs dead sym-helper daemon\n");
      printf("live: %s\n", live_line.c_str());
      printf("dead: %s\n", dead_line.c_str());
    }
  }

  return successes ? 0 : 1;
}
