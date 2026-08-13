/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#include "with-exit.h"

#include <stdio.h>

#include "check.h"

using aw_backtrace_internal::ExitCookie;
using aw_backtrace_internal::WithExit;

ExitCookie g_active_cookie;

int maybe_exit_normal_returns;

int maybe_exit(bool flag) {
  if (flag) {
    WithExit::Exit(g_active_cookie);
  }
  maybe_exit_normal_returns++;
  return 42;
}

int main() {
  int returned = 0;
  bool failed = WithExit::Run([&](ExitCookie c) {
    g_active_cookie = c;
    returned = maybe_exit(false);
  });

  CHECK(!failed);
  CHECK(returned == 42);
  CHECK(maybe_exit_normal_returns == 1);
  printf("Normal exit works!\n");

  returned = 0;

  failed = WithExit::Run([&](ExitCookie c) {
    g_active_cookie = c;
    returned = maybe_exit(true);
    CHECK(false);
  });

  CHECK(returned == 0);
  CHECK(failed);
  CHECK(maybe_exit_normal_returns == 1);
  printf("Non-local exit works!\n");
}
