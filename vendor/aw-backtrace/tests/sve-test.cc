/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
//
// Unwinding through a function with an SVE local, i.e. a scalable vector on
// the stack. gcc describes such a frame's CFA as sp + c + 8 * VG, a DWARF
// expression we don't evaluate, so the frame is guessed through its frame
// record. clang (with frame pointers) gives the same function an x29-based
// CFA instead, which is ordinary CFI. glibc's backtrace() (libgcc's unwinder)
// evaluates the expression exactly, so it is the reference.
//
// Only SveFrame is built for SVE, through the target attribute, so the binary
// runs on any aarch64 CPU, and the test is skipped at runtime without SVE.
// Older compilers may reject SVE intrinsics under the target attribute, so
// they skip it at compile time (generously: the versions below are just the
// ones known to work).
//
// Not a stepped test: the comparer traps on diagnostics, and gcc's frame
// produces the unsupported-expression one by design.

#include <execinfo.h>
#include <stdio.h>
#include <sys/auxv.h>

#include "aw-backtrace/aw-backtrace.h"
#include "check.h"
#include "utils.h"

#if defined(__aarch64__) && (defined(__clang__) ? __clang_major__ >= 17 : __GNUC__ >= 15)
#define HAVE_SVE_TEST 1
#include <arm_sve.h>
#include <asm/hwcap.h>
#endif

#if HAVE_SVE_TEST
namespace {

constexpr int kMaxFrames = 32;
void* aw_trace[kMaxFrames];
int aw_count;
void* ref_trace[kMaxFrames];
int ref_count;

NEVER_INLINE void Escape(void* p) {
  asm volatile("" ::"r"(p) : "memory");
}

NEVER_INLINE void CaptureBoth() {
  aw_count = aw_backtrace(nullptr, aw_trace, kMaxFrames, 0);
  ref_count = backtrace(ref_trace, kMaxFrames);
}

__attribute__((target("+sve"))) NEVER_INLINE int SveFrame(int x) {
  svint32_t v = svdup_s32(x);
  Escape(&v);
  CaptureBoth();
  Escape(&v);
  return x + 1;
}

void Dump(const char* what, void** trace, int count) {
  printf("%s: %d frames\n", what, count);
  for (int i = 0; i < count; i++) {
    printf("  %d: %p\n", i, trace[i]);
  }
}

}  // namespace
#endif  // HAVE_SVE_TEST

int main() {
#if HAVE_SVE_TEST
  if (!(getauxval(AT_HWCAP) & HWCAP_SVE)) {
    puts("no SVE on this CPU, skipping");
    return 0;
  }

  CHECK(SveFrame(41) == 42);
  Dump("aw_backtrace", aw_trace, aw_count);
  Dump("glibc backtrace", ref_trace, ref_count);

  // Frame 0 is in CaptureBoth for both, but at two different call sites.
  // Everything after it must match, SveFrame's caller included.
  CHECK(aw_count >= 3);
  CHECK(aw_count == ref_count);
  for (int i = 1; i < aw_count; i++) {
    CHECK(aw_trace[i] == ref_trace[i]);
  }
  puts("PASS");
#else
  puts("compiler too old for this test (or not aarch64), skipping");
#endif
  return 0;
}
