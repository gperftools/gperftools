/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "aw-backtrace/aw-backtrace.h"
#include "backtrace-comparer.h"

int answer;

jmp_buf jb;

#if __x86_64__
__attribute__((naked)) void bad_stack(void (*)(void)) {
  asm volatile(R"(
        xor %eax, %eax
        push %rax
        xor $1, %rbp
        call *%rdi
        xor $1, %rbp
        pop %rax
        ret
)");
}
#else
void bad_stack(void (*body)(void)) {
  body();
}
#endif

static volatile bool longjmp_hider;

__attribute__((noinline)) int rec(int acc, int acc2, int n) {
  if (n <= 0) {
    answer = acc + acc2;
    if (!longjmp_hider) {
      _longjmp(jb, 1);
    }
    return answer;
  }

  int ret = rec(acc2, acc + acc2, n - 1);
  asm volatile("" : : "r"(ret));
  return ret;
}

__attribute__((noinline)) int run(int n) {
  if (!_setjmp(jb)) {
    int res = rec(1, 0, n);
    __builtin_trap();
    return res;
  }
  return answer;
}

#ifndef SKIP_COMPARER
void inspect_bt() {
  void* bt[4];
  int count = aw_backtrace(nullptr, bt, 4, 0);
  printf("count = %d\n", count);
}
#endif

int main() {
#ifndef SKIP_COMPARER
  StartBacktraceComparer();
#endif

  int res = run(42);
  printf("res = %d\n", res);
  bad_stack([]() { printf("on \"bad stack\"\n"); });
}
