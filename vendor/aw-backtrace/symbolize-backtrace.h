/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#ifndef SYMBOLIZE_BACKTRACE_H_
#define SYMBOLIZE_BACKTRACE_H_

#include <stdint.h>

#include <string_view>

#include "function_ref.h"

struct SymbolizeOutcome {
  uintptr_t pc;
  std::string_view function;
  std::string_view filename;
  int lineno;
  bool inlined;
  std::string_view module;  // backing ELF path, empty if unresolved
  uintptr_t vaddr;          // ELF-relative vaddr; meaningful only when module is non-empty
};

// Abstract interface for symbolizer.
class Symbolizer {
 protected:
  virtual ~Symbolizer();

 public:
  virtual void Add(uintptr_t addr) = 0;
};

// Callback type for symbolization outcomes.
// Parameters: outcome, user_data
using SymbolizeCallback = void (*)(const SymbolizeOutcome&, void*);

void WithSymbolizer(void (*with_callback)(Symbolizer*, void*), void* with_data, SymbolizeCallback outcome_callback,
                    void* outcome_data);

// Executes the callback with a Symbolizer instance.
// The symbolizer accumulates addresses via Add().
// After the `with_callback` returns, or at some point before, the `outcome_callback`
// will be invoked for each resolved symbol (including inlined frames).
// This entire operation is designed to be async-signal-safe.
//
// Why such unusual CPS-like API? Well, we want to expose minimal API
// surface and have maximal flexibility on lifetime and memory
// management of Symbolizer* instances. Only giving Symbolizer* to a
// first callback achieves that. Having separate outcome callback
// gives us another flexibility on when outcomes callback will be
// invoked. The only constraint is they will be invoked in the same
// order as Symbolizer::Add was called, and some time before
// WithSymbolizerFnRef returns.
//
// Specific implementation in this project constructs SymbolizerImpl
// instance on stack (but may cache it for example), then invokes
// with_callback with it. As symbolizer receives Add calls, it buffers
// them up to certain number, then if the buffer is flushed or after
// with_callback returned we "flush" this buffered addresses with the
// dance of spawning symbolization helped, addr2line etc.
//
// There is pretty much identical API in gperftools, but the
// implementation currently exercises libbacktrace's symbolizer under
// the hood. Same API shape covering very different implementations
// (one batching, one not).
inline void WithSymbolizerFnRef(aw_backtrace_internal::FunctionRef<void(Symbolizer*)> with_callback,
                                aw_backtrace_internal::FunctionRef<void(const SymbolizeOutcome&)> outcome_callback) {
  WithSymbolizer(with_callback.fn, with_callback.data, outcome_callback.fn, outcome_callback.data);
}

// Async-signal-safe backtrace dumper.
// Uses helper process (addr2line) to symbolize.
// fd: file descriptor it writes to (e.g. STDERR_FILENO)
// stack: array of return addresses.
// stack_depth: number of entries in stack to dump.
// want_symbolize: if true, attempts to run addr2line.
// line_prefix: prefix for each line of output.
void DumpStackTraceToFD(int fd, void* const* stack, int stack_depth, bool want_symbolize,
                        std::string_view line_prefix = "    ");

#endif  // SYMBOLIZE_BACKTRACE_H_
