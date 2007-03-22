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
// All Rights Reserved.
//
// Author: Maxim Lifantsev
//

// NOTE: We almost never use CHECK and LOG in this module
//       because we might be running before/after the logging susbystem
//       is set up correctly.

#include "config.h"

#include <string>
#include <vector>
#include <map>
#include <google/perftools/hash_set.h>
#include <algorithm>

#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <assert.h>

#ifdef HAVE_LINUX_PTRACE_H
#include <linux/ptrace.h>
#endif
#ifdef HAVE_SYSCALL_H
#include <syscall.h>
#endif

#include <elf.h>

#include <google/stacktrace.h>
#include <google/heap-profiler.h>
#include <google/heap-checker.h>
#include "heap-profiler-inl.h"
#include "addressmap-inl.h"

#include "base/basictypes.h"
#include "base/commandlineflags.h"
#include "base/logging.h"
#include "base/elfcore.h"              // for i386_regs
#include "base/thread_lister.h"
#include "maybe_threads.h"

#ifdef HAVE_INTTYPES_H
#define __STDC_FORMAT_MACROS
#include <inttypes.h>
// TODO: have both SCNd64 and PRId64.  We don't bother since they're the same
#define LLX    "%"SCNx64             // how to read 64-bit hex
#define LLD    "%"SCNd64             // how to read 64-bit deciman
#else
#define LLX    "%llx"                // hope for the best
#define LLD    "%lld"
#endif

#ifndef	PATH_MAX
#ifdef MAXPATHLEN
#define	PATH_MAX	MAXPATHLEN
#else
#define	PATH_MAX	4096         // seems conservative for max filename len!
#endif
#endif

using std::string;
using std::map;
using std::vector;
using std::swap;
using std::make_pair;
using std::min;
using std::max;
using HASH_NAMESPACE::hash_set;

//----------------------------------------------------------------------
// Flags that control heap-checking
//----------------------------------------------------------------------

DEFINE_bool(heap_check_report, true,
            "If overall heap check should report the found leaks via pprof");

// These are not so much flags as internal configuration parameters that
// are set based on the argument to StartFromMain().
DEFINE_bool(heap_check_before_constructors, true,
            "deprecated; pretty much always true now");

DEFINE_bool(heap_check_after_destructors, false,
            "If overall heap check is to end after global destructors "
            "or right after all REGISTER_HEAPCHECK_CLEANUP's");

DEFINE_bool(heap_check_strict_check, true,
            "If overall heap check is to be done "
            "via HeapLeakChecker::*SameHeap "
            "or HeapLeakChecker::*NoLeaks call");
            // heap_check_strict_check == false
            // is useful only when heap_check_before_constructors == false

DEFINE_bool(heap_check_ignore_global_live, true,
            "If overall heap check is to ignore heap objects reachable "
            "from the global data");

DEFINE_bool(heap_check_ignore_thread_live, true,
            "If set to true, objects reachable from thread stacks "
            "and registers are not reported as leaks");

// Normally we'd make this a flag, but we can't do that in this case
// because it may need to be accessed after global destructors have
// started to run, which would delete flags.  Instead we make it a pointer,
// which will never get destroyed.
static string* flags_heap_profile_pprof = NULL;

// External accessors for the above
void HeapLeakChecker::set_heap_check_report(bool b) {
  FLAGS_heap_check_report = b;
}
void HeapLeakChecker::set_pprof_path(const char* s) {
  if (flags_heap_profile_pprof == NULL) {
    flags_heap_profile_pprof = new string(s);
  } else {
    flags_heap_profile_pprof->assign(s);
  }
}

void HeapLeakChecker::set_dump_directory(const char* s) {
  if (dump_directory_ == NULL)  dump_directory_ = new string;
  dump_directory_->assign(s);
}

bool HeapLeakChecker::heap_check_report() {
  return FLAGS_heap_check_report;
}
const char* HeapLeakChecker::pprof_path() {
  if (flags_heap_profile_pprof == NULL) {
    return INSTALL_PREFIX "/bin/pprof";  // our default value
  } else {
    return flags_heap_profile_pprof->c_str();
  }
}
const char* HeapLeakChecker::dump_directory() {
  if (dump_directory_ == NULL) {
    return "/tmp";  // our default value
  } else {
    return dump_directory_->c_str();
  }
}

//----------------------------------------------------------------------

DECLARE_int32(heap_profile_log); // in heap-profiler.cc

//----------------------------------------------------------------------
// HeapLeakChecker global data
//----------------------------------------------------------------------

// Global lock for the global data of this module
static pthread_mutex_t heap_checker_lock = PTHREAD_MUTEX_INITIALIZER;

// the disabled regexp accumulated
// via HeapLeakChecker::DisableChecksIn
static string* disabled_regexp = NULL;

//----------------------------------------------------------------------

// Heap profile prefix for leak checking profiles,
static string* profile_prefix = NULL;

// whole-program heap leak checker
static HeapLeakChecker* main_heap_checker = NULL;

// if we are doing (or going to do) any kind of heap-checking
// heap_checker_on == true implies HeapProfiler::is_on_ == true
static bool heap_checker_on = false;
// pid of the process that does whole-program heap leak checking
static pid_t heap_checker_pid = 0;

// if we did heap profiling during global constructors execution
static bool constructor_heap_profiling = false;

//----------------------------------------------------------------------
// HeapLeakChecker live object tracking components
//----------------------------------------------------------------------

// Cases of live object placement we distinguish
enum ObjectPlacement {
  MUST_BE_ON_HEAP,  // Must point to a live object of the matching size in the
                    // map of the heap in HeapProfiler when we get to it.
  IGNORED_ON_HEAP,  // Is a live (ignored) object on heap.
  IN_GLOBAL_DATA,   // Is part of global data region of the executable.
  THREAD_STACK,     // Part of a thread stack
  THREAD_REGISTERS, // Values in registers of some thread
};

// Information about an allocated object
struct AllocObject {
  void* ptr;              // the object
  uintptr_t size;         // its size
  ObjectPlacement place;  // where ptr points to

  AllocObject(void* p, size_t s, ObjectPlacement l)
    : ptr(p), size(s), place(l) { }
};

// All objects (memory ranges) ignored via HeapLeakChecker::IgnoreObject
// Key is the object's address; value is its size.
typedef map<uintptr_t, size_t> IgnoredObjectsMap;
static IgnoredObjectsMap* ignored_objects = NULL;

// All objects (memory ranges) that we consider to be the sources of pointers
// to live (not leaked) objects.
// At different times this holds (what can be reached from) global data regions
// and the objects we've been told to ignore.
// For any AllocObject::ptr "live_objects" is supposed to contain at most one
// record at any time. We maintain this by checking with HeapProfiler's map
// of the heap and removing the live heap objects we've handled from it.
// This vector is maintained as a stack and the frontier of reachable
// live heap objects in our flood traversal of them.
typedef vector<AllocObject> LiveObjectsStack;
static LiveObjectsStack* live_objects = NULL;

// A placeholder to fill-in the starting values for live_objects
// for each library so we can keep the library-name association for logging.
typedef map<string, LiveObjectsStack> LibraryLiveObjectsStacks;
static LibraryLiveObjectsStacks* library_live_objects = NULL;

// The disabled program counter addresses for profile dumping
// that are registered with HeapLeakChecker::DisableChecksUp
typedef hash_set<uintptr_t> DisabledAddressSet;
static DisabledAddressSet* disabled_addresses = NULL;

// Value stored in the map of disabled address ranges;
// its key is the end of the address range.
// We'll ignore allocations with a return address in a disabled range
// if the address occurs at 'max_depth' or less in the stack trace.
struct HeapLeakChecker::RangeValue {
  uintptr_t start_address;  // the start of the range
  int       max_depth;      // the maximal stack depth to disable at
};
typedef map<uintptr_t, HeapLeakChecker::RangeValue> DisabledRangeMap;
// The disabled program counter address ranges for profile dumping
// that are registered with HeapLeakChecker::DisableChecksFromTo.
static DisabledRangeMap* disabled_ranges = NULL;

// Stack range map: maps from the start address to the end address.
// These are used to not disable all allocated memory areas
// that are used for stacks so that we do treat stack pointers
// from dead stack frames as live.
typedef map<uintptr_t, uintptr_t> StackRangeMap;
static StackRangeMap* stack_ranges = NULL;

// This routine is called for every thread stack we know about.
static void RegisterStackRange(void* top, void* bottom) {
  char* p1 = min(reinterpret_cast<char*>(top),
                 reinterpret_cast<char*>(bottom));
  char* p2 = max(reinterpret_cast<char*>(top),
                 reinterpret_cast<char*>(bottom));
  if (HeapProfiler::kMaxLogging) {
    HeapProfiler::MESSAGE(1, "HeapChecker: Thread stack %p..%p (%d bytes)\n",
                          p1, p2, int(p2-p1));
  }
  live_objects->push_back(AllocObject(p1, uintptr_t(p2-p1), THREAD_STACK));
  stack_ranges->insert(make_pair(reinterpret_cast<uintptr_t>(p1),
                                 reinterpret_cast<uintptr_t>(p2)));
}

