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

#include <stdlib.h>   // for getenv()
#include <stdio.h>    // for snprintf(), sscanf()
#include <string.h>   // for memmove(), memchr(), etc.
#include <fcntl.h>    // for open()
#include <unistd.h>   // for read()
#include "base/sysinfo.h"
#include "base/logging.h"

// open/read/close can set errno, which may be illegal at this
// time, so prefer making the syscalls directly if we can.
#ifdef HAVE_SYSCALL_H
# include <syscall.h>
# define safeopen(filename, mode)  syscall(SYS_open, filename, mode)
# define saferead(fd, buffer, size)  syscall(SYS_read, fd, buffer, size)
# define safeclose(fd)  syscall(SYS_close, fd)
#else
# define safeopen(filename, mode)  open(filename, mode)
# define saferead(fd, buffer, size)  read(fd, buffer, size)
# define safeclose(fd)  close(fd)
#endif

const char* GetenvBeforeMain(const char* name) {
  // static is ok because this function should only be called before
  // main(), when we're single-threaded.
  static char envbuf[16<<10];
  if (*envbuf == '\0') {    // haven't read the environ yet
    int fd = safeopen("/proc/self/environ", O_RDONLY);
    if (fd == -1) {         // unable to open the file, fall back onto libc
      RAW_LOG(WARNING, "Unable to open /proc/self/environ, falling back "
                       "on getenv(\"%s\"), which may not work", name);
      return getenv(name);
    }
    // The -2 here guarantees the last two bytes of the buffer will be \0\0
    if (saferead(fd, envbuf, sizeof(envbuf) - 2) < 0) {   // error reading file
      safeclose(fd);
      RAW_LOG(WARNING, "Unable to read from /proc/self/environ, falling back "
                       "on getenv(\"%s\"), which may not work", name);
      return getenv(name);
    }
    safeclose(fd);
  }
  const int namelen = strlen(name);
  const char* p = envbuf;
  while (*p != '\0') {    // will happen at the \0\0 that terminates the buffer
    // proc file has the format NAME=value\0NAME=value\0NAME=value\0...
    const char* endp = (char*)memchr(p, '\0', sizeof(envbuf) - (p - envbuf));
    if (endp == NULL)            // this entry isn't NUL terminated
      return NULL;
    else if (!memcmp(p, name, namelen) && p[namelen] == '=')    // it's a match
      return p + namelen+1;      // point after =
    p = endp + 1;
  }
  return NULL;                   // env var never found
}

ProcMapsIterator::ProcMapsIterator(pid_t pid) {
  Init(pid, NULL);
}

ProcMapsIterator::ProcMapsIterator(pid_t pid, char *buffer) {
  Init(pid, buffer);
}

void ProcMapsIterator::Init(pid_t pid, char *buffer) {
  ibuf_ = buffer;
  dynamic_ibuf_ = NULL;
  if (!ibuf_) {
    // If the user didn't pass in any buffer storage, allocate it
    // now. This is the normal case; the signal handler passes in a
    // static buffer.
    dynamic_ibuf_ = new char[kBufSize];
    ibuf_ = dynamic_ibuf_;
  }

  stext_ = etext_ = nextline_ = ibuf_;
  ebuf_ = ibuf_ + kBufSize - 1;
  nextline_ = ibuf_;

  char filename[64];
  snprintf(filename, sizeof(filename), "/proc/%d/maps", pid?:getpid());
  // No error logging since this can be called from the crash dump
  // handler at awkward moments. Users should call Valid() before
  // using.
  fd_ = open(filename, O_RDONLY);
}  

ProcMapsIterator::~ProcMapsIterator() {
  delete[] dynamic_ibuf_;
  if (fd_ != -1) close(fd_);
}

bool ProcMapsIterator::Next(uint64 *start, uint64 *end, char **flags,
                       uint64 *offset, int64 *inode, char **filename) {

  do {
    // Advance to the start of the next line
    stext_ = nextline_;

    // See if we have a complete line in the buffer already
    nextline_ = static_cast<char *>(memchr (stext_, '\n', etext_ - stext_));
    if (!nextline_) {
      // Shift/fill the buffer so we do have a line
      int count = etext_ - stext_;

      // Move the current text to the start of the buffer
      memmove(ibuf_, stext_, count);
      stext_ = ibuf_;
      etext_ = ibuf_ + count;

      int nread = 0;            // fill up buffer with text
      while (etext_ < ebuf_ && (nread = read(fd_, etext_, ebuf_ - etext_)) > 0) {
        etext_ += nread;
      }

      // Zero out remaining characters in buffer at EOF to avoid returning
      // garbage from subsequent calls.
      if (etext_ != ebuf_ && nread == 0) {
        memset(etext_, 0, ebuf_ - etext_);
      }
      *etext_ = '\n';   // sentinel; safe because ibuf extends 1 char beyond ebuf
      nextline_ = static_cast<char *>(memchr (stext_, '\n', etext_ + 1 - stext_));
    }
    *nextline_ = 0;                // turn newline into nul
    nextline_ += ((nextline_ < etext_)? 1 : 0);  // skip nul if not end of text
    // stext_ now points at a nul-terminated line
    uint64 tmpstart, tmpend, tmpoffset;
    int64 tmpinode;
    int filename_offset;
    if (sscanf(stext_, "%llx-%llx %4s %llx %*x:%*x %lld %n",
               start ?: &tmpstart,
               end ?: &tmpend,
               flags_,
               offset ?: &tmpoffset,
               inode ?: &tmpinode, &filename_offset) != 5) continue;

    // We found an entry

    if (flags) *flags = flags_;
    if (filename) *filename = stext_ + filename_offset;
    return true;
  } while (etext_ > ibuf_);

  // We didn't find anything
  return false;
}
