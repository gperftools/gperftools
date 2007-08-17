/* Copyright (c) 2007, Google Inc.
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
 *
 * ---
 * Author: Craig Silverstein
 *
 * These are some portability typedefs and defines to make it a bit
 * easier to compile this code under VC++.
 *
 * Several of these are taken from glib:
 *    http://developer.gnome.org/doc/API/glib/glib-windows-compatability-functions.html
 */

#ifndef GOOGLE_BASE_WINDOWS_H__
#define GOOGLE_BASE_WINDOWS_H__

#include "config.h"

#ifdef WIN32

#define WIN32_LEAN_AND_MEAN  /* We always want minimal includes */
#include <windows.h>
#include <io.h>              /* because we so often use open/close/etc */
#include <stdarg.h>          /* for va_list */
#include <stdio.h>           /* need this to override stdio's (v)snprintf */

// 4018: signed/unsigned mismatch is common (and ok for signed_i < unsigned_i)
// 4244: otherwise we get problems when substracting two size_t's to an int
// 4288: VC++7 gets confused when a var is defined in a loop and then after it
// 4267: too many false positives for "conversion gives possible data loss"
// 4290: it's ok windows ignores the "throw" directive
// 4996: Yes, we're ok using "unsafe" functions like vsnprintf and getenv()
#pragma warning(disable:4018 4244 4288 4267 4290 4996)

// ----------------------------------- BASIC TYPES

#ifndef HAVE_STDINT_H
#ifndef HAVE___INT64    /* we need to have all the __intX names */
# error  Do not know how to set up type aliases.  Edit port.h for your system.
#endif
typedef __int8 int8_t;
typedef __int16 int16_t;
typedef __int32 int32_t;
typedef __int64 int64_t;
typedef unsigned __int8 uint8_t;
typedef unsigned __int16 uint16_t;
typedef unsigned __int32 uint32_t;
typedef unsigned __int64 uint64_t;
#endif

// I guess windows <types.h> doesn't include ssize_t by default?
typedef intptr_t ssize_t;

// ----------------------------------- THREADS
typedef DWORD pthread_t;
typedef DWORD pthread_key_t;
typedef volatile LONG pthread_once_t;
enum { PTHREAD_ONCE_INIT = 0 };   // important that this be 0! for SpinLock
#define pthread_self  GetCurrentThreadId
#define pthread_equal(pthread_t_1, pthread_t_2)  ((pthread_t_1)==(pthread_t_2))

// This replaces maybe_threads.{h,cc}
extern pthread_key_t PthreadKeyCreate(void (*destr_fn)(void*));  // in port.cc
#define perftools_pthread_key_create(pkey, destr_fn)  \
  *(pkey) = PthreadKeyCreate(destr_fn)
#define perftools_pthread_getspecific(key) \
  TlsGetValue(key)
#define perftools_pthread_setspecific(key, val) \
  TlsSetValue((key), (val))
// NOTE: this is Win98 and later.  For Win95 we could use a CRITICAL_SECTION...
#define perftools_pthread_once(once, init)  do {                \
  if (InterlockedCompareExchange(once, 1, 0) == 0) (init)();    \
} while (0)

// __declspec(thread) isn't usable in a dll opened via LoadLibrary().
// But it doesn't work to LoadLibrary() us anyway, because of all the
// things we need to do before main()!  So this kind of TLS is safe for us.
#define __thread __declspec(thread)

// Windows uses a spinlock internally for its mutexes, making our life easy!
// However, the Windows spinlock must always be initialized, making life hard,
// since we want LINKER_INITIALIZED.  We work around this by having the
// linker initialize a bool to 0, and check that before accessing the mutex.
// TODO(csilvers): figure out a faster way.
// This replaces spinlock.{h,cc}, and all the stuff it depends on (atomicops)
class SpinLock {
 public:
  SpinLock() : initialize_token_(PTHREAD_ONCE_INIT) {}
  // Used for global SpinLock vars (see base/spinlock.h for more details).
  enum StaticInitializer { LINKER_INITIALIZED };
  explicit SpinLock(StaticInitializer) : initialize_token_(PTHREAD_ONCE_INIT) {}
  ~SpinLock() {
    perftools_pthread_once(&initialize_token_, InitializeMutex);
    DeleteCriticalSection(&mutex_);
  }

  void Lock() {
    perftools_pthread_once(&initialize_token_, InitializeMutex);
    EnterCriticalSection(&mutex_);
  }
  void Unlock() {
    LeaveCriticalSection(&mutex_);
  }

  // Used in assertion checks: assert(lock.IsHeld()) (see base/spinlock.h).
  inline bool IsHeld() const {
    // This works, but probes undocumented internals, so I've commented it out.
    // c.f. http://msdn.microsoft.com/msdnmag/issues/03/12/CriticalSections/
    //return mutex_.LockCount>=0 && mutex_.OwningThread==GetCurrentThreadId();
    return true;
  }
 private:
  void InitializeMutex() { InitializeCriticalSection(&mutex_); }

