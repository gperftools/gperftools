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
 */

#ifndef WIN32
# error You should only be including windows/patch_functions.cc in a windows environment!
#endif

#include "config.h"
#include <windows.h>
#include <tlhelp32.h>     // for CreateToolhelp32Snapshot()
#include <base/logging.h>
#include "google/malloc_hook.h"
#include "preamble_patcher.h"

// These functions are how we override the memory allocation functions,
// just like tcmalloc.cc and malloc_hook.cc do.

// These are defined in tcmalloc.cc (with a bit of macro hackiness).
// We declare them here so we can replace the windows version with ours.
extern "C" void* Perftools_malloc(size_t size) __THROW;
extern "C" void Perftools_free(void* ptr) __THROW;
extern "C" void* Perftools_realloc(void* ptr, size_t size) __THROW;
extern "C" void* Perftools_calloc(size_t nmemb, size_t size) __THROW;

typedef void* (*Type_malloc)(size_t size) __THROW;
typedef void (*Type_free)(void* ptr) __THROW;
typedef void* (*Type_realloc)(void* ptr, size_t size) __THROW;
typedef  void* (*Type_calloc)(size_t nmemb, size_t size) __THROW;

// A Windows-API equivalent of malloc and free
typedef LPVOID (WINAPI *Type_HeapAlloc)(HANDLE hHeap, DWORD dwFlags,
                                        DWORD_PTR dwBytes);
typedef BOOL (WINAPI *Type_HeapFree)(HANDLE hHeap, DWORD dwFlags,
                                     LPVOID lpMem);
// A Windows-API equivalent of mmap and munmap, for "anonymous regions"
typedef LPVOID (WINAPI *Type_VirtualAllocEx)(HANDLE process, LPVOID address,
                                             SIZE_T size, DWORD type,
                                             DWORD protect);
typedef BOOL (WINAPI *Type_VirtualFreeEx)(HANDLE process, LPVOID address,
                                          SIZE_T size, DWORD type);
// A Windows-API equivalent of mmap and munmap, for actual files
typedef LPVOID (WINAPI *Type_MapViewOfFileEx)(HANDLE hFileMappingObject,
                                              DWORD dwDesiredAccess,
                                              DWORD dwFileOffsetHigh,
                                              DWORD dwFileOffsetLow,
                                              SIZE_T dwNumberOfBytesToMap,
                                              LPVOID lpBaseAddress);
typedef BOOL (WINAPI *Type_UnmapViewOfFile)(LPCVOID lpBaseAddress);

// all libc memory-alloaction routines go through one of these.
static Type_malloc Windows_malloc;
static Type_calloc Windows_calloc;
static Type_realloc Windows_realloc;
static Type_free Windows_free;

// All Windows memory-allocation routines call through to one of these.
static Type_HeapAlloc Windows_HeapAlloc;
static Type_HeapFree Windows_HeapFree;
static Type_VirtualAllocEx Windows_VirtualAllocEx;
static Type_VirtualFreeEx Windows_VirtualFreeEx;
static Type_MapViewOfFileEx Windows_MapViewOfFileEx;
static Type_UnmapViewOfFile Windows_UnmapViewOfFile;

static LPVOID WINAPI Perftools_HeapAlloc(HANDLE hHeap, DWORD dwFlags,
                                         DWORD_PTR dwBytes) {
  LPVOID result = Windows_HeapAlloc(hHeap, dwFlags, dwBytes);
  MallocHook::InvokeNewHook(result, dwBytes);
  return result;
}

static BOOL WINAPI Perftools_HeapFree(HANDLE hHeap, DWORD dwFlags,
                                      LPVOID lpMem) {
  MallocHook::InvokeDeleteHook(lpMem);
  return Windows_HeapFree(hHeap, dwFlags, lpMem);
}

static LPVOID WINAPI Perftools_VirtualAllocEx(HANDLE process, LPVOID address,
                                              SIZE_T size, DWORD type,
                                              DWORD protect) {
  LPVOID result = Windows_VirtualAllocEx(process, address, size, type, protect);
  // VirtualAllocEx() seems to be the Windows equivalent of mmap()
  MallocHook::InvokeMmapHook(result, address, size, protect, type, -1, 0);
  return result;
}

static BOOL WINAPI Perftools_VirtualFreeEx(HANDLE process, LPVOID address,
                                           SIZE_T size, DWORD type) {
  MallocHook::InvokeMunmapHook(address, size);
  return Windows_VirtualFreeEx(process, address, size, type);
}

static LPVOID WINAPI Perftools_MapViewOfFileEx(HANDLE hFileMappingObject,
                                               DWORD dwDesiredAccess,
                                               DWORD dwFileOffsetHigh,
                                               DWORD dwFileOffsetLow,
                                               SIZE_T dwNumberOfBytesToMap,
                                               LPVOID lpBaseAddress) {
  // For this function pair, you always deallocate the full block of
  // data that you allocate, so NewHook/DeleteHook is the right API.
  LPVOID result = Windows_MapViewOfFileEx(hFileMappingObject, dwDesiredAccess,
                                          dwFileOffsetHigh, dwFileOffsetLow,
                                          dwNumberOfBytesToMap, lpBaseAddress);
  MallocHook::InvokeNewHook(result, dwNumberOfBytesToMap);
  return result;
}

static BOOL WINAPI Perftools_UnmapViewOfFile(LPCVOID lpBaseAddress) {
  MallocHook::InvokeDeleteHook(lpBaseAddress);
  return Windows_UnmapViewOfFile(lpBaseAddress);
}