// Iterator for HeapProfiler::allocation_ to make objects allocated from
// disabled code regions live.
static void MakeDisabledLiveCallback(void* ptr, HeapProfiler::AllocValue v) {
  bool stack_disable = false;
  bool range_disable = false;
  for (int depth = 0; depth < v.bucket->depth_; depth++) {
    uintptr_t addr = reinterpret_cast<uintptr_t>(v.bucket->stack_[depth]);
    if (disabled_addresses  &&
        disabled_addresses->find(addr) != disabled_addresses->end()) {
      stack_disable = true;  // found; dropping
      break;
    }
    if (disabled_ranges) {
      DisabledRangeMap::const_iterator iter
        = disabled_ranges->upper_bound(addr);
      if (iter != disabled_ranges->end()) {
        assert(iter->first > addr);
        if (iter->second.start_address < addr  &&
            iter->second.max_depth > depth) {
          range_disable = true;  // in range; dropping
          break;
        }
      }
    }
  }
  if (stack_disable || range_disable) {
    uintptr_t start_address = reinterpret_cast<uintptr_t>(ptr);
    uintptr_t end_address = start_address + v.bytes;
    StackRangeMap::const_iterator iter
      = stack_ranges->lower_bound(start_address);
    if (iter != stack_ranges->end()) {
      assert(iter->first >= start_address);
      if (iter->second <= end_address) {
        // We do not disable (treat as live) whole allocated regions
        // if they are used to hold thread call stacks
        // (i.e. when we find a stack inside).
        // The reason is that we'll treat as live the currently used
        // stack portions anyway (see RegisterStackRange),
        // and the rest of the region where the stack lives can well
        // contain outdated stack variables which are not live anymore,
        // hence should not be treated as such.
        HeapProfiler::MESSAGE(2, "HeapChecker: "
                                 "Not %s-disabling %"PRIuS" bytes at %p"
                                 ": have stack inside: %p-%p\n",
                                 (stack_disable ? "stack" : "range"),
                                 v.bytes, ptr,
                                 (void*)iter->first, (void*)iter->second);
        return;
      }
    }
    if (HeapProfiler::kMaxLogging) {
      HeapProfiler::MESSAGE(2, "HeapChecker: "
                               "%s-disabling %"PRIuS" bytes at %p\n",
                               (stack_disable ? "stack" : "range"),
                               v.bytes, ptr);
    }
    live_objects->push_back(AllocObject(ptr, v.bytes, MUST_BE_ON_HEAP));
  }
}

static int GetStatusOutput(const char*  command, string* output) {
  // We don't want the heapchecker to run in the child helper
  // processes that we fork() as part of this process' heap check.

  // setenv() can call realloc(), so we don't want to call it while
  // the heap profiling is disabled. Instead just overwrite the final
  // char of the env var name, so it has a different name and gets
  // ignored in the child.  We assume the env looks like 'VAR=VALUE\0VAR=VALUE'
  char *env_heapcheck = getenv("HEAPCHECK");
  char *env_ldpreload = getenv("LD_PRELOAD");

  if (env_heapcheck) {
    assert(env_heapcheck[-1] == '=');
    env_heapcheck[-2] = '?';
  }
  if (env_ldpreload) {
    assert(env_ldpreload[-1] == '=');
    env_ldpreload[-2] = '?';
  }

  FILE* f = popen(command, "r");
  if (f == NULL) {
    fprintf(stderr, "popen returned NULL!!!\n");  // This shouldn't happen
    exit(1);
  }

  if (env_heapcheck) env_heapcheck[-2] = 'K';
  if (env_ldpreload) env_heapcheck[-2] = 'D';

  const int kMaxOutputLine = 10000;
  char line[kMaxOutputLine];
  while (fgets(line, sizeof(line), f) != NULL) {
    if (output)
      *output += line;
  }

  return pclose(f);
}

// RAII class for a file descriptor.

class FileDescriptor {
  public:
   FileDescriptor(int fd) : fd_(fd) { ; }
   ~FileDescriptor() { if (fd_ >= 0) close(fd_); }
   int Close() { int fd = fd_; fd_ = -1; return close(fd); }
   operator int() { return fd_; }
  private:
   int fd_;
};

// This function takes the fields from a /proc/self/maps line:
//
//   start_address  start address of a memory region.
//   end_address    end address of a memory region
//   permissions    rwx + private/shared bit
//   file_offset    file offset within the mapped file
//   (major:minor)  major and minor device number of the mapped file
//   inode          inode number of the mapped file
//   filename       filename of the mapped file
//
// First, if the region is not writeable, then it cannot have any heap
// pointers in it.
//
// It would be simple to just mark every writeable memory region as live.
// However, that would pick up unused bottom pieces of thread stacks
// and other rot.  So we need more complexity:
//
// Second, if the region is anonymous, we ignore it.  That skips the
// main stack and the thread stacks which are picked up elsewhere.
// Unfortunately that also skips the BSS portions of shared library
// segments, and we need to pick those up.
//
// So, for each writable memory region that is mapped to a file,
// we recover the original segment information from that file
// (segment headers, not section headers).  We pick out the segment
// that contains the given "file_offset", figure out where that
// segment was loaded into memory, and register all of the memory
// addresses for that segment.
//
// Picking out the right segment is a bit of a mess because the
// same file offset can appear in multiple segments!  For example:
//
//   LOAD  0x000000 0x08048000 0x08048000 0x231a8 0x231a8 R E 0x1000
//   LOAD  0x0231a8 0x0806c1a8 0x0806c1a8 0x01360 0x014e8 RW  0x1000
//
// File offset 0x23000 appears in both segments.  The second segment
// stars at 0x23000 because of rounding.  Fortunately, we skip the
// first segment because it is not writeable.  Most of the shared
// objects we see have one read-executable segment and one read-write
// segment so we really skate.
//
// If a shared library has no initialized data, only BSS, then the
// size of the read-write LOAD segment will be zero, the dynamic loader
// will create an anonymous memory region for the BSS but no named
// segment [I think -- gotta check ld-linux.so -- mec].  We will
// overlook that segment and never get here.  This is a bug.

static bool RecordGlobalDataLocked(uint64 start_address,
                                   uint64 end_address,
                                   const char* permissions,
                                   uint64 file_offset,
                                   int64 inode,
                                   const char* filename) {
  // Ignore non-writeable regions.
  if (strchr(permissions, 'w') == NULL)
    return true;

  // Ignore anonymous regions.
  // This drops BSS regions which causes us much work later.
  if (inode == 0)
    return true;

  // Grab some ELF types.
#ifdef _LP64
  typedef Elf64_Ehdr ElfFileHeader;
  typedef Elf64_Phdr ElfProgramHeader;
#else
  typedef Elf32_Ehdr ElfFileHeader;
  typedef Elf32_Phdr ElfProgramHeader;
#endif

  // Cannot mmap the ELF file because of the live ProcMapsIterator.
  // Have to read little pieces.  Fortunately there are just two.
  HeapProfiler::MESSAGE(2, "HeapChecker: Looking into %s\n", filename);
  FileDescriptor fd_elf(open(filename, O_RDONLY));
  if (fd_elf < 0)
    return false;

  // Read and validate the file header.
  ElfFileHeader efh;
  if (read(fd_elf, &efh, sizeof(efh)) != sizeof(efh))
    return false;
  if (memcmp(&efh.e_ident[0], ELFMAG, SELFMAG) != 0)
    return false;
  if (efh.e_version != EV_CURRENT)
    return false;
  if (efh.e_type != ET_EXEC && efh.e_type != ET_DYN)
    return false;

  // Read the segment headers.
  if (efh.e_phentsize != sizeof(ElfProgramHeader))
    return false;
  if (lseek(fd_elf, efh.e_phoff, SEEK_SET) != efh.e_phoff)
    return false;
  const size_t phsize = efh.e_phnum * efh.e_phentsize;
  ElfProgramHeader* eph = new ElfProgramHeader[efh.e_phnum];
  if (read(fd_elf, eph, phsize) != phsize) {
    delete[] eph;
    return false;
  }

  // Gonna need this page size for page boundary considerations.
  // Better be a power of 2.
  const int int_page_size = getpagesize();
  if (int_page_size <= 0 || int_page_size & (int_page_size-1))
    abort();
  const uint64 page_size = int_page_size;

  // Walk the segment headers.
  // Find the segment header that contains the given file offset.
  bool found_load_segment = false;
  for (int iph = 0; iph < efh.e_phnum; ++iph) {
    HeapProfiler::MESSAGE(3, "HeapChecker: %s %d: p_type: %d p_flags: %x\n",
                          filename, iph, eph[iph].p_type, eph[iph].p_flags);
    if (eph[iph].p_type == PT_LOAD && eph[iph].p_flags & PF_W) {
      // Sanity check the segment header.
      if (eph[iph].p_vaddr != eph[iph].p_paddr) {
        delete[] eph;
        return false;
      }
      if ((eph[iph].p_vaddr  & (page_size-1)) !=
          (eph[iph].p_offset & (page_size-1))) {
        delete[] eph;
        return false;
      }

      // The segment is not aligned in the ELF file, but will be
      // aligned in memory.  So round it to page boundaries.
      // Note: we lose if p_end is on the last page.
      const uint64 p_start = eph[iph].p_offset &~ (page_size-1);
      const uint64 p_end = ((eph[iph].p_offset + eph[iph].p_memsz)
                         + (page_size-1)) &~ (page_size-1);
      if (p_end < p_start) {
        delete[] eph;
        return false;
      }
      if (file_offset >= p_start && file_offset < p_end) {
        // Found it.
        if (found_load_segment) {
          delete[] eph;
          return false;
        }
        found_load_segment = true;

        // [p_start, p_end) is the file segment, where p_end is extended
        // for BSS (which does not actually come from the file).
        //
        // [start_address, end_address) is the memory region from
        // /proc/self/maps.
        //
        // The point of correspondence is:
        //   file_offset in filespace <-> start_address in memoryspace
        //
        // This point of correspondence is reliable because the kernel
        // virtual memory system actually uses this information for
        // demand-paging the file.
        //
        // A single file segment can get loaded into several contiguous
        // memory regions with different permissions.  I have seen as
        // many as four regions: no-permission alignment region +
        // read-only-after-relocation region + read-write data region +
        // read-write anonymous bss region.  So file_offset from the
        // memory region may not equal to p_start from the file segement;
        // file_offset can be anywhere in [p_start, p_end).

        // Check that [start_address, end_address) is wholly contained
        // in [p_start, p_end).
        if (end_address < start_address ||
            end_address - start_address > p_end - file_offset) {
          delete[] eph;
          return false;
        }

        // Calculate corresponding address and length for [p_start, p_end).
        if (file_offset - p_start > start_address) {
          delete[] eph;
          return false;
        }
        void* addr = reinterpret_cast<void*>(start_address -
                                             (file_offset - p_start));
        const uintptr_t length = p_end - p_start;

        // That is what we need.
        (*library_live_objects)[filename].
          push_back(AllocObject(addr, length, IN_GLOBAL_DATA));
      }
    }
  }
  delete[] eph;

  if (!found_load_segment) {
    HeapProfiler::MESSAGE(-1,
      "HeapChecker: no LOAD segment found in %s\n",
      filename);
    return false;
  }

  if (fd_elf.Close() < 0)
    return false;

  return true;
}

