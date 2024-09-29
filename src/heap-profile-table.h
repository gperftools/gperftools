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

// ---
// Author: Sanjay Ghemawat
//         Maxim Lifantsev (refactoring)
//

#ifndef BASE_HEAP_PROFILE_TABLE_H_
#define BASE_HEAP_PROFILE_TABLE_H_

#include "addressmap-inl.h"
#include "base/basictypes.h"
#include "base/generic_writer.h"
#include "base/logging.h"   // for RawFD
#include "heap-profile-stats.h"

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

  // data types ----------------------------

  // Profile stats.
  typedef HeapProfileStats Stats;

  // Info we can return about an allocation.
  struct AllocInfo {
    size_t object_size;  // size of the allocation
    const void* const* call_stack;  // call stack that made the allocation call
    int stack_depth;  // depth of call_stack
    bool live;
    bool ignored;
  };

  // Memory (de)allocator interface we'll use.
  typedef void* (*Allocator)(size_t size);
  typedef void  (*DeAllocator)(void* ptr);

  // interface ---------------------------

  HeapProfileTable(Allocator alloc, DeAllocator dealloc);
  ~HeapProfileTable();

  // Record an allocation at 'ptr' of 'bytes' bytes.  'stack_depth'
  // and 'call_stack' identifying the function that requested the
  // allocation. They can be generated using GetCallerStackTrace() above.
  void RecordAlloc(const void* ptr, size_t bytes,
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
  // All allocations start as non-live.
  bool MarkAsLive(const void* ptr);

  // If "ptr" points to a recorded allocation, mark it as "ignored".
  // Ignored objects are treated like other objects, except that they
  // are skipped in heap checking reports.
  void MarkAsIgnored(const void* ptr);

  // Return current total (de)allocation statistics.  It doesn't contain
  // mmap'ed regions.
  const Stats& total() const { return total_; }

  // Allocation data iteration callback: gets passed object pointer and
  // fully-filled AllocInfo.
  typedef void (*AllocIterator)(const void* ptr, const AllocInfo& info);

  // Iterate over the allocation profile data calling "callback"
  // for every allocation.
  void IterateAllocs(AllocIterator callback) const {
    address_map_->Iterate([callback] (const void* ptr, AllocValue* v) {
      AllocInfo info;
      info.object_size = v->bytes;
      info.call_stack = v->bucket()->stack;
      info.stack_depth = v->bucket()->depth;
      info.live = v->live();
      info.ignored = v->ignore();
      callback(ptr, info);
    });
  }

  void SaveProfile(tcmalloc::GenericWriter* write) const;

  // Cleanup any old profile files matching prefix + ".*" + kFileExt.
  static void CleanupOldProfiles(const char* prefix);

 private:
  // data types ----------------------------

  // Hash table bucket to hold (de)allocation stats
  // for a given allocation call stack trace.
  typedef HeapProfileBucket Bucket;

  // Info stored in the address map
  struct AllocValue {
    // Access to the stack-trace bucket
    Bucket* bucket() const {
      return reinterpret_cast<Bucket*>(bucket_rep & ~uintptr_t(kMask));
    }
    // This also does set_live(false).
    void set_bucket(Bucket* b) { bucket_rep = reinterpret_cast<uintptr_t>(b); }
    size_t  bytes;   // Number of bytes in this allocation

    // Access to the allocation liveness flag (for leak checking)
    bool live() const { return bucket_rep & kLive; }
    void set_live(bool l) {
      bucket_rep = (bucket_rep & ~uintptr_t(kLive)) | (l ? kLive : 0);
    }

    // Should this allocation be ignored if it looks like a leak?
    bool ignore() const { return bucket_rep & kIgnore; }
    void set_ignore(bool r) {
      bucket_rep = (bucket_rep & ~uintptr_t(kIgnore)) | (r ? kIgnore : 0);
    }

   private:
    // We store a few bits in the bottom bits of bucket_rep.
    // (Alignment is at least four, so we have at least two bits.)
    static const int kLive = 1;
    static const int kIgnore = 2;
    static const int kMask = kLive | kIgnore;

    uintptr_t bucket_rep;
  };

  // helper for FindInsideAlloc
  static size_t AllocValueSize(const AllocValue& v) { return v.bytes; }

  typedef AddressMap<AllocValue> AllocationMap;

  // helpers ----------------------------

  // Unparse bucket b and print its portion of profile dump into given
  // writer.
  //
  // "extra" is appended to the unparsed bucket.  Typically it is empty,
  // but may be set to something like " heapprofile" for the total
  // bucket to indicate the type of the profile.
  static void UnparseBucket(const Bucket& b,
                            tcmalloc::GenericWriter* writer,
                            const char* extra);

  // Get the bucket for the caller stack trace 'key' of depth 'depth'
  // creating the bucket if needed.
  Bucket* GetBucket(int depth, const void* const key[]);

  // Write contents of "*allocations" as a heap profile to
  // "file_name".  "total" must contain the total of all entries in
  // "*allocations".
  static bool WriteProfile(const char* file_name,
                           const Bucket& total,
                           AllocationMap* allocations);

  // data ----------------------------

  // Memory (de)allocator that we use.
  Allocator alloc_;
  DeAllocator dealloc_;

  // Overall profile stats; we use only the Stats part,
  // but make it a Bucket to pass to UnparseBucket.
  Bucket total_;

  // Bucket hash table for malloc.
  // We hand-craft one instead of using one of the pre-written
  // ones because we do not want to use malloc when operating on the table.
  // It is only few lines of code, so no big deal.
  Bucket** bucket_table_;
  int num_buckets_;

  // Map of all currently allocated objects and mapped regions we know about.
  AllocationMap* address_map_;

  DISALLOW_COPY_AND_ASSIGN(HeapProfileTable);
};

#endif  // BASE_HEAP_PROFILE_TABLE_H_
