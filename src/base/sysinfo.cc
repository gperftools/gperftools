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

#include <config.h>
#if (defined(_WIN32) || defined(__MINGW32__)) && !defined(__CYGWIN__) && !defined(__CYGWIN32)
# define PLATFORM_WINDOWS 1
#endif

#include "base/sysinfo.h"
#include "base/commandlineflags.h"
#include "base/dynamic_annotations.h"   // for RunningOnValgrind
#include "base/logging.h"

#include <tuple>

#include <ctype.h>    // for isspace()
#include <stdlib.h>   // for getenv()
#include <stdio.h>    // for snprintf(), sscanf()
#include <string.h>   // for memmove(), memchr(), etc.
#include <fcntl.h>    // for open()
#include <errno.h>    // for errno
#ifdef HAVE_UNISTD_H
#include <unistd.h>   // for read()
#endif

// open/read/close can set errno, which may be illegal at this
// time, so prefer making the syscalls directly if we can.
#if HAVE_SYS_SYSCALL_H
# include <sys/syscall.h>
#endif
#ifdef SYS_open   // solaris 11, at least sometimes, only defines SYS_openat
# define safeopen(filename, mode)  syscall(SYS_open, filename, mode)
#else
# define safeopen(filename, mode)  open(filename, mode)
#endif
#ifdef SYS_read
# define saferead(fd, buffer, size)  syscall(SYS_read, fd, buffer, size)
#else
# define saferead(fd, buffer, size)  read(fd, buffer, size)
#endif
#ifdef SYS_close
# define safeclose(fd)  syscall(SYS_close, fd)
#else
# define safeclose(fd)  close(fd)
#endif

// ----------------------------------------------------------------------
// GetenvBeforeMain()
// GetUniquePathFromEnv()
//    Some non-trivial getenv-related functions.
// ----------------------------------------------------------------------

// we reimplement memcmp and friends to avoid depending on any glibc
// calls too early in the process lifetime. This allows us to use
// GetenvBeforeMain from inside ifunc handler
static int slow_memcmp(const void *_a, const void *_b, size_t n) {
  const uint8_t *a = reinterpret_cast<const uint8_t *>(_a);
  const uint8_t *b = reinterpret_cast<const uint8_t *>(_b);
  while (n-- != 0) {
    uint8_t ac = *a++;
    uint8_t bc = *b++;
    if (ac != bc) {
      if (ac < bc) {
        return -1;
      }
      return 1;
    }
  }
  return 0;
}

static const char *slow_memchr(const char *s, int c, size_t n) {
  uint8_t ch = static_cast<uint8_t>(c);
  while (n--) {
    if (*s++ == ch) {
      return s - 1;
    }
  }
  return 0;
}

static size_t slow_strlen(const char *s) {
  const char *s2 = slow_memchr(s, '\0', static_cast<size_t>(-1));
  return s2 - s;
}

// It's not safe to call getenv() in the malloc hooks, because they
// might be called extremely early, before libc is done setting up
// correctly.  In particular, the thread library may not be done
// setting up errno.  So instead, we use the built-in __environ array
// if it exists, and otherwise read /proc/self/environ directly, using
// system calls to read the file, and thus avoid setting errno.
// /proc/self/environ has a limit of how much data it exports (around
// 8K), so it's not an ideal solution.
const char* GetenvBeforeMain(const char* name) {
  const int namelen = slow_strlen(name);
#if defined(HAVE___ENVIRON)   // if we have it, it's declared in unistd.h
  if (__environ) {            // can exist but be NULL, if statically linked
    for (char** p = __environ; *p; p++) {
      if (!slow_memcmp(*p, name, namelen) && (*p)[namelen] == '=')
        return *p + namelen+1;
    }
    return NULL;
  }
#endif
#if defined(PLATFORM_WINDOWS)
  // TODO(mbelshe) - repeated calls to this function will overwrite the
  // contents of the static buffer.
  static char envvar_buf[1024];  // enough to hold any envvar we care about
  if (!GetEnvironmentVariableA(name, envvar_buf, sizeof(envvar_buf)-1))
    return NULL;
  return envvar_buf;
#endif
  // static is ok because this function should only be called before
  // main(), when we're single-threaded.
  static char envbuf[16<<10];
  if (*envbuf == '\0') {    // haven't read the environ yet
    int fd = safeopen("/proc/self/environ", O_RDONLY);
    // The -2 below guarantees the last two bytes of the buffer will be \0\0
    if (fd == -1 ||           // unable to open the file, fall back onto libc
        saferead(fd, envbuf, sizeof(envbuf) - 2) < 0) { // error reading file
      RAW_VLOG(1, "Unable to open /proc/self/environ, falling back "
               "on getenv(\"%s\"), which may not work", name);
      if (fd != -1) safeclose(fd);
      return getenv(name);
    }
    safeclose(fd);
  }
  const char* p = envbuf;
  while (*p != '\0') {    // will happen at the \0\0 that terminates the buffer
    // proc file has the format NAME=value\0NAME=value\0NAME=value\0...
    const char* endp = (char*)slow_memchr(p, '\0',
                                          sizeof(envbuf) - (p - envbuf));
    if (endp == NULL)            // this entry isn't NUL terminated
      return NULL;
    else if (!slow_memcmp(p, name, namelen) && p[namelen] == '=')    // it's a match
      return p + namelen+1;      // point after =
    p = endp + 1;
  }
  return NULL;                   // env var never found
}

