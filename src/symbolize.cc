// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
// Copyright (c) 2024, gperftools Contributors
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#include "config.h"

#include "symbolize.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#if HAVE_CXA_DEMANGLE
#include <cxxabi.h>
#endif

#include "libbacktrace_api.h"
#include "base/function_ref.h"

namespace tcmalloc {

void DumpStackTraceToStderr(void * const *stack, int stack_depth,
                            bool want_symbolize, std::string_view line_prefix) {
  backtrace_state* state = nullptr;
  if (want_symbolize) {
    // note, create fresh un-threaded backtrace state which we then
    // "dispose" at the end. This is contrary to libbacktrace's normal
    // recommendations.
    state = tcmalloc_backtrace_create_state(nullptr, /*threaded = */0, nullptr, nullptr);
  }

  auto callback = [&] (uintptr_t pc, const char* filename, int lineno, const char* function) {
    if (function == nullptr) {
      fprintf(stderr, "%.*s%p\n",
              (int)line_prefix.size(), line_prefix.data(),
              reinterpret_cast<void*>(pc));
      return;
    }

    char* demangled = nullptr;
#if HAVE_CXA_DEMANGLE
    size_t length;
    int status = -1;
    demangled = __cxxabiv1::__cxa_demangle(function, nullptr, &length, &status);
    if (status != 0) {
      free(demangled);
      demangled = nullptr;
    }
#endif

    if (filename != nullptr) {
      fprintf(stderr, "%.*s%p %s %s:%d\n",
              (int)line_prefix.size(), line_prefix.data(),
              reinterpret_cast<void*>(pc),
              demangled ? demangled : function,
              filename, lineno);
    } else {
      fprintf(stderr, "%.*s%p %s\n",
              (int)line_prefix.size(), line_prefix.data(),
              reinterpret_cast<void*>(pc),
              demangled ? demangled : function);
    }

    free(demangled);
  };

  for (int i = 0; i < stack_depth; i++) {
    struct _call_data {
      decltype(&callback) callback_ptr;
      uintptr_t pc;
    } call_data;
    call_data.callback_ptr = &callback;
    call_data.pc = reinterpret_cast<uintptr_t>(stack[i]) - 1;

    backtrace_full_callback success = [] (void* data, uintptr_t pc, const char* filename, int lineno, const char* function) -> int {
      (*static_cast<_call_data*>(data)->callback_ptr)(pc, filename, lineno, function);
      return 0;
    };
    backtrace_error_callback error = [] (void* data, const char* msg, int errnum) {
      fprintf(stderr, "symbolization step failed (errnum=%d): %s\n", errnum, msg);
      auto real_data = static_cast<_call_data*>(data);
      (*real_data->callback_ptr)(real_data->pc, nullptr, 0, nullptr);
    };

    if (!want_symbolize) {
      success(&call_data, call_data.pc, nullptr, 0, nullptr);
    } else {
      tcmalloc_backtrace_pcinfo(state, call_data.pc,
                                success,
                                error,
                                &call_data);
    }
  }

  if (want_symbolize) {
    tcmalloc_backtrace_dispose_state(state);
  }
}

}  // namespace tcmalloc
