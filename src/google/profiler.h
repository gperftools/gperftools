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
// Module for CPU profiling based on periodic pc-sampling.
//
// To use this module, link it into your program.  There should
// be no slowdown caused by this unless you activate the profiler
// using one of the steps given below.
//
// To activate the profiler, do one of the following:
//
//    1. Before starting the program, set the environment variable
//       "CPUPROFILE" to be the name of the file to which the profile
//       data should be written.
//
//    2. Programmatically, start and stop the profiler using
//       the routines "ProfilerStart(filename)" and "ProfilerStop()".
//
// All threads in the program are profiled whenever profiling is on.
// There used to be a mechanism where a subset of the threads could be
// profiled, but that functionality no longer exists (it would not
// work correctly in new systems since the interval timer used by the
// profiler is a per-address-space setting in new systems instead of
// being a per-thread setting in 2.4 and earlier systems).
//
// Limitation: on 2.4 and earlier kernels, just the main thread will
// be profiled.
//
// Use pprof to view the resulting profile output.  If you have dot and
// gv installed, you can also get a graphical representation of CPU usage.
//    % pprof <path_to_executable> <profile_file_name>
//    % pprof --dot <path_to_executable> <profile_file_name>
//    % pprof --gv  <path_to_executable> <profile_file_name>

#ifndef _GOOGLE_PROFILER_H
#define _GOOGLE_PROFILER_H

#include <time.h>       // For time_t

// Start profiling and write profile info into fname.
extern bool ProfilerStart(const char* fname);

// Stop profiling. Can be started again with ProfilerStart(), but
// the currently accumulated profiling data will be cleared.
extern void ProfilerStop();

// Flush any currently buffered profiling state to the profile file.
// Has no effect if the profiler has not been started.
extern void ProfilerFlush();


// DEPRECATED: these functions were used to enable/disable profiling
// in the current thread, but no longer do anything.
extern void ProfilerEnable();
extern void ProfilerDisable();

// Returns true if profile is currently enabled
extern bool ProfilingIsEnabledForAllThreads();

// Routine for registering new threads with the profiler.  This is
// most usefully called when a new thread is first entered.
extern void ProfilerRegisterThread();

// Stores state about profiler's current status into "*state".
struct ProfilerState {
  bool   enabled;                // Is profiling currently enabled?
  time_t start_time;             // If enabled, when was profiling started?
  char   profile_name[1024];     // Name of profile file being written, or '\0'
  int    samples_gathered;       // Number of samples gatheered to far (or 0)
};
extern void ProfilerGetCurrentState(ProfilerState* state);

// ------------------------- ProfilerThreadState -----------------------
// DEPRECATED: this class is no longer needed.
//
// A small helper class that allows a thread to periodically check if
// profiling has been enabled or disabled, and to react appropriately
// to ensure that activity in the current thread is included in the
// profile.  Usage:
//
//  ProfileThreadState profile_state;
//  while (true) {
//    ... do some thread work ...
//    profile_state.ThreadCheck();
//  }
class ProfilerThreadState {
 public:
  ProfilerThreadState() { }

  // Called in a thread to enable or disable profiling on the thread
  // based on whether profiling is currently on or off.
  // DEPRECATED: No longer needed
  void ThreadCheck() { }
};

#endif /* _GOOGLE_PROFILER_H */
