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
// Author: Sanjay Ghemawat
//
// A fast map from addresses to values.  Assumes that addresses are
// clustered.  The main use is intended to be for heap-profiling.
// May be too memory-hungry for other uses.
//
// We use a user-defined allocator/de-allocator so that we can use
// this data structure during heap-profiling.
//
// IMPLEMENTATION DETAIL:
//
// Some default definitions/parameters:
//  * Block      -- aligned 128-byte region of the address space
//  * Cluster    -- aligned 1-MB region of the address space
//  * Block-ID   -- block-number within a cluster
//  * Cluster-ID -- Starting address of cluster divided by cluster size
//
// We use a three-level map to represent the state:
//  1. A hash-table maps from a cluster-ID to the data for that cluster.
//  2. For each non-empty cluster we keep an array indexed by
//     block-ID tht points to the first entry in the linked-list
//     for the block.
//  3. At the bottom, we keep a singly-linked list of all
//     entries in a block (for non-empty blocks).
//
//    hash table
//  +-------------+
//  | id->cluster |---> ...
//  |     ...     |
//  | id->cluster |--->  Cluster
//  +-------------+     +-------+    Data for one block
//                      |  nil  |   +------------------------------------+
//                      |   ----+---|->[addr/value]-->[addr/value]-->... |
//                      |  nil  |   +------------------------------------+
//                      |   ----+--> ...
//                      |  nil  |
//                      |  ...  |
//                      +-------+
//
// Note that we require zero-bytes of overhead for completely empty
// clusters.  The minimum space requirement for a cluster is the size
// of the hash-table entry plus a pointer value for each block in
// the cluster.  Empty blocks impose no extra space requirement.
//
// The cost of a lookup is:
//      a. A hash-table lookup to find the cluster
//      b. An array access in the cluster structure
//      c. A traversal over the linked-list for a block

#ifndef _ADDRESSMAP_H
#define _ADDRESSMAP_H

#include "google/perftools/config.h"
#include <stddef.h>
#include <string.h>
#if defined HAVE_STDINT_H
#include <stdint.h>             // to get uint16_t (ISO naming madness)
#elif defined HAVE_INTTYPES_H
#include <inttypes.h>           // another place uint16_t might be defined
#else
#include <sys/types.h>          // our last best hope
#endif

template <class Value>
class AddressMap {
 public:
  typedef void* (*Allocator)(size_t);
  typedef void  (*DeAllocator)(void*);
  typedef void* Key;

  // Create an AddressMap that uses the specified allocator/deallocator.
  // The allocator/deallocator should behave like malloc/free.
  // For instance, the allocator does not need to return initialized memory.
  AddressMap(Allocator alloc, DeAllocator dealloc);
  ~AddressMap();

  // If the map contains an entry for "key", store the associated
  // value in "*result" and return true.  Else return false.
  bool Find(Key key, Value* result);

  // Insert <key,value> into the map.  Any old value associated
  // with key is forgotten.
  void Insert(Key key, Value value);

  // Remove any entry for key in the map.  If an entry was found
  // and removed, stores the associated value in "*removed_value"
  // and returns true.  Else returns false.
  bool FindAndRemove(Key key, Value* removed_value);

 private:
  typedef uintptr_t Number;

  // The implementation assumes that addresses inserted into the map
  // will be clustered.  We take advantage of this fact by splitting
  // up the address-space into blocks and using a linked-list entry
  // for each block.

  // Size of each block.  There is one linked-list for each block, so
  // do not make the block-size too big.  Oterwise, a lot of time
  // will be spent traversing linked lists.
  static const int kBlockBits = 7;
  static const int kBlockSize = 1 << kBlockBits;

  // Entry kept in per-block linked-list
  struct Entry {
    Entry* next;
    Key    key;
    Value  value;
  };

  // We further group a sequence of consecutive blocks into a cluster.
  // The data for a cluster is represented as a dense array of
  // linked-lists, one list per contained block.
  static const int kClusterBits = 13;
  static const int kClusterSize = 1 << (kBlockBits + kClusterBits);
  static const int kClusterBlocks = 1 << kClusterBits;

  // We use a simple chaining hash-table to represent the clusters.
  struct Cluster {
    Cluster* next;                      // Next cluster in chain
    Number   id;                        // Cluster ID
    Entry*   blocks[kClusterBlocks];    // Per-block linked-lists
  };

  // Number of hash-table entries.  With the block-size/cluster-size
  // defined above, each cluster covers 1 MB, so an 4K entry
  // hash-table will give an average hash-chain length of 1 for 4GB of
  // in-use memory.
  static const int kHashBits = 12;
  static const int kHashSize = 1 << 12;

  // Number of entry objects allocated at a time
  static const int ALLOC_COUNT = 64;

  Cluster**     hashtable_;             // The hash-table
  Entry*        free_;                  // Free list of unused Entry objects