  pthread_once_t initialize_token_;
  CRITICAL_SECTION mutex_;
};

class SpinLockHolder {  // Acquires a spinlock for as long as the scope lasts
 private:
  SpinLock* lock_;
 public:
  inline explicit SpinLockHolder(SpinLock* l) : lock_(l) { l->Lock(); }
  inline ~SpinLockHolder() { lock_->Unlock(); }
};

// This replaces testutil.{h,cc}
extern PERFTOOLS_DLL_DECL void RunInThread(void (*fn)());
extern PERFTOOLS_DLL_DECL void RunManyInThread(void (*fn)(), int count);
extern PERFTOOLS_DLL_DECL void RunManyInThreadWithId(void (*fn)(int), int count,
                                                     int stacksize);


// ----------------------------------- MMAP and other memory allocation
#define MAP_FAILED  0
#define MREMAP_FIXED  2  // the value in linux, though it doesn't really matter
// These, when combined with the mmap invariants below, yield the proper action
#define PROT_READ      PAGE_READWRITE
#define PROT_WRITE     PAGE_READWRITE
#define MAP_ANONYMOUS  MEM_RESERVE
#define MAP_PRIVATE    MEM_COMMIT
#define MAP_SHARED     MEM_RESERVE   // value of this #define is 100% arbitrary

// VirtualAlloc is only a replacement for mmap when certain invariants are kept
#define mmap(start, length, prot, flags, fd, offset)                          \
  ( (start) == NULL && (fd) == -1 && (offset) == 0 &&                         \
    (prot) == (PROT_READ|PROT_WRITE) && (flags) == (MAP_PRIVATE|MAP_ANONYMOUS)\
      ? VirtualAlloc(0, length, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)     \
      : NULL )

#define munmap(start, length)   (VirtualFree(start, 0, MEM_RELEASE) ? 0 : -1)

// We could maybe use VirtualAlloc for sbrk as well, but no need
#define sbrk(increment)  ( (void*)-1 )   // sbrk returns -1 on failure


// ----------------------------------- STRING ROUTINES

// We can't just use _vsnprintf and _snprintf as drop-in-replacements,
// because they don't always NUL-terminate. :-(  We also can't use the
// name vsnprintf, since windows defines that (but not snprintf (!)).
extern PERFTOOLS_DLL_DECL int snprintf(char *str, size_t size,
                                       const char *format, ...);
extern PERFTOOLS_DLL_DECL int safe_vsnprintf(char *str, size_t size,
                                             const char *format, va_list ap);
#define vsnprintf(str, size, format, ap)  safe_vsnprintf(str, size, format, ap)

// ----------------------------------- FILE IO
#define PATH_MAX 1024
enum { STDIN_FILENO = 0, STDOUT_FILENO = 1, STDERR_FILENO = 2 };
#define getcwd  _getcwd
#define access  _access
#define open    _open
#define read    _read
#define write   _write
#define lseek   _lseek
#define close   _close
#define popen   _popen
#define pclose  _pclose

// ----------------------------------- SYSTEM/PROCESS
typedef int pid_t;
#define getpid  _getpid

extern PERFTOOLS_DLL_DECL int getpagesize();   // in port.cc

// ----------------------------------- OTHER

#define srandom  srand
#define random   rand

#define __THROW throw()

// ----------------------------------- TCMALLOC-SPECIFIC

// By defining this, we get away without having to get a StackTrace
// But maybe play around with ExperimentalGetStackTrace in port.cc
#define NO_TCMALLOC_SAMPLES

// tcmalloc.cc calls this so we can patch VirtualAlloc() et al.
// TODO(csilvers): instead of patching the functions, consider just replacing
// them.  To do this, add the following post-build step the the .vcproj:
// <Tool Name="VCPostBuildEventTool"
//       CommandLine="lib /out:$(OutDir)\tmp.lib /remove:build\intel\xst_obj\dbgheap.obj libcd.lib
//       lib /out:$(OutDir)\foo.lib $(OutDir)\libem.lib $(OutDir)\tmp.lib
// "/>
// libem.lib is a library you write that defines calloc, free, malloc,
// realloc, _calloc_dbg, _free_dbg, _msize_dbg, _malloc_dbg,
// _realloc_dbg, _CrtDumpMemoryLeaks, and _CrtSetDbgFlag (at least for
// VC++ 7.1).  The list of functions to override, and the name/location of
// the files to remove, may differ between VC++ versions.

extern PERFTOOLS_DLL_DECL void PatchWindowsFunctions();
extern PERFTOOLS_DLL_DECL void UnpatchWindowsFunctions();

#endif  /* WIN32 */

#endif  /* GOOGLE_BASE_WINDOWS_H__ */
