// Copyright 2007 and onwards Google Inc.
// Author: Craig Silverstein
//
// Produce stack trace.  I'm guessing (hoping!) the code is much like
// for x86.  For apple machines, at least, it seems to be; see
//    http://developer.apple.com/documentation/mac/runtimehtml/RTArch-59.html
// Linux has similar code: http://patchwork.ozlabs.org/linuxppc/patch?id=8882

#include <stdint.h>   // for uintptr_t
#include <stdlib.h>   // for NULL
#include <google/stacktrace.h>

// Given a pointer to a stack frame, locate and return the calling
// stackframe, or return NULL if no stackframe can be found. Perform
// sanity checks to reduce the chance that a bad pointer is returned.
static void **NextStackFrame(void **old_sp) {
  void **new_sp = (void **) *old_sp;

  // Check that the transition from frame pointer old_sp to frame
  // pointer new_sp isn't clearly bogus
  if (new_sp <= old_sp) return NULL;
  if ((uintptr_t)new_sp & (sizeof(void *) - 1)) return NULL;
  if ((uintptr_t)new_sp - (uintptr_t)old_sp > 100000) return NULL;
  return new_sp;
}

int GetStackTrace(void** result, int max_depth, int skip_count) {
  void **sp;
  // Apple OS X uses an old version of gnu as -- both Darwin 7.9.0 (Panther)
  // and Darwin 8.8.1 (Tiger) use as 1.38.  This means we have to use a
  // different asm syntax.  I don't know quite the best way to discriminate
  // systems using the old as from the new one; I've gone with __APPLE__.
  // TODO(csilvers): use autoconf instead, to look for 'as --version' == 1 or 2
#ifdef __APPLE__
  __asm__ volatile ("mr %0,r1" : "=r" (sp));
#else
  __asm__ volatile ("mr %0,1" : "=r" (sp));
#endif

  int n = 0;
  while (sp && n < max_depth) {
    if (skip_count > 0) {
      skip_count--;
    } else {
#ifdef __LP64__   // Indicates 64-bit pointers under OS
      result[n++] = *(sp+2);   // ?? untested
#else
      result[n++] = *(sp+1);
#endif
    }
    sp = NextStackFrame(sp);
  }
  return n;
}
