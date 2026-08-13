/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include <csignal>
#include <string_view>

#include "sim_stepper.h"

static void xsigprocmask(int how, sigset_t* newset, sigset_t* oldset) {
  int rv = sigprocmask(how, newset, oldset);
  if (rv < 0) {
    perror("sigprocmask");
    abort();
  }
}

extern bool sim_stepper_disable_interpreter;

size_t stepped;

int main(int argc, char** argv) {
  if (argc > 1 && std::string_view{argv[1]} == "0") {
    sim_stepper_disable_interpreter = true;
  }

  StartSingleStepper([](void*, void*) { stepped++; }, nullptr);

  sigset_t set;
  sigset_t old;
  sigfillset(&set);
  xsigprocmask(SIG_BLOCK, &set, &old);

  printf("with signals disabled\n");

  xsigprocmask(SIG_SETMASK, &old, 0);

  printf("after signals re-enabled! stepped = %zu\n", stepped);
}
