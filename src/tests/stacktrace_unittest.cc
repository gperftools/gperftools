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

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include "base/commandlineflags.h"
#include "base/logging.h"
#include "google/stacktrace.h"

#ifdef HAVE_EXECINFO_H
#include <execinfo.h>
#endif

// Obtain a backtrace, verify that the expected callers are present in the
// backtrace, and maybe print the backtrace to stdout. 

//-----------------------------------------------------------------------//
void CheckStackTrace4(int i);
void CheckStackTrace3(int i);
void CheckStackTrace2(int i);
void CheckStackTrace1(int i);
void CheckStackTrace(int i);
//-----------------------------------------------------------------------//

// The sequence of functions whose return addresses we expect to see in the
// backtrace.
const int BACKTRACE_STEPS = 5;
void * expected_stack[BACKTRACE_STEPS] = { 
  (void *) &CheckStackTrace4,
  (void *) &CheckStackTrace3,
  (void *) &CheckStackTrace2,
  (void *) &CheckStackTrace1,
  (void *) &CheckStackTrace,
};

// Depending on the architecture/compiler/libraries, (not sure which)
// the current function may or may not appear in the backtrace.
// For gcc-2:
//
// stack[0] is ret addr within CheckStackTrace4
// stack[1] is ret addr within CheckStackTrace3
// stack[2] is ret addr within CheckStackTrace2
// stack[3] is ret addr within CheckStackTrace1
// stack[4] is ret addr within CheckStackTrace
//
// For gcc3-k8:
//
// stack[0] is ret addr within CheckStackTraceLeaf
// stack[1] is ret addr within CheckStackTrace4
// ...
// stack[5] is ret addr within CheckStackTrace

// The google version does not include the caller in the 
// backtrace.  Some other version might.  (glibc backtrace()?)
const int self_in_backtrace = 0;

//-----------------------------------------------------------------------//

void CheckRetAddrIsInFunction( void * ret_addr, void * function_start_addr)
{
  const int typ_fn_len = 0x40; // assume relevant functions are only 0x40 bytes long
  CHECK_GE(ret_addr, function_start_addr);
  CHECK_LE(ret_addr, (void *) ((char *) function_start_addr + typ_fn_len));
}

//-----------------------------------------------------------------------//

void CheckStackTraceLeaf(void) {
  const int STACK_LEN = 10;
  void *stack[STACK_LEN];
  int size;

  size = GetStackTrace(stack, STACK_LEN, 0);
  printf("Obtained %d stack frames.\n", size);
  CHECK_LE(size, STACK_LEN);
  
  for (int i = 0; i < BACKTRACE_STEPS; i++)
  {
    CheckRetAddrIsInFunction(stack[i + self_in_backtrace], expected_stack[i]);
  }


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

//-----------------------------------------------------------------------//

/* Dummy functions to make the backtrace more interesting. */
void CheckStackTrace4(int i) { for (int j = i; j >= 0; j--) CheckStackTraceLeaf(); }
void CheckStackTrace3(int i) { for (int j = i; j >= 0; j--) CheckStackTrace4(j); }
void CheckStackTrace2(int i) { for (int j = i; j >= 0; j--) CheckStackTrace3(j); }
void CheckStackTrace1(int i) { for (int j = i; j >= 0; j--) CheckStackTrace2(j); }
void CheckStackTrace(int i)  { for (int j = i; j >= 0; j--) CheckStackTrace1(j); }

//-----------------------------------------------------------------------//

int main(int argc, char ** argv) {
  
  CheckStackTrace(0);
  
  printf("PASS\n");
  return 0;
}
