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
// Produce stack trace.
//
// There are three different ways we can try to get the stack trace:
//
// 1) Our hand-coded stack-unwinder.  This depends on a certain stack
//    layout, which is used by gcc (and those systems using a
//    gcc-compatible ABI) on x86 systems, at least since gcc 2.95.
//    It uses the frame pointer to do its work.
//
// 2) The libunwind library.  This is still in development, and as a
//    separate library adds a new dependency, abut doesn't need a frame
//    pointer.  It also doesn't call malloc.
//
// 3) The gdb unwinder -- also the one used by the c++ exception code.
//    It's obviously well-tested, but has a fatal flaw: it can call
//    malloc() from the unwinder.  This is a problem because we're
//    trying to use the unwinder to instrument malloc().
//
// Note: if you add a new implementation here, make sure it works
// correctly when GetStackTrace() is called with max_depth == 0.
// Some code may do that.

#include "config.h"

// First, the i386 case.
#if defined(__i386__) && __GNUC__ >= 2
# if !defined(NO_FRAME_POINTER)
#   include "stacktrace_x86-inl.h"
# else
#   include "stacktrace_generic-inl.h"
# endif

// Now, the x86_64 case.
#elif defined(__x86_64__) && __GNUC__ >= 2
# if !defined(NO_FRAME_POINTER)
#   include "stacktrace_x86-inl.h"
# elif defined(HAVE_LIBUNWIND_H)  // a proxy for having libunwind installed
#   define UNW_LOCAL_ONLY
#   include "stacktrace_libunwind-inl.h"
# elif 0
    // This is the unwinder used by gdb, which can call malloc (see above).
    // We keep this code around, so we can test cases where libunwind
    // doesn't work, but there's no way to enable it except for manually
    // editing this file (by replacing this "elif 0" with "elif 1", e.g.).
#   include "stacktrace_x86_64-inl.h"
# elif defined(__linux)
#   error Cannnot calculate stack trace: need either libunwind or frame-pointers (see INSTALL file)
# else
#   error Cannnot calculate stack trace: need libunwind (see INSTALL file)
# endif

// The PowerPC case
#elif defined(__ppc__) && __GNUC__ >= 2
# if !defined(NO_FRAME_POINTER)
#   include "stacktrace_powerpc-inl.h"
# else
#   include "stacktrace_generic-inl.h"
# endif

// OK, those are all the processors we know how to deal with.
#else
# error Cannot calculate stack trace: will need to write for your environment
#endif
