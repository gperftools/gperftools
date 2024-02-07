/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
 * Copyright (c) 2024, gperftools Contributors
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "config.h"

#include "base/proc_maps_iterator.h"

#include <ctype.h>    // for isspace()
#include <stdlib.h>   // for getenv()
#include <stdio.h>    // for snprintf(), sscanf()
#include <string.h>   // for memmove(), memchr(), etc.
#include <fcntl.h>    // for open()
#include <errno.h>    // for errno
#include <limits.h>   // for PATH_MAX

#ifdef HAVE_UNISTD_H
#include <unistd.h>   // for read()
#endif

#if (defined(_WIN32) || defined(__MINGW32__)) && !defined(__CYGWIN__) && !defined(__CYGWIN32)
# define PLATFORM_WINDOWS 1
#endif

#if (defined(_WIN32) || defined(__MINGW32__)) && (!defined(__CYGWIN__) && !defined(__CYGWIN32__))
#include <windows.h>   // for DWORD etc
#include <tlhelp32.h>         // for Module32First()
#endif

#if defined __MACH__          // Mac OS X, almost certainly
#include <mach-o/dyld.h>      // for iterating over dll's in ProcMapsIter
#include <mach-o/loader.h>    // for iterating over dll's in ProcMapsIter
#include <sys/types.h>
#include <sys/sysctl.h>       // how we figure out numcpu's on OS X
#elif defined __FreeBSD__
#include <sys/sysctl.h>
#elif defined __sun__         // Solaris
#include <procfs.h>           // for, e.g., prmap_t
#elif defined(PLATFORM_WINDOWS)
#include <process.h>          // for getpid() (actually, _getpid())
#include <shlwapi.h>          // for SHGetValueA()
#include <tlhelp32.h>         // for Module32First()
#elif defined(__QNXNTO__)
#include <sys/mman.h>
#include <sys/sysmacros.h>
#endif

#include "base/logging.h"

#ifdef PLATFORM_WINDOWS
#ifdef MODULEENTRY32
// In a change from the usual W-A pattern, there is no A variant of
// MODULEENTRY32.  Tlhelp32.h #defines the W variant, but not the A.
// In unicode mode, tlhelp32.h #defines MODULEENTRY32 to be
// MODULEENTRY32W.  These #undefs are the only way I see to get back
// access to the original, ascii struct (and related functions).
#undef MODULEENTRY32
#undef Module32First
#undef Module32Next
#undef PMODULEENTRY32
#undef LPMODULEENTRY32
#endif  /* MODULEENTRY32 */
// MinGW doesn't seem to define this, perhaps some windowsen don't either.
#ifndef TH32CS_SNAPMODULE32
#define TH32CS_SNAPMODULE32  0
#endif  /* TH32CS_SNAPMODULE32 */
#endif  /* PLATFORM_WINDOWS */

// Re-run fn until it doesn't cause EINTR.
#define NO_INTR(fn)  do {} while ((fn) < 0 && errno == EINTR)

