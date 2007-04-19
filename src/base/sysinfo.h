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
//
// ---
// Author: Mike Burrows

#ifndef _SYSINFO_H_
#define _SYSINFO_H_

#include <time.h>
#include <unistd.h>    // for pid_t
#include <stddef.h>    // for size_t
#include <limits.h>    // for PATH_MAX
#include "base/basictypes.h"

// This getenv prefers using /proc/self/environ to calling getenv().
// It's intended to be used in routines that run before main(), when
// the state required for getenv() may not be set up yet.  In particular,
// errno isn't set up until relatively late (after the pthreads library
// has a chance to make it threadsafe), and getenv() doesn't work until then.
// Note that /proc only has the environment at the time the application was
// started, so this routine ignores setenv() calls/etc.  Also note it only
// reads the first 16K of the environment.
const char* GetenvBeforeMain(const char* name);


// A ProcMapsIterator abstracts access to /proc/maps for a given
// process. Needs to be stack-allocatable and avoid using stdio/malloc
// so it can be used in the google stack dumper.
class ProcMapsIterator {

 public:

  static const size_t kBufSize = PATH_MAX + 1024;

  // Create a new iterator for the specified pid
  explicit ProcMapsIterator(pid_t pid);

  // Create an iterator with specified storage (for use in signal
  // handler). "buffer" should point to an area of size kBufSize
  // buffer can be NULL in which case a bufer will be allocated.
  ProcMapsIterator(pid_t pid, char *buffer);

  // Iterate through maps_backing instead of maps if use_maps_backing
  // is true.  Otherwise the same as above.  buffer can be NULL and
  // it will allocate a buffer itself.
  ProcMapsIterator(pid_t pid, char *buffer, bool use_maps_backing);

  // Returns true if the iterator successfully initialized;
  bool Valid() const { return fd_ != -1; }

  // Returns a pointer to the most recently parsed line. Only valid
  // after Next() returns true, and until the iterator is destroyed or
  // Next() is called again.
  const char *CurrentLine() const { return stext_; }

  // Find the next entry in /proc/maps; return true if found or false
  // if at the end of the file.
  //
  // Any of the result pointers can be NULL if you're not interested
  // in those values.
  //
  // If "flags" and "filename" are passed, they end up pointing to
  // storage within the ProcMapsIterator that is valid only until the
  // iterator is destroyed or Next() is called again. The caller may
  // modify the contents of these strings (up as far as the first NUL,
  // and only until the subsequent call to Next()) if desired.

  // The offsets are all uint64 in order to handle the case of a
  // 32-bit process running on a 64-bit kernel
  bool Next(uint64 *start, uint64 *end, char **flags,
            uint64 *offset, int64 *inode, char **filename);

  bool NextExt(uint64 *start, uint64 *end, char **flags,
               uint64 *offset, int64 *inode, char **filename,
               uint64 *file_mapping, uint64 *file_pages,
               uint64 *anon_mapping, uint64 *anon_pages);

  ~ProcMapsIterator();

 private:

  void Init(pid_t pid, char *buffer, bool use_maps_backing);

  char *ibuf_;        // input buffer
  char *stext_;       // start of text
  char *etext_;       // end of text
  char *nextline_;    // start of next line
  char *ebuf_;        // end of buffer (1 char for a nul)
  int   fd_;          // filehandle on /proc/*/maps
  char flags_[10];
  char* dynamic_ibuf_; // dynamically-allocated ibuf_
  bool using_maps_backing_; // true if we are looking at maps_backing instead of maps.

};

#endif   /* #ifndef _SYSINFO_H_ */