// See if 'library' from /proc/self/maps has base name 'library_base'
// i.e. contains it and has '.' or '-' after it.
static bool IsLibraryNamed(const char* library, const char* library_base) {
  const char* p = strstr(library, library_base);
  size_t sz = strlen(library_base);
  return p != NULL  &&  (p[sz] == '.'  ||  p[sz] == '-');
}

void HeapLeakChecker::DisableLibraryAllocs(const char* library,
                                           void* start_address,
                                           void* end_address) {
  int depth = 0;
  // TODO(maxim): maybe this should be extended to also use objdump
  //              and pick the text portion of the library more precisely.
  if (IsLibraryNamed(library, "/libpthread")  ||
        // libpthread has a lot of small "system" leaks we don't care about.
        // In particular it allocates memory to store data supplied via
        // pthread_setspecific (which can be the only pointer to a heap object).
      IsLibraryNamed(library, "/libdl")  ||
        // library loaders leak some "system" heap that we don't care about
      IsLibraryNamed(library, "/libcrypto")
      // Sometimes libcrypto of OpenSSH is compiled with -fomit-frame-pointer
      // (any library can be, of course, but this one often is because speed
      // is so important for making crypto usable).  We ignore all its
      // allocations because we can't see the call stacks.
     ) {
    depth = 1;  // only disable allocation calls directly from the library code
  } else if (IsLibraryNamed(library, "/ld")
               // library loader leaks some "system" heap
               // (e.g. thread-local storage) that we don't care about
            ) {
    depth = 2;  // disable allocation calls directly from the library code
                // and at depth 2 from it.
    // We need depth 2 here solely because of a libc bug that
    // forces us to jump through __memalign_hook and MemalignOverride hoops
    // in tcmalloc.cc.
    // Those buggy __libc_memalign() calls are in ld-linux.so and happen for
    // thread-local storage allocations that we want to ignore here.
    // We go with the depth-2 hack as a workaround for this libc bug:
    // otherwise we'd need to extend MallocHook interface
    // so that correct stack depth adjustment can be propagated from
    // the exceptional case of MemalignOverride.
    // Using depth 2 here should not mask real leaks because ld-linux.so
    // does not call user code.
  }
  if (depth) {
    HeapProfiler::MESSAGE(1, "HeapChecker: "
                          "Disabling allocations from %s at depth %d:\n",
                          library, depth);
    DisableChecksFromTo(start_address, end_address,
                        depth);
  }
}

HeapLeakChecker::ProcMapsResult
HeapLeakChecker::UseProcMaps(ProcMapsTask proc_maps_task) {
  FILE* const fp = fopen("/proc/self/maps", "r");
  if (!fp) {
    int errsv = errno;
    HeapProfiler::MESSAGE(-1, "HeapChecker:  "
                          "Could not open /proc/self/maps: errno=%d.  "
                          "Libraries will not be handled correctly.\n",
                          errsv);
    return CANT_OPEN_PROC_MAPS;
  }
  char proc_map_line[1024];
  bool saw_shared_lib = false;
  while (fgets(proc_map_line, sizeof(proc_map_line), fp) != NULL) {
    // All lines starting like
    // "401dc000-4030f000 r??p 00132000 03:01 13991972  lib/bin"
    // identify a data and code sections of a shared library or our binary
    uint64 start_address, end_address, file_offset, inode;
    int size;
    char permissions[5], *filename;
    if (sscanf(proc_map_line, LLX"-"LLX" %4s "LLX" %*x:%*x "LLD" %n",
               &start_address, &end_address, permissions,
               &file_offset, &inode, &size) != 5) continue;
    proc_map_line[strlen(proc_map_line) - 1] = '\0';  // zap the newline
    filename = proc_map_line + size;
    HeapProfiler::MESSAGE(4, "HeapChecker: "
                          "Looking at /proc/self/maps line:\n  %s\n",
                          proc_map_line);

    if (start_address >= end_address)
      abort();

    // Determine if any shared libraries are present.
    if (inode != 0 && strstr(filename, "lib") && strstr(filename, ".so")) {
      saw_shared_lib = true;
    }

    if (proc_maps_task == DISABLE_LIBRARY_ALLOCS) {
      if (inode != 0 && strncmp(permissions, "r-xp", 4) == 0) {
        DisableLibraryAllocs(filename,
                             reinterpret_cast<void*>(start_address),
                             reinterpret_cast<void*>(end_address));
      }
    }

    if (proc_maps_task == RECORD_GLOBAL_DATA_LOCKED) {
      if (!RecordGlobalDataLocked(start_address, end_address, permissions,
                                  file_offset, inode, filename)) {
        HeapProfiler::MESSAGE(
          -1, "HeapChecker: failed RECORD_GLOBAL_DATA_LOCKED on %s\n",
          filename);
        abort();
      }
    }
  }
  fclose(fp);

  if (!saw_shared_lib) {
    HeapProfiler::MESSAGE(-1, "HeapChecker: "
                              "No shared libs detected.  "
                              "Will likely report false leak positives "
                              "for statically linked executables.\n");
    return NO_SHARED_LIBS_IN_PROC_MAPS;
  }
  return PROC_MAPS_USED;
}

// Total number and size of live objects dropped from the profile.
static int64 live_objects_total = 0;
static int64 live_bytes_total = 0;

// pid of the thread that is doing the current leak check
// (protected by our lock; IgnoreAllLiveObjectsLocked sets it)
static pid_t self_thread_pid = 0;

