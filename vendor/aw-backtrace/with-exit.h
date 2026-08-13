/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#ifndef WITH_EXIT_H_
#define WITH_EXIT_H_
#include <setjmp.h>
#include <stdint.h>

#include "function_ref.h"

// Non-local exits as they should have been. No "setjmp returns twice"
// complications.
//
// WithExit::Run(body) calls body with a cookie. Anything body reaches
// can hand that cookie back to WithExit::Exit to abandon everything
// down to (and including) the body and resume right after the Run
// call, which then reports true. If body just returns, Run reports
// false.
//
// Nothing runs on the way out. Every frame between Exit and Run is
// discarded without executing destructors, so no code inside body may
// rely on RAII for anything that matters.
//
// The implementation is a thin wrapper over _setjmp/_longjmp.
namespace aw_backtrace_internal {

struct ExitCookie {
  uintptr_t data;

  bool operator==(const ExitCookie&) const = default;
};

inline constexpr ExitCookie kInvalidExit = ExitCookie{};

struct WithExit {
  // Returns false if body returned normally, true if it exited via Exit.
  __attribute__((noinline)) static bool Run(FunctionRef<void(ExitCookie)> body) {
    RunFrame frame;
    if (_setjmp(frame.buf) != 0) {
      return true;
    }
    body(ExitCookie{reinterpret_cast<uintptr_t>(&frame)});
    return false;
  }

  // Discards every frame down to the Run call that produced cookie,
  // making it return true.
  [[noreturn]] static void Exit(ExitCookie cookie) {
    _longjmp(reinterpret_cast<RunFrame*>(cookie.data)->buf, 1);
  }

 private:
  // jmp_buf is a little odd (array type), so wrap it in a struct to keep
  // the cookie round-trip sane.
  struct RunFrame {
    jmp_buf buf;
  };
};

}  // namespace aw_backtrace_internal

#endif  // WITH_EXIT_H_
