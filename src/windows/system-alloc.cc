// Copyright (c) 2013, Google Inc.
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
// Author: Petr Hosek

#ifndef _WIN32
# error You should only be including windows/system-alloc.cc in a windows environment!
#endif

#include <config.h>
#include <windows.h>
#include <gperftools/malloc_extension.h>
#include "base/logging.h"
#include "base/spinlock.h"
#include "internal_logging.h"
#include "system-alloc.h"

static SpinLock spinlock(SpinLock::LINKER_INITIALIZED);

// The current system allocator declaration
SysAllocator* sys_alloc = NULL;
// Number of bytes taken from system.
size_t TCMalloc_SystemTaken = 0;

class VirtualSysAllocator : public SysAllocator {
public:
  VirtualSysAllocator() : SysAllocator() {
  }
  void* Alloc(size_t size, size_t *actual_size, size_t alignment);
};
static char virtual_space[sizeof(VirtualSysAllocator)];

// This is mostly like MmapSysAllocator::Alloc, except it does these weird
// munmap's in the middle of the page, which is forbidden in windows.
void* VirtualSysAllocator::Alloc(size_t size, size_t *actual_size,
                                 size_t alignment) {
  // Align on the pagesize boundary
  const int pagesize = getpagesize();
  if (alignment < pagesize) alignment = pagesize;
  size = ((size + alignment - 1) / alignment) * alignment;

  // Safest is to make actual_size same as input-size.
  if (actual_size) {
    *actual_size = size;
  }

  // Ask for extra memory if alignment > pagesize
  size_t extra = 0;
  if (alignment > pagesize) {
    extra = alignment - pagesize;
  }

  void* result = VirtualAlloc(0, size + extra,
                              MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
  if (result == NULL)
    return NULL;

  // Adjust the return memory so it is aligned
  uintptr_t ptr = reinterpret_cast<uintptr_t>(result);
  size_t adjust = 0;
  if ((ptr & (alignment - 1)) != 0) {
    adjust = alignment - (ptr & (alignment - 1));
  }

  ptr += adjust;
  return reinterpret_cast<void*>(ptr);
}

#ifdef _MSC_VER

extern "C" SysAllocator* tc_get_sysalloc_override(SysAllocator *def);
extern "C" SysAllocator* tc_get_sysalloc_default(SysAllocator *def)
{
  return def;
}

#if defined(_M_IX86)
#pragma comment(linker, "/alternatename:_tc_get_sysalloc_override=_tc_get_sysalloc_default")
#elif defined(_M_X64)
#pragma comment(linker, "/alternatename:tc_get_sysalloc_override=tc_get_sysalloc_default")
#endif

#else // !_MSC_VER

extern "C" ATTRIBUTE_NOINLINE
SysAllocator* tc_get_sysalloc_override(SysAllocator *def)
{
  return def;
}

#endif

static bool system_alloc_inited = false;
void InitSystemAllocators(void) {
  VirtualSysAllocator *alloc = new (virtual_space) VirtualSysAllocator();
  sys_alloc = tc_get_sysalloc_override(alloc);
}

extern PERFTOOLS_DLL_DECL
void* TCMalloc_SystemAlloc(size_t size, size_t *actual_size,
			   size_t alignment) {
  SpinLockHolder lock_holder(&spinlock);

  if (!system_alloc_inited) {
    InitSystemAllocators();
    system_alloc_inited = true;
  }

  void* result = sys_alloc->Alloc(size, actual_size, alignment);
  if (result != NULL) {
    if (actual_size) {
      TCMalloc_SystemTaken += *actual_size;
    } else {
      TCMalloc_SystemTaken += size;
    }
  }
  return result;
}

extern PERFTOOLS_DLL_DECL
bool TCMalloc_SystemRelease(void* start, size_t length) {
  // TODO(csilvers): should I be calling VirtualFree here?
  return false;
}

bool RegisterSystemAllocator(SysAllocator *allocator, int priority) {
  return false;   // we don't allow registration on windows, right now
}

void DumpSystemAllocatorStats(TCMalloc_Printer* printer) {
  // We don't dump stats on windows, right now
}
