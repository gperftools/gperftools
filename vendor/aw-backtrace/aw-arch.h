/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#ifndef AW_ARCH_H_
#define AW_ARCH_H_

#include <stddef.h>
#include <stdint.h>
#include <ucontext.h>

#include <type_traits>

#if defined(__x86_64__)
#include "aw-arch-x86_64.h"
#elif defined(__aarch64__)
#include "aw-arch-aarch64.h"
#else
#error "Unsupported architecture"
#endif

// Fills *out with the Cursor of the function that calls it, as of that call:
// pc is the return address into the caller, sp and fp are the caller's. That
// is exactly what an unwind step out of a callee produces, so the result is an
// ordinary non-leaf frame: the walk looks up pc - 1, i.e. the call site, and
// the caller's own CFI takes it from there.
//
// This is how the entry points (aw_backtrace_full and friends) start a walk
// without a ucontext. They capture their *own* frame, skip reporting it
// (skip_first_callback), and let the regular CFI step find their caller.
// Nothing about the caller's sp can be recovered from the frame record alone
// on aarch64: AAPCS64 does not say where in the frame the record goes, and
// gcc moves it to the top of the frame whenever a function gets a stack
// canary. Unwinding through our own frame needs nothing but correct CFI at a
// call site, which every compiler has to get right for exceptions anyway.
//
// Plain asm (aw-asm-*.S), not inline asm and not a builtin: there is no
// builtin that does this (clang's __builtin_dwarf_cfa is just the frame
// address on aarch64), and a real call does not depend on any inlining
// decision. The asm hardcodes Cursor's layout, hence the asserts.
extern "C" __attribute__((visibility("hidden"))) void aw_capture_caller_cursor(aw_backtrace_internal::Cursor* out);

static_assert(std::is_standard_layout<aw_backtrace_internal::Cursor>::value, "aw-asm-*.S");
static_assert(offsetof(aw_backtrace_internal::Cursor, pc) == 0, "aw-asm-*.S");
static_assert(offsetof(aw_backtrace_internal::Cursor, sp) == 8, "aw-asm-*.S");
static_assert(offsetof(aw_backtrace_internal::Cursor, fp) == 16, "aw-asm-*.S");

#endif  // AW_ARCH_H_
