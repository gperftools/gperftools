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

#include <fcntl.h>
#include <glob.h>
#include <string>

#include "heap-profile-table.h"

#include "base/logging.h"
#include <google/stacktrace.h>
#include <google/malloc_hook.h>
#include "base/commandlineflags.h"

using std::sort;
using std::string;

//----------------------------------------------------------------------

DEFINE_bool(cleanup_old_heap_profiles,
            EnvToBool("HEAP_PROFILE_CLEANUP", true),
            "At initialization time, delete old heap profiles.");

//----------------------------------------------------------------------

// header of the dumped heap profile
static const char kProfileHeader[] = "heap profile: ";
static const char kProcSelfMapsHeader[] = "\nMAPPED_LIBRARIES:\n";

//----------------------------------------------------------------------

const char HeapProfileTable::kFileExt[] = ".heap";

const int HeapProfileTable::kHashTableSize;
const int HeapProfileTable::kMaxStackTrace;

//----------------------------------------------------------------------

// We strip out different number of stack frames in debug mode
// because less inlining happens in that case
#ifdef NDEBUG
static const int kStripFrames = 2;
#else
static const int kStripFrames = 3;
#endif

// Re-run fn until it doesn't cause EINTR.
#define NO_INTR(fn)  do {} while ((fn) < 0 && errno == EINTR)

// Wrapper around ::write to undo it's potential partiality.
static void FDWrite(int fd, const char* buf, size_t len) {
  while (1) {
    ssize_t r;
    NO_INTR(r = write(fd, buf, len));
    if (r <= 0) break;
    buf += r;
    len -= r;
  }
}

// For sorting Stats or Buckets by in-use space
static bool ByAllocatedSpace(HeapProfileTable::Stats* a,
                             HeapProfileTable::Stats* b) {
  // Return true iff "a" has more allocated space than "b"
  return (a->alloc_size - a->free_size) > (b->alloc_size - b->free_size);
}

// Helper to add the list of mapped shared libraries to a profile.
// Fill formatted "/proc/self/maps" contents into buffer 'buf' of size 'size'
// and return the actual size occupied in 'buf'.
// We do not provision for 0-terminating 'buf'.
static int FillProcSelfMaps(char buf[], int size) {
  int buflen = snprintf(buf, size, kProcSelfMapsHeader);
  if (buflen < 0 || buflen >= size) return 0;
  int maps = open("/proc/self/maps", O_RDONLY);
  if (maps >= 0) {
    while (buflen < size) {
      ssize_t r;
      NO_INTR(r = read(maps, buf + buflen, size - buflen));
      if (r <= 0) break;
      buflen += r;
    }
    NO_INTR(close(maps));
  }
  return buflen;
}

// Dump the same data as FillProcSelfMaps reads to fd.
// It seems easier to repeat parts of FillProcSelfMaps here than to
// reuse it via a call.
static void DumpProcSelfMaps(int fd) {
  FDWrite(fd, kProcSelfMapsHeader, sizeof(kProcSelfMapsHeader)-1);  // chop \0
  int maps = open("/proc/self/maps", O_RDONLY);
  if (maps >= 0) {
    char buf[512];
    while (1) {
      ssize_t r;
      NO_INTR(r = read(maps, buf, sizeof(buf)));
      if (r <= 0) break;
      FDWrite(fd, buf, r);
    }
    NO_INTR(close(maps));
  }
}

//----------------------------------------------------------------------

HeapProfileTable::HeapProfileTable(Allocator alloc, DeAllocator dealloc)
    : alloc_(alloc), dealloc_(dealloc) {
  // Make the table
  const int table_bytes = kHashTableSize * sizeof(*table_);
  table_ = reinterpret_cast<Bucket**>(alloc_(table_bytes));
  memset(table_, 0, table_bytes);
  // Make allocation map
  allocation_ =
    new(alloc_(sizeof(AllocationMap))) AllocationMap(alloc_, dealloc_);
  // init the rest:
  memset(&total_, 0, sizeof(total_));
  num_buckets_ = 0;
}

HeapProfileTable::~HeapProfileTable() {
  // free allocation map
  allocation_->~AllocationMap();
  dealloc_(allocation_);
  allocation_ = NULL;
  // free hash table
  for (int b = 0; b < kHashTableSize; b++) {
    for (Bucket* x = table_[b]; x != 0; /**/) {
      Bucket* b = x;
      x = x->next;
      dealloc_(b->stack);
      dealloc_(b);
    }
  }
  dealloc_(table_);
  table_ = NULL;
}

