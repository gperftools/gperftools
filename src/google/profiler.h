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
// To use this module, link it into your program.  To activate it
// at runtime, set the environment variable "CPUPROFILE" to be the
// name of the file in which the profile data should be written.
// (If you don't set the environment variable, no profiling will
// happen, and the program should run without any slowdowns.)
//
// Once you have done this, there are two ways to determine which
// region(s) of code should be profiled:
//
// 1. If you set the "PROFILESELECTED" environment variable,
//    only regions of code that are surrounded with "ProfilerEnable()"
//    and "ProfilerDisable()" will be profiled.
// 2. Otherwise, the main thread, and any thread that has had 
//    ProfilerRegisterThread() called on it, will be profiled.
//
// Use pprof to view the resulting profile output.  If you have dot and
// gv installed, you can also get a graphical representation of CPU usage.
//    % pprof <path_to_executable> <profile_file_name>
//    % pprof --dot <path_to_executable> <profile_file_name>
//    % pprof --gv  <path_to_executable> <profile_file_name>

#ifndef _GOOGLE_PROFILER_H
#define _GOOGLE_PROFILER_H

// Start profiling and write profile info into fname.
extern bool ProfilerStart(const char* fname);

// Stop profiling. Can be started again with ProfilerStart(), but
// the currently accumulated profiling data will be cleared.
extern void ProfilerStop();


// These functions have no effect if profiling has not been activated
// globally (by specifying the "CPUPROFILE" environment variable or by
// calling ProfilerStart() ).

// Profile in the given thread.  This is most usefully called when a
// new thread is first entered.  Note this may not work if
// PROFILESELECTED is set.
extern void ProfilerRegisterThread();

// Turn profiling on and off, if PROFILESELECTED has been called.
extern void ProfilerEnable();
extern void ProfilerDisable();

// Write out the current profile information to disk.
extern void ProfilerFlush();

// ------------------------- ProfilerThreadState -----------------------
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
  ProfilerThreadState();

  // Called in a thread to enable or disable profiling on the thread
  // based on whether profiling is currently on or off.
  void ThreadCheck();

private:
  bool          was_enabled_;   // True if profiling was on in our last call
};

#endif /* _GOOGLE_PROFILER_H */
