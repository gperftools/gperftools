/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
//
// Originally authored for gperftools and relicensed to 0BSD by the author.
#ifndef BASE_SINGLE_STEPPER_H_
#define BASE_SINGLE_STEPPER_H_
// #include "config.h"

#include <optional>

namespace tcmalloc {

class SingleStepper {
 public:
  using SteppingCallbackFn = void (*)(void* uc, SingleStepper* stepper);

  // Start intercepts SIGTRAP and enables single-stepping (in
  // currently running thread). On ~each instruction given callback
  // will be invoked with signal's ucontext.
  //
  // NOTE: it Crashes if Start is invoked twice.
  virtual void Start(SteppingCallbackFn callback) = 0;

  virtual void Stop() = 0;

  // To be called from stepping callback. Inspects pending instruction
  // and returns true if it is some locking instruction. Uses lock
  // prefix on x86.
  virtual bool IsAtLockInstruction(void* uc) = 0;

  static std::optional<SingleStepper*> Get();

 protected:
  virtual ~SingleStepper();
};

}  // namespace tcmalloc

#endif  // BASE_SINGLE_STEPPER_H_
