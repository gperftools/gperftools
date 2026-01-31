/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
 * Copyright (c) 2026, gperftools Contributors
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#ifndef BASE_SINGLE_STEPPER_H_
#define BASE_SINGLE_STEPPER_H_
#include "config.h"

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
