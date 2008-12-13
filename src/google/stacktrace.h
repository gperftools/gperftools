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
// Routines to extract the current stack trace.  These functions are
// thread-safe.

#ifndef GOOGLE_STACKTRACE_H_
#define GOOGLE_STACKTRACE_H_

// Annoying stuff for windows -- makes sure clients can import these functions
#ifndef PERFTOOLS_DLL_DECL
# ifdef _WIN32
#   define PERFTOOLS_DLL_DECL  __declspec(dllimport)
# else
#   define PERFTOOLS_DLL_DECL
# endif
#endif


// Skips the most recent "skip_count" stack frames (also skips the
// frame generated for the "GetStackFrames" routine itself), and then
// records the pc values for up to the next "max_depth" frames in
// "pcs", and the corresponding stack frame sizes in "sizes".  Returns
// the number of values recorded in "pcs"/"sizes".
//
// Example:
//      main() { foo(); }
//      foo() { bar(); }
//      bar() {
//        void* pcs[10];
//        int sizes[10];
//        int depth = GetStackFrames(pcs, sizes, 10, 1);
//      }
//
// The GetStackFrames call will skip the frame for "bar".  It will
// return 2 and will produce pc values that map to the following
// procedures:
//      pcs[0]       foo
//      pcs[1]       main
// (Actually, there may be a few more entries after "main" to account for
// startup procedures.)
// And corresponding stack frame sizes will also be recorded:
//    sizes[0]       16
//    sizes[1]       16
// (Stack frame sizes of 16 above are just for illustration purposes.)
// Stack frame sizes of 0 or less indicate that those frame sizes couldn't
// be identified.
//
// This routine may return fewer stack frame entries than are
// available. Also note that "pcs" and "sizes" must both be non-NULL.
extern PERFTOOLS_DLL_DECL int GetStackFrames(void** pcs, int* sizes, int max_depth,
                          int skip_count);

// This is similar to the GetStackFrames routine, except that it returns
// the stack trace only, and not the stack frame sizes as well.
// Example:
//      main() { foo(); }
//      foo() { bar(); }
//      bar() {
//        void* result[10];
//        int depth = GetStackTrace(result, 10, 1);
//      }
//
// This produces:
//      result[0]       foo
//      result[1]       main
//           ....       ...
//
// "result" must not be NULL.
extern PERFTOOLS_DLL_DECL int GetStackTrace(void** result, int max_depth,
                                            int skip_count);

#endif /* GOOGLE_STACKTRACE_H_ */
