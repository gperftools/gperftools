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

#include "google/perftools/config.h"
#include <stdio.h>
#include <stdlib.h>
#include "base/commandlineflags.h"
#include "base/logging.h"
#include "google/stacktrace.h"

#ifdef HAVE_EXECINFO_H
#include <execinfo.h>
#endif

void CheckStackTrace(int i);

/* Obtain a backtrace, verify that we are the great-great-grandchild of
 * CheckStackTrace, and maybe print the backtrace to stdout. 
 */
void CheckStackTraceLeaf(void) {
  const int STACK_LEN = 10;
  void *stack[STACK_LEN];
  int size;

  size = GetStackTrace(stack, STACK_LEN, 0);
  printf("Obtained %d stack frames.\n", size);
  CHECK_LE(size, STACK_LEN);
  
  // for some reason, CheckStackTraceLeaf doesn't show up in the backtrace
  // stack[size - 1] is in CheckStackTrace4
  // stack[size - 2] is in CheckStackTrace3
  // stack[size - 3] is in CheckStackTrace2
  // stack[size - 4] is in CheckStackTrace1
  // stack[size - 5] is in CheckStackTrace
  CHECK_GE(stack[size - 4], (void*) &CheckStackTrace);
  CHECK_LE(stack[size - 4], (char*) &CheckStackTrace + 0x40);	// assume function is only 0x40 bytes long


#ifdef HAVE_EXECINFO_H
  {
    char **strings = backtrace_symbols(stack, size);
    
    for (int i = 0; i < size; i++)
      printf("%s\n", strings[i]);
    printf("CheckStackTrace() addr: %p\n", &CheckStackTrace);
    free(strings);
  }
#endif

}

/* Dummy functions to make the backtrace more interesting. */
void CheckStackTrace4(int i) { for (int j = i; j >= 0; j--) CheckStackTraceLeaf(); }
void CheckStackTrace3(int i) { for (int j = i; j >= 0; j--) CheckStackTrace4(j); }
void CheckStackTrace2(int i) { for (int j = i; j >= 0; j--) CheckStackTrace3(j); }
void CheckStackTrace1(int i) { for (int j = i; j >= 0; j--) CheckStackTrace2(j); }
void CheckStackTrace(int i)  { for (int j = i; j >= 0; j--) CheckStackTrace1(j); }

int main(int argc, char ** argv) {
  
  CheckStackTrace(0);
  
  printf("PASS\n");
  return 0;
}