// Ideally to avoid deadlocks this function should not result in any libc
// or other function calls that might need to lock a mutex:
// It is called when all threads of a process are stopped
// at arbitrary points thus potentially holding those locks.
//
// In practice we are calling some simple i/o functions
// for logging messages and use the memory allocator in here.
// This is known to be buggy: It is able to cause deadlocks
// when we request a lock that a stopped thread happens to hold.
// But both of the above as far as we know have so far
// not resulted in any deadlocks in practice,
// so for now we are taking our change that the deadlocks
// have insignificant frequency.
//
// If such deadlocks become a problem we should make the i/o calls
// into appropriately direct system calls (or eliminate them),
// in particular write() is not safe and vsnprintf() is potentially dangerous
// due to reliance on locale functions
// (these are called through HeapProfiler::MESSAGE()).
//
// Eliminating the potential for deadlocks in
// (or the need itself for) the memory allocator is more involved.
// With some lock-all-allocator's-locks helper hooks into our allocator
// implementations we can avoid deadlocks in our allocator code itself,
// but the potential for deadlock in a libc functions the allocator uses
// is still there as long as there are such lock-using functions that
// can also be called on their own e.g. by third-party code.
// This is e.g. the case for __libc_malloc that debugallocation.cc uses.
// It might be a better idea to reorganize the data structures so that
// everything that happens within IgnoreLiveThreads does not need to
// allocate more memory.
//
int HeapLeakChecker::IgnoreLiveThreads(void* parameter,
                                       int num_threads,
                                       pid_t* thread_pids,
                                       va_list ap) {
  if (HeapProfiler::kMaxLogging) {
    HeapProfiler::MESSAGE(2, "HeapChecker: Found %d threads (from pid %d)\n",
                          num_threads, getpid());
  }

  // We put the registers from other threads here
  // to make pointers stored in them live.
  vector<void*> thread_registers;

  int failures = 0;
  for (int i = 0; i < num_threads; ++i) {
    // the leak checking thread itself is handled
    // specially via self_thread_stack, not here:
    if (thread_pids[i] == self_thread_pid) continue;
    if (HeapProfiler::kMaxLogging) {
      HeapProfiler::MESSAGE(2, "HeapChecker: Handling thread with pid %d\n",
                            thread_pids[i]);
    }
#if defined(HAVE_LINUX_PTRACE_H) && defined(HAVE_SYSCALL_H) && defined(DUMPER)
    i386_regs thread_regs;
#define sys_ptrace(r,p,a,d)  syscall(SYS_ptrace, (r), (p), (a), (d))
    // We use sys_ptrace to avoid thread locking
    // because this is called from ListAllProcessThreads
    // when all but this thread are suspended.
    if (sys_ptrace(PTRACE_GETREGS, thread_pids[i], NULL, &thread_regs) == 0) {
      void* stack_top;
      void* stack_bottom;
      if (GetStackExtent((void*) thread_regs.BP, &stack_top, &stack_bottom)) {
        // Need to use SP, not BP here to also get the data
        // from the very last stack frame:
        RegisterStackRange((void*) thread_regs.SP, stack_bottom);
      } else {
        failures += 1;
      }
      // Make registers live (just in case PTRACE_ATTACH resulted in some
      // register pointers still being in the registers and not on the stack):
      for (void** p = (void**)&thread_regs;
           p < (void**)(&thread_regs + 1); ++p) {
        if (HeapProfiler::kMaxLogging) {
          HeapProfiler::MESSAGE(3, "HeapChecker: Thread register %p\n", *p);
        }
        thread_registers.push_back(*p);
      }
    } else {
      failures += 1;
    }
#else
    failures += 1;
#endif
  }
  // Use all the collected thread (stack) liveness sources:
  IgnoreLiveObjectsLocked("threads stack data", "");
  if (thread_registers.size()) {
    // Make thread registers be live heap data sources.
    // we rely here on the fact that vector is in one memory chunk:
    live_objects->push_back(AllocObject(&thread_registers[0],
                                        thread_registers.size() * sizeof(void*),
                                        THREAD_REGISTERS));
    IgnoreLiveObjectsLocked("threads register data", "");
  }
  // Do all other liveness walking while all threads are stopped:
  IgnoreNonThreadLiveObjectsLocked();
  // Can now resume the threads:
  ResumeAllProcessThreads(num_threads, thread_pids);
  return failures;
}

// Info about the self thread stack extent
struct HeapLeakChecker::StackExtent {
  bool have;
  void* top;
  void* bottom;
};

// Stack info of the thread that is doing the current leak check
// (protected by our lock; IgnoreAllLiveObjectsLocked sets it)
static HeapLeakChecker::StackExtent self_thread_stack;

void HeapLeakChecker::IgnoreNonThreadLiveObjectsLocked() {
  // Register our own stack:
  if (HeapProfiler::kMaxLogging) {
    HeapProfiler::MESSAGE(2, "HeapChecker: Handling self thread with pid %d\n",
                          self_thread_pid);
  }
  if (self_thread_stack.have) {
    // important that all stack ranges
    // (including the one from the check initiator here)
    // are known before we start looking at them in MakeDisabledLiveCallback:
    RegisterStackRange(self_thread_stack.top, self_thread_stack.bottom);
    IgnoreLiveObjectsLocked("stack data", "");
  } else {
    HeapProfiler::MESSAGE(0, "HeapChecker: Stack not found "
                             "for this thread; may get false leak reports\n");
  }
  // Make objects we were told to ignore live:
  if (ignored_objects) {
    HeapProfiler::AllocValue alloc_value;
    for (IgnoredObjectsMap::const_iterator object = ignored_objects->begin();
         object != ignored_objects->end(); ++object) {
      void* ptr = reinterpret_cast<void*>(object->first);
      live_objects->
        push_back(AllocObject(ptr, object->second, MUST_BE_ON_HEAP));
      // we do this liveness check for ignored_objects before doing any
      // live heap walking to make sure it does not fail needlessly:
      bool have_on_heap =
        HeapProfiler::HaveOnHeapLocked(&ptr, &alloc_value);
      if (!(have_on_heap  &&  object->second == alloc_value.bytes)) {
        HeapProfiler::MESSAGE(-1, "HeapChecker: "
                              "%p of %"PRIuS" bytes "
                              "from an IgnoreObject() disappeared\n",
                              ptr, object->second);
        abort();
      }
    }
    IgnoreLiveObjectsLocked("ignored objects", "");
  }
  // Make code-address-disabled objects live and ignored:
  // This in particular makes all thread-specific data live
  // because the basic data structure to hold pointers to thread-specific data
  // is allocated from libpthreads and we have range-disabled that
  // library code with UseProcMaps(DISABLE_LIBRARY_ALLOCS);
  // so now we declare all thread-specific data reachable from there as live.
  HeapProfiler::allocation_->Iterate(MakeDisabledLiveCallback);
  IgnoreLiveObjectsLocked("disabled code", "");
  // Actually make global data live:
  if (FLAGS_heap_check_ignore_global_live) {
    for (LibraryLiveObjectsStacks::iterator l = library_live_objects->begin();
         l != library_live_objects->end(); ++l) {
      if (live_objects->size()) abort();
      live_objects->swap(l->second);
      IgnoreLiveObjectsLocked("in globals of\n  ", l->first.c_str());
    }
    delete library_live_objects;
  }
}

// For this call we are free to call new/delete from this thread:
// heap profiler will ignore them without acquiring its lock:
void HeapLeakChecker::
IgnoreAllLiveObjectsLocked(const StackExtent& self_stack) {
  if (live_objects)  abort();
  live_objects = new LiveObjectsStack;
  stack_ranges = new StackRangeMap;
  if (HeapProfiler::ignored_objects_)  abort();
  HeapProfiler::ignored_objects_ = new HeapProfiler::IgnoredObjectSet;
  // Record global data as live:
  // We need to do it before we stop the threads in ListAllProcessThreads
  // below; otherwise deadlocks are possible
  // when we try to fork to execute objdump in UseProcMaps.
  if (FLAGS_heap_check_ignore_global_live) {
    library_live_objects = new LibraryLiveObjectsStacks;
    UseProcMaps(RECORD_GLOBAL_DATA_LOCKED);
  }
  // Ignore all thread stacks:
  bool executed_with_threads_stopped = false;
  self_thread_pid = getpid();
  self_thread_stack = self_stack;
  if (FLAGS_heap_check_ignore_thread_live) {
    // We fully suspend the threads right here before any liveness checking
    // and keep them suspended for the whole time of liveness checking
    // inside of the IgnoreLiveThreads callback.
    // (The threads can't (de)allocate due to profiler's lock but
    //  if not suspended they could still mess with the pointer
    //  graph while we walk it).
    int r = ListAllProcessThreads(NULL, IgnoreLiveThreads);
    executed_with_threads_stopped = (r >= 0);
    if (r == -1) {
      HeapProfiler::MESSAGE(0, "HeapChecker: Could not find thread stacks; "
                               "may get false leak reports\n");
    } else if (r != 0) {
      HeapProfiler::MESSAGE(0, "HeapChecker: Thread stacks not found "
                               "for %d threads; may get false leak reports\n",
                            r);
    } else {
      if (HeapProfiler::kMaxLogging) {
        HeapProfiler::MESSAGE(2, "HeapChecker: Thread stacks appear"
                                 " to be found for all threads\n");
      }
    }
  } else {
    HeapProfiler::MESSAGE(0, "HeapChecker: Not looking for thread stacks; "
                             "objects reachable only from there "
                             "will be reported as leaks\n");
  }
  // Do all other live data ignoring here if we did not do it
  // within thread listing callback with all threads stopped.
  if (!executed_with_threads_stopped)  IgnoreNonThreadLiveObjectsLocked();
  if (live_objects_total) {
    HeapProfiler::MESSAGE(0, "HeapChecker: "
                          "Ignoring "LLD" reachable "
                          "objects of "LLD" bytes\n",
                          live_objects_total, live_bytes_total);
  }
  // Free these: we made them here and heap profiler never saw them
  delete live_objects;
  live_objects = NULL;
  delete stack_ranges;
  stack_ranges = NULL;
}

