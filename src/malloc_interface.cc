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

#include "google/perftools/config.h"
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
#include "google/malloc_interface.h"

using STL_NAMESPACE::string;

// Default implementation -- does nothing
MallocInterface::~MallocInterface() { }
bool MallocInterface::VerifyAllMemory() { return true; }
bool MallocInterface::VerifyNewMemory(void* p) { return true; }
bool MallocInterface::VerifyArrayNewMemory(void* p) { return true; }
bool MallocInterface::VerifyMallocMemory(void* p) { return true; }

bool MallocInterface::GetNumericProperty(const char* property, size_t* value) {
  return false;
}

bool MallocInterface::SetNumericProperty(const char* property, size_t value) {
  return false;
}

void MallocInterface::GetStats(char* buffer, int length) {
  assert(length > 0);
  buffer[0] = '\0';
}

bool MallocInterface::MallocMemoryStats(int* blocks, size_t* total,
                                       int histogram[kMallocHistogramSize]) {
  *blocks = 0;
  *total = 0;
  memset(histogram, sizeof(histogram), 0);
  return true;
}

void** MallocInterface::ReadStackTraces() {
  return NULL;
}

// The current malloc interface object.  We also keep a pointer to
// the default implementation so that the heap-leak checker does not
// complain about a memory leak.

static pthread_once_t module_init = PTHREAD_ONCE_INIT;
static MallocInterface* default_instance = NULL;
static MallocInterface* current_instance = NULL;

static void InitModule() {
  default_instance = new MallocInterface;
  current_instance = default_instance;
}

MallocInterface* MallocInterface::instance() {
  pthread_once(&module_init, InitModule);
  return current_instance;
}

void MallocInterface::Register(MallocInterface* implementation) {
  pthread_once(&module_init, InitModule);
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
    for (int i = 0; i < Depth(entry); i++) {
      uintptr_t pc = reinterpret_cast<uintptr_t>(PC(entry, i));
      h = (h << 8) | (h >> (8*(sizeof(h)-1)));
      h += (pc * 31) + (pc * 7) + (pc * 3);
    }
    return h;
  }
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

void DebugStringWriter(const char* str, void* arg) {
  string* result = reinterpret_cast<string*>(arg);
  *result += str;
}

}

void MallocInterface::GetHeapSample(string* result) {
  void** entries = ReadStackTraces();
  if (entries == NULL) {
    *result += "this malloc implementation does not support sampling\n";
    return;
  }

  // Group together all entries with same stack trace
  StackTraceTable table;
  int total_count = 0;
  int total_size = 0;
  for (void** entry = entries; Count(entry) != 0; entry += 3 + Depth(entry)) {
    StackTraceTable::iterator iter = table.find(entry);
    total_count += Count(entry);
    total_size += Size(entry);
    if (iter == table.end()) {
      // New occurrence
      table.insert(entry);
    } else {
      void** canonical = *iter;
      canonical[0] = reinterpret_cast<void*>(Count(canonical) + Count(entry));
      canonical[1] = reinterpret_cast<void*>(Size(canonical) +  Size(entry));
    }
  }

  char buf[100];
  snprintf(buf, sizeof(buf), "heap profile: %6d: %8d [%6d: %8d] @\n",
           total_count, total_size, total_count, total_size);
  *result += buf;
  for (StackTraceTable::iterator iter = table.begin();
       iter != table.end();
       ++iter) {
    void** entry = *iter;
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

  // TODO(menage) Get this working in google-perftools
  //DumpAddressMap(DebugStringWriter, result);

  delete[] entries;
}
