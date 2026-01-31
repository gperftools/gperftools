/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
 * Copyright (c) 2024, gperftools Contributors
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
#include "single_stepper.h"

#if __linux__ && __x86_64__ && !defined(TCMALLOC_OMIT_SINGLE_STEPPER)

#include <errno.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#include <atomic>

#include "base/spinlock.h"
#include "base/static_storage.h"

#include "base/logging.h"
#include "base/cleanup.h"

#define STEPPER_SUPPORTED 1

namespace tcmalloc {
namespace {

struct SingleStepperImpl : public SingleStepper {
  ~SingleStepperImpl() = default;

  static inline std::atomic<bool> active = {};
  static inline SingleStepperImpl* active_impl = nullptr;
  static constexpr uintptr_t kTF = 0x100;  // Trace flag in x86 FLAGS register.

  SteppingCallbackFn callback;

  static void step_handler(int signo, siginfo_t* si, void* _uc) {
    tcmalloc::Cleanup preserve_errno([errno_save = errno]() { errno = errno_save; });

    ucontext_t* uc = static_cast<ucontext_t*>(_uc);
    auto at_rip = reinterpret_cast<uint8_t*>(uc->uc_mcontext.gregs[REG_RIP]);
    if (try_handle_sigtrap_blocking(at_rip, uc)) {
      return;
    }

    if (!active.load(std::memory_order_relaxed)) {
      uc->uc_mcontext.gregs[REG_EFL] &= ~kTF;
      return;
    }

    uc->uc_mcontext.gregs[REG_EFL] |= kTF;

    active_impl->callback(uc, active_impl);
  }

  static bool try_handle_sigtrap_blocking(uint8_t* at_rip, ucontext_t* uc) {
    if (at_rip[0] != 0x0f || at_rip[1] != 0x05) {
      return false;
    }

    // syscall instruction. Lets check if someone is about to block
    // SIGTRAP. If so we must turn off single-stepping, because
    // otherwise blocked SIGTRAP and pending single-stepping will kill
    // the process.

    auto& regs = uc->uc_mcontext.gregs;
    if (regs[REG_RAX] != SYS_rt_sigprocmask) {
      return false;
    }
    if (regs[REG_RDI] != SIG_SETMASK && regs[REG_RDI] != SIG_BLOCK) {
      return false;
    }
    sigset_t* newmask = reinterpret_cast<sigset_t*>(regs[REG_RSI]);
    if (!newmask || !sigismember(newmask, SIGTRAP)) {
      return false;
    }

    // okay, once we detected this case, we drop single-stepping
    // flag, block SIGTRAP and raise it. So that when SIGTRAP is
    // eventually unblocked, we'll get back to signal hander and
    // re-set single-stepping back.
    regs[REG_EFL] &= ~kTF;
    raise(SIGTRAP);
    sigset_t* oldmask = reinterpret_cast<sigset_t*>(regs[REG_RDX]);
    if (oldmask) {
      *oldmask = uc->uc_sigmask;
      regs[REG_RDX] = 0;  // handle "get old mask" part, so we can block
      // our signal
    }
    sigaddset(&uc->uc_sigmask, SIGTRAP);

    return true;
  }

  void Start(SteppingCallbackFn callback) override {
    CHECK(!active.load(std::memory_order_relaxed));

    this->callback = callback;
    active_impl = this;

    // Then we prepare SIGTRAP signal handler.
    {
      struct sigaction sa;
      memset(&sa, 0, sizeof(sa));
      sa.sa_sigaction = step_handler;
      sa.sa_flags = SA_RESTART | SA_SIGINFO;
      CHECK(sigaction(SIGTRAP, &sa, nullptr) == 0);
    }

    active.store(true, std::memory_order_relaxed);
    raise(SIGTRAP);
  }

  void Stop() override { active.store(false, std::memory_order_relaxed); }

  bool IsAtLockInstruction(void* uc) override {
    ucontext_t* real_uc = static_cast<ucontext_t*>(uc);
    auto at_rip = reinterpret_cast<uint8_t*>(real_uc->uc_mcontext.gregs[REG_RIP]);
    return (*at_rip == 0xf0);  // LOCK prefix
  }
};

SingleStepperImpl* GetStepper() {
  static TrivialOnce once;
  static StaticStorage<SingleStepperImpl> storage;
  once.RunOnce([]() { storage.Construct(); });
  return storage.get();
}

}  // anonymous namespace
}  // namespace tcmalloc

#endif

namespace tcmalloc {

SingleStepper::~SingleStepper() = default;

std::optional<SingleStepper*> SingleStepper::Get() {
#if STEPPER_SUPPORTED
  return GetStepper();
#else
  return std::nullopt;
#endif
}

}  // namespace tcmalloc