HeapProfileTable::Bucket* HeapProfileTable::GetBucket(int skip_count) {
  // Get raw stack trace
  void* key[kMaxStackTrace];
  int depth =
    MallocHook::GetCallerStackTrace(key, kMaxStackTrace, skip_count+1);
  // Make hash-value
  uintptr_t h = 0;
  for (int i = 0; i < depth; i++) {
    h += reinterpret_cast<uintptr_t>(key[i]);
    h += h << 10;
    h ^= h >> 6;
  }
  h += h << 3;
  h ^= h >> 11;

  // Lookup stack trace in table
  const size_t key_size = sizeof(key[0]) * depth;
  unsigned int buck = ((unsigned int) h) % kHashTableSize;
  for (Bucket* b = table_[buck]; b != 0; b = b->next) {
    if ((b->hash == h) &&
        (b->depth == depth) &&
        (memcmp(b->stack, key, key_size) == 0)) {
      return b;
    }
  }

  // Create new bucket
  void** kcopy = reinterpret_cast<void**>(alloc_(key_size));
  memcpy(kcopy, key, key_size);
  Bucket* b = reinterpret_cast<Bucket*>(alloc_(sizeof(Bucket)));
  memset(b, 0, sizeof(*b));
  b->hash  = h;
  b->depth = depth;
  b->stack = kcopy;
  b->next  = table_[buck];
  table_[buck] = b;
  num_buckets_++;
  return b;
}

void HeapProfileTable::RecordAlloc(void* ptr, size_t bytes, int skip_count) {
  Bucket* b = GetBucket(kStripFrames + skip_count + 1);
  b->allocs++;
  b->alloc_size += bytes;
  total_.allocs++;
  total_.alloc_size += bytes;

  AllocValue v;
  v.bucket = b;
  v.bytes = bytes;
  allocation_->Insert(ptr, v);
}

void HeapProfileTable::RecordFree(void* ptr) {
  AllocValue v;
  if (allocation_->FindAndRemove(ptr, &v)) {
    Bucket* b = v.bucket;
    b->frees++;
    b->free_size += v.bytes;
    total_.frees++;
    total_.free_size += v.bytes;
  }
}

bool HeapProfileTable::FindAlloc(void* ptr, size_t* object_size) const {
  AllocValue alloc_value;
  if (allocation_->Find(ptr, &alloc_value)) {
    *object_size = alloc_value.bytes;
    return true;
  }
  return false;
}

bool HeapProfileTable::FindAllocDetails(void* ptr, AllocInfo* info) const {
  AllocValue alloc_value;
  if (allocation_->Find(ptr, &alloc_value)) {
    info->object_size = alloc_value.bytes;
    info->call_stack = alloc_value.bucket->stack;
    info->stack_depth = alloc_value.bucket->depth;
    return true;
  }
  return false;
}

void HeapProfileTable::MapArgsAllocIterator(
    void* ptr, AllocValue v, AllocIterator callback) {
  AllocInfo info;
  info.object_size = v.bytes;
  info.call_stack = v.bucket->stack;
  info.stack_depth = v.bucket->depth;
  callback(ptr, info);
}

void HeapProfileTable::IterateAllocs(AllocIterator callback) const {
  allocation_->Iterate(MapArgsAllocIterator, callback);
}

// We'd be happier using snprintfer, but we don't to reduce dependencies.
int HeapProfileTable::UnparseBucket(const Bucket& b,
                                    char* buf, int buflen, int bufsize,
                                    Stats* profile_stats) {
  profile_stats->allocs += b.allocs;
  profile_stats->alloc_size += b.alloc_size;
  profile_stats->frees += b.frees;
  profile_stats->free_size += b.free_size;
  int printed =
    snprintf(buf + buflen, bufsize - buflen, "%6d: %8lld [%6d: %8lld] @",
             b.allocs - b.frees,
             b.alloc_size - b.free_size,
             b.allocs,
             b.alloc_size);
  // If it looks like the snprintf failed, ignore the fact we printed anything
  if (printed < 0 || printed >= bufsize - buflen) return buflen;
  buflen += printed;
  for (int d = 0; d < b.depth; d++) {
    printed = snprintf(buf + buflen, bufsize - buflen, " 0x%08lx",
                       (unsigned long)b.stack[d]);
    if (printed < 0 || printed >= bufsize - buflen) return buflen;
    buflen += printed;
  }
  printed = snprintf(buf + buflen, bufsize - buflen, "\n");
  if (printed < 0 || printed >= bufsize - buflen) return buflen;
  buflen += printed;
  return buflen;
}

