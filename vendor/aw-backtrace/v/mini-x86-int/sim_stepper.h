// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
// SPDX-License-Identifier: 0BSD
#ifndef SIM_STEPPER_H_
#define SIM_STEPPER_H_
#include <stdint.h>
#include <sys/types.h>

#include <atomic>

extern std::atomic<uint64_t> sim_stepper_simulated_count;
extern std::atomic<uint64_t> sim_stepper_total_count;
extern std::atomic<uint64_t> sim_stepper_altstack_skips_count;

using SimSteppingCallbackFn = void (*)(void* uc, void* data);

void StartSingleStepper(SimSteppingCallbackFn fn, void* data);
void StopSingleStepper();

#endif  // SIM_STEPPER_H_
