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
#ifndef LIBBACKTRACE_API_H_
#define LIBBACKTRACE_API_H_

#include <stddef.h>

extern "C" {

// those are originally declared in libbacktrace/backtrace.h, but lets
// declare only the subset and renamed function names that we use. Our
// backtrace-integration contract is to maintain it to match
// libbacktrace's definitions.

struct backtrace_state;
typedef int (*backtrace_full_callback) (void *data, uintptr_t pc,
					const char *filename, int lineno,
					const char *function);
typedef void (*backtrace_error_callback) (void *data, const char *msg,
					  int errnum);

struct backtrace_state *tcmalloc_backtrace_create_state(
  const char *filename, int threaded,
  backtrace_error_callback error_callback, void *data);

int tcmalloc_backtrace_pcinfo(
  struct backtrace_state *state, uintptr_t pc,
  backtrace_full_callback callback,
  backtrace_error_callback error_callback,
  void *data);

typedef void (*backtrace_syminfo_callback) (void *data, uintptr_t pc,
					    const char *symname,
					    uintptr_t symval,
					    uintptr_t symsize);

int tcmalloc_backtrace_syminfo(struct backtrace_state *state, uintptr_t addr,
                               backtrace_syminfo_callback callback,
                               backtrace_error_callback error_callback,
                               void *data);

// backtrace-alloc.cc

// This is part of our "special sauce" that lets is release all memory
// allocated by libbacktrace state instance. We rely on some
// implementation details.
void tcmalloc_backtrace_dispose_state(struct backtrace_state* state);

// This is originally defined in internal.h which we cannot include here.
//
// This is internal libbacktrace api used to allocate memory. We
// replace their implementation with ours (based on low_level_alloc
// facility) and with extra feature of being able to mass-free all of
// it.
struct backtrace_vector
{
  /* The base of the vector.  */
  void *base;
  /* The number of bytes in the vector.  */
  size_t size;
  /* The number of bytes available at the current allocation.  */
  size_t alc;
};

extern void *tcmalloc_backtrace_alloc(
  struct backtrace_state *state, size_t size,
  backtrace_error_callback error_callback,
  void *data);

extern void tcmalloc_backtrace_free(
  struct backtrace_state *state, void *mem,
  size_t size,
  backtrace_error_callback error_callback,
  void *data);

extern void *tcmalloc_backtrace_vector_grow(
  struct backtrace_state *state, size_t size,
  backtrace_error_callback error_callback,
  void *data,
  struct backtrace_vector *vec);

extern void* tcmalloc_backtrace_vector_finish (
  struct backtrace_state *state,
  struct backtrace_vector *vec,
  backtrace_error_callback error_callback,
  void *data);

extern int tcmalloc_backtrace_vector_release (
  struct backtrace_state *state,
  struct backtrace_vector *vec,
  backtrace_error_callback error_callback,
  void *data);

}  // extern "C"


#endif  // LIBBACKTRACE_API_H_
