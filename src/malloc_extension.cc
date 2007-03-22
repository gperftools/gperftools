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
// Author: Sanjay Ghemawat <opensource@google.com>

#include "config.h"
#include <assert.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#if defined HAVE_STDINT_H
#include <stdint.h>
#elif defined HAVE_INTTYPES_H
#include <inttypes.h>
#else
#include <sys/types.h>
#endif
#include <string>
#include "google/perftools/hash_set.h"
#include "google/malloc_extension.h"
#include "maybe_threads.h"

using STL_NAMESPACE::string;

// Note: this routine is meant to be called before threads are spawned.
void MallocExtension::Initialize() {
  static bool initialize_called = false;

  if (initialize_called) return;
  initialize_called = true;

  // GNU libc++ versions 3.3 and 3.4 obey the environment variables
  // GLIBCPP_FORCE_NEW and GLIBCXX_FORCE_NEW respectively.  Setting
  // one of these variables forces the STL default allocator to call
  // new() or delete() for each allocation or deletion.  Otherwise
  // the STL allocator tries to avoid the high cost of doing
  // allocations by pooling memory internally.  However, tcmalloc
  // does allocations really fast, especially for the types of small
  // items one sees in STL, so it's better off just using us.
  // TODO: control whether we do this via an environment variable?
  setenv("GLIBCPP_FORCE_NEW", "1", false /* no overwrite*/);
  setenv("GLIBCXX_FORCE_NEW", "1", false /* no overwrite*/);

  // Now we need to make the setenv 'stick', which it may not do since
  // the env is flakey before main() is called.  But luckily stl only
  // looks at this env var the first time it tries to do an alloc, and
  // caches what it finds.  So we just cause an stl alloc here.
  string dummy("I need to be allocated");
  dummy += "!";         // so the definition of dummy isn't optimized out
}

// Default implementation -- does nothing
MallocExtension::~MallocExtension() { }
bool MallocExtension::VerifyAllMemory() { return true; }
bool MallocExtension::VerifyNewMemory(void* p) { return true; }
bool MallocExtension::VerifyArrayNewMemory(void* p) { return true; }
bool MallocExtension::VerifyMallocMemory(void* p) { return true; }

bool MallocExtension::GetNumericProperty(const char* property, size_t* value) {
  return false;
}

bool MallocExtension::SetNumericProperty(const char* property, size_t value) {
  return false;
}

void MallocExtension::GetStats(char* buffer, int length) {
  assert(length > 0);
  buffer[0] = '\0';
}

bool MallocExtension::MallocMemoryStats(int* blocks, size_t* total,
                                       int histogram[kMallocHistogramSize]) {
  *blocks = 0;
  *total = 0;
  memset(histogram, sizeof(histogram), 0);
  return true;
}

void** MallocExtension::ReadStackTraces() {
  return NULL;
}

void** MallocExtension::ReadHeapGrowthStackTraces() {
  return NULL;
}

// The current malloc extension object.  We also keep a pointer to
// the default implementation so that the heap-leak checker does not
// complain about a memory leak.

static pthread_once_t module_init = PTHREAD_ONCE_INIT;
static MallocExtension* default_instance = NULL;
static MallocExtension* current_instance = NULL;

static void InitModule() {
  default_instance = new MallocExtension;
  current_instance = default_instance;
}

MallocExtension* MallocExtension::instance() {
  perftools_pthread_once(&module_init, InitModule);
  return current_instance;
}

void MallocExtension::Register(MallocExtension* implementation) {
  perftools_pthread_once(&module_init, InitModule);
  current_instance = implementation;
}

// -----------------------------------------------------------------------
// Heap sampling support
// -----------------------------------------------------------------------

