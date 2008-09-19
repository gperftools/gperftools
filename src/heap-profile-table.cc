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

#include "config.h"

#ifdef HAVE_UNISTD_H
#include <unistd.h>   // for write()
#endif
#include <fcntl.h>    // for open()
#ifdef HAVE_GLOB_H
#include <glob.h>
#endif
#ifdef HAVE_INTTYPES_H
#include <inttypes.h> // for PRIxPTR
#endif
#include <errno.h>
#include <string>
#include <algorithm>  // for sort(), equal(), and copy()

#include "heap-profile-table.h"

#include "base/logging.h"
#include <google/stacktrace.h>
#include <google/malloc_hook.h>
#include "base/commandlineflags.h"
#include "base/sysinfo.h"

using std::sort;
using std::equal;
using std::copy;
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
const int HeapProfileTable::kMaxStackDepth;

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
  while (len > 0) {
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
  ProcMapsIterator::Buffer iterbuf;
  ProcMapsIterator it(0, &iterbuf);   // 0 means "current pid"

  uint64 start, end, offset;
  int64 inode;
  char *flags, *filename;
  int bytes_written = 0;
  while (it.Next(&start, &end, &flags, &offset, &inode, &filename)) {
    bytes_written += it.FormatLine(buf + bytes_written, size - bytes_written,
                                   start, end, flags, offset, inode, filename,
                                   0);
  }
  return bytes_written;
}

