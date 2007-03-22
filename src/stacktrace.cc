// Copyright (c) 2005, Google Inc.
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

// ---
// Author: Sanjay Ghemawat
//
// Produce stack trace

#include "config.h"
#include <stdlib.h>
#include "google/stacktrace.h"

#undef IMPLEMENTED_STACK_TRACE

// Linux/x86 implementation (requires the binary to be compiled with
// frame pointers)
#if defined(__i386__) && defined(__linux) && !defined(NO_FRAME_POINTER)
#define IMPLEMENTED_STACK_TRACE
#include "stacktrace_x86-inl.h"
#endif

#if !defined(IMPLEMENTED_STACK_TRACE) && defined(__x86_64__) && HAVE_LIBUNWIND_H
#define IMPLEMENTED_STACK_TRACE
#define UNW_LOCAL_ONLY
#include "stacktrace_libunwind-inl.h"
#endif

#if !defined(IMPLEMENTED_STACK_TRACE) && defined(__x86_64__) && HAVE_UNWIND_H
// This implementation suffers from deadlocks. Don't enable it.
#define IMPLEMENTED_STACK_TRACE
#include "stacktrace_x86_64-inl.h"
#endif

#if !defined(IMPLEMENTED_STACK_TRACE) && !defined(__x86_64__) && HAVE_EXECINFO_H
#define IMPLEMENTED_STACK_TRACE
#include "stacktrace_generic-inl.h"
#endif

#ifndef IMPLEMENTED_STACK_TRACE
#error Cannot calculate stack trace: will need to write for your environment
#endif