int HeapProfileTable::FillOrderedProfile(char buf[], int size) const {
  // We can't allocate list on the stack, as this would overflow on threads
  // running with a small stack size.
  Bucket** list =
    reinterpret_cast<Bucket**>(alloc_(sizeof(Bucket) * num_buckets_));

  int n = 0;
  for (int b = 0; b < kHashTableSize; b++) {
    for (Bucket* x = table_[b]; x != 0; x = x->next) {
      list[n++] = x;
    }
  }
  RAW_DCHECK(n == num_buckets_, "");

  sort(list, list + num_buckets_, ByAllocatedSpace);

  Stats stats;
  memset(&stats, 0, sizeof(stats));
  int buflen = snprintf(buf, size, kProfileHeader);
  if (buflen < 0 || buflen >= size) return 0;
  buflen = UnparseBucket(total_, buf, buflen, size, &stats);
  for (int i = 0; i < n; i++) {
    buflen = UnparseBucket(*list[i], buf, buflen, size, &stats);
  }
  RAW_DCHECK(buflen < size, "");

  dealloc_(list);

  buflen += FillProcSelfMaps(buf + buflen, size - buflen);

  return buflen;
}

void HeapProfileTable::FilteredDumpIterator(void* ptr, AllocValue v,
                                            const DumpArgs& args) {
  if (args.filter(ptr, v.bytes)) return;
  Bucket b;
  memset(&b, 0, sizeof(b));
  b.allocs = 1;
  b.alloc_size = v.bytes;
  void* stack[kMaxStackTrace + 1];
  b.depth = v.bucket->depth + int(args.dump_alloc_addresses);
  b.stack = stack;
  if (args.dump_alloc_addresses) stack[0] = ptr;
  memcpy(stack + int(args.dump_alloc_addresses),
         v.bucket->stack, sizeof(stack[0]) * v.bucket->depth);
  char buf[1024];
  int len = UnparseBucket(b, buf, 0, sizeof(buf), args.profile_stats);
  FDWrite(args.fd, buf, len);
}

bool HeapProfileTable::DumpFilteredProfile(const char* file_name,
                                           DumpFilter filter,
                                           bool dump_alloc_addresses,
                                           Stats* profile_stats) const {
  RAW_VLOG(1, "Dumping filtered heap profile to %s", file_name);
  int fd = open(file_name, O_WRONLY|O_CREAT|O_TRUNC, 0644);
  if (fd >= 0) {
    FDWrite(fd, kProfileHeader, strlen(kProfileHeader));
    char buf[512];
    int len = UnparseBucket(total_, buf, 0, sizeof(buf), profile_stats);
    FDWrite(fd, buf, len);
    memset(profile_stats, 0, sizeof(*profile_stats));
    const DumpArgs args(fd, dump_alloc_addresses, filter, profile_stats);
    allocation_->Iterate<const DumpArgs&>(FilteredDumpIterator, args);
    DumpProcSelfMaps(fd);
    NO_INTR(close(fd));
    return true;
  } else {
    RAW_LOG(ERROR, "Failed dumping filtered heap profile to %s", file_name);
    return false;
  }
}

void HeapProfileTable::CleanupOldProfiles(const char* prefix) {
  if (!FLAGS_cleanup_old_heap_profiles)
    return;
  string pattern = string(prefix) + ".*" + kFileExt;
  glob_t g;
  const int r = glob(pattern.c_str(), GLOB_ERR, NULL, &g);
  if (r == 0 || r == GLOB_NOMATCH) {
    const int prefix_length = strlen(prefix);
    for (int i = 0; i < g.gl_pathc; i++) {
      const char* fname = g.gl_pathv[i];
      if ((strlen(fname) >= prefix_length) &&
          (memcmp(fname, prefix, prefix_length) == 0)) {
        RAW_VLOG(0, "Removing old heap profile %s", fname);
        unlink(fname);
      }
    }
  }
  globfree(&g);
}