// Dump the same data as FillProcSelfMaps reads to fd.
// It seems easier to repeat parts of FillProcSelfMaps here than to
// reuse it via a call.
static void DumpProcSelfMaps(int fd) {
  ProcMapsIterator::Buffer iterbuf;
  ProcMapsIterator it(0, &iterbuf);   // 0 means "current pid"

  uint64 start, end, offset;
  int64 inode;
  char *flags, *filename;
  ProcMapsIterator::Buffer linebuf;
  while (it.Next(&start, &end, &flags, &offset, &inode, &filename)) {
    int written = it.FormatLine(linebuf.buf_, sizeof(linebuf.buf_),
                                start, end, flags, offset, inode, filename,
                                0);
    FDWrite(fd, linebuf.buf_, written);
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

HeapProfileTable::Bucket* HeapProfileTable::GetBucket(int depth,
                                                      const void* const key[]) {
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
  unsigned int buck = ((unsigned int) h) % kHashTableSize;
  for (Bucket* b = table_[buck]; b != 0; b = b->next) {
    if ((b->hash == h) &&
        (b->depth == depth) &&
        equal(key, key + depth, b->stack)) {
      return b;
    }
  }

  // Create new bucket
  const size_t key_size = sizeof(key[0]) * depth;
  const void** kcopy = reinterpret_cast<const void**>(alloc_(key_size));
  copy(key, key + depth, kcopy);
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

void HeapProfileTable::RecordAlloc(const void* ptr, size_t bytes,
                                   int skip_count) {
  void* key[kMaxStackDepth];
  int depth = MallocHook::GetCallerStackTrace(
    key, kMaxStackDepth, kStripFrames + skip_count + 1);
  RecordAllocWithStack(ptr, bytes, depth, key);
}

void HeapProfileTable::RecordAllocWithStack(
    const void* ptr, size_t bytes, int stack_depth,
    const void* const call_stack[]) {
  Bucket* b = GetBucket(stack_depth, call_stack);
  b->allocs++;
  b->alloc_size += bytes;
  total_.allocs++;
  total_.alloc_size += bytes;

  AllocValue v;
  v.set_bucket(b);  // also did set_live(false)
  v.bytes = bytes;
  allocation_->Insert(ptr, v);
}

void HeapProfileTable::RecordFree(const void* ptr) {
  AllocValue v;
  if (allocation_->FindAndRemove(ptr, &v)) {
    Bucket* b = v.bucket();
    b->frees++;
    b->free_size += v.bytes;
    total_.frees++;
    total_.free_size += v.bytes;
  }
}

bool HeapProfileTable::FindAlloc(const void* ptr, size_t* object_size) const {
  const AllocValue* alloc_value = allocation_->Find(ptr);
  if (alloc_value != NULL) *object_size = alloc_value->bytes;
  return alloc_value != NULL;
}

bool HeapProfileTable::FindAllocDetails(const void* ptr,
                                        AllocInfo* info) const {
  const AllocValue* alloc_value = allocation_->Find(ptr);
  if (alloc_value != NULL) {
    info->object_size = alloc_value->bytes;
    info->call_stack = alloc_value->bucket()->stack;
    info->stack_depth = alloc_value->bucket()->depth;
  }
  return alloc_value != NULL;
}

bool HeapProfileTable::FindInsideAlloc(const void* ptr,
                                       size_t max_size,
                                       const void** object_ptr,
                                       size_t* object_size) const {
  const AllocValue* alloc_value =
    allocation_->FindInside(&AllocValueSize, max_size, ptr, object_ptr);
  if (alloc_value != NULL) *object_size = alloc_value->bytes;
  return alloc_value != NULL;
}

bool HeapProfileTable::MarkAsLive(const void* ptr) {
  AllocValue* alloc = allocation_->FindMutable(ptr);
  if (alloc && !alloc->live()) {
    alloc->set_live(true);
    return true;
  }
  return false;
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
    snprintf(buf + buflen, bufsize - buflen, "%6d: %8"PRId64" [%6d: %8"PRId64"] @",
             b.allocs - b.frees,
             b.alloc_size - b.free_size,
             b.allocs,
             b.alloc_size);
  // If it looks like the snprintf failed, ignore the fact we printed anything
  if (printed < 0 || printed >= bufsize - buflen) return buflen;
  buflen += printed;
  for (int d = 0; d < b.depth; d++) {
    printed = snprintf(buf + buflen, bufsize - buflen, " 0x%08" PRIxPTR,
                       reinterpret_cast<uintptr_t>(b.stack[d]));
    if (printed < 0 || printed >= bufsize - buflen) return buflen;
    buflen += printed;
  }
  printed = snprintf(buf + buflen, bufsize - buflen, "\n");
  if (printed < 0 || printed >= bufsize - buflen) return buflen;
  buflen += printed;
  return buflen;
}

HeapProfileTable::Bucket** 
HeapProfileTable::MakeSortedBucketList() const {
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

  return list;
}

void HeapProfileTable::IterateOrderedAllocContexts(
    AllocContextIterator callback) const {
  Bucket** list = MakeSortedBucketList();
  AllocContextInfo info;
  for (int i = 0; i < num_buckets_; ++i) {
    *static_cast<Stats*>(&info) = *static_cast<Stats*>(list[i]);
    info.stack_depth = list[i]->depth;
    info.call_stack = list[i]->stack;
    callback(info);
  }
  dealloc_(list);
}

int HeapProfileTable::FillOrderedProfile(char buf[], int size) const {
  Bucket** list = MakeSortedBucketList();

  // Our file format is "bucket, bucket, ..., bucket, proc_self_maps_info".
  // In the cases buf is too small, we'd rather leave out the last
  // buckets than leave out the /proc/self/maps info.  To ensure that,
  // we actually print the /proc/self/maps info first, then move it to
  // the end of the buffer, then write the bucket info into whatever
  // is remaining, and then move the maps info one last time to close
  // any gaps.  Whew!
  int map_length = snprintf(buf, size, "%s", kProcSelfMapsHeader);
  if (map_length < 0 || map_length >= size) return 0;
  map_length += FillProcSelfMaps(buf + map_length, size - map_length);
  RAW_DCHECK(map_length <= size, "");
  char* const map_start = buf + size - map_length;      // move to end
  memmove(map_start, buf, map_length);
  size -= map_length;

  Stats stats;
  memset(&stats, 0, sizeof(stats));
  int bucket_length = snprintf(buf, size, "%s", kProfileHeader);
  if (bucket_length < 0 || bucket_length >= size) return 0;
  bucket_length = UnparseBucket(total_, buf, bucket_length, size, &stats);
  for (int i = 0; i < num_buckets_; i++) {
    bucket_length = UnparseBucket(*list[i], buf, bucket_length, size, &stats);
  }
  RAW_DCHECK(bucket_length < size, "");

  dealloc_(list);

  RAW_DCHECK(buf + bucket_length <= map_start, "");
  memmove(buf + bucket_length, map_start, map_length);  // close the gap

  return bucket_length + map_length;
}

inline
void HeapProfileTable::DumpNonLiveIterator(const void* ptr, AllocValue* v,
                                           const DumpArgs& args) {
  if (v->live()) {
    v->set_live(false);
    return;
  }
  Bucket b;
  memset(&b, 0, sizeof(b));
  b.allocs = 1;
  b.alloc_size = v->bytes;
  const void* stack[kMaxStackDepth + 1];
  b.depth = v->bucket()->depth + static_cast<int>(args.dump_alloc_addresses);
  b.stack = stack;
  if (args.dump_alloc_addresses) stack[0] = ptr;
  memcpy(stack + static_cast<int>(args.dump_alloc_addresses),
         v->bucket()->stack, sizeof(stack[0]) * v->bucket()->depth);
  char buf[1024];
  int len = UnparseBucket(b, buf, 0, sizeof(buf), args.profile_stats);
  FDWrite(args.fd, buf, len);
}

bool HeapProfileTable::DumpNonLiveProfile(const char* file_name,
                                          bool dump_alloc_addresses,
                                          Stats* profile_stats) const {
  RAW_VLOG(1, "Dumping non-live heap profile to %s", file_name);
  int fd = open(file_name, O_WRONLY|O_CREAT|O_TRUNC, 0644);
  if (fd >= 0) {
    FDWrite(fd, kProfileHeader, strlen(kProfileHeader));
    char buf[512];
    int len = UnparseBucket(total_, buf, 0, sizeof(buf), profile_stats);
    FDWrite(fd, buf, len);
    memset(profile_stats, 0, sizeof(*profile_stats));
    const DumpArgs args(fd, dump_alloc_addresses, profile_stats);
    allocation_->Iterate<const DumpArgs&>(DumpNonLiveIterator, args);
    FDWrite(fd, kProcSelfMapsHeader, strlen(kProcSelfMapsHeader));
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
#if defined(HAVE_GLOB_H)
  glob_t g;
  const int r = glob(pattern.c_str(), GLOB_ERR, NULL, &g);
  if (r == 0 || r == GLOB_NOMATCH) {
    const int prefix_length = strlen(prefix);
    for (int i = 0; i < g.gl_pathc; i++) {
      const char* fname = g.gl_pathv[i];
      if ((strlen(fname) >= prefix_length) &&
          (memcmp(fname, prefix, prefix_length) == 0)) {
        RAW_VLOG(1, "Removing old heap profile %s", fname);
        unlink(fname);
      }
    }
  }
  globfree(&g);
#else   /* HAVE_GLOB_H */
  RAW_LOG(WARNING, "Unable to remove old heap profiles (can't run glob())");
#endif
}
