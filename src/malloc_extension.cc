// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
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

#include <config.h>

#include "gperftools/malloc_extension.h"
#include "gperftools/malloc_extension_c.h"

#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <string>

#include <algorithm>
#include <atomic>

#include "base/dynamic_annotations.h"
#include "base/googleinit.h"
#include "base/proc_maps_iterator.h"
#include "tcmalloc_internal.h"

#include "gperftools/tcmalloc.h"

static void DumpAddressMap(std::string* result) {
  tcmalloc::StringGenericWriter writer(result);
  writer.AppendStr("\nMAPPED_LIBRARIES:\n");
  tcmalloc::SaveProcSelfMaps(&writer);
}

void MallocExtension::Initialize() {}

// SysAllocator implementation
SysAllocator::~SysAllocator() {}

// Default implementation -- does nothing
MallocExtension::~MallocExtension() { }
bool MallocExtension::VerifyAllMemory() { return true; }
bool MallocExtension::VerifyNewMemory(const void* p) { return true; }
bool MallocExtension::VerifyArrayNewMemory(const void* p) { return true; }
bool MallocExtension::VerifyMallocMemory(const void* p) { return true; }

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
  memset(histogram, 0, sizeof(*histogram) * kMallocHistogramSize);
  return true;
}

void** MallocExtension::ReadStackTraces(int* sample_period) {
  return nullptr;
}

void** MallocExtension::ReadHeapGrowthStackTraces() {
  return nullptr;
}

void MallocExtension::MarkThreadIdle() {
  // Default implementation does nothing
}

void MallocExtension::MarkThreadBusy() {
  // Default implementation does nothing
}

SysAllocator* MallocExtension::GetSystemAllocator() {
  return nullptr;
}

void MallocExtension::SetSystemAllocator(SysAllocator *a) {
  // Default implementation does nothing
}

void MallocExtension::ReleaseToSystem(size_t num_bytes) {
  // Default implementation does nothing
}

void MallocExtension::ReleaseFreeMemory() {
  ReleaseToSystem(static_cast<size_t>(-1));   // SIZE_T_MAX
}

void MallocExtension::SetMemoryReleaseRate(double rate) {
  // Default implementation does nothing
}

double MallocExtension::GetMemoryReleaseRate() {
  return -1.0;
}

size_t MallocExtension::GetEstimatedAllocatedSize(size_t size) {
  return size;
}

size_t MallocExtension::GetAllocatedSize(const void* p) {
  assert(GetOwnership(p) != kNotOwned);
  return 0;
}

MallocExtension::Ownership MallocExtension::GetOwnership(const void* p) {
  return kUnknownOwnership;
}

void MallocExtension::GetFreeListSizes(
  std::vector<MallocExtension::FreeListInfo>* v) {
  v->clear();
}

size_t MallocExtension::GetThreadCacheSize() {
  return 0;
}

void MallocExtension::MarkThreadTemporarilyIdle() {
  // Default implementation does nothing
}

// The current malloc extension object.

static std::atomic<MallocExtension*> current_instance;

MallocExtension* MallocExtension::instance() {
  MallocExtension* inst = current_instance.load(std::memory_order_relaxed);
  if (PREDICT_TRUE(inst != nullptr)) {
    return inst;
  }

  // if MallocExtension isn't set up yet, it could be we're called
  // super-early. Trigger tcmalloc initialization and assume it will
  // set up instance().
  tc_free(tc_malloc(32));
  return instance();
}

void MallocExtension::Register(MallocExtension* implementation) {
  current_instance.store(implementation);
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

void PrintCountAndSize(MallocExtensionWriter* writer,
                       uintptr_t count, uintptr_t size) {
  char buf[100];
  snprintf(buf, sizeof(buf),
           "%6" PRIu64 ": %8" PRIu64 " [%6" PRIu64 ": %8" PRIu64 "] @",
           static_cast<uint64_t>(count),
           static_cast<uint64_t>(size),
           static_cast<uint64_t>(count),
           static_cast<uint64_t>(size));
  writer->append(buf, strlen(buf));
}

void PrintHeader(MallocExtensionWriter* writer,
                 const char* label, void** entries) {
  // Compute the total count and total size
  uintptr_t total_count = 0;
  uintptr_t total_size = 0;
  for (void** entry = entries; Count(entry) != 0; entry += 3 + Depth(entry)) {
    total_count += Count(entry);
    total_size += Size(entry);
  }

  const char* const kTitle = "heap profile: ";
  writer->append(kTitle, strlen(kTitle));
  PrintCountAndSize(writer, total_count, total_size);
  writer->append(" ", 1);
  writer->append(label, strlen(label));
  writer->append("\n", 1);
}

void PrintStackEntry(MallocExtensionWriter* writer, void** entry) {
  PrintCountAndSize(writer, Count(entry), Size(entry));

  for (int i = 0; i < Depth(entry); i++) {
    char buf[32];
    snprintf(buf, sizeof(buf), " %p", PC(entry, i));
    writer->append(buf, strlen(buf));
  }
  writer->append("\n", 1);
}

}