// This function does not change HeapProfiler's state:
// we record ignored live objects in HeapProfiler::ignored_objects_
// instead of modifying the heap profile.
void HeapLeakChecker::IgnoreLiveObjectsLocked(const char* name,
                                              const char* name2) {
  int64 live_object_count = 0;
  int64 live_byte_count = 0;
  while (!live_objects->empty()) {
    void* object = live_objects->back().ptr;
    size_t size = live_objects->back().size;
    const ObjectPlacement place = live_objects->back().place;
    live_objects->pop_back();
    HeapProfiler::AllocValue alloc_value;
    if (place == MUST_BE_ON_HEAP  &&
        HeapProfiler::HaveOnHeapLocked(&object, &alloc_value)  &&
        HeapProfiler::ignored_objects_
          ->insert(reinterpret_cast<uintptr_t>(object)).second) {
      live_object_count += 1;
      live_byte_count += size;
    }
    HeapProfiler::MESSAGE(5, "HeapChecker: "
                          "Looking for heap pointers "
                          "in %p of %"PRIuS" bytes\n", object, size);
    // Try interpretting any byte sequence in object,size as a heap pointer
    const size_t alignment = sizeof(void*);
      // alignment at which we should consider pointer positions here
      // use 1 if any alignment is ok
    const size_t remainder = reinterpret_cast<uintptr_t>(object) % alignment;
    if (remainder) {
      reinterpret_cast<char*&>(object) += alignment - remainder;
      if (size >= alignment - remainder) {
        size -= alignment - remainder;
      } else {
        size = 0;
      }
    }
    while (size >= sizeof(void*)) {
// TODO(jandrews): Make this part of the configure script.
#define UNALIGNED_LOAD32(_p) (*reinterpret_cast<const uint32 *>(_p))
      void* ptr = reinterpret_cast<void*>(UNALIGNED_LOAD32(object));
      void* current_object = object;
      reinterpret_cast<char*&>(object) += alignment;
      size -= alignment;
      if (ptr == NULL)  continue;
      HeapProfiler::MESSAGE(8, "HeapChecker: "
                            "Trying pointer to %p at %p\n",
                            ptr, current_object);
      // Do not need the following since the data for these
      // is not recorded by heap-profiler:
      // if (ptr == live_objects  ||
      //     ptr == HeapProfiler::ignored_objects_)  continue;
      if (HeapProfiler::HaveOnHeapLocked(&ptr, &alloc_value)  &&
          HeapProfiler::ignored_objects_
            ->insert(reinterpret_cast<uintptr_t>(ptr)).second) {
        // We take the (hopefully low) risk here of encountering by accident
        // a byte sequence in memory that matches an address of
        // a heap object which is in fact leaked.
        // I.e. in very rare and probably not repeatable/lasting cases
        // we might miss some real heap memory leaks.
        HeapProfiler::MESSAGE(5, "HeapChecker: "
                              "Found pointer to %p"
                              " of %"PRIuS" bytes at %p\n",
                              ptr, alloc_value.bytes, current_object);
        live_object_count += 1;
        live_byte_count += alloc_value.bytes;
        live_objects->push_back(AllocObject(ptr, alloc_value.bytes,
                                            IGNORED_ON_HEAP));
      }
    }
  }
  live_objects_total += live_object_count;
  live_bytes_total += live_byte_count;
  if (live_object_count) {
    HeapProfiler::MESSAGE(1, "HeapChecker: "
                          "Removed "LLD" live heap objects"
                          " of "LLD" bytes: %s%s\n",
                          live_object_count, live_byte_count, name, name2);
  }
}

//----------------------------------------------------------------------
// HeapLeakChecker leak check disabling components
//----------------------------------------------------------------------

void HeapLeakChecker::DisableChecksUp(int stack_frames) {
  if (!heap_checker_on) return;
  if (stack_frames < 1)  abort();
  void* stack[1];
  if (GetStackTrace(stack, 1, stack_frames+1) != 1)  abort();
  DisableChecksAt(stack[0]);
}

void HeapLeakChecker::DisableChecksAt(void* address) {
  if (!heap_checker_on) return;
  if (pthread_mutex_lock(&heap_checker_lock) != 0)  abort();
  DisableChecksAtLocked(address);
  if (pthread_mutex_unlock(&heap_checker_lock) != 0)  abort();
}

bool HeapLeakChecker::HaveDisabledChecksUp(int stack_frames) {
  if (!heap_checker_on) return false;
  if (stack_frames < 1)  abort();
  void* stack[1];
  if (GetStackTrace(stack, 1, stack_frames+1) != 1)  abort();
  return HaveDisabledChecksAt(stack[0]);
}

bool HeapLeakChecker::HaveDisabledChecksAt(void* address) {
  if (!heap_checker_on) return false;
  if (pthread_mutex_lock(&heap_checker_lock) != 0)  abort();
  bool result = disabled_addresses != NULL  &&
                disabled_addresses->
                  find(reinterpret_cast<uintptr_t>(address)) !=
                disabled_addresses->end();
  if (pthread_mutex_unlock(&heap_checker_lock) != 0)  abort();
  return result;
}

void HeapLeakChecker::DisableChecksIn(const char* pattern) {
  if (!heap_checker_on) return;
  if (pthread_mutex_lock(&heap_checker_lock) != 0)  abort();
  DisableChecksInLocked(pattern);
  if (pthread_mutex_unlock(&heap_checker_lock) != 0)  abort();
}

void* HeapLeakChecker::GetDisableChecksStart() {
  if (!heap_checker_on) return NULL;
  void* start_address;
  if (GetStackTrace(&start_address, 1, 1) != 1)  abort();
  return start_address;
}

void HeapLeakChecker::DisableChecksToHereFrom(void* start_address) {
  if (!heap_checker_on) return;
  void* end_address;
  if (GetStackTrace(&end_address, 1, 1) != 1)  abort();
  if (start_address > end_address)  swap(start_address, end_address);
  DisableChecksFromTo(start_address, end_address,
                      10000);  // practically no stack depth limit:
                               // heap profiler keeps much shorter stack traces
}

void HeapLeakChecker::IgnoreObject(void* ptr) {
  if (!heap_checker_on) return;
  if (pthread_mutex_lock(&heap_checker_lock) != 0)  abort();
  IgnoreObjectLocked(ptr);
  if (pthread_mutex_unlock(&heap_checker_lock) != 0)  abort();
}

void HeapLeakChecker::IgnoreObjectLocked(void* ptr) {
  HeapProfiler::AllocValue alloc_value;
  if (HeapProfiler::HaveOnHeap(&ptr, &alloc_value)) {
    HeapProfiler::MESSAGE(1, "HeapChecker: "
                          "Going to ignore live object "
                          "at %p of %"PRIuS" bytes\n",
                          ptr, alloc_value.bytes);
    if (ignored_objects == NULL)  {
      ignored_objects = new IgnoredObjectsMap;
      IgnoreObjectLocked(ignored_objects);
        // ignore self in case we are not ignoring global data
    }
    if (!ignored_objects->insert(make_pair(reinterpret_cast<uintptr_t>(ptr),
                                           alloc_value.bytes)).second) {
      HeapProfiler::MESSAGE(-1, "HeapChecker: "
                            "%p is already being ignored\n", ptr);
      abort();
    }
  }
}

void HeapLeakChecker::UnIgnoreObject(void* ptr) {
  if (!heap_checker_on) return;
  HeapProfiler::AllocValue alloc_value;
  if (pthread_mutex_lock(&heap_checker_lock) != 0)  abort();
  bool ok = HeapProfiler::HaveOnHeap(&ptr, &alloc_value);
  if (ok) {
    ok = false;
    if (ignored_objects) {
      IgnoredObjectsMap::iterator object =
        ignored_objects->find(reinterpret_cast<uintptr_t>(ptr));
      if (object != ignored_objects->end()  &&
          alloc_value.bytes == object->second) {
        ignored_objects->erase(object);
        ok = true;
        HeapProfiler::MESSAGE(1, "HeapChecker: "
                              "Now not going to ignore live object "
                              "at %p of %"PRIuS" bytes\n",
                              ptr, alloc_value.bytes);
      }
    }
  }
  if (pthread_mutex_unlock(&heap_checker_lock) != 0)  abort();
  if (!ok) {
    HeapProfiler::MESSAGE(-1, "HeapChecker: "
                          "%p has not been ignored\n", ptr);
    abort();
  }
}

//----------------------------------------------------------------------
// HeapLeakChecker non-static functions
//----------------------------------------------------------------------

