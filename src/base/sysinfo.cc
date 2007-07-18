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

#include "config.h"
#include <stdlib.h>   // for getenv()
#include <stdio.h>    // for snprintf(), sscanf()
#include <string.h>   // for memmove(), memchr(), etc.
#include <fcntl.h>    // for open()
#ifdef HAVE_UNISTD_H
#include <unistd.h>   // for read()
#endif
#include "base/sysinfo.h"
#include "base/commandlineflags.h"
#include "base/logging.h"

DEFINE_string(procfs_prefix, "", "string to prepend to filenames opened "
              "via OpenProcFile");

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

static void ConstructFilename(const char* spec, pid_t pid,
                              char* buf, int buf_size) {
  // We are duplicating the code here for performance.
  // The second call requires constructing a new string object
  // and then destructing it again, which is a waste if
  // the string ends up being the same as the passed in cstring
  // anyway.
  if (FLAGS_procfs_prefix.empty()) {
    CHECK_LT(snprintf(buf, buf_size,
                      spec,
                      pid ? pid : getpid()), buf_size);
  } else {
    CHECK_LT(snprintf(buf, buf_size,
                      (FLAGS_procfs_prefix + spec).c_str(),
                      pid ? pid : getpid()), buf_size);
  }
}

ProcMapsIterator::ProcMapsIterator(pid_t pid) {
  Init(pid, NULL, false);
}

ProcMapsIterator::ProcMapsIterator(pid_t pid, Buffer *buffer) {
  Init(pid, buffer, false);
}

ProcMapsIterator::ProcMapsIterator(pid_t pid, Buffer *buffer,
                                   bool use_maps_backing) {
  Init(pid, buffer, use_maps_backing);
}

void ProcMapsIterator::Init(pid_t pid, Buffer *buffer,
                            bool use_maps_backing) {
  using_maps_backing_ = use_maps_backing;
  dynamic_buffer_ = NULL;
  if (!buffer) {
    // If the user didn't pass in any buffer storage, allocate it
    // now. This is the normal case; the signal handler passes in a
    // static buffer.
    dynamic_buffer_ = new Buffer;
    buffer = dynamic_buffer_;
  }

  ibuf_ = buffer->buf_;

  stext_ = etext_ = nextline_ = ibuf_;
  ebuf_ = ibuf_ + Buffer::kBufSize - 1;
  nextline_ = ibuf_;

  if (use_maps_backing) {
    ConstructFilename("/proc/%d/maps_backing", pid,
                      ibuf_, Buffer::kBufSize);
  } else {
    ConstructFilename("/proc/%d/maps", pid,
                      ibuf_, Buffer::kBufSize);
  }

  // No error logging since this can be called from the crash dump
  // handler at awkward moments. Users should call Valid() before
  // using.
  fd_ = open(ibuf_, O_RDONLY);
}

ProcMapsIterator::~ProcMapsIterator() {
  delete dynamic_buffer_;
  if (fd_ != -1) close(fd_);
}

bool ProcMapsIterator::Next(uint64 *start, uint64 *end, char **flags,
                            uint64 *offset, int64 *inode, char **filename) {
  return NextExt(start, end, flags, offset, inode, filename, NULL, NULL,
                 NULL, NULL, NULL);
}

// This has too many arguments.  It should really be building
// a map object and returning it.  The problem is that this is called
// when the memory allocator state is undefined, hence the arguments.
bool ProcMapsIterator::NextExt(uint64 *start, uint64 *end, char **flags,
                               uint64 *offset, int64 *inode, char **filename,
                               uint64 *file_mapping, uint64 *file_pages,
                               uint64 *anon_mapping, uint64 *anon_pages,
                               dev_t *dev) {

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
    int major, minor;
    unsigned filename_offset;
    if (sscanf(stext_, "%llx-%llx %4s %llx %x:%x %lld %n",
               start ? start : &tmpstart,
               end ? end : &tmpend,
               flags_,
               offset ? offset : &tmpoffset,
               &major, &minor,
               inode ? inode : &tmpinode, &filename_offset) != 7) continue;

    // We found an entry

    if (flags) *flags = flags_;
    if (filename) *filename = stext_ + filename_offset;
    if (dev) *dev = minor | (major << 8);

    if (using_maps_backing_) {
      // Extract and parse physical page backing info.
      char *backing_ptr = stext_ + filename_offset +
          strlen(stext_+filename_offset);

      // find the second '('
      int paren_count = 0;
      while (--backing_ptr > stext_) {
        if (*backing_ptr == '(') {
          ++paren_count;
          if (paren_count >= 2) {
            uint64 tmp_file_mapping;
            uint64 tmp_file_pages;
            uint64 tmp_anon_mapping;
            uint64 tmp_anon_pages;

            sscanf(backing_ptr+1, "F %llx %lld) (A %llx %lld)",
                   file_mapping ? file_mapping : &tmp_file_mapping,
                   file_pages ? file_pages : &tmp_file_pages,
                   anon_mapping ? anon_mapping : &tmp_anon_mapping,
                   anon_pages ? anon_pages : &tmp_anon_pages);
            // null terminate the file name (there is a space
            // before the first (.
            backing_ptr[-1] = 0;
            break;
          }
        }
      }
    }

    return true;
  } while (etext_ > ibuf_);

  // We didn't find anything
  return false;
}
