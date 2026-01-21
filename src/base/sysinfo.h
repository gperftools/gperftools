// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
// Copyright (c) 2006, Google Inc.
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

// All functions here are thread-hostile due to file caching unless
// commented otherwise.

#ifndef _SYSINFO_H_
#define _SYSINFO_H_

#include <config.h>

#include <stddef.h>    // for size_t
#include <stdint.h>
#include <limits.h>    // for PATH_MAX

#include "base/basictypes.h"

// This getenv function is safe to call before the C runtime is
// initialized.  On Windows, it utilizes GetEnvironmentVariableW()
// (and the we handle some trivial naive utf16-to-ascii
// conversion). On rest platforms it actually just uses getenv which
// appears to be safe to use from malloc-related contexts.
const char* GetenvBeforeMain(const char* name);

// This takes as an argument an environment-variable name (like
// CPUPROFILE) whose value is supposed to be a file-path, and sets
// path to that path, and returns true.  Non-trivial for surprising
// reasons, as documented in sysinfo.cc.  path must have space PATH_MAX.
bool GetUniquePathFromEnv(const char* env_name, char* path);

int GetSystemCPUsCount();


namespace tcmalloc {
ATTRIBUTE_VISIBILITY_HIDDEN const char* GetProgramInvocationName();

// SafeSetEnv is like setenv, but avoiding malloc or any locks. Only
// works on UNIXy systems. Few things to note:
//
// * just like setenv, it is totally thread-hostile. We use it in some
//   very early initialization (via GetUniquePathFromEnv), where it is
//   okay-ish to assume single thread.
//
// * it sorta leaks memory. It works by mmap-ing memory for the new
//   environ and name=value pair and overwriting environ
//   variable. Successive SafeSetEnv will leak environ memory
//   allocated by the previous invokations. So shouldn't be big deal.
ATTRIBUTE_VISIBILITY_HIDDEN void SafeSetEnv(const char* name, const char* value);
}  // namespace tcmalloc

#endif   /* #ifndef _SYSINFO_H_ */
