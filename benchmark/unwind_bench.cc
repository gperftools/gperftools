// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
#include "config.h"

#include "base/basictypes.h"
#include "gperftools/stacktrace.h"

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#if HAVE_LIBUNWIND_H
#include <libunwind.h>
#endif

#include "run_benchmark.h"

#if defined(__GNUC__) && defined(__has_attribute)
#if defined(__linux__) && defined(__x86_64__) && defined(_LP64) && __has_attribute(naked)

#include <stddef.h>
#include <ucontext.h>

#define BENCHMARK_UCONTEXT_STUFF 1

extern "C" void getcontext_light(ucontext_t *ctx);

#define R(r) offsetof(ucontext_t, uc_mcontext.gregs[r])

__attribute__((naked))
void getcontext_light(ucontext_t *ctx) {
  __asm__ __volatile__(
	"\tmovq     %%rbx, %c0(%%rdi)\n"
	"\tmovq     %%rbp, %c1(%%rdi)\n"
	"\tmovq     %%r12, %c2(%%rdi)\n"
	"\tmovq     %%r13, %c3(%%rdi)\n"
	"\tmovq     %%r14, %c4(%%rdi)\n"
	"\tmovq     %%r15, %c5(%%rdi)\n"

	"\tmovq     (%%rsp), %%rcx\n"
	"\tmovq     %%rcx, %c6(%%rdi)\n"
	"\tleaq     8(%%rsp), %%rcx\n"                /* Exclude the return address.  */
	"\tmovq     %%rcx, %c7(%%rdi)\n"
	"\tret\n"
        : : "i" (R(REG_RBX)),
          "i" (R(REG_RBP)),
          "i" (R(REG_R12)),
          "i" (R(REG_R13)),
          "i" (R(REG_R14)),
          "i" (R(REG_R15)),
          "i" (R(REG_RIP)),
          "i" (R(REG_RSP)));
}

#undef R

#endif
#endif

#define MAX_FRAMES 2048
static void *frames[MAX_FRAMES];

enum measure_mode {
  MODE_NOOP,
  MODE_WITH_CONTEXT,
  MODE_WITHOUT_CONTEXT
};

static int ATTRIBUTE_NOINLINE measure_unwind(int maxlevel, int mode) {
  int n;

  if (mode == MODE_NOOP)
    return 0;

  if (mode == MODE_WITH_CONTEXT) {
#if BENCHMARK_UCONTEXT_STUFF
    ucontext_t uc;
    getcontext_light(&uc);
    n = GetStackTraceWithContext(frames, MAX_FRAMES, 0, &uc);
#else
    abort();
#endif
  } else {
    n = GetStackTrace(frames, MAX_FRAMES, 0);
  }
  if (n < maxlevel) {
    fprintf(stderr, "Expected at least %d frames, got %d\n", maxlevel, n);
    abort();
  }
  return 0;
}

static int ATTRIBUTE_NOINLINE frame_forcer(int rv) {
#ifdef __GNUC__
  // aargh, clang is too smart and it optimizes out f1 "loop" even
  // despite frame_forcer trick. So lets resort to less portable asm
  // volatile thingy.
  __asm__ __volatile__ ("");
#endif
  return rv;
}

static int ATTRIBUTE_NOINLINE f1(int level, int maxlevel, int mode) {
  if (level == maxlevel)
    return frame_forcer(measure_unwind(maxlevel, mode));
  return frame_forcer(f1(level + 1, maxlevel, mode));
}

static void bench_unwind_no_op(long iterations, uintptr_t param) {
  do {
    f1(0, param, MODE_NOOP);
    iterations -= param;
  } while (iterations > 0);
}

#if BENCHMARK_UCONTEXT_STUFF
static void bench_unwind_context(long iterations, uintptr_t param) {
  do {
    f1(0, param, MODE_WITH_CONTEXT);
    iterations -= param;
  } while (iterations > 0);
}
#endif

static void bench_unwind_no_context(long iterations, uintptr_t param) {
  do {
    f1(0, param, MODE_WITHOUT_CONTEXT);
    iterations -= param;
  } while (iterations > 0);
}

int main(void) {
#if BENCHMARK_UCONTEXT_STUFF
  report_benchmark("unwind_context", bench_unwind_context, 1024);
#endif
  report_benchmark("unwind_no_context", bench_unwind_no_context, 1024);
  report_benchmark("unwind_no_op", bench_unwind_no_op, 1024);

//// TODO: somehow this fails at linking step. Figure out why this is missing
// #if HAVE_LIBUNWIND_H
//   unw_set_caching_policy(unw_local_addr_space, UNW_CACHE_PER_THREAD);
// #endif
  return 0;
}
