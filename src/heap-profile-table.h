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

#ifndef BASE_HEAP_PROFILE_TABLE_H_
#define BASE_HEAP_PROFILE_TABLE_H_

#include "addressmap-inl.h"
#include "base/basictypes.h"

// Table to maintain a heap profile data inside,
// i.e. the set of currently active heap memory allocations.
// thread-unsafe and non-reentrant code:
// each instance object must be used by one thread
// at a time w/o self-recursion.
//
// TODO(maxim): add a unittest for this class.
class HeapProfileTable {
 public:

  // Extension to be used for heap pforile files.
  static const char kFileExt[];

  // Longest stack trace we record.
  static const int kMaxStackDepth = 32;

  // data types ----------------------------

  // Profile stats.
  struct Stats {
    int32 allocs;      // Number of allocation calls
    int32 frees;       // Number of free calls
    int64 alloc_size;  // Total size of all allocated objects so far
    int64 free_size;   // Total size of all freed objects so far

    // semantic equality
    bool Equivalent(const Stats& x) const {
      return allocs - frees == x.allocs - x.frees  &&
             alloc_size - free_size == x.alloc_size - x.free_size;
    }
  };

  // Info we can return about an allocation.
  struct AllocInfo {
    size_t object_size;  // size of the allocation
    const void* const* call_stack;  // call stack that made the allocation call
    int stack_depth;  // depth of call_stack
  };

  // Info we return about an allocation context.
  // An allocation context is a unique caller stack trace
  // of an allocation operation.
  struct AllocContextInfo : public Stats {
    int stack_depth;                // Depth of stack trace
    const void* const* call_stack;  // Stack trace
  };

  // Memory (de)allocator interface we'll use.
  typedef void* (*Allocator)(size_t size);
  typedef void  (*DeAllocator)(void* ptr);

  // interface ---------------------------

  HeapProfileTable(Allocator alloc, DeAllocator dealloc);
  ~HeapProfileTable();

  // Record an allocation at 'ptr' of 'bytes' bytes.
  // skip_count gives the number of stack frames between this call
  // and the memory allocation function that was asked to do the allocation.
  void RecordAlloc(const void* ptr, size_t bytes, int skip_count);

  // Direct version of RecordAlloc when the caller stack to use
  // is already known: call_stack of depth stack_depth.
  void RecordAllocWithStack(const void* ptr, size_t bytes,
                            int stack_depth, const void* const call_stack[]);

  // Record the deallocation of memory at 'ptr'.
  void RecordFree(const void* ptr);

  // Return true iff we have recorded an allocation at 'ptr'.
  // If yes, fill *object_size with the allocation byte size.
  bool FindAlloc(const void* ptr, size_t* object_size) const;
  // Same as FindAlloc, but fills all of *info.
  bool FindAllocDetails(const void* ptr, AllocInfo* info) const;

  // Return true iff "ptr" points into a recorded allocation
  // If yes, fill *object_ptr with the actual allocation address
  // and *object_size with the allocation byte size.
  // max_size specifies largest currently possible allocation size.
  bool FindInsideAlloc(const void* ptr, size_t max_size,
                       const void** object_ptr, size_t* object_size) const;

  // If "ptr" points to a recorded allocation and it's not marked as live
  // mark it as live and return true. Else return false.
  // All allocations start as non-live, DumpNonLiveProfile below
  // also makes all allocations non-live.
  bool MarkAsLive(const void* ptr);

  // Return current total (de)allocation statistics.
  const Stats& total() const { return total_; }

  // Allocation data iteration callback: gets passed object pointer and
  // fully-filled AllocInfo.
  typedef void (*AllocIterator)(const void* ptr, const AllocInfo& info);

  // Iterate over the allocation profile data calling "callback"
  // for every allocation.
  void IterateAllocs(AllocIterator callback) const {
    allocation_->Iterate(MapArgsAllocIterator, callback);
  }

  // Allocation context profile data iteration callback
  typedef void (*AllocContextIterator)(const AllocContextInfo& info);

  // Iterate over the allocation context profile data calling "callback"
  // for every allocation context. Allocation contexts are ordered by the
  // size of allocated space.
  void IterateOrderedAllocContexts(AllocContextIterator callback) const;

