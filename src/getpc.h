// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
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
// Author: Craig Silverstein
//
// This is an internal header file used by profiler.cc.  It defines
// the single (inline) function GetPC.  GetPC is used in a signal
// handler to figure out the instruction that was being executed when
// the signal-handler was triggered.
//
// To get this, we use the ucontext_t argument to the signal-handler
// callback, which holds the full context of what was going on when
// the signal triggered.  How to get from a ucontext_t to a Program
// Counter is OS-dependent.

#ifndef BASE_GETPC_H_
#define BASE_GETPC_H_

#include "config.h"

// On many linux systems, we may need _GNU_SOURCE to get access to
// the defined constants that define the register we want to see (eg
// REG_EIP).  Note this #define must come first!
#define _GNU_SOURCE 1

#ifdef HAVE_ASM_PTRACE_H
#include <asm/ptrace.h>
#endif
#if HAVE_SYS_UCONTEXT_H
#include <sys/ucontext.h>
#elif HAVE_UCONTEXT_H
#include <ucontext.h>       // for ucontext_t (and also mcontext_t)
#elif defined(HAVE_CYGWIN_SIGNAL_H)
#include <cygwin/signal.h>
typedef ucontext ucontext_t;
#endif

// If this doesn't compile, it's probably because
// PC_FROM_UCONTEXT is the empty string.  You need to figure out
// the right value for your system, and add it to the list in
// configure.ac (or set it manually in your config.h).
inline void* GetPC(const ucontext_t& signal_ucontext) {
#if defined(__s390__) && !defined(__s390x__)
  // Mask out the AMODE31 bit from the PC recorded in the context.
  return (void*)((unsigned long)signal_ucontext.PC_FROM_UCONTEXT & 0x7fffffffUL);
#else
  return (void*)signal_ucontext.PC_FROM_UCONTEXT;   // defined in config.h
#endif
}

#endif  // BASE_GETPC_H_