void HeapLeakChecker::DumpProfileLocked(bool start,
                                        const StackExtent& self_stack) {
  assert(!HeapProfiler::dumping_);  // not called from dumping code
  HeapProfiler::MESSAGE(0, "HeapChecker: %s check \"%s\"\n",
                        (start ? "Starting" : "Ending"), name_);
  // Make the heap profile while letting our thread work with the heap
  // without profiling this activity into the regular heap profile,
  // while at the same time we hold the lock
  // and do not let other threads work with the heap:
  assert(!HeapProfiler::self_disable_);
  HeapProfiler::self_disabled_tid_ = pthread_self();
  // stop normal heap profiling in our thread:
  HeapProfiler::self_disable_ = true;
  { // scope
    IgnoreAllLiveObjectsLocked(self_stack);
    HeapProfiler::dump_for_leaks_ = true;
    string* file_name = new string(*profile_prefix + "." + name_ +
                                   (start ? "-beg.heap" : "-end.heap"));
    HeapProfiler::DumpLocked("leak check", file_name->c_str());
    delete file_name;  // want explicit control of the destruction point
    HeapProfiler::dump_for_leaks_ = false;
    delete HeapProfiler::ignored_objects_;
    HeapProfiler::ignored_objects_ = NULL;
  }
  // resume normal heap profiling in our thread:
  HeapProfiler::self_disable_ = false;
  // Check that we made no heap changes ourselves
  // while normal heap profiling was paused:
  int64 self_disabled_bytes = HeapProfiler::self_disabled_.alloc_size_ -
                              HeapProfiler::self_disabled_.free_size_;
  int64 self_disabled_allocs = HeapProfiler::self_disabled_.allocs_ -
                               HeapProfiler::self_disabled_.frees_;
  if (self_disabled_bytes != 0  ||  self_disabled_allocs != 0) {
    HeapProfiler::MESSAGE(-1, "HeapChecker: "
                          "internal HeapChecker leak of "LLD" objects "
                          "and/or "LLD" bytes\n",
                          self_disabled_allocs, self_disabled_bytes);
    abort();
  }
}

void HeapLeakChecker::Create(const char *name) {
  name_ = NULL;
  has_checked_ = false;
  char* n = new char[strlen(name) + 1];   // do this before we lock
  IgnoreObject(n);  // otherwise it might be treated as live due to our stack
  if (pthread_mutex_lock(&heap_checker_lock) != 0)  abort();
  // Heap activity in other threads is paused for this whole function.
  HeapProfiler::Lock();
  if (heap_checker_on) {
    assert(strchr(name, '/') == NULL);  // must be a simple name
    assert(name_ == NULL);  // so this is not a memory leak
    name_ = n;
    memcpy(name_, name, strlen(name) + 1);
    // get our stack range to make its proper portion live
    StackExtent self_stack;
    self_stack.have = GetStackExtent(NULL, &self_stack.top, &self_stack.bottom);
    DumpProfileLocked(true, self_stack);  // start
    start_inuse_bytes_ = static_cast<size_t>(HeapProfiler::profile_.alloc_size_ -
                                             HeapProfiler::profile_.free_size_);
    start_inuse_allocs_ = static_cast<size_t>(HeapProfiler::profile_.allocs_ -
                                              HeapProfiler::profile_.frees_);
    if (HeapProfiler::kMaxLogging) {
      HeapProfiler::MESSAGE(1, "HeapChecker: "
                               "Start check \"%s\" profile: "
                               "%"PRIuS"d bytes in %"PRIuS"d objects\n",
                               name_, start_inuse_bytes_, start_inuse_allocs_);
    }
  } else {
    HeapProfiler::MESSAGE(-1, "HeapChecker: "
                          "Heap checker is not active, "
                          "hence checker \"%s\" will do nothing!\n", name);
    HeapProfiler::MESSAGE(-1, "HeapChecker: "
                          "To activate set the HEAPCHECK environment "
                          "variable.\n");
  }
  HeapProfiler::Unlock();
  if (pthread_mutex_unlock(&heap_checker_lock) != 0)  abort();
  if (name_ == NULL) {
    UnIgnoreObject(n);
    delete[] n;  // must be done after we unlock
  }
}

HeapLeakChecker::HeapLeakChecker(const char *name) {
  assert(strcmp(name, "_main_") != 0);  // reserved
  Create(name);
}

DECLARE_int64(heap_profile_allocation_interval);
DECLARE_int64(heap_profile_inuse_interval);

// Save pid of main thread for using in naming dump files
int32 HeapLeakChecker::main_thread_pid_ = getpid();
// Directory in which to dump profiles
string* HeapLeakChecker::dump_directory_ = NULL;
#ifdef HAVE_PROGRAM_INVOCATION_NAME
extern char* program_invocation_name;
extern char* program_invocation_short_name;
static const char* invocation_name() { return program_invocation_short_name; }
static const char* invocation_path() { return program_invocation_name; }
#else
static const char* invocation_name() { return "heap_checker"; }
static const char* invocation_path() { return "heap_checker"; }  // I guess?
#endif

HeapLeakChecker::HeapLeakChecker() {
  Create("_main_");
}

ssize_t HeapLeakChecker::BytesLeaked() const {
  if (!has_checked_) {
    HeapProfiler::MESSAGE(-1, "HeapChecker: "
                          "*NoLeaks|SameHeap must execute before this call\n");
    abort();
  }
  return inuse_bytes_increase_;
}

ssize_t HeapLeakChecker::ObjectsLeaked() const {
  if (!has_checked_) {
    HeapProfiler::MESSAGE(-1, "HeapChecker: "
                          "*NoLeaks|SameHeap must execute before this call\n");
    abort();
  }
  return inuse_allocs_increase_;
}