// ---------------------------------------------------------------------

#define PATCH_KERNEL32(name)  do {                      \
  CHECK_EQ(sidestep::SIDESTEP_SUCCESS,                  \
           sidestep::PreamblePatcher::Patch(            \
               "kernel32", #name,                       \
               &Perftools_##name, &Windows_##name));    \
} while (0)

#define UNPATCH(name)  do {                                             \
  CHECK_EQ(sidestep::SIDESTEP_SUCCESS,                                  \
           sidestep::PreamblePatcher::Unpatch(                          \
               (Type_##name)&name, &Perftools_##name, Windows_##name)); \
} while (0)

void PatchWindowsFunctions() {
  // Luckily, Patch() doesn't call malloc or windows alloc routines
  // itself -- though it does call new (we can use PatchWithStub to
  // get around that, and will need to if we need to patch new).

  // TODO(csilvers): should we be patching GlobalAlloc/LocalAlloc instead,
  //                 for pre-XP systems?
  PATCH_KERNEL32(HeapAlloc);
  PATCH_KERNEL32(HeapFree);
  PATCH_KERNEL32(VirtualAllocEx);
  PATCH_KERNEL32(VirtualFreeEx);
  PATCH_KERNEL32(MapViewOfFileEx);
  PATCH_KERNEL32(UnmapViewOfFile);

  // Now we need to override malloc, calloc, realloc, and free.  Note
  // that other memory-allocation routines (including new/delete) are
  // overridden in tcmalloc.cc.  These are overridden here because
  // they're special for windows: they're the only libc memory
  // routines that are defined by the Microsoft C runtime library
  // (CRT) that we can't just override.  We have two different ways of
  // patching them: if malloc/etc are defined in a DLL, we just use
  // the DLL/function name, like above.  If not (we're statically
  // linked) we can get away with just passing in &malloc directly.
  // Take a snapshot of all modules in the specified process.
  HANDLE hModuleSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE |
                                                TH32CS_SNAPMODULE32,
                                                GetCurrentProcessId());
  if (hModuleSnap != INVALID_HANDLE_VALUE) {
    MODULEENTRY32 me32;
    me32.dwSize = sizeof(me32);   // needed by windows, apparently
    if (Module32First(hModuleSnap, &me32)) {
      do {
        LPCSTR lib = me32.szModule;
        if (sidestep::SIDESTEP_SUCCESS ==
            sidestep::PreamblePatcher::Patch(
                lib, "malloc", &Perftools_malloc, &Windows_malloc)
            &&
            sidestep::SIDESTEP_SUCCESS ==
            sidestep::PreamblePatcher::Patch(
                lib, "calloc", &Perftools_calloc, &Windows_calloc)
            &&
            sidestep::SIDESTEP_SUCCESS ==
            sidestep::PreamblePatcher::Patch(
                lib, "realloc", &Perftools_realloc, &Windows_realloc)
            &&
            sidestep::SIDESTEP_SUCCESS ==
            sidestep::PreamblePatcher::Patch(
                lib, "free", &Perftools_free, &Windows_free)
            ) {
          break;
        } else {
          Windows_malloc = NULL;   // reset to indicate failure
          Windows_calloc = NULL;
          Windows_realloc = NULL;
          Windows_free = NULL;
        }
      } while (Module32Next(hModuleSnap, &me32));
    }
    CloseHandle(hModuleSnap);
  }
  if (Windows_malloc == NULL && Windows_calloc == NULL &&
      Windows_realloc == NULL && Windows_free == NULL) {
    // probably means we're statically linked
    // NOTE: we need to cast the windows calls, because we're not quite
    // sure of their type (in particular, some versions have __THROW, some
    // don't).  We don't care to that level of detail, hence the cast.
    CHECK_EQ(sidestep::SIDESTEP_SUCCESS,
             sidestep::PreamblePatcher::Patch(
                 (Type_malloc)&malloc, &Perftools_malloc, &Windows_malloc));
    CHECK_EQ(sidestep::SIDESTEP_SUCCESS,
             sidestep::PreamblePatcher::Patch(
                 (Type_calloc)&calloc, &Perftools_calloc, &Windows_calloc));
    CHECK_EQ(sidestep::SIDESTEP_SUCCESS,
             sidestep::PreamblePatcher::Patch(
                 (Type_realloc)&realloc, &Perftools_realloc, &Windows_realloc));
    CHECK_EQ(sidestep::SIDESTEP_SUCCESS,
             sidestep::PreamblePatcher::Patch(
                 (Type_free)&free, &Perftools_free, &Windows_free));
  }
}

void UnpatchWindowsFunctions() {
  // We need to go back to the system malloc/etc at global destruct time,
  // so objects that were constructed before tcmalloc, using the system
  // malloc, can destroy themselves using the system free.  This depends
  // on DLLs unloading in the reverse order in which they load!
  //
  // We also go back to the default HeapAlloc/etc, just for consistency.
  // Who knows, it may help avoid weird bugs in some situations.
  UNPATCH(HeapAlloc);
  UNPATCH(HeapFree);
  UNPATCH(VirtualAllocEx);
  UNPATCH(VirtualFreeEx);
  UNPATCH(MapViewOfFileEx);
  UNPATCH(UnmapViewOfFile);

  UNPATCH(malloc);
  UNPATCH(calloc);
  UNPATCH(realloc);
  UNPATCH(free);
}
