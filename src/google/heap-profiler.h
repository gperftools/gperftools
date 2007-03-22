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
// This module is safe to link into any program you may wish to profile at some
// point.  It will not cause any noticeable slowdowns unless you activate it at
// some point in your program.  So, for instance, you can do something like
// this (using GNU getopt-long extensions):
//
// int main (int argc, char **argv) {
//   static struct option long_options[] = {
//     {"heap-profile", 1, 0, 0},
//   };
//   int option_index = 0;
//   int c = getopt_long (argc, argv, "", long_options, &option_index);
//
//   if (c == 0 && !strcmp(long_options[option_index].name, "heap-profile")) {
//     HeapProfilerStart(optarg);
//   }
//
//   /* ... */
// }
//
// This allows you to easily profile your program at any time without having to
// recompile, and doesn't slow things down if you are not profiling.
//
// Heap profiles will be written to a sequence of files whose name
// starts with the supplied prefix.
//
// Example:
//   % bin/programname --heap_profile=foo ...
//   % ls foo.*
//      foo.0000.heap
//      foo.0001.heap
//      foo.0002.heap
//      ...
//
// If heap-profiling is turned on, a profile file is dumped every GB
// of allocated data.  You can override this behavior by calling
// HeapProfilerSetAllocationInterval() to a number of bytes N.  If
// you do that, a profile file will be dumped after every N bytes of
// allocations.
//
// If heap profiling is on, we also dump a profile when the
// in-use-bytes reach a new high-water-mark.  Only increases of at
// least 100MB are considered significant changes in the
// high-water-mark.  This number can be changed by calling
// HeapProfilerSetInuseInterval() with a different byte-value.
//
// STL WARNING: The HeapProfiler does not accurately track allocations in
// many STL implementations.  This is because it is common for the default STL
// allocator to keep an internal pool of memory and nevery return it to the
// system.  This means that large allocations may be attributed to an object
// that you know was destroyed.  For a simple example, see
// TestHeapLeakCheckerSTL in src/tests/heap-checker_unittest.cc.
//
// This issue is resolved for GCC 3.3 and 3.4 by setting the environment
// variable GLIBCXX_FORCE_NEW, which forces the STL allocator to call `new' and
// `delete' explicitly for every allocation and deallocation.  For GCC 3.2 and
// previous you will need to compile your source with -D__USE_MALLOC.  For
// other compilers / STL libraries, there may be a similar solution;  See your
// implementation's documentation for information.

#ifndef _HEAP_PROFILER_H
#define _HEAP_PROFILER_H

#include <google/perftools/basictypes.h> // For int64 definition
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

// ---- Configuration accessors ----

// Prefix to which we dump heap profiles.  If empty, we do not dump.  This
// must be set to your desired value before HeapProfiler::Init() is called.
// Default: empty
extern void HeapProfilerSetDumpPath(const char* path);

// Level of logging used by the heap profiler and heap checker (if applicable)
// Default: 0
extern void HeapProfilerSetLogLevel(int level);

// Dump heap profiling information once every specified number of bytes
// allocated by the program.  Default: 1GB
extern void HeapProfilerSetAllocationInterval(int64 interval);

// Dump heap profiling information whenever the high-water 
// memory usage mark increases by the specified number of
// bytes.  Default: 100MB
extern void HeapProfilerSetInuseInterval(int64 interval);

#endif /* _HEAP_PROFILER_H */