extern "C" {
  const char* TCMallocGetenvSafe(const char* name) {
    return GetenvBeforeMain(name);
  }
}

// HPC environment auto-detection
// For HPC applications (MPI, OpenSHMEM, etc), it is typical for multiple
// processes not engaged in parent-child relations to be executed on the
// same host.
// In order to enable gperftools to analyze them, these processes need to be
// assigned individual file paths for the files being used.
// The function below is trying to discover well-known HPC environments and
// take advantage of that environment to generate meaningful profile filenames
//
// Returns true iff we need to append process pid to
// GetUniquePathFromEnv value. Second and third return values are
// strings to be appended to path for extra identification.
static std::tuple<bool, const char*, const char*> QueryHPCEnvironment() {
  auto mk = [] (bool a, const char* b, const char* c) {
    // We have to work around gcc 5 bug in tuple constructor. It
    // doesn't let us do {a, b, c}
    //
    // TODO(2023-09-27): officially drop gcc 5 support
    return std::make_tuple<bool, const char*, const char*>(std::move(a), std::move(b), std::move(c));
  };

  // Check for the PMIx environment
  const char* envval = getenv("PMIX_RANK");
  if (envval != nullptr && *envval != 0) {
    // PMIx exposes the rank that is convenient for process identification
    // Don't append pid, since we have rank to differentiate.
    return mk(false, ".rank-", envval);
  }

  // Check for the Slurm environment
  envval = getenv("SLURM_JOB_ID");
  if (envval != nullptr && *envval != 0) {
    // Slurm environment detected
    const char* procid = getenv("SLURM_PROCID");
    if (procid != nullptr && *procid != 0) {
      // Use Slurm process ID to differentiate
      return mk(false, ".slurmid-", procid);
    }
    // Need to add PID to avoid conflicts
    return mk(true, "", "");
  }

  // Check for Open MPI environment
  envval = getenv("OMPI_HOME");
  if (envval != nullptr && *envval != 0) {
    return mk(true, "", "");
  }

  // Check for Hydra process manager (MPICH)
  envval = getenv("PMI_RANK");
  if (envval != nullptr && *envval != 0) {
    return mk(false, ".rank-", envval);
  }

  // No HPC environment was detected
  return mk(false, "", "");
}

namespace {
int GetPID() {
#ifdef _WIN32
  return _getpid();
#else
  return getpid();
#endif
}
}  // namespace

// This takes as an argument an environment-variable name (like
// CPUPROFILE) whose value is supposed to be a file-path, and sets
// path to that path, and returns true.  If the env var doesn't exist,
// or is the empty string, leave path unchanged and returns false.
// The reason this is non-trivial is that this function handles munged
// pathnames.  Here's why:
//
// If we're a child process of the 'main' process, we can't just use
// getenv("CPUPROFILE") -- the parent process will be using that path.
// Instead we append our pid to the pathname.  How do we tell if we're a
// child process?  Ideally we'd set an environment variable that all
// our children would inherit.  But -- and this is seemingly a bug in
// gcc -- if you do a setenv() in a shared libarary in a global
// constructor, the environment setting is lost by the time main() is
// called.  The only safe thing we can do in such a situation is to
// modify the existing envvar.  So we do a hack: in the parent, we set
// the high bit of the 1st char of CPUPROFILE.  In the child, we
// notice the high bit is set and append the pid().  This works
// assuming cpuprofile filenames don't normally have the high bit set
// in their first character!  If that assumption is violated, we'll
// still get a profile, but one with an unexpected name.
// TODO(csilvers): set an envvar instead when we can do it reliably.
bool GetUniquePathFromEnv(const char* env_name, char* path) {
  char* envval = getenv(env_name);

  if (envval == nullptr || *envval == '\0') {
    return false;
  }

  const char* append1 = "";
  const char* append2 = "";
  bool pidIsForced;
  std::tie(pidIsForced, append1, append2) = QueryHPCEnvironment();

  // Generate the "forcing" environment variable name in a form of
  // <ORIG_ENVAR>_USE_PID that requests PID to be used in the file names
  char forceVarName[256];
  snprintf(forceVarName, sizeof(forceVarName), "%s_USE_PID", env_name);

  pidIsForced = pidIsForced || EnvToBool(forceVarName, false);

  // Get information about the child bit and drop it
  const bool childBitDetected = (*envval & 128) != 0;
  *envval &= ~128;

  if (pidIsForced || childBitDetected) {
    snprintf(path, PATH_MAX, "%s%s%s_%d",
             envval, append1, append2, GetPID());
  } else {
    snprintf(path, PATH_MAX, "%s%s%s", envval, append1, append2);
  }

  // Set the child bit for the fork'd processes, unless appending pid
  // was forced by either _USE_PID thingy or via MPI detection stuff.
  if (childBitDetected || !pidIsForced) {
    *envval |= 128;
  }
  return true;
}

int GetSystemCPUsCount()
{
#if defined(PLATFORM_WINDOWS)
  // Get the number of processors.
  SYSTEM_INFO info;
  GetSystemInfo(&info);
  return  info.dwNumberOfProcessors;
#else
  long rv = sysconf(_SC_NPROCESSORS_ONLN);
  if (rv < 0) {
    return 1;
  }
  return static_cast<int>(rv);
#endif
}