void MallocExtension::GetHeapSample(MallocExtensionWriter* writer) {
  int sample_period = 0;
  void** entries = ReadStackTraces(&sample_period);
  if (entries == nullptr) {
    const char* const kErrorMsg =
        "This malloc implementation does not support sampling.\n"
        "As of 2005/01/26, only tcmalloc supports sampling, and\n"
        "you are probably running a binary that does not use\n"
        "tcmalloc.\n";
    writer->append(kErrorMsg, strlen(kErrorMsg));
    return;
  }

  char label[32];
  snprintf(label, sizeof(label), "heap_v2/%d", sample_period);
  PrintHeader(writer, label, entries);
  for (void** entry = entries; Count(entry) != 0; entry += 3 + Depth(entry)) {
    PrintStackEntry(writer, entry);
  }
  delete[] entries;

  DumpAddressMap(writer);
}

void MallocExtension::GetHeapGrowthStacks(MallocExtensionWriter* writer) {
  void** entries = ReadHeapGrowthStackTraces();
  if (entries == nullptr) {
    const char* const kErrorMsg =
        "This malloc implementation does not support "
        "ReadHeapGrowthStackTraces().\n"
        "As of 2005/09/27, only tcmalloc supports this, and you\n"
        "are probably running a binary that does not use tcmalloc.\n";
    writer->append(kErrorMsg, strlen(kErrorMsg));
    return;
  }

  // Do not canonicalize the stack entries, so that we get a
  // time-ordered list of stack traces, which may be useful if the
  // client wants to focus on the latest stack traces.
  PrintHeader(writer, "growth", entries);
  for (void** entry = entries; Count(entry) != 0; entry += 3 + Depth(entry)) {
    PrintStackEntry(writer, entry);
  }
  delete[] entries;

  DumpAddressMap(writer);
}

void MallocExtension::Ranges(void* arg, RangeFunction func) {
  // No callbacks by default
}

// These are C shims that work on the current instance.

#define C_SHIM(fn, retval, paramlist, arglist)          \
  extern "C" PERFTOOLS_DLL_DECL retval MallocExtension_##fn paramlist {    \
    return MallocExtension::instance()->fn arglist;     \
  }

C_SHIM(VerifyAllMemory, int, (void), ());
C_SHIM(VerifyNewMemory, int, (const void* p), (p));
C_SHIM(VerifyArrayNewMemory, int, (const void* p), (p));
C_SHIM(VerifyMallocMemory, int, (const void* p), (p));
C_SHIM(MallocMemoryStats, int,
       (int* blocks, size_t* total, int histogram[kMallocHistogramSize]),
       (blocks, total, histogram));

C_SHIM(GetStats, void,
       (char* buffer, int buffer_length), (buffer, buffer_length));
C_SHIM(GetNumericProperty, int,
       (const char* property, size_t* value), (property, value));
C_SHIM(SetNumericProperty, int,
       (const char* property, size_t value), (property, value));

C_SHIM(MarkThreadIdle, void, (void), ());
C_SHIM(MarkThreadBusy, void, (void), ());
C_SHIM(ReleaseFreeMemory, void, (void), ());
C_SHIM(ReleaseToSystem, void, (size_t num_bytes), (num_bytes));
C_SHIM(SetMemoryReleaseRate, void, (double rate), (rate));
C_SHIM(GetMemoryReleaseRate, double, (void), ());
C_SHIM(GetEstimatedAllocatedSize, size_t, (size_t size), (size));
C_SHIM(GetAllocatedSize, size_t, (const void* p), (p));
C_SHIM(GetThreadCacheSize, size_t, (void), ());
C_SHIM(MarkThreadTemporarilyIdle, void, (void), ());

// Can't use the shim here because of the need to translate the enums.
extern "C"
MallocExtension_Ownership MallocExtension_GetOwnership(const void* p) {
  return static_cast<MallocExtension_Ownership>(
      MallocExtension::instance()->GetOwnership(p));
}