bool HeapLeakChecker::DoNoLeaks(bool same_heap,
                                bool do_full,
                                bool do_report) {
  // Heap activity in other threads is paused for this function
  // until we got all profile difference info.
  if (pthread_mutex_lock(&heap_checker_lock) != 0)  abort();
  HeapProfiler::Lock();
  if (heap_checker_on) {
    if (name_ == NULL) {
      HeapProfiler::MESSAGE(-1, "HeapChecker: "
                            "*NoLeaks|SameHeap must be called only once"
                            " and profiling must be not turned on "
                            "after construction of a HeapLeakChecker\n");
      abort();
    }
    // get our stack range to make its proper portion live
    StackExtent self_stack;
    self_stack.have = GetStackExtent(NULL, &self_stack.top, &self_stack.bottom);
    DumpProfileLocked(false, self_stack);  // end
    const bool use_initial_profile =
      !(FLAGS_heap_check_before_constructors  &&  this == main_heap_checker);
    if (!use_initial_profile) {  // compare against empty initial profile
      start_inuse_bytes_ = 0;
      start_inuse_allocs_ = 0;
    }
    int64 end_inuse_bytes = HeapProfiler::profile_.alloc_size_ -
                            HeapProfiler::profile_.free_size_;
    int64 end_inuse_allocs = HeapProfiler::profile_.allocs_ -
                             HeapProfiler::profile_.frees_;
    if (HeapProfiler::kMaxLogging) {
      HeapProfiler::MESSAGE(1, "HeapChecker: "
                               "End check \"%s\" profile: "
                               ""LLD" bytes in "LLD" objects\n",
                               name_, end_inuse_bytes, end_inuse_allocs);
    }
    inuse_bytes_increase_ = (ssize_t)(end_inuse_bytes - start_inuse_bytes_);
    inuse_allocs_increase_ = (ssize_t)(end_inuse_allocs - start_inuse_allocs_);
    has_checked_ = true;
    HeapProfiler::Unlock();
    if (pthread_mutex_unlock(&heap_checker_lock) != 0)  abort();
    bool see_leaks =
      (same_heap ? (inuse_bytes_increase_ != 0 || inuse_allocs_increase_ != 0)
                 : (inuse_bytes_increase_ > 0 || inuse_allocs_increase_ > 0));
    if (see_leaks || do_full) {
      const char* gv_command_tail
        = " --edgefraction=1e-10 --nodefraction=1e-10 --gv 2>/dev/null";
      string ignore_re;
      if (disabled_regexp) {
        ignore_re += " --ignore='^";
        ignore_re += *disabled_regexp;
        ignore_re += "$'";
      }
      // It would be easier to use a string here than a static buffer, but
      // some STLs can give us spurious leak alerts (since the STL tries to
      // do its own memory pooling), so we avoid it by using STL as little
      // as possible for "big" objects that might require "lots" of memory.
      char command[6 * PATH_MAX + 200];
      if (use_initial_profile) {
        // compare against initial profile only if need to
        const char* drop_negative = same_heap ? "" : " --drop_negative";
        snprintf(command, sizeof(command), "%s --base=\"%s.%s-beg.heap\" %s ",
                 pprof_path(), profile_prefix->c_str(), name_,
                 drop_negative);
      } else {
        snprintf(command, sizeof(command), "%s",
                 pprof_path());
      }
      snprintf(command + strlen(command), sizeof(command) - strlen(command),
               " %s \"%s.%s-end.heap\" %s --inuse_objects --lines",
               invocation_path(), profile_prefix->c_str(),
               name_, ignore_re.c_str());
                   // --lines is important here to catch leaks when !see_leaks
      char cwd[PATH_MAX+1];
      if (getcwd(cwd, sizeof(cwd)) != cwd)  abort();
      if (see_leaks) {
        HeapProfiler::MESSAGE(-1, "HeapChecker: "
                              "Heap memory leaks of "LLD" bytes and/or "
                              ""LLD" allocations detected by check \"%s\".\n\n",
                              (int64)inuse_bytes_increase_,
                              (int64)inuse_allocs_increase_,
                              name_);
        HeapProfiler::MESSAGE(-1, "HeapChecker: "			      
                              "To investigate leaks manually use e.g.\n"
                              "cd %s; "  // for proper symbol resolution
                              "%s%s\n\n",
                              cwd, command, gv_command_tail);
      }
      string output;
      int checked_leaks = 0;
      if ((see_leaks && do_report) || do_full) {
        if (access(pprof_path(), X_OK|R_OK) != 0) {
          HeapProfiler::MESSAGE(-1, "HeapChecker: "
                                "WARNING: Skipping pprof check:"
                                " could not run it at %s\n",
                                pprof_path());
        } else {
          // We don't care about pprof's stderr as long as it
          // succeeds with empty report:
          checked_leaks = GetStatusOutput(command, &output);
          if (checked_leaks != 0) {
            HeapProfiler::MESSAGE(-1, "ERROR: Could not run pprof at %s\n",
                                  pprof_path());
            abort();
          }
        }
        if (see_leaks && output.empty() && checked_leaks == 0) {
          HeapProfiler::MESSAGE(-1, "HeapChecker: "
                                "These must be leaks that we disabled"
                                " (pprof succeeded)! This check WILL FAIL"
                                " if the binary is strip'ped!\n");
          see_leaks = false;
        }
        // do not fail the check just due to us being a stripped binary
        if (!see_leaks  &&  strstr(output.c_str(), "nm: ") != NULL  &&
            strstr(output.c_str(), ": no symbols") != NULL)  output.resize(0);
        if (!(see_leaks || checked_leaks == 0))  abort();
      }
      if (see_leaks  &&  use_initial_profile) {
        HeapProfiler::MESSAGE(-1, "HeapChecker: "
                              "CAVEAT: Some of the reported leaks might have "
                              "occurred before check \"%s\" was started!\n",
                              name_);
      }
      bool tricky_leaks = !output.empty();
      if (!see_leaks && tricky_leaks) {
        HeapProfiler::MESSAGE(-1, "HeapChecker: "
                              "Tricky heap memory leaks of"
                              " no bytes and no allocations "
                              "detected by check \"%s\".\n", name_);
        HeapProfiler::MESSAGE(-1, "HeapChecker: "
                              "To investigate leaks manually uge e.g.\n"
                              "cd %s; "  // for proper symbol resolution
                              "%s%s\n\n",
                              name_, cwd, command, gv_command_tail);
        if (use_initial_profile) {
          HeapProfiler::MESSAGE(-1, "HeapChecker: "
                                "CAVEAT: Some of the reported leaks might have "
                                "occurred before check \"%s\" was started!\n",
                                name_);
        }
        see_leaks = true;
      }
      if (see_leaks && do_report) {
        if (checked_leaks == 0) {
          HeapProfiler::MESSAGE(-1, "HeapChecker: "
                                "Below is this pprof's output:\n\n");
          write(STDERR_FILENO, output.data(), output.size());
          HeapProfiler::MESSAGE(-1, "\n\n");
        } else {
          HeapProfiler::MESSAGE(-1, "HeapChecker: "
                                "pprof has failed\n\n");
        }
      }
    } else {
      HeapProfiler::MESSAGE(0, "HeapChecker: No leaks found\n");
    }
    UnIgnoreObject(name_);
    delete [] name_;
    name_ = NULL;
    return !see_leaks;
  } else {
    if (name_ != NULL) {
      HeapProfiler::MESSAGE(-1, "HeapChecker: "
                            "Profiling must stay enabled "
                            "during leak checking\n");
      abort();
    }
    HeapProfiler::Unlock();
    if (pthread_mutex_unlock(&heap_checker_lock) != 0)  abort();
    return true;
  }
}

HeapLeakChecker::~HeapLeakChecker() {
  if (name_ != NULL) {
    HeapProfiler::MESSAGE(-1, "HeapChecker: "
                          "Some *NoLeaks|SameHeap method"
                          " must be called on the checker\n");
    abort();
  }
}

//----------------------------------------------------------------------
// HeapLeakChecker overall heap check components
//----------------------------------------------------------------------

vector<HeapCleaner::void_function>* HeapCleaner::heap_cleanups_ = NULL;

// When a HeapCleaner object is intialized, add its function to the static list
// of cleaners to be run before leaks checking.
HeapCleaner::HeapCleaner(void_function f) {
  if (heap_cleanups_ == NULL)
    heap_cleanups_ = new vector<HeapCleaner::void_function>;
  heap_cleanups_->push_back(f);
}

// Run all of the cleanup functions and delete the vector.
void HeapCleaner::RunHeapCleanups() {
  if (!heap_cleanups_)
    return;
  for (int i = 0; i < heap_cleanups_->size(); i++) {
    void (*f)(void) = (*heap_cleanups_)[i];
    f();
  }
  delete heap_cleanups_;
  heap_cleanups_ = NULL;
}

// Program exit heap cleanup registered with atexit().
// Will not get executed when we crash on a signal.
void HeapLeakChecker::RunHeapCleanups() {
  if (heap_checker_pid == getpid()) {  // can get here (via forks?)
                                       // with other pids
    HeapCleaner::RunHeapCleanups();
    if (!FLAGS_heap_check_after_destructors) {
      DoMainHeapCheck();
    }
  }
}

// Called from main() immediately after setting any requisite parameters
// from HeapChecker and HeapProfiler.
void HeapLeakChecker::InternalInitStart(const string& heap_check_type) {
  if (heap_check_type.empty()) {
    heap_checker_on = false;
  } else {
    if (main_heap_checker) {
      // means this method was called twice.  We'll just ignore the 2nd call
      return;
    }
    if (!constructor_heap_profiling) {
      HeapProfiler::MESSAGE(-1, "HeapChecker: Can not start so late. "
                            "You have to enable heap checking by\n"
                            "setting the environment variable HEAPCHECK.\n");
      abort();
    }
    // Set all flags
    if (heap_check_type == "minimal") {
      // The least we can check.
      FLAGS_heap_check_before_constructors = false;  // (ignore more)
      FLAGS_heap_check_after_destructors = false;  // to after cleanup
                                                   // (most data is live)
      FLAGS_heap_check_strict_check = false;  // < profile check (ignore more)
      FLAGS_heap_check_ignore_thread_live = true;  // ignore all live
      FLAGS_heap_check_ignore_global_live = true;  // ignore all live
    } else if (heap_check_type == "normal") {
      // Faster than 'minimal' and not much stricter.
      FLAGS_heap_check_before_constructors = true;  // from no profile (fast)
      FLAGS_heap_check_after_destructors = false;  // to after cleanup
                                                   // (most data is live)
      FLAGS_heap_check_strict_check = true;  // == profile check (fast)
      FLAGS_heap_check_ignore_thread_live = true;  // ignore all live
      FLAGS_heap_check_ignore_global_live = true;  // ignore all live
    } else if (heap_check_type == "strict") {
      // A bit stricter than 'normal': global destructors must fully clean up
      // after themselves if they are present.
      FLAGS_heap_check_before_constructors = true;  // from no profile (fast)
      FLAGS_heap_check_after_destructors = true;  // to after destructors
                                                  // (less data live)
      FLAGS_heap_check_strict_check = true;  // == profile check (fast)
      FLAGS_heap_check_ignore_thread_live = true;  // ignore all live
      FLAGS_heap_check_ignore_global_live = true;  // ignore all live
    } else if (heap_check_type == "draconian") {
      // Drop not very portable and not very exact live heap flooding.
      FLAGS_heap_check_before_constructors = true;  // from no profile (fast)
      FLAGS_heap_check_after_destructors = true;  // to after destructors
                                                  // (need them)
      FLAGS_heap_check_strict_check = true;  // == profile check (fast)
      FLAGS_heap_check_ignore_thread_live = false;  // no live flood (stricter)
      FLAGS_heap_check_ignore_global_live = false;  // no live flood (stricter)
    } else if (heap_check_type == "as-is") {
      // do nothing: use other flags as is
    } else if (heap_check_type == "local") {
      // do nothing
    } else {
      LogPrintf(FATAL, "Unsupported heap_check flag: %s",
                heap_check_type.c_str());
    }
    assert(heap_checker_pid == getpid());
    heap_checker_on = true;
    if (!HeapProfiler::is_on_)  abort();
    ProcMapsResult pm_result = UseProcMaps(DISABLE_LIBRARY_ALLOCS);
      // might neeed to do this more than once
      // if one later dynamically loads libraries that we want disabled
    if (pm_result != HeapLeakChecker::PROC_MAPS_USED) {
      heap_checker_on = false;
      HeapProfiler::MESSAGE(0, "HeapChecker: Turning itself off\n");
      HeapProfiler::StopForLeaks();
      return;
    }

    // make a good place and name for heap profile leak dumps
    profile_prefix = new string(dump_directory());
    *profile_prefix += "/";
    *profile_prefix += invocation_name();
    HeapProfiler::CleanupProfiles(profile_prefix->c_str());

    // Finalize prefix for dumping leak checking profiles.
    char pid_buf[15];
    if (main_thread_pid_ == 0)  // possible if we're called before constructors
      main_thread_pid_ = getpid();
    snprintf(pid_buf, sizeof(pid_buf), ".%d", main_thread_pid_);
    *profile_prefix += pid_buf;
    assert(HeapProfiler::need_for_leaks_);

    // Make sure new/delete hooks are installed properly
    // and heap profiler is indeed able to keep track
    // of the objects being allocated.
    // We test this to make sure we are indeed checking for leaks.
    HeapProfiler::AllocValue alloc_value;
    char* test_str = new char[5];
    void* ptr = test_str;
    if (!HeapProfiler::HaveOnHeap(&ptr, &alloc_value))  abort();
    ptr = test_str;
    delete [] test_str;
    if (HeapProfiler::HaveOnHeap(&ptr, &alloc_value))  abort();
    // If we crash in the above code, it probably means that
    // "nm <this_binary> | grep new" will show that tcmalloc's new/delete
    // implementation did not get linked-in into this binary
    // (i.e. nm will list __builtin_new and __builtin_vec_new as undefined).
    // This is probably impossible.

    if (heap_check_type != "local") {
      // Schedule registered heap cleanup
      atexit(RunHeapCleanups);
      assert(main_heap_checker == NULL);
      main_heap_checker = new HeapLeakChecker();
    }
  }
  if (!heap_checker_on  &&  constructor_heap_profiling) {
    // turns out do not need checking in the end; can stop profiling
    HeapProfiler::MESSAGE(0, "HeapChecker: Turning itself off\n");
    HeapProfiler::StopForLeaks();
  }
}

