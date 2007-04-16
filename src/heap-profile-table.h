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

// ---
// Author: Sanjay Ghemawat
//         Maxim Lifantsev (refactoring)
//

#ifndef BASE_HEAP_PROFILE_TABLE_H__
#define BASE_HEAP_PROFILE_TABLE_H__

#include "addressmap-inl.h"
#include "base/basictypes.h"

// Table to maintain a heap profile data inside,
// i.e. the set of currently active heap memory allocations.
// Not thread-safe and non-reentrant code:
// each instance object must be used by one thread
// at a time w/o self-recursion.
//
// TODO(maxim): add a unittest for this class.
class HeapProfileTable {
 public:

  // Extension to be used for heap pforile files.
  static const char kFileExt[];

  // data types ----------------------------

  // Profile stats.
  struct Stats {
    int32 allocs;      // Number of allocation calls
    int32 frees;       // Number of free calls
    int64 alloc_size;  // Total size of all allocated objects so far
    int64 free_size;   // Total size of all freed objects so far
  };

  // Info we can return about an allocation.
  struct AllocInfo {
    size_t object_size;  // size of the allocation
    void* const* call_stack;  // call stack that made the allocation call
    int stack_depth;  // depth of call_stack
  };

  // Memory (de)allocator interface we'll use.
  typedef void* (*Allocator)(size_t);
  typedef void  (*DeAllocator)(void*);

  // interface ---------------------------

  HeapProfileTable(Allocator alloc, DeAllocator dealloc);
  ~HeapProfileTable();

  // Record an allocation at 'ptr' of 'bytes' bytes.
  // skip_count gives the number of stack frames between this call
  // and the memory allocation function that was asked to do the allocation.
  void RecordAlloc(void* ptr, size_t bytes, int skip_count);

  // Record the deallocation of memory at 'ptr'.
  void RecordFree(void* ptr);

  // Return true iff we have recorded an allocation at 'ptr'.
  // If yes, fill *object_size with the allocation byte size.
  bool FindAlloc(void* ptr, size_t* object_size) const;
  // Same as FindAlloc, but fills all of *info.
  bool FindAllocDetails(void* ptr, AllocInfo* info) const;

  // Return current total (de)allocation statistics.
  const Stats& total() const { return total_; }

  // Allocation data iteration callback: gets passed object pointer and
  // fully-filled AllocInfo.
  typedef void (*AllocIterator)(void* ptr, const AllocInfo& info);

  // Iterate over the allocation profile data calling "callback"
  // for every allocation.
  void IterateAllocs(AllocIterator callback) const;

  // Fill profile data into buffer 'buf' of size 'size'
  // and return the actual size occupied by the dump in 'buf'.
  // The profile buckets are dumped in the decreasing order
  // of currently allocated bytes.
  // We do not provision for 0-terminating 'buf'.
  int FillOrderedProfile(char buf[], int size) const;
  
  // Allocation data dump filtering callback:
  // gets passed object pointer and size
  // needs to return true iff the object is to be filtered out of the dump.
  typedef bool (*DumpFilter)(void* ptr, size_t size);

  // Dump current heap profile for leak checking purposes to file_name
  // while filtering the objects by "filter".
  // Also write the sums of allocated byte and object counts in the dump
  // to *alloc_bytes and *alloc_objects.
  // dump_alloc_addresses controls if object addresses are dumped.
  bool DumpFilteredProfile(const char* file_name,
                           DumpFilter filter,
                           bool dump_alloc_addresses,
                           Stats* profile_stats) const;

  // Cleanup any old profile files matching prefix + ".*" + kFileExt.
  static void CleanupOldProfiles(const char* prefix);

 private:

  // data types ----------------------------

  // Hash table bucket to hold (de)allocation stats
  // for a given allocation call stack trace.
  struct Bucket : public Stats {
    uintptr_t hash;   // Hash value of the stack trace
    int       depth;  // Depth of stack trace
    void**    stack;  // Stack trace
    Bucket*   next;   // Next entry in hash-table
  };

  // Info stored in the address map
  struct AllocValue {
    Bucket* bucket;  // The stack-trace bucket
    size_t  bytes;   // Number of bytes in this allocation
  };

  typedef AddressMap<AllocValue> AllocationMap;

  // Arguments that need to be passed FilteredDumpIterator callback below.
  struct DumpArgs {
    int fd;  // file to write to
    bool dump_alloc_addresses;  // if we are dumping allocation's addresses
    DumpFilter filter;  // dumping filter
    Stats* profile_stats;  // stats to update

    DumpArgs(int a, bool b, DumpFilter c, Stats* d)
      : fd(a), dump_alloc_addresses(b), filter(c), profile_stats(d) { }
  };

  // helpers ----------------------------

  // Unparse bucket b and print its portion of profile dump into buf.
  // We return the amount of space in buf that we use.  We start printing
  // at buf + buflen, and promise not to go beyond buf + bufsize.
  // We do not provision for 0-terminating 'buf'.
  // We update *profile_stats by counting bucket b.
  static int UnparseBucket(const Bucket& b,
                           char* buf, int buflen, int bufsize,
                           Stats* profile_stats);

  // Get the bucket for the current stack trace creating one if needed
  // (skip "skip_count" most recent frames).
  Bucket* GetBucket(int skip_count);

  // Helper for IterateAllocs to do callback signature conversion
  // from AllocationMap::Iterate to AllocIterator.
  static void MapArgsAllocIterator(void* ptr, AllocValue v,
                                   AllocIterator callback);

  // Helper for DumpFilteredProfile to do object-granularity
  // heap profile dumping. It gets passed to AllocationMap::Iterate.
  static void FilteredDumpIterator(void* ptr, AllocValue v,
                                   const DumpArgs& args);

  // data ----------------------------

  // Size for table_.
  static const int kHashTableSize = 179999;

  // Longest stack trace we record.
  static const int kMaxStackTrace = 32;

  // Memory (de)allocator that we use.
  Allocator alloc_;
  DeAllocator dealloc_;

  // Overall profile stats; we use only the Stats part,
  // but make it a Bucket to pass to UnparseBucket.
  Bucket total_;

  // Bucket hash table.
  // We hand-craft one instead of using one of the pre-written
  // ones because we do not want to use malloc when operating on the table.
  // It is only few lines of code, so no big deal.
  Bucket** table_;
  int num_buckets_;

  // Map of all currently allocated objects we know about.
  AllocationMap* allocation_;

  DISALLOW_EVIL_CONSTRUCTORS(HeapProfileTable);
};

#endif  // BASE_HEAP_PROFILE_TABLE_H__