namespace tcmalloc {

namespace {

// Finds |c| in |text|, and assign '\0' at the found position.
// The original character at the modified position should be |c|.
// A pointer to the modified position is stored in |endptr|.
// |endptr| should not be NULL.
bool ExtractUntilChar(char *text, int c, char **endptr) {
  CHECK_NE(text, NULL);
  CHECK_NE(endptr, NULL);
  char *found;
  found = strchr(text, c);
  if (found == NULL) {
    *endptr = NULL;
    return false;
  }

  *endptr = found;
  *found = '\0';
  return true;
}

// Increments |*text_pointer| while it points a whitespace character.
// It is to follow sscanf's whilespace handling.
void SkipWhileWhitespace(char **text_pointer, int c) {
  if (isspace(c)) {
    while (isspace(**text_pointer) && isspace(*((*text_pointer) + 1))) {
      ++(*text_pointer);
    }
  }
}

template<class T>
T StringToInteger(char *text, char **endptr, int base) {
  assert(false);
  return T();
}

template<>
inline int StringToInteger<int>(char *text, char **endptr, int base) {
  return strtol(text, endptr, base);
}

template<>
inline int64_t StringToInteger<int64_t>(char *text, char **endptr, int base) {
  return strtoll(text, endptr, base);
}

template<>
inline uint64_t StringToInteger<uint64_t>(char *text, char **endptr, int base) {
  return strtoull(text, endptr, base);
}

template<typename T>
T StringToIntegerUntilChar(
    char *text, int base, int c, char **endptr_result) {
  CHECK_NE(endptr_result, NULL);
  *endptr_result = NULL;

  char *endptr_extract;
  if (!ExtractUntilChar(text, c, &endptr_extract))
    return 0;

  T result;
  char *endptr_strto;
  result = StringToInteger<T>(text, &endptr_strto, base);
  *endptr_extract = c;

  if (endptr_extract != endptr_strto)
    return 0;

  *endptr_result = endptr_extract;
  SkipWhileWhitespace(endptr_result, c);

  return result;
}

char *CopyStringUntilChar(
    char *text, unsigned out_len, int c, char *out) {
  char *endptr;
  if (!ExtractUntilChar(text, c, &endptr))
    return NULL;

  strncpy(out, text, out_len);
  out[out_len-1] = '\0';
  *endptr = c;

  SkipWhileWhitespace(&endptr, c);
  return endptr;
}

template<typename T>
bool StringToIntegerUntilCharWithCheck(
    T *outptr, char *text, int base, int c, char **endptr) {
  *outptr = StringToIntegerUntilChar<T>(*endptr, base, c, endptr);
  if (*endptr == NULL || **endptr == '\0') return false;
  ++(*endptr);
  return true;
}

bool ParseProcMapsLine(char *text, uint64_t *start, uint64_t *end,
                       char *flags, uint64_t *offset,
                       int64_t *inode,
                       unsigned *filename_offset) {
#if defined(__linux__) || defined(__NetBSD__)
  /*
   * It's similar to:
   * sscanf(text, "%"SCNx64"-%"SCNx64" %4s %"SCNx64" %x:%x %"SCNd64" %n",
   *        start, end, flags, offset, major, minor, inode, filename_offset)
   */
  char *endptr = text;
  if (endptr == NULL || *endptr == '\0')  return false;

  if (!StringToIntegerUntilCharWithCheck(start, endptr, 16, '-', &endptr))
    return false;

  if (!StringToIntegerUntilCharWithCheck(end, endptr, 16, ' ', &endptr))
    return false;

  endptr = CopyStringUntilChar(endptr, 5, ' ', flags);
  if (endptr == NULL || *endptr == '\0')  return false;
  ++endptr;

  if (!StringToIntegerUntilCharWithCheck(offset, endptr, 16, ' ', &endptr))
    return false;

  int64_t dummy;
  if (!StringToIntegerUntilCharWithCheck(&dummy, endptr, 16, ':', &endptr))
    return false;

  if (!StringToIntegerUntilCharWithCheck(&dummy, endptr, 16, ' ', &endptr))
    return false;

  if (!StringToIntegerUntilCharWithCheck(inode, endptr, 10, ' ', &endptr))
    return false;

  *filename_offset = (endptr - text);
  return true;
#else
  return false;
#endif
}

template <typename Body>
bool ForEachLine(const char* path, const Body& body) {
#ifdef __FreeBSD__
  // FreeBSD requires us to read all of the maps file at once, so
  // we have to make a buffer that's "always" big enough
  // TODO(alk): thats not good enough. We can do better.
  static constexpr size_t kBufSize = 102400;
#else   // a one-line buffer is good enough
  static constexpr size_t kBufSize = PATH_MAX + 1024;
#endif
  char buf[kBufSize];
  char* const buf_end = buf + sizeof(buf) - 1;

  int fd;
  NO_INTR(fd = open(path, O_RDONLY));
  if (fd < 0) {
    return false;
  }

  char* sbuf = nullptr;
  char* ebuf = nullptr;

  bool eof = false;

  for (;;) {
    char* nextline = static_cast<char*>(memchr(sbuf, '\n', ebuf - sbuf));

    if (nextline != nullptr) {
      RAW_CHECK(nextline < ebuf, "BUG");

      *nextline = 0; // Turn newline into '\0'.

      if (!body(sbuf, nextline)) {
        break;
      }

      sbuf = nextline + 1;
      continue;
    }

    int count = ebuf - sbuf;

    // TODO: what if we face way too long line ?

    if (eof) {
      if (count == 0) {
        break; // done
      }

      // Last read ended up without trailing newline. Lets add
      // it. Note, we left one byte margin above, so we're able to
      // write this and not get past end of buf.
      *ebuf++ = '\n';
      continue;
    }

    // Move the current text to the start of the buffer
    memmove(buf, sbuf, count);
    sbuf = buf;
    ebuf = sbuf + count;

    int nread;
    NO_INTR(nread = read(fd, ebuf, buf_end - ebuf));

    // Read failures are not expected, but lets not crash if this
    // happens in non-debug mode.
    DCHECK_GE(nread, 0);
    if (nread < 0) {
      nread = 0;
    }

    if (nread == 0) {
      eof = true;
    }
    ebuf += nread;
    // Retry memchr above.
  }

  close(fd);
  return true;
}

inline
bool DoIterateLinux(const char* path, void (*body)(const ProcMapping& mapping, void* arg), void* arg) {
  // TODO: line_end is unused ?
  return ForEachLine(
    path,
    [&] (char* line_start, char* line_end) {
      unsigned filename_offset;
      char flags_tmp[10];

      ProcMapping mapping;
      if (!ParseProcMapsLine(line_start,
                             &mapping.start, &mapping.end,
                             flags_tmp,
                             &mapping.offset, &mapping.inode,
                             &filename_offset)) {
        int c = line_end - line_start;
        fprintf(stderr, "bad line %d:\n%.*s\n----\n", c, c, line_start);
        return false;
      }

      mapping.filename = line_start + filename_offset;
      mapping.flags = flags_tmp;
      body(mapping, arg);
      return true;
    });
}

#if defined(__QNXNTO__)
inline
bool DoIterateQNX(void (*body)(const ProcMapping& mapping, void* arg), void* arg) {
  return ForEachLine(
    "proc/self/pmap",
    [&] (char* line_start, char* line_end) {
      // https://www.qnx.com/developers/docs/7.1/#com.qnx.doc.neutrino.sys_arch/topic/vm_calculations.html
      // vaddr,size,flags,prot,maxprot,dev,ino,offset,rsv,guardsize,refcnt,mapcnt,path
      // 0x00000025e9df9000,0x0000000000053000,0x00000071,0x05,0x0f,0x0000040b,0x0000000000000094,
      //   0x0000000000000000,0x0000000000000000,0x00000000,0x00000005,0x00000003,/system/xbin/cat
      char flags_tmp[10];
      ProcMapping mapping;
      unsigned filename_offset;

      uint64_t q_vaddr, q_size, q_ino, q_offset;
      uint32_t q_flags, q_dev, q_prot;
      if (sscanf(line_start,
                 "0x%" SCNx64 ",0x%" SCNx64 ",0x%" SCNx32        \
                 ",0x%" SCNx32 ",0x%*x,0x%" SCNx32 ",0x%" SCNx64 \
                 ",0x%" SCNx64 ",0x%*x,0x%*x,0x%*x,0x%*x,%n",
                 &q_vaddr,
                 &q_size,
                 &q_flags,
                 &q_prot,
                 &q_dev,
                 &q_ino,
                 &q_offset,
                 &filename_offset) != 7) {
        return false;
      }

      mapping.start = q_vaddr;
      mapping.end = q_vaddr + q_size;
      mapping.offset = q_offset;
      mapping.inode = q_ino;
      // right shifted by 8 bits, restore it
      q_prot <<= 8;

      char flags_tmp[5] = {
        q_prot & PROT_READ ? 'r' : '-',
        q_prot & PROT_WRITE ? 'w' : '-',
        q_prot & PROT_EXEC ? 'x' : '-',
        q_flags & MAP_SHARED ? 's' : 'p',
        '\0'
      };

      mapping.flags = flags_tmp;
      mapping.filename = line_start + filename_offset;

      body(mapping, arg);

      return true;
    });
}
#endif

inline
bool DoIterateFreeBSD(void (*body)(const ProcMapping& mapping, void* arg), void* arg) {
  return ForEachLine(
    "/proc/curproc/map",
    [&] (char* line_start, char* line_end) {
      if (line_start == line_end) {
        return true; // freebsd is weird
      }
      ProcMapping mapping;
      memset(&mapping, 0, sizeof(mapping));

      unsigned filename_offset;
      char flags_tmp[10];
      // start end resident privateresident obj(?) prot refcnt shadowcnt
      // flags copy_on_write needs_copy type filename:
      // 0x8048000 0x804a000 2 0 0xc104ce70 r-x 1 0 0x0 COW NC vnode /bin/cat
      if (sscanf(line_start, "0x%" SCNx64 " 0x%" SCNx64 " %*d %*d %*p %3s %*d %*d 0x%*x %*s %*s %*s %n",
                 &mapping.start,
                 &mapping.end,
                 flags_tmp,
                 &filename_offset) != 3) {
        return false;
      }

      mapping.flags = flags_tmp;
      mapping.filename = line_start + filename_offset;

      body(mapping, arg);

      return true;
    });
}

#if defined(__sun__)
inline
bool DoIterateSolaris(void (*body)(const ProcMapping& mapping, void* arg), void* arg) {
  int fd;
  NO_INTR(fd = open("/proc/self/map", O_RDONLY));

  // This is based on MA_READ == 4, MA_WRITE == 2, MA_EXEC == 1
  static char kPerms[8][4] = {
    "---", "--x", "-w-", "-wx",
    "r--", "r-x", "rw-", "rwx" };

  static_assert(MA_READ == 4, "solaris MA_READ must equal 4");
  static_assert(MA_WRITE == 2, "solaris MA_WRITE must equal 2");
  static_assert(MA_EXEC == 1, "solaris MA_EXEC must equal 1");

  constexpr ssize_t kFilenameLen = PATH_MAX;
  char current_filename[kFilenameLen];

  int nread = 0;            // fill up buffer with text
  prmap_t mapinfo;

  for (;;) {
    NO_INTR(nread = read(fd, &mapinfo, sizeof(prmap_t)));
    if (nread != sizeof(prmap_t)) {
      CHECK_EQ(nread, 0);
      break;
    }

    char object_path[PATH_MAX + 1000];
    CHECK_LT(snprintf(object_path, sizeof(object_path),
                      "/proc/self/path/%s", mapinfo.pr_mapname),
             sizeof(object_path));
    ssize_t len = readlink(object_path, current_filename, kFilenameLen);
    if (len < 0) {
      len = 0;
    }
    CHECK_LT(len, kFilenameLen);
    current_filename[len] = '\0';

    ProcMapping mapping;
    memset(&mapping, 0, sizeof(mapping));

    mapping.start = mapinfo.pr_vaddr;
    mapping.end = mapinfo.pr_vaddr + mapinfo.pr_size;
    mapping.flags = kPerms[mapinfo.pr_mflags & 7];
    mapping.offset = mapinfo.pr_offset;
    mapping.filename = current_filename;

    body(mapping, arg);
  }

  close(fd);
  return true;
}
#endif

#if defined(PLATFORM_WINDOWS)
inline
bool DoIterateWindows(void (*body)(const ProcMapping& mapping, void* arg), void* arg) {
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE |
                                             TH32CS_SNAPMODULE32,
                                             GetCurrentProcessId());
  if (snapshot == INVALID_HANDLE_VALUE) {
    return false;
  }

