// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
// Copyright (c) 2014, 2024, gperftools Contributors
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

#ifndef MALLOC_BACKTRACE_H
#define MALLOC_BACKTRACE_H

#include "config.h"

#include "base/basictypes.h"
#include "gperftools/stacktrace.h"

namespace tcmalloc {

#ifdef NO_TCMALLOC_SAMPLES

inline int GrabBacktrace(void** result, int max_depth, int skip_count) { return 0; }

inline void MallocBacktraceInit() {}

#else

// GrabBacktrace is the API to use when capturing backtrace for
// various tcmalloc features. It has optional emergency malloc
// integration for occasional case where stacktrace capturing method
// calls back to malloc (so we divert those calls to emergency malloc
// facility).
int GrabBacktrace(void** result, int max_depth, int skip_count);

#endif

}  // namespace tcmalloc

#ifndef THIS_IS_MALLOC_BACKTRACE_CC
// When something includes this file, don't let us use 'regular'
// stacktrace API directly.
#define GetStackTrace(...) missing
#define GetStackTraceWithContext(...) missing
#define GetStackFrames(...) missing
#define GetStackFramesWithContext(...) missing
#endif // THIS_IS_MALLOC_BACKTRACE_CC

#endif  // MALLOC_BACKTRACE_H