void HeapLeakChecker::DoMainHeapCheck() {
  assert(heap_checker_pid == getpid());
  if (main_heap_checker) {
    bool same_heap = FLAGS_heap_check_strict_check;
    if (FLAGS_heap_check_before_constructors)  same_heap = true;
      // false here just would make it slower in this case
      // (we don't use the starting profile anyway)
    bool do_full = !same_heap;  // do it if it can help ignore false leaks
    bool do_report = FLAGS_heap_check_report;
    HeapProfiler::MESSAGE(0, "HeapChecker: "
                             "Checking for whole-program memory leaks\n");
    if (!main_heap_checker->DoNoLeaks(same_heap, do_full, do_report)) {
      HeapProfiler::MESSAGE(-1, "ERROR: Leaks found in main heap check, aborting\n");
      abort();
    }
    delete main_heap_checker;
    main_heap_checker = NULL;
  }
}

//----------------------------------------------------------------------
// HeapLeakChecker global constructor/destructor ordering components
//----------------------------------------------------------------------

// This is a workaround for nptl threads library in glibc
// (that is e.g. commonly used for 2.6 linux kernel-based distributons).
// nptl has an optimization for allocating thread-specific data,
// that we have to work around here:
// It preallocates (as specific_1stblock) the first second-level block
// of PTHREAD_KEY_2NDLEVEL_SIZE pointers to thread-specific data
// inside of the thread descriptor data structure itself.
// Since we sometimes can't proclaim this first block as live data,
// the workaround here simply uses up that block 
// (by allocating a bunch of pthread-specific keys and forgeting about them)
// as to force allocation of new second-level thread-specific data pointer
// blocks, which would be know to heap profiler/checker for sure
// since it's active by now.
static inline void PThreadSpecificHack() {
  // the value 32 corresponds to PTHREAD_KEY_2NDLEVEL_SIZE
  // in glibc's pthread implementation
  const int kKeyBlock = 32;
  for (int i = 0; i < kKeyBlock; ++i) {
    pthread_key_t key;
    if (perftools_pthread_key_create(&key, NULL) != 0)  abort();
  }
}

void HeapLeakChecker::BeforeConstructors() {
  // The user indicates a desire for heap-checking via the HEAPCHECK
  // environment variable.  If it's not set, there's no way to do
  // heap-checking.
  if (!getenv("HEAPCHECK")) {
    return;
  }

  // heap-checker writes out files.  Thus, for security reasons, we don't
  // recognize the env. var. to turn on heap-checking if we're setuid.
  if (getuid() != geteuid()) {
    HeapProfiler::MESSAGE(0, ("HeapChecker: ignoring HEAPCHECK because "
                              "program seems to be setuid\n"));
    return;
  }

  if (constructor_heap_profiling)  abort();
  constructor_heap_profiling = true;
  HeapProfiler::Init();  // only necessary if our constructor runs before theirs
  // If the user has HEAPPROFILE set, Init() will have turned on profiling.
  // If not, we need to do it manually here.
  HeapProfiler::StartForLeaks();
  heap_checker_on = true;
  PThreadSpecificHack();


  // The value of HEAPCHECK is the mode they want.  If we don't
  // recognize it, we default to "normal".
  const char* heap_check_type = getenv("HEAPCHECK");
  assert(heap_check_type);  // we checked that in the if above
  if ( heap_check_type[0] == '\0') {
    // don't turn on heap checking for missing or empty env. var.
  } else if ( !strcmp(heap_check_type, "minimal") ||
              !strcmp(heap_check_type, "normal") ||
              !strcmp(heap_check_type, "strict") ||
              !strcmp(heap_check_type, "draconian") ||
              !strcmp(heap_check_type, "local") ) {
    HeapLeakChecker::InternalInitStart(heap_check_type);
  } else {
    HeapLeakChecker::InternalInitStart("normal");         // the default
  }
}

extern bool heap_leak_checker_bcad_variable;  // in heap-checker-bcad.cc

// Whenever the heap checker library is linked in, this should be called before
// all global object constructors run.  This can be tricky and depends on
// heap-checker-bcad.o being the last file linked in.
void HeapLeakChecker_BeforeConstructors() {
  heap_checker_pid = getpid();  // set it always
  // just to reference it, so that heap-checker-bcad.o is linked in
  heap_leak_checker_bcad_variable = true;
  HeapLeakChecker::BeforeConstructors();
}

// This function is executed after all global object destructors run.
void HeapLeakChecker_AfterDestructors() {
  if (heap_checker_pid == getpid()) {  // can get here (via forks?)
                                       // with other pids
    if (FLAGS_heap_check_after_destructors && main_heap_checker) {
      HeapLeakChecker::DoMainHeapCheck();
      poll(NULL, 0, 500);
      // Need this hack to wait for other pthreads to exit.
      // Otherwise tcmalloc finds errors on a free() call from pthreads.
    }
  }
}

//----------------------------------------------------------------------
// HeapLeakChecker disabling helpers
//----------------------------------------------------------------------

// These functions are at the end of the file to prevent their inlining:

void HeapLeakChecker::DisableChecksInLocked(const char* pattern) {
  // make disabled_regexp
  if (disabled_regexp == NULL) {
    disabled_regexp = new string;
    IgnoreObjectLocked(disabled_regexp);
      // in case we are not ignoring global data
  }
  HeapProfiler::MESSAGE(1, "HeapChecker: "
                        "Disabling leaks checking in stack traces "
                        "under frames maching \"%s\"\n", pattern);
  if (disabled_regexp->size())  *disabled_regexp += '|';
  *disabled_regexp += pattern;
}

void HeapLeakChecker::DisableChecksFromTo(void* start_address,
                                          void* end_address,
                                          int max_depth) {
  assert(start_address < end_address);
  if (pthread_mutex_lock(&heap_checker_lock) != 0)  abort();
  if (disabled_ranges == NULL) {
    disabled_ranges = new DisabledRangeMap;
    IgnoreObjectLocked(disabled_ranges);
      // in case we are not ignoring global data
  }
  RangeValue value;
  value.start_address = reinterpret_cast<uintptr_t>(start_address);
  value.max_depth = max_depth;
  if (disabled_ranges->
        insert(make_pair(reinterpret_cast<uintptr_t>(end_address),
                         value)).second) {
    HeapProfiler::MESSAGE(1, "HeapChecker: "
                          "Disabling leaks checking in stack traces "
                          "under frame addresses between %p..%p\n",
                          start_address, end_address);
  }
  if (pthread_mutex_unlock(&heap_checker_lock) != 0)  abort();
}

void HeapLeakChecker::DisableChecksAtLocked(void* address) {
  if (disabled_addresses == NULL) {
    disabled_addresses = new DisabledAddressSet;
    IgnoreObjectLocked(disabled_addresses);
      // in case we are not ignoring global data
  }
  // disable the requested address
  if (disabled_addresses->insert(reinterpret_cast<uintptr_t>(address)).second) {
    HeapProfiler::MESSAGE(1, "HeapChecker: "
                          "Disabling leaks checking in stack traces "
                          "under frame address %p\n",
                          address);
  }
}