namespace {

// Accessors
uintptr_t Count(void** entry) {
  return reinterpret_cast<uintptr_t>(entry[0]);
}
uintptr_t Size(void** entry) {
  return reinterpret_cast<uintptr_t>(entry[1]);
}
uintptr_t Depth(void** entry) {
  return reinterpret_cast<uintptr_t>(entry[2]);
}
void* PC(void** entry, int i) {
  return entry[3+i];
}

// Hash table routines for grouping all entries with same stack trace
struct StackTraceHash {
  size_t operator()(void** entry) const {
    uintptr_t h = 0;
    for (unsigned int i = 0; i < Depth(entry); i++) {
      uintptr_t pc = reinterpret_cast<uintptr_t>(PC(entry, i));
      h = (h << 8) | (h >> (8*(sizeof(h)-1)));
      h += (pc * 31) + (pc * 7) + (pc * 3);
    }
    return h;
  }
  // Less operator for MSVC's hash containers.
  bool operator()(void** entry1, void** entry2) const {
    if (Depth(entry1) != Depth(entry2))
      return Depth(entry1) < Depth(entry2);
    for (int i = 0; i < Depth(entry1); i++) {
      if (PC(entry1, i) != PC(entry2, i)) {
        return PC(entry1, i) < PC(entry2, i);
      }
    }
    return false;  // entries are equal
  }
  // These two public members are required by msvc.  4 and 8 are the
  // default values.
  static const size_t bucket_size = 4;
  static const size_t min_buckets = 8;
};

struct StackTraceEqual {
  bool operator()(void** entry1, void** entry2) const {
    if (Depth(entry1) != Depth(entry2)) return false;
    for (int i = 0; i < Depth(entry1); i++) {
      if (PC(entry1, i) != PC(entry2, i)) {
        return false;
      }
    }
    return true;
  }
};

typedef HASH_NAMESPACE::hash_set<void**, StackTraceHash, StackTraceEqual> StackTraceTable;

void PrintHeader(string* result, const char* label, void** entries) {
  // Compute the total count and total size
  uintptr_t total_count = 0;
  uintptr_t total_size = 0;
  for (void** entry = entries; Count(entry) != 0; entry += 3 + Depth(entry)) {
    total_count += Count(entry);
    total_size += Size(entry);
  }

  char buf[200];
  snprintf(buf, sizeof(buf),
           "heap profile: %6lld: %8lld [%6lld: %8lld] @ %s\n",
           static_cast<long long>(total_count),
           static_cast<long long>(total_size),
           static_cast<long long>(total_count),
           static_cast<long long>(total_size),
           label);
  *result += buf;
}

void PrintStackEntry(string* result, void** entry) {
  char buf[100];
  snprintf(buf, sizeof(buf), "%6d: %8d [%6d: %8d] @",
           int(Count(entry)), int(Size(entry)),
           int(Count(entry)), int(Size(entry)));
  *result += buf;
  for (int i = 0; i < Depth(entry); i++) {
    snprintf(buf, sizeof(buf), " %p", PC(entry, i));
    *result += buf;
  }
  *result += "\n";
}

}

void MallocExtension::GetHeapSample(string* result) {
  void** entries = ReadStackTraces();
  if (entries == NULL) {
    *result += "This malloc implementation does not support sampling.\n"
               "As of 2005/01/26, only tcmalloc supports sampling, and you\n"
               "are probably running a binary that does not use tcmalloc.\n";
    return;
  }

  // Group together all entries with same stack trace
  StackTraceTable table;
  for (void** entry = entries; Count(entry) != 0; entry += 3 + Depth(entry)) {
    StackTraceTable::iterator iter = table.find(entry);
    if (iter == table.end()) {
      // New occurrence
      table.insert(entry);
    } else {
      void** canonical = *iter;
      canonical[0] = reinterpret_cast<void*>(Count(canonical) + Count(entry));
      canonical[1] = reinterpret_cast<void*>(Size(canonical) +  Size(entry));
    }
  }

  PrintHeader(result, "heap", entries);
  for (StackTraceTable::iterator iter = table.begin();
       iter != table.end();
       ++iter) {
    PrintStackEntry(result, *iter);
  }

  // TODO(menage) Get this working in google-perftools
  //DumpAddressMap(DebugStringWriter, result);
  delete[] entries;
}

void MallocExtension::GetHeapGrowthStacks(std::string* result) {
  void** entries = ReadHeapGrowthStackTraces();
  if (entries == NULL) {
    *result += "This malloc implementation does not support "
               "ReadHeapGrowhStackTraces().\n"
               "As of 2005/09/27, only tcmalloc supports this, and you\n"
               "are probably running a binary that does not use tcmalloc.\n";
    return;
  }

  // Do not canonicalize the stack entries, so that we get a
  // time-ordered list of stack traces, which may be useful if the
  // client wants to focus on the latest stack traces.

  PrintHeader(result, "growth", entries);
  for (void** entry = entries; Count(entry) != 0; entry += 3 + Depth(entry)) {
    PrintStackEntry(result, entry);
  }
  delete[] entries;

  // TODO(menage) Get this working in google-perftools
  //DumpAddressMap(DebugStringWriter, result);
}
