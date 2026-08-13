/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#undef NDEBUG  // we use assert for effect
#include "sim_stepper.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

// Function to test stepping on
__attribute__((naked)) void TestFunction() {
  // Simple sequence of supported instructions
  // nop: 1 byte
  // add $1, %rax: 4 bytes (0x48 0x83 0xc0 0x01)
  // nop: 1 byte
  asm volatile(R"(
	nop
	add $1, %rax
	nop
	ret
)");
}

int main() {
  printf("Starting sim_stepper_test...\n");

  static int step_count = 0;
  // Start stepping
  StartSingleStepper([](void*, void*) -> void { step_count++; }, nullptr);

  // Execute function on main stack
  TestFunction();

  StopSingleStepper();

  printf("Steps: %d\n", step_count);
  printf("Total instructions: %ld\n", sim_stepper_total_count.load());
  printf("Simulated instructions: %ld\n", sim_stepper_simulated_count.load());

  if (step_count < 3) {
    printf("FAIL: Expected step_count >= 3 (got %d)\n", step_count);
    return 1;
  }

  printf("PASS\n");
}
