/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#include "sim_stepper.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>

#include "base/cleanup.h"
#include "base/single_stepper.h"
#include "mini-x86-int.h"

using tcmalloc::SingleStepper;

std::atomic<uint64_t> sim_stepper_simulated_count;
std::atomic<uint64_t> sim_stepper_total_count;
std::atomic<uint64_t> sim_stepper_altstack_skips_count;

bool sim_stepper_disable_interpreter;

namespace {

SimSteppingCallbackFn user_callback;
void* user_callback_data;

bool IsOnAltstack(const ucontext_t* uc) {
  if (uc->uc_stack.ss_flags & SS_DISABLE) return false;
  if (uc->uc_stack.ss_size == 0) return false;

  uintptr_t sp = static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RSP]);

  uintptr_t stack_base = reinterpret_cast<uintptr_t>(uc->uc_stack.ss_sp);
  uintptr_t stack_size = uc->uc_stack.ss_size;

  return (sp - stack_base) < stack_size;
}

void SteppingWrapper(void* uc_void, SingleStepper* stepper) {
  ucontext_t* uc = static_cast<ucontext_t*>(uc_void);

  while (true) {
    sim_stepper_total_count++;
    if (!user_callback) {
      stepper->Stop();
      break;
    }

    {
      tcmalloc::Cleanup preserve_errno([errno_save = errno]() { errno = errno_save; });
      user_callback(uc, user_callback_data);
    }

    if (sim_stepper_disable_interpreter) {
      break;
    }

    if (IsOnAltstack(uc)) {
      sim_stepper_altstack_skips_count++;
      break;
    }

    if (!mini_x86_int_try_advance(uc)) {
      break;
    }
    sim_stepper_simulated_count++;
  }
}

}  // namespace

void StartSingleStepper(SimSteppingCallbackFn fn, void* data) {
  user_callback = fn;
  user_callback_data = data;
  SingleStepper::Get().value()->Start(SteppingWrapper);
}

void StopSingleStepper() {
  user_callback = nullptr;
  //  SingleStepper::Get().value()->Stop();
}

// static __attribute__((destructor)) void print_stats() {
//   StopSingleStepper();
//   printf("Total number of instructions ran: %lld\n", (long long)sim_stepper_total_count.load());
//   printf("  of them are simulated: %lld (%.2f %%)\n", (long long)sim_stepper_simulated_count.load(),
//          sim_stepper_simulated_count.load() * 100.0 / sim_stepper_total_count.load());
// }
