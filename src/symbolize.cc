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

namespace tcmalloc {

class SymbolizePrinter {
public:
  SymbolizePrinter(backtrace_state* state, FunctionRef<void(const SymbolizeOutcome& outcome)> outcome_callback)
    : state_(state), outcome_callback_{outcome_callback} {}

  void OnePC(uintptr_t pc) {
    if (!state_) {
      DemangleAndPrint(pc, nullptr, 0, nullptr, 0);
      return;
    }

    pc_ = pc;
    want_syminfo_ = false;
    tcmalloc_backtrace_pcinfo(state_, pc,
                              &pcinfo_success,
                              &pcinfo_error,
                              this);
    if (want_syminfo_) {
      tcmalloc_backtrace_syminfo(state_, pc,
                                 &syminfo_success,
                                 &syminfo_error,
                                 this);
    }
  }

  static int pcinfo_success(void* data, uintptr_t pc, const char* filename, int lineno, const char* function) {
    auto printer = static_cast<SymbolizePrinter*>(data);
    if (function == nullptr) {
      printer->want_syminfo_ = true;
      return 1;
    }

    printer->DemangleAndPrint(pc, filename, lineno, function, 0);

    return 0;
  }

  static void pcinfo_error(void* data, const char* msg, int errnum) {
    fprintf(stderr, "symbolization step failed (errnum=%d): %s\n", errnum, msg);

    auto printer = static_cast<SymbolizePrinter*>(data);
    printer->want_syminfo_ = true;
  }

  static void syminfo_success(void* data, uintptr_t pc, const char* symname, uintptr_t symval, uintptr_t symsize) {
    static_cast<SymbolizePrinter*>(data)->DemangleAndPrint(pc, nullptr, 0, symname, symval);
  }

  static void syminfo_error(void* data, const char* msg, int errnum) {
    fprintf(stderr, "symbolization syminfo step failed (errnum=%d): %s\n", errnum, msg);
    auto printer = static_cast<SymbolizePrinter*>(data);
    printer->DemangleAndPrint(printer->pc_, nullptr, 0, nullptr, 0);
  }

  void DemangleAndPrint(uintptr_t pc, const char* filename, int lineno, const char* function, uintptr_t symval) {
    char* demangled = nullptr;

#if HAVE_CXA_DEMANGLE
    size_t length;
    int status = -1;
    if (function != nullptr) {
      demangled = __cxxabiv1::__cxa_demangle(function, nullptr, &length, &status);
      if (status != 0) {
        free(demangled);
        demangled = nullptr;
      }
    }
#endif

    SymbolizeOutcome outcome;
    outcome.pc = pc;
    outcome.function = demangled ? demangled : function;
    outcome.original_function = function;
    outcome.filename = filename;
    outcome.lineno = lineno;
    outcome.symval = symval;

    outcome_callback_(outcome);

    free(demangled);
  }

private:
  backtrace_state* const state_;
  FunctionRef<void(const SymbolizeOutcome&)> const outcome_callback_;

  uintptr_t pc_{};
  bool want_syminfo_;
};

SymbolizerAPI::SymbolizerAPI(FunctionRef<void(const SymbolizeOutcome& outcome)> *callback)
  : callback_(callback),
    // note, we create fresh un-threaded backtrace state which we
    // "dispose" at the end. This is contrary to libbacktrace's normal
    // recommendations.
    state_(tcmalloc_backtrace_create_state(nullptr, /*threaded = */0, nullptr, nullptr)) {}

void SymbolizerAPI::Add(uintptr_t addr) const {
  SymbolizePrinter{state_, *callback_}.OnePC(addr);
}

SymbolizerAPI::~SymbolizerAPI() {
  tcmalloc_backtrace_dispose_state(state_);
}

void DumpStackTraceToStderr(void * const *stack, int stack_depth,
                            bool want_symbolize, std::string_view line_prefix) {
  if (!want_symbolize) {
    for (int i = 0; i < stack_depth; i++) {
      fprintf(stderr,"%.*s%p\n",
              (int)line_prefix.size(), line_prefix.data(),
              stack[i]);
    }
    return;
  }

  SymbolizerAPI::With(
    [&] (const SymbolizerAPI& api) {
      for (int i = 0; i < stack_depth; i++) {
        api.Add(reinterpret_cast<uintptr_t>(stack[i]) - 1);
      }
    },
    [&] (const SymbolizeOutcome& o) {
      if (o.filename != nullptr) {
        // We assume that function name is not blank in this case.
        fprintf(stderr, "%.*s%p %s %s:%d\n",
                (int)line_prefix.size(), line_prefix.data(),
                reinterpret_cast<void*>(o.pc),
                o.function,
                o.filename, o.lineno);
      } else if (o.function == nullptr) {
        fprintf(stderr, "%.*s%p\n",
                (int)line_prefix.size(), line_prefix.data(),
                reinterpret_cast<void*>(o.pc));
      } else if (o.symval != 0) {
        fprintf(stderr, "%.*s%p %s + %zu\n",
                (int)line_prefix.size(), line_prefix.data(),
                reinterpret_cast<void*>(o.pc),
                o.function, o.pc - o.symval);
      } else {
        fprintf(stderr, "%.*s%p %s\n",
                (int)line_prefix.size(), line_prefix.data(),
                reinterpret_cast<void*>(o.pc),
                o.function);
      }
    });
}

}  // namespace tcmalloc