  MODULEENTRY32 mod_entry;
  memset(&mod_entry, 0, sizeof(mod_entry));

  ProcMapping mapping;
  memset(&mapping, 0, sizeof(mapping));
  static char kDefaultPerms[5] = "r-xp";
  BOOL ok;

  for (;;) {
    if (mod_entry.dwSize == 0) {  // only possible before first call
      mod_entry.dwSize = sizeof(mod_entry);
      ok = Module32First(snapshot, &mod_entry);
    } else {
      ok = Module32Next(snapshot, &mod_entry);
    }
    if (!ok) {
      break;
    }

    uint64_t base_addr = reinterpret_cast<DWORD_PTR>(mod_entry.modBaseAddr);
    mapping.start = base_addr;
    mapping.end = base_addr + mod_entry.modBaseSize;
    mapping.flags = kDefaultPerms;
    mapping.filename = mod_entry.szExePath;

    body(mapping, arg);
  }

  CloseHandle(snapshot);
  return true;
}
#endif  // defined(PLATFORM_WINDOWS)

#if defined(__MACH__)

// A templatized helper function instantiated for Mach (OS X) only.
// It can handle finding info for both 32 bits and 64 bits.
// Returns true if it successfully handled the hdr, false else.
template<uint32_t kMagic, uint32_t kLCSegment,
         typename MachHeader, typename SegmentCommand>
bool NextExtMachHelper(const mach_header* hdr,
                              int current_image, int current_load_cmd,
                              uint64_t *start, uint64_t *end, const char **flags,
                              uint64_t *offset, int64_t *inode, const char **filename) {
  static char kDefaultPerms[5] = "r-xp";
  if (hdr->magic != kMagic)
    return false;
  const char* lc = (const char *)hdr + sizeof(MachHeader);
  // TODO(csilvers): make this not-quadradic (increment and hold state)
  for (int j = 0; j < current_load_cmd; j++)  // advance to *our* load_cmd
    lc += ((const load_command *)lc)->cmdsize;
  if (((const load_command *)lc)->cmd == kLCSegment) {
    const intptr_t dlloff = _dyld_get_image_vmaddr_slide(current_image);
    const SegmentCommand* sc = (const SegmentCommand *)lc;
    if (start) *start = sc->vmaddr + dlloff;
    if (end) *end = sc->vmaddr + sc->vmsize + dlloff;
    if (flags) *flags = kDefaultPerms;  // can we do better?
    if (offset) *offset = sc->fileoff;
    if (inode) *inode = 0;
    if (filename)
      *filename = const_cast<char*>(_dyld_get_image_name(current_image));
    return true;
  }

  return false;
}

bool DoIterateOSX(void (*body)(const ProcMapping& mapping, void* arg), void* arg) {
  int current_image = _dyld_image_count();   // count down from the top
  int current_load_cmd = -1;

  ProcMapping mapping;
  memset(&mapping, 0, sizeof(mapping));

reenter:
  for (; current_image >= 0; current_image--) {
    const mach_header* hdr = _dyld_get_image_header(current_image);
    if (!hdr) continue;
    if (current_load_cmd < 0)   // set up for this image
      current_load_cmd = hdr->ncmds;  // again, go from the top down

    // We start with the next load command (we've already looked at this one).
    for (current_load_cmd--; current_load_cmd >= 0; current_load_cmd--) {
#ifdef MH_MAGIC_64
      if (NextExtMachHelper<MH_MAGIC_64, LC_SEGMENT_64,
          struct mach_header_64, struct segment_command_64>(
            hdr, current_image, current_load_cmd,
            &mapping.start, &mapping.end,
            &mapping.flags,
            &mapping.offset, &mapping.inode, &mapping.filename)) {
        body(mapping, arg);
        goto reenter;
      }
#endif
      if (NextExtMachHelper<MH_MAGIC, LC_SEGMENT,
          struct mach_header, struct segment_command>(
            hdr, current_image, current_load_cmd,
            &mapping.start, &mapping.end,
            &mapping.flags,
            &mapping.offset, &mapping.inode, &mapping.filename)) {
        body(mapping, arg);
        goto reenter;
      }
    }
    // If we get here, no more load_cmd's in this image talk about
    // segments.  Go on to the next image.
  }

  return true;
}
#endif  // __MACH__

void FormatLine(tcmalloc::GenericWriter* writer,
                uint64_t start, uint64_t end, const char *flags,
                uint64_t offset, int64_t inode,
                const char *filename, dev_t dev) {
  // We assume 'flags' looks like 'rwxp' or 'rwx'.
  char r = (flags && flags[0] == 'r') ? 'r' : '-';
  char w = (flags && flags[0] && flags[1] == 'w') ? 'w' : '-';
  char x = (flags && flags[0] && flags[1] && flags[2] == 'x') ? 'x' : '-';
  // p always seems set on linux, so we set the default to 'p', not '-'
  char p = (flags && flags[0] && flags[1] && flags[2] && flags[3] != 'p')
      ? '-' : 'p';

  writer->AppendF("%08" PRIx64 "-%08" PRIx64 " %c%c%c%c %08" PRIx64 " %02x:%02x %-11" PRId64,
                  start, end, r,w,x,p, offset,
                  static_cast<int>(dev/256), static_cast<int>(dev%256),
                  inode);
  writer->AppendStr(filename);
  writer->AppendStr("\n");
}

}  // namespace

bool DoForEachProcMapping(void (*body)(const ProcMapping& mapping, void* arg), void* arg) {
#if defined(PLATFORM_WINDOWS)
  return DoIterateWindows(body, arg);
#elif defined(__MACH__)
  return DoIterateOSX(body, arg);
#elif defined(__sun__)
  return DoIterateSolaris(body, arg);
#elif defined(__QNXNTO__)
  return DoIterateQNX(body, arg);
#elif defined(__FreeBSD__)
  return DoIterateFreeBSD(body, arg);
#else
  return DoIterateLinux("/proc/self/maps", body, arg);
#endif
}

void SaveProcSelfMaps(GenericWriter* writer) {
  ForEachProcMapping([writer] (const ProcMapping& mapping) {
    FormatLine(writer,
               mapping.start, mapping.end, mapping.flags,
               mapping.offset, mapping.inode,
               mapping.filename, 0);
  });
}

void SaveProcSelfMapsToRawFD(RawFD fd) {
  RawFDGenericWriter<> writer(fd);
  SaveProcSelfMaps(&writer);
}

}  // namespace tcmalloc
