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
// Module for heap-profiling.
//
// For full(er) information, see doc/heapprofile.html
//
// This module can be linked into your program with
// no slowdown caused by this unless you activate the profiler
// using one of the following methods:
//
//    1. Before starting the program, set the environment variable
//       "HEAPPROFILE" to be the name of the file to which the profile
//       data should be written.
//
//    2. Programmatically, start and stop the profiler using the
//       routines "HeapProfilerStart(filename)" and "HeapProfilerStop()".
//
// Use pprof to view the resulting profile output.
//    % google3/perftools/pprof <path_to_executable> <profile_file_name>
//    % google3/perftools/pprof --gv  <path_to_executable> <profile_file_name>

#ifndef BASE_HEAP_PROFILER_H__
#define BASE_HEAP_PROFILER_H__

#include <stddef.h>

// Start profiling and arrange to write profile data to file names
// of the form: "prefix.0000", "prefix.0001", ...
extern void HeapProfilerStart(const char* prefix);

// Stop heap profiling.  Can be restarted again with HeapProfilerStart(),
// but the currently accumulated profiling information will be cleared.
extern void HeapProfilerStop();

// Dump a profile now - can be used for dumping at a hopefully
// quiescent state in your program, in order to more easily track down
// memory leaks. Will include the reason in the logged message
extern void HeapProfilerDump(const char *reason);

// Generate current heap profiling information.  The returned pointer
// is a null-terminated string allocated using malloc() and should be
// free()-ed as soon as the caller does not need it anymore.
extern char* GetHeapProfile();

#endif /* BASE_HEAP_PROFILER_H__ */