  // Multiplicative hash function:
  // The value "kHashMultiplier" is the bottom 32 bits of
  //    int((sqrt(5)-1)/2 * 2^32)
  // This is a good multiplier as suggested in CLR, Knuth.  The hash
  // value is taken to be the top "k" bits of the bottom 32 bits
  // of the muliplied value.
  static const uint32_t kHashMultiplier = 2654435769u;
  static int HashInt(Number x) {
    // Multiply by a constant and take the top bits of the result.
    const uint32_t m = static_cast<uint32_t>(x) * kHashMultiplier;
    return static_cast<int>(m >> (32 - kHashBits));
  }

  // Find cluster object for specified address.  If not found
  // and "create" is true, create the object.  If not found
  // and "create" is false, return NULL.
  Cluster* FindCluster(Number address, bool create) {
    // Look in hashtable
    const Number cluster_id = address >> (kBlockBits + kClusterBits);
    const int h = HashInt(cluster_id);
    for (Cluster* c = hashtable_[h]; c != NULL; c = c->next) {
      if (c->id == cluster_id) {
        return c;
      }
    }

    // Create cluster if necessary
    if (create) {
      Cluster* c = New<Cluster>(1);
      c->id = cluster_id;
      c->next = hashtable_[h];
      hashtable_[h] = c;
      return c;
    }
    return NULL;
  }

  // Return the block ID for an address within its cluster
  static int BlockID(Number address) {
    return (address >> kBlockBits) & (kClusterBlocks - 1);
  }

  //--------------------------------------------------------------
  // Memory management -- we keep all objects we allocate linked
  // together in a singly linked list so we can get rid of them
  // when we are all done.  Furthermore, we allow the client to
  // pass in custom memory allocator/deallocator routines.
  //--------------------------------------------------------------
  struct Object {
    Object* next;
    // The real data starts here
  };

  Allocator     alloc_;                 // The allocator
  DeAllocator   dealloc_;               // The deallocator
  Object*       allocated_;             // List of allocated objects

  // Allocates a zeroed array of T with length "num".  Also inserts
  // the allocated block into a linked list so it can be deallocated
  // when we are all done.
  template <class T> T* New(int num) {
    void* ptr = (*alloc_)(sizeof(Object) + num*sizeof(T));
    memset(ptr, 0, sizeof(Object) + num*sizeof(T));
    Object* obj = reinterpret_cast<Object*>(ptr);
    obj->next = allocated_;
    allocated_ = obj;
    return reinterpret_cast<T*>(reinterpret_cast<Object*>(ptr) + 1);
  }
};

// More implementation details follow:

template <class Value>
AddressMap<Value>::AddressMap(Allocator alloc, DeAllocator dealloc)
  : free_(NULL),
    alloc_(alloc),
    dealloc_(dealloc),
    allocated_(NULL) {
  hashtable_ = New<Cluster*>(kHashSize);
}

template <class Value>
AddressMap<Value>::~AddressMap() {
  // De-allocate all of the objects we allocated
  for (Object* obj = allocated_; obj != NULL; ) {
    Object* next = obj->next;
    (*dealloc_)(obj);
    obj = next;
  }
}

template <class Value>
bool AddressMap<Value>::Find(Key key, Value* result) {
  const Number num = reinterpret_cast<Number>(key);
  const Cluster* const c = FindCluster(num, false/*do not create*/);
  if (c != NULL) {
    for (const Entry* e = c->blocks[BlockID(num)]; e != NULL; e = e->next) {
      if (e->key == key) {
        *result = e->value;
        return true;
      }
    }
  }
  return false;
}

template <class Value>
void AddressMap<Value>::Insert(Key key, Value value) {
  const Number num = reinterpret_cast<Number>(key);
  Cluster* const c = FindCluster(num, true/*create*/);

  // Look in linked-list for this block
  const int block = BlockID(num);
  for (Entry* e = c->blocks[block]; e != NULL; e = e->next) {
    if (e->key == key) {
      e->value = value;
      return;
    }
  }

  // Create entry
  if (free_ == NULL) {
    // Allocate a new batch of entries and add to free-list
    Entry* array = New<Entry>(ALLOC_COUNT);
    for (int i = 0; i < ALLOC_COUNT-1; i++) {
      array[i].next = &array[i+1];
    }
    array[ALLOC_COUNT-1].next = free_;
    free_ = &array[0];
  }
  Entry* e = free_;
  free_ = e->next;
  e->key = key;
  e->value = value;
  e->next = c->blocks[block];
  c->blocks[block] = e;
}

template <class Value>
bool AddressMap<Value>::FindAndRemove(Key key, Value* removed_value) {
  const Number num = reinterpret_cast<Number>(key);
  Cluster* const c = FindCluster(num, false/*do not create*/);
  if (c != NULL) {
    for (Entry** p = &c->blocks[BlockID(num)]; *p != NULL; p = &(*p)->next) {
      Entry* e = *p;
      if (e->key == key) {
        *removed_value = e->value;
        *p = e->next;         // Remove e from linked-list
        e->next = free_;      // Add e to free-list
        free_ = e;
        return true;
      }
    }
  }
  return false;
}

#endif /* _ADDRESSMAP_H */
