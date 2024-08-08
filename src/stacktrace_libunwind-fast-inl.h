// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
// Copyright (C) 2016 Hudson River Trading LLC <opensource@hudson-trading.com>

// ---
// Author: Nathan Wang
//
// Produce stack trace using libunwind's faster unw_backtrace method.
// This only implements GetStackTrace_libunwind_fast() and falls back to
// GetStack..._libunwind() for the remaining functions. Both libunwind and
// libunwind-fast need to be enabled to use this.

#ifndef BASE_STACKTRACE_LIBINWIND_FAST_INL_H_
#define BASE_STACKTRACE_LIBINWIND_FAST_INL_H_
// Note: this file is included into stacktrace.cc more than once.
// Anything that should only be defined once should be here:

// We only need local unwinder.
#define UNW_LOCAL_ONLY

extern "C" {
#include <assert.h>
#include <libunwind.h>
#include <string.h> // for memset()
}
#include "gperftools/stacktrace.h"

#include "base/basictypes.h"
#include "base/logging.h"

// Sometimes, we can try to get a stack trace from within a stack
// trace, because libunwind can call mmap (maybe indirectly via an
// internal mmap based memory allocator), and that mmap gets trapped
// and causes a stack-trace request.  If were to try to honor that
// recursive request, we'd end up with infinite recursion or deadlock.
// Luckily, it's safe to ignore those subsequent traces.  In such
// cases, we return 0 to indicate the situation.
static __thread int libunwind_fast_recursive ATTR_INITIAL_EXEC;

#endif // BASE_STACKTRACE_LIBINWIND_FAST_INL_H_

// Note: this part of the file is included several times.
// Do not put globals below.

// The following 4 functions are generated from the code below:
//   GetStack{Trace,Frames}()
//   GetStack{Trace,Frames}WithContext()
//
// These functions take the following args:
//   void** result: the stack-trace, as an array
//   int* sizes: the size of each stack frame, as an array
//               (GetStackFrames* only)
//   int max_depth: the size of the result (and sizes) array(s)
//   int skip_count: how many stack pointers to skip before storing in result
//   void* ucp: a ucontext_t* (GetStack{Trace,Frames}WithContext only)
static int GET_STACK_TRACE_OR_FRAMES {
#if (!IS_WITH_CONTEXT && !IS_STACK_FRAMES)
  // GetStackTrace(): Use unw_backtrace() to get the backtrace more quickly.

  // unw_backtrace doesn't allow us to skip traces, so we need to store the full
  // backtrace in a temporary array and copy the result over.
  constexpr size_t kMaxBacktraceSize = 128;

  if (PREDICT_FALSE(skip_count + 2 + max_depth > kMaxBacktraceSize)) {
    // if the search depth is too large, revert back to default libunwind
    // implementation.
    return GetStackTrace_libunwind(result, max_depth, skip_count);
  }

  if (libunwind_fast_recursive) {
    return 0;
  }
  ++libunwind_fast_recursive;

  void *libunwind_backtrace_result[kMaxBacktraceSize];

  // Do not include current and parent frame
  skip_count += 2;
  int stacktrace_size =
      unw_backtrace(libunwind_backtrace_result, max_depth + skip_count);

  if (PREDICT_FALSE(stacktrace_size <= skip_count)) {
    --libunwind_fast_recursive;
    return 0;
  }

  std::copy(libunwind_backtrace_result + skip_count,
            libunwind_backtrace_result + stacktrace_size, result);

  --libunwind_fast_recursive;
  return stacktrace_size - skip_count;
#endif
#if (IS_WITH_CONTEXT && !IS_STACK_FRAMES)
  return GetStackTraceWithContext_libunwind(result, max_depth, skip_count, ucp);
#endif
#if (!IS_WITH_CONTEXT && IS_STACK_FRAMES)
  return GetStackFrames_libunwind(result, sizes, max_depth, skip_count);
#endif
#if (IS_WITH_CONTEXT && IS_STACK_FRAMES)
  return GetStackFramesWithContext_libunwind(result, sizes, max_depth,
                                             skip_count, ucp);
#endif
}
