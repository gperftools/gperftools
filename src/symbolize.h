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

#ifndef TCMALLOC_SYMBOLIZE_H_
#define TCMALLOC_SYMBOLIZE_H_

#include "config.h"

#include <stdint.h>

#include <string_view>

#include "base/basictypes.h"
#include "base/function_ref.h"

extern "C" {
  struct backtrace_state;
}

namespace tcmalloc {

ATTRIBUTE_VISIBILITY_HIDDEN
void DumpStackTraceToStderr(
  void * const *stack, int stack_depth, bool want_symbolize,
  std::string_view line_prefix);

struct SymbolizeOutcome {
  uintptr_t pc;
  const char* function;
  const char* filename;
  int lineno;
  uintptr_t symval;
  const char* original_function;
};

class ATTRIBUTE_VISIBILITY_HIDDEN SymbolizerAPI {
public:
  explicit SymbolizerAPI(FunctionRef<void(const SymbolizeOutcome& outcome)> *callback);
  ~SymbolizerAPI();

  static void With(FunctionRef<void(const SymbolizerAPI& api)> body,
                   FunctionRef<void(const SymbolizeOutcome&)> callback) {
    body(SymbolizerAPI{&callback});
  }

  void Add(uintptr_t addr) const;
private:
  FunctionRef<void(const SymbolizeOutcome&)> *callback_;
  backtrace_state* state_;
};

}  // namespace tcmalloc

#endif  // TCMALLOC_SYMBOLIZE_H_