  // Fill profile data into buffer 'buf' of size 'size'
  // and return the actual size occupied by the dump in 'buf'.
  // The profile buckets are dumped in the decreasing order
  // of currently allocated bytes.
  // We do not provision for 0-terminating 'buf'.
  int FillOrderedProfile(char buf[], int size) const;

  // Allocation data dump filtering callback:
  // gets passed object pointer and size
  // needs to return true iff the object is to be filtered out of the dump.
  typedef bool (*DumpFilter)(const void* ptr, size_t size);

  // Dump non-live portion of the current heap profile
  // for leak checking purposes to file_name.
  // Also reset all objects to non-live state
  // and write the sums of allocated byte and object counts in the dump
  // to *alloc_bytes and *alloc_objects.
  // dump_alloc_addresses controls if object addresses are dumped.
  bool DumpNonLiveProfile(const char* file_name,
                          bool dump_alloc_addresses,
                          Stats* profile_stats) const;

  // Cleanup any old profile files matching prefix + ".*" + kFileExt.
  static void CleanupOldProfiles(const char* prefix);

 private:

  // data types ----------------------------

  // Hash table bucket to hold (de)allocation stats
  // for a given allocation call stack trace.
  struct Bucket : public Stats {
    uintptr_t    hash;   // Hash value of the stack trace
    int          depth;  // Depth of stack trace
    const void** stack;  // Stack trace
    Bucket*      next;   // Next entry in hash-table
  };

  // Info stored in the address map
  struct AllocValue {
    // Access to the stack-trace bucket
    Bucket* bucket() const {
      return reinterpret_cast<Bucket*>(bucket_rep & ~uintptr_t(1));
    }
    // This also does set_live(false).
    void set_bucket(Bucket* b) { bucket_rep = reinterpret_cast<uintptr_t>(b); }
    size_t  bytes;   // Number of bytes in this allocation
    // Access to the allocation liveness flag (for leak checking)
    bool live() const { return bucket_rep & 1; }
    void set_live(bool l) { bucket_rep = (bucket_rep & ~uintptr_t(1)) | l; }
   private:
    uintptr_t bucket_rep;  // Bucket* with the lower bit being the "live" flag:
                           // pointers point to at least 4-byte aligned storage.
  };

  // helper for FindInsideAlloc
  static size_t AllocValueSize(const AllocValue& v) { return v.bytes; }

  typedef AddressMap<AllocValue> AllocationMap;

  // Arguments that need to be passed DumpNonLiveIterator callback below.
  struct DumpArgs {
    int fd;  // file to write to
    bool dump_alloc_addresses;  // if we are dumping allocation's addresses
    Stats* profile_stats;  // stats to update

    DumpArgs(int a, bool b, Stats* d)
      : fd(a), dump_alloc_addresses(b), profile_stats(d) { }
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

  // Get the bucket for the caller stack trace 'key' of depth 'depth'
  // creating the bucket if needed.
  Bucket* GetBucket(int depth, const void* const key[]);

  // Helper for IterateAllocs to do callback signature conversion
  // from AllocationMap::Iterate to AllocIterator.
  static void MapArgsAllocIterator(const void* ptr, AllocValue* v,
                                   AllocIterator callback) {
    AllocInfo info;
    info.object_size = v->bytes;
    info.call_stack = v->bucket()->stack;
    info.stack_depth = v->bucket()->depth;
    callback(ptr, info);
  }

  // Helper for DumpNonLiveProfile to do object-granularity
  // heap profile dumping. It gets passed to AllocationMap::Iterate.
  inline static void DumpNonLiveIterator(const void* ptr, AllocValue* v,
                                         const DumpArgs& args);

  // Helper for IterateOrderedAllocContexts and FillOrderedProfile.
  // Creates a sorted list of Buckets whose length is num_buckets_.
  // The caller is responsible for dellocating the returned list.
  Bucket** MakeSortedBucketList() const;

  // data ----------------------------

  // Size for table_.
  static const int kHashTableSize = 179999;

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

#endif  // BASE_HEAP_PROFILE_TABLE_H_
