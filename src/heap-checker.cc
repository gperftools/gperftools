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

#include "config.h"

#include <fcntl.h>    // for O_RDONLY (we use syscall to do actual reads)
#include <string.h>
#include <errno.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_MMAP
#include <sys/mman.h>
#endif
#include <sys/poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <assert.h>

#ifdef HAVE_LINUX_PTRACE_H
#include <linux/ptrace.h>
#endif
#ifdef HAVE_SYS_SYSCALL_H
#include <sys/syscall.h>
#endif

#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>

#include <google/heap-checker.h>

#include "base/basictypes.h"
#include "base/googleinit.h"
#include "base/logging.h"
#include <google/stacktrace.h>
#include "base/commandlineflags.h"
#include "base/elfcore.h"              // for i386_regs
#include "base/thread_lister.h"
#include "heap-profile-table.h"
#include "base/low_level_alloc.h"
#include <google/malloc_hook.h>
#include <google/malloc_extension.h>
#include "memory_region_map.h"
#include "base/spinlock.h"
#include "base/sysinfo.h"
#include "base/stl_allocator.h"

using std::string;
using std::basic_string;
using std::pair;
using std::map;
using std::set;
using std::vector;
using std::swap;
using std::make_pair;
using std::min;
using std::max;
using std::less;
using std::char_traits;

//----------------------------------------------------------------------
// Flags that control heap-checking
//----------------------------------------------------------------------

DEFINE_string(heap_check,
              EnvToString("HEAPCHECK", ""),
              "The heap leak checking to be done over the whole executable: "
              "\"minimal\", \"normal\", \"strict\", "
              "\"draconian\", \"as-is\", and \"local\" "
              " or the empty string are the supported choices. "
              "(See HeapLeakChecker::InternalInitStart for details.)");

DEFINE_bool(heap_check_report,
            EnvToBool("HEAP_CHECK_REPORT", true),
            "If overall heap check should report the found leaks via pprof");

DEFINE_bool(heap_check_before_constructors,
            true,
            "deprecated; pretty much always true now");

DEFINE_bool(heap_check_after_destructors,
            EnvToBool("HEAP_CHECK_AFTER_DESTRUCTORS", false),
            "If overall heap check is to end after global destructors "
            "or right after all REGISTER_HEAPCHECK_CLEANUP's");

DEFINE_bool(heap_check_strict_check,
            EnvToBool("HEAP_CHECK_STRICT_CHECK", true),
            "If overall heap check is to be done "
            "via HeapLeakChecker::*SameHeap "
            "or HeapLeakChecker::*NoLeaks call");
            // heap_check_strict_check == false
            // is useful only when heap_check_before_constructors == false

DEFINE_bool(heap_check_ignore_global_live,
            EnvToBool("HEAP_CHECK_IGNORE_GLOBAL_LIVE", true),
            "If overall heap check is to ignore heap objects reachable "
            "from the global data");

DEFINE_bool(heap_check_identify_leaks,
            EnvToBool("HEAP_CHECK_IDENTIFY_LEAKS", false),
            "If heap check should generate the addresses of the leaked objects "
            "in the memory leak profiles");

DEFINE_bool(heap_check_ignore_thread_live,
            EnvToBool("HEAP_CHECK_IGNORE_THREAD_LIVE", true),
            "If set to true, objects reachable from thread stacks "
            "and registers are not reported as leaks");

DEFINE_bool(heap_check_test_pointer_alignment,
            EnvToBool("HEAP_CHECK_TEST_POINTER_ALIGNMENT", false),
            "Set to true to check if the found leak can be due to "
            "use of unaligned pointers");

DEFINE_bool(heap_check_run_under_gdb,
            EnvToBool("HEAP_CHECK_RUN_UNDER_GDB", false),
            "If false, turns off heap-checking library when running under gdb "
            "(normally, set to 'true' only when debugging the heap-checker)");

//----------------------------------------------------------------------

DEFINE_string(heap_profile_pprof,
              EnvToString("PPROF_PATH", "pprof"),
              "Path to pprof to call for full leak checking.");

DEFINE_string(heap_check_dump_directory,
              EnvToString("HEAP_CHECK_DUMP_DIRECTORY", "/tmp"),
              "Directory to put heap-checker leak dump information");

// Copy of FLAGS_heap_profile_pprof.
// Need this since DoNoLeaks can happen
// after FLAGS_heap_profile_pprof is destroyed.
static string* flags_heap_profile_pprof = &FLAGS_heap_profile_pprof;

//----------------------------------------------------------------------
// HeapLeakChecker global data
//----------------------------------------------------------------------

// Global lock for (most of) the global data of this module.
// We could use pthread's lock here, but spinlock is faster.
static SpinLock heap_checker_lock(SpinLock::LINKER_INITIALIZED);

//----------------------------------------------------------------------

// Heap profile prefix for leak checking profiles
static string* profile_prefix = NULL;

// Whole-program heap leak checker
static HeapLeakChecker* main_heap_checker = NULL;
// Whether we will use main_heap_checker to do a check at program exit
static bool do_main_heap_check = false;

// The heap profile we use to collect info about the heap.
static HeapProfileTable* heap_profile = NULL;

// If we are doing (or going to do) any kind of heap-checking.
static bool heap_checker_on = false;
// pid of the process that does whole-program heap leak checking
static pid_t heap_checker_pid = 0;

// If we did heap profiling during global constructors execution
static bool constructor_heap_profiling = false;

//----------------------------------------------------------------------
// HeapLeakChecker's own memory allocator that is
// independent of the normal program allocator.
//----------------------------------------------------------------------

// Wrapper of LowLevelAlloc for STL_Allocator and direct use.
// We always access Allocate/Free in this class under held heap_checker_lock,
// this allows us to protect the period when threads are stopped
// at random spots with ListAllProcessThreads by heap_checker_lock,
// w/o worrying about the lock in LowLevelAlloc::Arena.
// We rely on the fact that we use an own arena with an own lock here.
class HeapLeakChecker::Allocator {
 public:
  static void Init() {
    RAW_DCHECK(arena_ == NULL, "");
    arena_ = LowLevelAlloc::NewArena(0, LowLevelAlloc::DefaultArena());
  }
  static void Shutdown() {
    if (!LowLevelAlloc::DeleteArena(arena_)  ||  alloc_count_ != 0) {
      RAW_LOG(FATAL, "Internal heap checker leak of %d objects", alloc_count_);
    }
  }
  static int alloc_count() { return alloc_count_; }
  static void* Allocate(size_t n) {
    RAW_DCHECK(arena_  &&  heap_checker_lock.IsHeld(), "");
    void* p = LowLevelAlloc::AllocWithArena(n, arena_);
    if (p) alloc_count_ += 1;
    return p;
  }
  static void Free(void* p) {
    RAW_DCHECK(heap_checker_lock.IsHeld(), "");
    if (p) alloc_count_ -= 1;
    LowLevelAlloc::Free(p);
  }
  // destruct, free, and make *p to be NULL
  template<typename T> static void DeleteAndNull(T** p) {
    (*p)->~T();
    Free(*p);
    *p = NULL;
  }
  template<typename T> static void DeleteAndNullIfNot(T** p) {
    if (*p != NULL) DeleteAndNull(p);
  }
 private:
  static LowLevelAlloc::Arena* arena_;
  static int alloc_count_;
};

LowLevelAlloc::Arena* HeapLeakChecker::Allocator::arena_ = NULL;
int HeapLeakChecker::Allocator::alloc_count_ = 0;

//----------------------------------------------------------------------
// HeapLeakChecker live object tracking components
//----------------------------------------------------------------------

// Cases of live object placement we distinguish
enum ObjectPlacement {
  MUST_BE_ON_HEAP,  // Must point to a live object of the matching size in the
                    // heap_profile map of the heap when we get to it
  IGNORED_ON_HEAP,  // Is a live (ignored) object on heap
  MAYBE_LIVE,       // Is simply a piece of writable memory from /proc/self/maps
  IN_GLOBAL_DATA,   // Is part of global data region of the executable
  THREAD_DATA,      // Part of a thread stack (and a thread descriptor with TLS)
  THREAD_REGISTERS, // Values in registers of some thread
};

// Information about an allocated object
struct AllocObject {
  const void* ptr;        // the object
  uintptr_t size;         // its size
  ObjectPlacement place;  // where ptr points to

  AllocObject(const void* p, size_t s, ObjectPlacement l)
    : ptr(p), size(s), place(l) { }
};

typedef basic_string<char, char_traits<char>,
                     STL_Allocator<char, HeapLeakChecker::Allocator>
                    > HCL_string;
// the disabled regexp accumulated
// via HeapLeakChecker::DisableChecksIn
static HCL_string* disabled_regexp = NULL;

// All objects (memory ranges) ignored via HeapLeakChecker::IgnoreObject
// Key is the object's address; value is its size.
typedef map<uintptr_t, size_t, less<uintptr_t>,
            STL_Allocator<pair<const uintptr_t, size_t>,
                          HeapLeakChecker::Allocator>
           > IgnoredObjectsMap;
static IgnoredObjectsMap* ignored_objects = NULL;

// All objects (memory ranges) that we consider to be the sources of pointers
// to live (not leaked) objects.
// At different times this holds (what can be reached from) global data regions
// and the objects we've been told to ignore.
// For any AllocObject::ptr "live_objects" is supposed to contain at most one
// record at any time. We maintain this by checking with the heap_profile map
// of the heap and removing the live heap objects we've handled from it.
// This vector is maintained as a stack and the frontier of reachable
// live heap objects in our flood traversal of them.
typedef vector<AllocObject,
               STL_Allocator<AllocObject, HeapLeakChecker::Allocator>
              > LiveObjectsStack;
static LiveObjectsStack* live_objects = NULL;

// A placeholder to fill-in the starting values for live_objects
// for each library so we can keep the library-name association for logging.
typedef map<HCL_string, LiveObjectsStack, less<HCL_string>,
            STL_Allocator<pair<const HCL_string, LiveObjectsStack>,
                          HeapLeakChecker::Allocator>
           > LibraryLiveObjectsStacks;
static LibraryLiveObjectsStacks* library_live_objects = NULL;

// Objects to be removed from the heap profile when we dump it.
typedef set<const void*, less<const void*>,
            STL_Allocator<const void*, HeapLeakChecker::Allocator>
           > ProfileAdjustObjectSet;
static ProfileAdjustObjectSet* profile_adjust_objects = NULL;

// The disabled program counter addresses for profile dumping
// that are registered with HeapLeakChecker::DisableChecksUp
typedef set<uintptr_t, less<uintptr_t>,
            STL_Allocator<uintptr_t, HeapLeakChecker::Allocator>
           > DisabledAddressSet;
static DisabledAddressSet* disabled_addresses = NULL;

// Value stored in the map of disabled address ranges;
// its key is the end of the address range.
// We'll ignore allocations with a return address in a disabled range
// if the address occurs at 'max_depth' or less in the stack trace.
struct HeapLeakChecker::RangeValue {
  uintptr_t start_address;  // the start of the range
  int       max_depth;      // the maximal stack depth to disable at
};
typedef map<uintptr_t, HeapLeakChecker::RangeValue, less<uintptr_t>,
            STL_Allocator<pair<const uintptr_t, HeapLeakChecker::RangeValue>,
                          HeapLeakChecker::Allocator>
           > DisabledRangeMap;
// The disabled program counter address ranges for profile dumping
// that are registered with HeapLeakChecker::DisableChecksFromToLocked.
static DisabledRangeMap* disabled_ranges = NULL;

// Set of stack tops.
// These are used to consider live only appropriate chunks of the memory areas
// that are used for stacks (and maybe thread-specific data as well)
// so that we do not treat pointers from outdated stack frames as live.
typedef set<uintptr_t, less<uintptr_t>,
            STL_Allocator<uintptr_t, HeapLeakChecker::Allocator>
           > StackTopSet;
static StackTopSet* stack_tops = NULL;

// A map of ranges of code addresses for the system libraries
// that can mmap/mremap/sbrk-allocate memory regions for stacks
// and thread-local storage that we want to consider as live global data.
// Maps from the end address to the start address.
typedef map<uintptr_t, uintptr_t, less<uintptr_t>,
            STL_Allocator<pair<const uintptr_t, uintptr_t>,
                          HeapLeakChecker::Allocator>
           > GlobalRegionCallerRangeMap;
static GlobalRegionCallerRangeMap* global_region_caller_ranges = NULL;

// TODO(maxim): make our big data structs into own modules

//----------------------------------------------------------------------

// Simple hook into execution of global object constructors,
// so that we do not call pthread_self() when it does not yet work.
static bool libpthread_initialized = false;
static bool initializer = (libpthread_initialized = true, true);

// Our hooks for MallocHook
static void NewHook(const void* ptr, size_t size) {
  if (ptr != NULL) {
    RAW_VLOG(7, "Recording Alloc: %p of %"PRIuS, ptr, size);
    heap_checker_lock.Lock();
    heap_profile->RecordAlloc(ptr, size, 0);
    heap_checker_lock.Unlock();
    RAW_VLOG(8, "Alloc Recorded: %p of %"PRIuS"", ptr, size);
  }
}

static void DeleteHook(const void* ptr) {
  if (ptr != NULL) {
    RAW_VLOG(7, "Recording Free %p", ptr);
    heap_checker_lock.Lock();
    heap_profile->RecordFree(ptr);
    heap_checker_lock.Unlock();
    RAW_VLOG(8, "Free Recorded: %p", ptr);
  }
}

//----------------------------------------------------------------------

enum StackDirection {
  GROWS_TOWARDS_HIGH_ADDRESSES,
  GROWS_TOWARDS_LOW_ADDRESSES,
  UNKNOWN_DIRECTION
};

static StackDirection GetStackDirection(const int* ptr);  // defined below

// Function pointer to trick compiler into not inlining a call:
static StackDirection (*do_stack_direction)(const int* ptr) = GetStackDirection;

// Determine which way the stack grows:
// Call with NULL argument.
static StackDirection GetStackDirection(const int* ptr) {
  int a_local;
  if (ptr == NULL) return do_stack_direction(&a_local);
  if (&a_local > ptr) return GROWS_TOWARDS_HIGH_ADDRESSES;
  if (&a_local < ptr) return GROWS_TOWARDS_LOW_ADDRESSES;
  RAW_CHECK(0, "");  // &a_local == ptr, i.e. the recursive call got inlined
                     // and we can't do it (need more hoops to prevent inlining)
  return UNKNOWN_DIRECTION;
}

// Direction of stack growth (will initialize via GetStackDirection())
static StackDirection stack_direction = UNKNOWN_DIRECTION;

// This routine is called for every thread stack we know about to register it.
static void RegisterStack(const void* top_ptr) {
  RAW_VLOG(1, "Thread stack at %p", top_ptr);
  uintptr_t top = reinterpret_cast<uintptr_t>(top_ptr);
  stack_tops->insert(top);  // add for later use

  // make sure stack_direction is initialized
  if (stack_direction == UNKNOWN_DIRECTION) {
    stack_direction = GetStackDirection(NULL);
  }

  // Find memory region with this stack
  MemoryRegionMap::Region region;
  if (MemoryRegionMap::FindStackRegion(top, &region)) {
    // Make the proper portion of the stack live:
    if (stack_direction == GROWS_TOWARDS_LOW_ADDRESSES) {
      RAW_VLOG(2, "Live stack at %p of %"PRIuS" bytes",
                  top_ptr, region.end_addr - top);
      live_objects->push_back(AllocObject(top_ptr, region.end_addr - top,
                                          THREAD_DATA));
    } else {  // GROWS_TOWARDS_HIGH_ADDRESSES
      RAW_VLOG(2, "Live stack at %p of %"PRIuS" bytes",
                  (void*)region.start_addr, top - region.start_addr);
      live_objects->push_back(AllocObject((void*)region.start_addr,
                                          top - region.start_addr,
                                          THREAD_DATA));
    }
  } else {  // not in MemoryRegionMap, look in library_live_objects
    for (LibraryLiveObjectsStacks::iterator lib = library_live_objects->begin();
         lib != library_live_objects->end(); ++lib) {
      for (LiveObjectsStack::iterator span = lib->second.begin();
           span != lib->second.end(); ++span) {
        uintptr_t start = reinterpret_cast<uintptr_t>(span->ptr);
        uintptr_t end = start + span->size;
        if (start <= top  &&  top < end) {
          RAW_VLOG(2, "Stack at %p is inside /proc/self/maps chunk %p..%p",
                      top_ptr, (void*)start, (void*)end);
          // Shrink start..end region by chopping away the memory regions in
          // MemoryRegionMap that land in it to undo merging of regions
          // in /proc/self/maps, so that we correctly identify what portion
          // of start..end is actually the stack region.
          uintptr_t stack_start = start;
          uintptr_t stack_end = end;
          // can optimize-away this loop, but it does not run often
          for (MemoryRegionMap::RegionIterator r =
                 MemoryRegionMap::BeginRegionLocked();
               r != MemoryRegionMap::EndRegionLocked(); ++r) {
            if (top < r->start_addr  &&  r->start_addr < stack_end) {
              stack_end = r->start_addr;
            }
            if (stack_start < r->end_addr  &&  r->end_addr <= top) {
              stack_start = r->end_addr;
            }
          }
          if (stack_start != start  ||  stack_end != end) {
            RAW_VLOG(2, "Stack at %p is actually inside memory chunk %p..%p",
                        top_ptr, (void*)stack_start, (void*)stack_end);
          }
          // Make the proper portion of the stack live:
          if (stack_direction == GROWS_TOWARDS_LOW_ADDRESSES) {
            RAW_VLOG(2, "Live stack at %p of %"PRIuS" bytes",
                        top_ptr, stack_end - top);
            live_objects->push_back(
              AllocObject(top_ptr, stack_end - top, THREAD_DATA));
          } else {  // GROWS_TOWARDS_HIGH_ADDRESSES
            RAW_VLOG(2, "Live stack at %p of %"PRIuS" bytes",
                        (void*)stack_start, top - stack_start);
            live_objects->push_back(
              AllocObject((void*)stack_start, top - stack_start, THREAD_DATA));
          }
          lib->second.erase(span);  // kill the rest of the region
          // Put the non-stack part(s) of the region back:
          if (stack_start != start) {
            lib->second.push_back(AllocObject((void*)start, stack_start - start,
                                  MAYBE_LIVE));
          }
          if (stack_end != end) {
            lib->second.push_back(AllocObject((void*)stack_end, end - stack_end,
                                  MAYBE_LIVE));
          }
          return;
        }
      }
    }
    RAW_LOG(ERROR, "Memory region for stack at %p not found. "
                   "Will likely report false leak positives.", top_ptr);
  }
}

// Iterator for heap allocation map data to make objects allocated from
// disabled regions of code to be live.
static void MakeDisabledLiveCallback(const void* ptr,
                                     const HeapProfileTable::AllocInfo& info) {
  bool stack_disable = false;
  bool range_disable = false;
  for (int depth = 0; depth < info.stack_depth; depth++) {
    uintptr_t addr = reinterpret_cast<uintptr_t>(info.call_stack[depth]);
    if (disabled_addresses  &&
        disabled_addresses->find(addr) != disabled_addresses->end()) {
      stack_disable = true;  // found; dropping
      break;
    }
    if (disabled_ranges) {
      DisabledRangeMap::const_iterator iter
        = disabled_ranges->upper_bound(addr);
      if (iter != disabled_ranges->end()) {
        RAW_DCHECK(iter->first > addr, "");
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
    uintptr_t end_address = start_address + info.object_size;
    StackTopSet::const_iterator iter
      = stack_tops->lower_bound(start_address);
    if (iter != stack_tops->end()) {
      RAW_DCHECK(*iter >= start_address, "");
      if (*iter < end_address) {
        // We do not disable (treat as live) whole allocated regions
        // if they are used to hold thread call stacks
        // (i.e. when we find a stack inside).
        // The reason is that we'll treat as live the currently used
        // stack portions anyway (see RegisterStack),
        // and the rest of the region where the stack lives can well
        // contain outdated stack variables which are not live anymore,
        // hence should not be treated as such.
        RAW_VLOG(2, "Not %s-disabling %"PRIuS" bytes at %p"
                    ": have stack inside: %p",
                    (stack_disable ? "stack" : "range"),
                    info.object_size, ptr, (void*)*iter);
        return;
      }
    }
    RAW_VLOG(2, "%s-disabling %"PRIuS" bytes at %p",
                (stack_disable ? "Stack" : "Range"), info.object_size, ptr);
    live_objects->push_back(AllocObject(ptr, info.object_size,
                                        MUST_BE_ON_HEAP));
  }
}

// This function takes some fields from a /proc/self/maps line:
//
//   start_address  start address of a memory region.
//   end_address    end address of a memory region
//   permissions    rwx + private/shared bit
//   filename       filename of the mapped file
//
// If the region is not writeable, then it cannot have any heap
// pointers in it, otherwise we record it as a candidate live region
// to get filtered later.

static void RecordGlobalDataLocked(uintptr_t start_address,
                                   uintptr_t end_address,
                                   const char* permissions,
                                   const char* filename) {
  // Ignore non-writeable regions.
  if (strchr(permissions, 'w') == NULL) return;
  if (filename == NULL  ||  *filename == '\0')  filename = "UNNAMED";
  RAW_VLOG(2, "Looking into %s: 0x%" PRIxPTR "..0x%" PRIxPTR,
              filename, start_address, end_address);
  (*library_live_objects)[filename].
    push_back(AllocObject(reinterpret_cast<void*>(start_address),
                          end_address - start_address,
                          MAYBE_LIVE));
}

// See if 'library' from /proc/self/maps has base name 'library_base'
// i.e. contains it and has '.' or '-' after it.
static bool IsLibraryNamed(const char* library, const char* library_base) {
  const char* p = strstr(library, library_base);
  size_t sz = strlen(library_base);
  return p != NULL  &&  (p[sz] == '.'  ||  p[sz] == '-');
}

void HeapLeakChecker::DisableLibraryAllocsLocked(const char* library,
                                                 uintptr_t start_address,
                                                 uintptr_t end_address) {
  RAW_DCHECK(heap_checker_lock.IsHeld(), "");
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
        // allocations because we can't see the call stacks.  We'd prefer
        // HeapLeakChecker::DisableChecksIn("default_malloc_ex"
        //                                  "|default_realloc_ex")
        // but that doesn't work when the end-result binary is stripped.
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
    RAW_VLOG(1, "Disabling allocations from %s at depth %d:", library, depth);
    DisableChecksFromToLocked(reinterpret_cast<void*>(start_address),
                              reinterpret_cast<void*>(end_address),
                              depth);
    if (IsLibraryNamed(library, "/libpthread")  ||
        IsLibraryNamed(library, "/libdl")  ||
        IsLibraryNamed(library, "/ld")) {
      RAW_VLOG(1, "Global memory regions made by %s will be live data",
                  library);
      if (global_region_caller_ranges == NULL) {
        global_region_caller_ranges =
          new (Allocator::Allocate(sizeof(GlobalRegionCallerRangeMap)))
            GlobalRegionCallerRangeMap;
      }
      global_region_caller_ranges
        ->insert(make_pair(end_address, start_address));
    }
  }
}

HeapLeakChecker::ProcMapsResult HeapLeakChecker::UseProcMapsLocked(
                                  ProcMapsTask proc_maps_task) {
  RAW_DCHECK(heap_checker_lock.IsHeld(), "");
  // Need to provide own scratch memory to ProcMapsIterator:
  ProcMapsIterator::Buffer buffer;
  ProcMapsIterator it(0, &buffer);
  if (!it.Valid()) {
    int errsv = errno;
    RAW_LOG(ERROR, "Could not open /proc/self/maps: errno=%d. "
                   "Libraries will not be handled correctly.", errsv);
    return CANT_OPEN_PROC_MAPS;
  }
  uint64 start_address, end_address, file_offset;
  int64 inode;
  char *permissions, *filename;
  bool saw_shared_lib = false;
  while (it.Next(&start_address, &end_address, &permissions,
                 &file_offset, &inode, &filename)) {
    if (start_address >= end_address) {
      // Warn if a line we can be interested in is ill-formed:
      if (inode != 0) {
        RAW_LOG(ERROR, "Errors reading /proc/self/maps. "
                       "Some global memory regions will not "
                       "be handled correctly.");
      }
      // Silently skip other ill-formed lines: some are possible
      // probably due to the interplay of how /proc/self/maps is updated
      // while we read it in chunks in ProcMapsIterator and
      // do things in this loop.
      continue;
    }
    // Determine if any shared libraries are present.
    if (inode != 0 && strstr(filename, "lib") && strstr(filename, ".so")) {
      saw_shared_lib = true;
    }
    switch (proc_maps_task) {
      case DISABLE_LIBRARY_ALLOCS:
        // All lines starting like
        // "401dc000-4030f000 r??p 00132000 03:01 13991972  lib/bin"
        // identify a data and code sections of a shared library or our binary
        if (inode != 0 && strncmp(permissions, "r-xp", 4) == 0) {
          DisableLibraryAllocsLocked(filename, start_address, end_address);
        }
        break;
      case RECORD_GLOBAL_DATA:
        RecordGlobalDataLocked(start_address, end_address,
                               permissions, filename);
        break;
      default:
        RAW_CHECK(0, "");
    }
  }
  if (!saw_shared_lib) {
    RAW_LOG(ERROR, "No shared libs detected. Will likely report false leak "
                   "positives for statically linked executables.");
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

// Status of our thread listing callback execution
// (protected by our lock; used from within IgnoreAllLiveObjectsLocked)
static enum {
  CALLBACK_NOT_STARTED,
  CALLBACK_STARTED,
  CALLBACK_COMPLETED,
} thread_listing_status = CALLBACK_NOT_STARTED;

// Ideally to avoid deadlocks this function should not result in any libc
// or other function calls that might need to lock a mutex:
// It is called when all threads of a process are stopped
// at arbitrary points thus potentially holding those locks.
//
// In practice we are calling some simple i/o and sprintf-type library functions
// for logging messages, but use only our own LowLevelAlloc::Arena allocator.
//
// This is known to be buggy: the library i/o function calls are able to cause
// deadlocks when they request a lock that a stopped thread happens to hold.
// This issue as far as we know have so far not resulted in any deadlocks
// in practice, so for now we are taking our chance that the deadlocks
// have insignificant frequency.
//
// If such deadlocks become a problem we should make the i/o calls
// into appropriately direct system calls (or eliminate them),
// in particular write() is not safe and vsnprintf() is potentially dangerous
// due to reliance on locale functions (these are called through RAW_LOG
// and in other ways).
//
int HeapLeakChecker::IgnoreLiveThreads(void* parameter,
                                       int num_threads,
                                       pid_t* thread_pids,
                                       va_list ap) {
  thread_listing_status = CALLBACK_STARTED;
  RAW_VLOG(2, "Found %d threads (from pid %d)", num_threads, getpid());

  if (FLAGS_heap_check_ignore_global_live) {
    UseProcMapsLocked(RECORD_GLOBAL_DATA);
  }

  // We put the registers from other threads here
  // to make pointers stored in them live.
  vector<void*, STL_Allocator<void*, Allocator> > thread_registers;

  int failures = 0;
  for (int i = 0; i < num_threads; ++i) {
    // the leak checking thread itself is handled
    // specially via self_thread_stack, not here:
    if (thread_pids[i] == self_thread_pid) continue;
    RAW_VLOG(2, "Handling thread with pid %d", thread_pids[i]);
#if defined(HAVE_LINUX_PTRACE_H) && defined(HAVE_SYS_SYSCALL_H) && defined(DUMPER)
    i386_regs thread_regs;
#define sys_ptrace(r, p, a, d)  syscall(SYS_ptrace, (r), (p), (a), (d))
    // We use sys_ptrace to avoid thread locking
    // because this is called from ListAllProcessThreads
    // when all but this thread are suspended.
    if (sys_ptrace(PTRACE_GETREGS, thread_pids[i], NULL, &thread_regs) == 0) {
      // Need to use SP to get all the data from the very last stack frame:
      RegisterStack((void*) thread_regs.SP);
      // Make registers live (just in case PTRACE_ATTACH resulted in some
      // register pointers still being in the registers and not on the stack):
      for (void** p = (void**)&thread_regs;
           p < (void**)(&thread_regs + 1); ++p) {
        RAW_VLOG(3, "Thread register %p", *p);
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
    RAW_VLOG(2, "Live registers at %p of %"PRIuS" bytes",
                &thread_registers[0], thread_registers.size() * sizeof(void*));
    live_objects->push_back(AllocObject(&thread_registers[0],
                                        thread_registers.size() * sizeof(void*),
                                        THREAD_REGISTERS));
    IgnoreLiveObjectsLocked("threads register data", "");
  }
  // Do all other liveness walking while all threads are stopped:
  IgnoreNonThreadLiveObjectsLocked();
  // Can now resume the threads:
  ResumeAllProcessThreads(num_threads, thread_pids);
  thread_listing_status = CALLBACK_COMPLETED;
  return failures;
}

// Stack top of the thread that is doing the current leak check
// (protected by our lock; IgnoreAllLiveObjectsLocked sets it)
static const void* self_thread_stack_top;

void HeapLeakChecker::IgnoreNonThreadLiveObjectsLocked() {
  RAW_VLOG(2, "Handling self thread with pid %d", self_thread_pid);
  // Register our own stack:

  // Important that all stack ranges (including the one here)
  // are known before we start looking at them in MakeDisabledLiveCallback:
  RegisterStack(self_thread_stack_top);
  IgnoreLiveObjectsLocked("stack data", "");

  // Make objects we were told to ignore live:
  if (ignored_objects) {
    for (IgnoredObjectsMap::const_iterator object = ignored_objects->begin();
         object != ignored_objects->end(); ++object) {
      const void* ptr = reinterpret_cast<const void*>(object->first);
      RAW_VLOG(2, "Ignored live object at %p of %"PRIuS" bytes",
                  ptr, object->second);
      live_objects->
        push_back(AllocObject(ptr, object->second, MUST_BE_ON_HEAP));
      // we do this liveness check for ignored_objects before doing any
      // live heap walking to make sure it does not fail needlessly:
      size_t object_size;
      if (!(HaveOnHeapLocked(&ptr, &object_size)  &&
            object->second == object_size)) {
        RAW_LOG(FATAL, "Object at %p of %"PRIuS" bytes from an"
                       " IgnoreObject() has disappeared", ptr, object->second);
      }
    }
    IgnoreLiveObjectsLocked("ignored objects", "");
  }

  // Make code-address-disabled objects live and ignored:
  // This in particular makes all thread-specific data live
  // because the basic data structure to hold pointers to thread-specific data
  // is allocated from libpthreads and we have range-disabled that
  // library code with UseProcMapsLocked(DISABLE_LIBRARY_ALLOCS);
  // so now we declare all thread-specific data reachable from there as live.
  heap_profile->IterateAllocs(MakeDisabledLiveCallback);
  IgnoreLiveObjectsLocked("disabled code", "");

  // Actually make global data live:
  if (FLAGS_heap_check_ignore_global_live) {
    bool have_null_region_callers = false;
    for (LibraryLiveObjectsStacks::iterator l = library_live_objects->begin();
         l != library_live_objects->end(); ++l) {
      RAW_CHECK(live_objects->empty(), "");
      // Process library_live_objects in l->second
      // filtering them by MemoryRegionMap:
      // It's safe to iterate over MemoryRegionMap
      // w/o locks here as we are inside MemoryRegionMap::Lock().
      // The only change to MemoryRegionMap possible in this loop
      // is region addition as a result of allocating more memory
      // for live_objects. This won't invalidate the RegionIterator
      // or the intent of the loop.
      // --see the comment by MemoryRegionMap::BeginRegionLocked().
      for (MemoryRegionMap::RegionIterator region =
             MemoryRegionMap::BeginRegionLocked();
           region != MemoryRegionMap::EndRegionLocked(); ++region) {
        // "region" from MemoryRegionMap is to be subtracted from
        // (tentatively live) regions in l->second
        // if it has a stack inside or it was allocated by
        // a non-special caller (not one covered by a range
        // in global_region_caller_ranges).
        // This will in particular exclude all memory chunks used
        // by the heap itself as well as what's been allocated with
        // any allocator on top of mmap.
        bool subtract = true;
        if (!region->is_stack  &&  global_region_caller_ranges) {
          if (region->caller == static_cast<uintptr_t>(NULL)) {
            have_null_region_callers = true;
          } else {
            GlobalRegionCallerRangeMap::const_iterator iter
              = global_region_caller_ranges->upper_bound(region->caller);
            if (iter != global_region_caller_ranges->end()) {
              RAW_DCHECK(iter->first > region->caller, "");
              if (iter->second < region->caller) {  // in special region
                subtract = false;
              }
            }
          }
        }
        if (subtract) {
          // The loop puts the result of filtering l->second into live_objects:
          for (LiveObjectsStack::const_iterator i = l->second.begin();
               i != l->second.end(); ++i) {
            // subtract *region from *i
            uintptr_t start = reinterpret_cast<uintptr_t>(i->ptr);
            uintptr_t end = start + i->size;
            if (region->start_addr <= start  &&  end <= region->end_addr) {
              // full deletion due to subsumption
            } else if (start < region->start_addr  &&
                       region->end_addr < end) {  // cutting-out split
              live_objects->push_back(AllocObject(i->ptr,
                                                  region->start_addr - start,
                                                  IN_GLOBAL_DATA));
              live_objects->push_back(AllocObject((void*)region->end_addr,
                                                  end - region->end_addr,
                                                  IN_GLOBAL_DATA));
            } else if (region->end_addr > start  &&
                       region->start_addr <= start) {  // cut from start
              live_objects->push_back(AllocObject((void*)region->end_addr,
                                                  end - region->end_addr,
                                                  IN_GLOBAL_DATA));
            } else if (region->start_addr > start  &&
                       region->start_addr < end) {  // cut from end
              live_objects->push_back(AllocObject(i->ptr,
                                                  region->start_addr - start,
                                                  IN_GLOBAL_DATA));
            } else {  // pass: no intersection
              live_objects->push_back(AllocObject(i->ptr, i->size,
                                                  IN_GLOBAL_DATA));
            }
          }
          // Move live_objects back into l->second
          // for filtering by the next region.
          live_objects->swap(l->second);
          live_objects->clear();
        }
      }
      // Now get and use live_objects from the final version of l->second:
      if (VLOG_IS_ON(2)) {
        for (LiveObjectsStack::const_iterator i = l->second.begin();
             i != l->second.end(); ++i) {
          RAW_VLOG(2, "Library live region at %p of %"PRIuS" bytes",
                      i->ptr, i->size);
        }
      }
      live_objects->swap(l->second);
      IgnoreLiveObjectsLocked("in globals of\n  ", l->first.c_str());
    }
    if (have_null_region_callers) {
      RAW_LOG(ERROR, "Have memory regions w/o callers: "
                     "might report false leaks");
    }
    Allocator::DeleteAndNull(&library_live_objects);
  }
}

void HeapLeakChecker::IgnoreAllLiveObjectsLocked(const void* self_stack_top) {
  RAW_CHECK(live_objects == NULL, "");
  live_objects = new (Allocator::Allocate(sizeof(LiveObjectsStack)))
                   LiveObjectsStack;
  stack_tops = new (Allocator::Allocate(sizeof(StackTopSet))) StackTopSet;
  // Record global data as live:
  if (FLAGS_heap_check_ignore_global_live) {
    library_live_objects =
      new (Allocator::Allocate(sizeof(LibraryLiveObjectsStacks)))
        LibraryLiveObjectsStacks;
  }
  // Ignore all thread stacks:
  thread_listing_status = CALLBACK_NOT_STARTED;
  bool need_to_ignore_non_thread_objects = true;
  self_thread_pid = getpid();
  self_thread_stack_top = self_stack_top;
  if (FLAGS_heap_check_ignore_thread_live) {
    // We fully suspend the threads right here before any liveness checking
    // and keep them suspended for the whole time of liveness checking
    // inside of the IgnoreLiveThreads callback.
    // (The threads can't (de)allocate due to lock on the delete hook but
    //  if not suspended they could still mess with the pointer
    //  graph while we walk it).
    int r = ListAllProcessThreads(NULL, IgnoreLiveThreads);
    need_to_ignore_non_thread_objects = r < 0;
    if (r < 0) {
      RAW_LOG(WARNING, "Thread finding failed with %d errno=%d", r, errno);
      if (thread_listing_status == CALLBACK_COMPLETED) {
        RAW_LOG(INFO, "Thread finding callback "
                      "finished ok; hopefully everything is fine");
        need_to_ignore_non_thread_objects = false;
      } else if (thread_listing_status == CALLBACK_STARTED) {
        RAW_LOG(FATAL, "Thread finding callback was "
                       "interrupted or crashed; can't fix this");
      } else {  // CALLBACK_NOT_STARTED
        RAW_LOG(ERROR, "Could not find thread stacks. "
                       "Will likely report false leak positives.");
      }
    } else if (r != 0) {
      RAW_LOG(ERROR, "Thread stacks not found for %d threads. "
                     "Will likely report false leak positives.", r);
    } else {
      RAW_VLOG(2, "Thread stacks appear to be found for all threads");
    }
  } else {
    RAW_LOG(WARNING, "Not looking for thread stacks; "
                     "objects reachable only from there "
                     "will be reported as leaks");
  }
  // Do all other live data ignoring here if we did not do it
  // within thread listing callback with all threads stopped.
  if (need_to_ignore_non_thread_objects) {
    if (FLAGS_heap_check_ignore_global_live) {
      UseProcMapsLocked(RECORD_GLOBAL_DATA);
    }
    IgnoreNonThreadLiveObjectsLocked();
  }
  if (live_objects_total) {
    RAW_VLOG(0, "Ignoring %"PRId64" reachable objects of %"PRId64" bytes",
                live_objects_total, live_bytes_total);
  }
  // Free these: we made them here and heap_profile never saw them
  Allocator::DeleteAndNull(&live_objects);
  Allocator::DeleteAndNull(&stack_tops);
}

// Alignment at which we should consider pointer positions
// in IgnoreLiveObjectsLocked. Use 1 if any alignment is ok.
static size_t pointer_alignment = sizeof(void*);
// Global lock for HeapLeakChecker::DoNoLeaks to protect pointer_alignment.
static SpinLock alignment_checker_lock(SpinLock::LINKER_INITIALIZED);

// This function does not change heap_profile's state:
// we only record live objects to be skipped into profile_adjust_objects
// instead of modifying the heap_profile itself.
void HeapLeakChecker::IgnoreLiveObjectsLocked(const char* name,
                                              const char* name2) {
  int64 live_object_count = 0;
  int64 live_byte_count = 0;
  while (!live_objects->empty()) {
    const void* object = live_objects->back().ptr;
    size_t size = live_objects->back().size;
    const ObjectPlacement place = live_objects->back().place;
    live_objects->pop_back();
    size_t object_size;
    if (place == MUST_BE_ON_HEAP  &&
        HaveOnHeapLocked(&object, &object_size)  &&
        profile_adjust_objects->insert(object).second) {
      live_object_count += 1;
      live_byte_count += size;
    }
    RAW_VLOG(4, "Looking for heap pointers in %p of %"PRIuS" bytes",
                object, size);
    // Try interpretting any byte sequence in object,size as a heap pointer:
    const size_t remainder =
      reinterpret_cast<uintptr_t>(object) % pointer_alignment;
    if (remainder) {
      object = (reinterpret_cast<const char*>(object) +
                pointer_alignment - remainder);
      if (size >= pointer_alignment - remainder) {
        size -= pointer_alignment - remainder;
      } else {
        size = 0;
      }
    }
    while (size >= sizeof(void*)) {
      const void* ptr;
      memcpy(&ptr, object, sizeof(ptr));  // size-independent UNALIGNED_LOAD
      const void* current_object = object;
      object = reinterpret_cast<const char*>(object) + pointer_alignment;
      size -= pointer_alignment;
      if (ptr == NULL)  continue;
      RAW_VLOG(8, "Trying pointer to %p at %p", ptr, current_object);
      size_t object_size;
      if (HaveOnHeapLocked(&ptr, &object_size)  &&
          profile_adjust_objects->insert(ptr).second) {
        // We take the (hopefully low) risk here of encountering by accident
        // a byte sequence in memory that matches an address of
        // a heap object which is in fact leaked.
        // I.e. in very rare and probably not repeatable/lasting cases
        // we might miss some real heap memory leaks.
        RAW_VLOG(5, "Found pointer to %p of %"PRIuS" bytes at %p",
                    ptr, object_size, current_object);
        live_object_count += 1;
        live_byte_count += object_size;
        live_objects->push_back(AllocObject(ptr, object_size, IGNORED_ON_HEAP));
      }
    }
  }
  live_objects_total += live_object_count;
  live_bytes_total += live_byte_count;
  if (live_object_count) {
    RAW_VLOG(1, "Removed %"PRId64" live heap objects of %"PRId64" bytes: %s%s",
                live_object_count, live_byte_count, name, name2);
  }
}

bool HeapLeakChecker::HeapProfileFilter(const void* ptr, size_t size) {
  if (profile_adjust_objects->find(ptr) != profile_adjust_objects->end()) {
    RAW_VLOG(4, "Ignoring object at %p of %"PRIuS" bytes", ptr, size);
    // erase so we can later test that all adjust-objects got utilized
    profile_adjust_objects->erase(ptr);
    return true;
  }
  return false;
}

//----------------------------------------------------------------------
// HeapLeakChecker leak check disabling components
//----------------------------------------------------------------------

void HeapLeakChecker::DisableChecksUp(int stack_frames) {
  if (!heap_checker_on) return;
  RAW_CHECK(stack_frames >= 1, "");
  void* stack[1];
  if (GetStackTrace(stack, 1, stack_frames + 1) != 1) {
    RAW_LOG(FATAL, "Can't get stack trace");
  }
  DisableChecksAt(stack[0]);
}

void HeapLeakChecker::DisableChecksAt(const void* address) {
  if (!heap_checker_on) return;
  heap_checker_lock.Lock();
  DisableChecksAtLocked(address);
  heap_checker_lock.Unlock();
}

bool HeapLeakChecker::HaveDisabledChecksUp(int stack_frames) {
  if (!heap_checker_on) return false;
  RAW_CHECK(stack_frames >= 1, "");
  void* stack[1];
  if (GetStackTrace(stack, 1, stack_frames + 1) != 1) {
    RAW_LOG(FATAL, "Can't get stack trace");
  }
  return HaveDisabledChecksAt(stack[0]);
}

bool HeapLeakChecker::HaveDisabledChecksAt(const void* address) {
  if (!heap_checker_on) return false;
  heap_checker_lock.Lock();
  bool result = disabled_addresses != NULL  &&
                disabled_addresses->
                  find(reinterpret_cast<uintptr_t>(address)) !=
                disabled_addresses->end();
  heap_checker_lock.Unlock();
  return result;
}

void HeapLeakChecker::DisableChecksIn(const char* pattern) {
  if (!heap_checker_on) return;
  heap_checker_lock.Lock();
  DisableChecksInLocked(pattern);
  heap_checker_lock.Unlock();
}

void* HeapLeakChecker::GetDisableChecksStart() {
  if (!heap_checker_on) return NULL;
  void* start_address = NULL;
  if (GetStackTrace(&start_address, 1, 1) != 1) {
    RAW_LOG(FATAL, "Can't get stack trace");
  }
  return start_address;
}

void HeapLeakChecker::DisableChecksToHereFrom(const void* start_address) {
  if (!heap_checker_on) return;
  void* end_address_ptr = NULL;
  if (GetStackTrace(&end_address_ptr, 1, 1) != 1) {
    RAW_LOG(FATAL, "Can't get stack trace");
  }
  const void* end_address = end_address_ptr;
  if (start_address > end_address)  swap(start_address, end_address);
  heap_checker_lock.Lock();
  DisableChecksFromToLocked(start_address, end_address, 10000);
    // practically no stack depth limit:
    // our heap_profile keeps much shorter stack traces
  heap_checker_lock.Unlock();
}

void HeapLeakChecker::IgnoreObject(const void* ptr) {
  if (!heap_checker_on) return;
  heap_checker_lock.Lock();
  IgnoreObjectLocked(ptr);
  heap_checker_lock.Unlock();
}

void HeapLeakChecker::IgnoreObjectLocked(const void* ptr) {
  size_t object_size;
  if (HaveOnHeapLocked(&ptr, &object_size)) {
    RAW_VLOG(1, "Going to ignore live object at %p of %"PRIuS" bytes",
                ptr, object_size);
    if (ignored_objects == NULL)  {
      ignored_objects = new (Allocator::Allocate(sizeof(IgnoredObjectsMap)))
                          IgnoredObjectsMap;
    }
    if (!ignored_objects->insert(make_pair(reinterpret_cast<uintptr_t>(ptr),
                                           object_size)).second) {
      RAW_LOG(FATAL, "Object at %p is already being ignored", ptr);
    }
  }
}

void HeapLeakChecker::UnIgnoreObject(const void* ptr) {
  if (!heap_checker_on) return;
  heap_checker_lock.Lock();
  size_t object_size;
  bool ok = HaveOnHeapLocked(&ptr, &object_size);
  if (ok) {
    ok = false;
    if (ignored_objects) {
      IgnoredObjectsMap::iterator object =
        ignored_objects->find(reinterpret_cast<uintptr_t>(ptr));
      if (object != ignored_objects->end()  &&  object_size == object->second) {
        ignored_objects->erase(object);
        ok = true;
        RAW_VLOG(1, "Now not going to ignore live object "
                    "at %p of %"PRIuS" bytes", ptr, object_size);
      }
    }
  }
  heap_checker_lock.Unlock();
  if (!ok)  RAW_LOG(FATAL, "Object at %p has not been ignored", ptr);
}

//----------------------------------------------------------------------
// HeapLeakChecker non-static functions
//----------------------------------------------------------------------

void HeapLeakChecker::DumpProfileLocked(ProfileType profile_type,
                                        const void* self_stack_top,
                                        size_t* alloc_bytes,
                                        size_t* alloc_objects) {
  RAW_VLOG(0, "%s check \"%s\"%s",
              (profile_type == START_PROFILE ? "Starting"
                                             : "At an end point for"),
              name_,
              (pointer_alignment == 1 ? " w/o pointer alignment" : ""));
  // Sanity check that nobody is messing with the hooks we need:
  // Important to have it here: else we can misteriously SIGSEGV
  // in IgnoreLiveObjectsLocked inside ListAllProcessThreads's callback
  // by looking into a region that got unmapped w/o our knowledge.
  MemoryRegionMap::CheckMallocHooks();
  if (MallocHook::GetNewHook() != NewHook  ||
      MallocHook::GetDeleteHook() != DeleteHook) {
    RAW_LOG(FATAL, "new/delete malloc hooks got changed");
  }
  // Make the heap profile, other threads are locked out.
  RAW_CHECK(profile_adjust_objects == NULL, "");
  const int alloc_count = Allocator::alloc_count();
  profile_adjust_objects =
    new (Allocator::Allocate(sizeof(ProfileAdjustObjectSet)))
      ProfileAdjustObjectSet;
  IgnoreAllLiveObjectsLocked(self_stack_top);
  const int len = profile_prefix->size() + strlen(name_) + 10 + 2;
  char* file_name = reinterpret_cast<char*>(Allocator::Allocate(len));
  snprintf(file_name, len, "%s.%s%s%s",
           profile_prefix->c_str(), name_,
           profile_type == START_PROFILE ? "-beg" : "-end",
           HeapProfileTable::kFileExt);
  HeapProfileTable::Stats stats;
  bool ok = heap_profile->DumpFilteredProfile(
    file_name, HeapProfileFilter, FLAGS_heap_check_identify_leaks, &stats);
  RAW_CHECK(ok, "No sense to continue");
  *alloc_bytes = stats.alloc_size - stats.free_size;
  *alloc_objects = stats.allocs - stats.frees;
  Allocator::Free(file_name);
  RAW_CHECK(profile_adjust_objects->empty(),
            "Some objects to ignore are not on the heap");
  Allocator::DeleteAndNull(&profile_adjust_objects);
  // Check that we made no leaks ourselves:
  if (Allocator::alloc_count() != alloc_count) {
    RAW_LOG(FATAL, "Internal HeapChecker leak of %d objects",
                   Allocator::alloc_count() - alloc_count);
  }
}

void HeapLeakChecker::Create(const char *name) {
  name_ = NULL;
  has_checked_ = false;
  char* n = new char[strlen(name) + 1];   // do this before we lock
  IgnoreObject(n);  // otherwise it might be treated as live due to our stack
  alignment_checker_lock.Lock();
  heap_checker_lock.Lock();
  // Heap activity in other threads is paused for this whole function.
  MemoryRegionMap::Lock();
  if (heap_checker_on) {
    RAW_DCHECK(strchr(name, '/') == NULL, "must be a simple name");
    name_ = n;
    memcpy(name_, name, strlen(name) + 1);
    // Use our stack ptr to make stack data live:
    int a_local_var;
    DumpProfileLocked(START_PROFILE, &a_local_var,
                      &start_inuse_bytes_, &start_inuse_allocs_);
    RAW_VLOG(1, "Start check \"%s\" profile: %"PRIuS" bytes "
                "in %"PRIuS" objects",
                name_, start_inuse_bytes_, start_inuse_allocs_);
  } else {
    RAW_LOG(WARNING, "Heap checker is not active, "
                     "hence checker \"%s\" will do nothing!", name);
    RAW_LOG(WARNING, "To activate set the HEAPCHECK environment variable.\n");
  }
  MemoryRegionMap::Unlock();
  heap_checker_lock.Unlock();
  alignment_checker_lock.Unlock();
  if (name_ == NULL) {
    UnIgnoreObject(n);
    delete[] n;  // must be done after we unlock
  }
}

HeapLeakChecker::HeapLeakChecker(const char *name) {
  RAW_DCHECK(strcmp(name, "_main_") != 0, "_main_ is reserved");
  Create(name);
}

HeapLeakChecker::HeapLeakChecker() {
  Create("_main_");
}

ssize_t HeapLeakChecker::BytesLeaked() const {
  if (!has_checked_) {
    RAW_LOG(FATAL, "*NoLeaks|SameHeap must execute before this call");
  }
  return inuse_bytes_increase_;
}

ssize_t HeapLeakChecker::ObjectsLeaked() const {
  if (!has_checked_) {
    RAW_LOG(FATAL, "*NoLeaks|SameHeap must execute before this call");
  }
  return inuse_allocs_increase_;
}

// Save pid of main thread for using in naming dump files
static int32 main_thread_pid = getpid();
#ifdef HAVE_PROGRAM_INVOCATION_NAME
extern char* program_invocation_name;
extern char* program_invocation_short_name;
static const char* invocation_name() { return program_invocation_short_name; }
static const char* invocation_path() { return program_invocation_name; }
#else
static const char* invocation_name() { return "<your binary>"; }
static const char* invocation_path() { return "<your binary>"; }
#endif

static void MakeCommand(const char* basename,
                        bool check_type_is_no_leaks,
                        bool use_initial_profile,
                        const string& prefix,
                        string* beg_profile,
                        string* end_profile,
                        string* command) {
  string ignore_re;
  if (disabled_regexp) {
    ignore_re += " --ignore='^";
    ignore_re += disabled_regexp->c_str();
    ignore_re += "$'";
  }
  *command += *flags_heap_profile_pprof;
  if (use_initial_profile) {
    // compare against initial profile only if need to
    *beg_profile = prefix + "." + basename +
                 "-beg" + HeapProfileTable::kFileExt;
    *command += string(" --base=\"") + *beg_profile + "\"";
  }
  if (check_type_is_no_leaks)  *command += string(" --drop_negative");
  *end_profile = prefix + "." + basename + "-end" + HeapProfileTable::kFileExt;
  *command += string(" ") +
              invocation_path() +
              " \"" + *end_profile + "\"" + ignore_re + " --inuse_objects";
  if (!FLAGS_heap_check_identify_leaks) {
    *command += " --lines";  // important to catch leaks when !see_leaks
  } else {
    *command += " --addresses";  // stronger than --lines and prints
                                 // unresolvable object addresses
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
    fprintf(stderr, "popen(%s) failed!\n", command); // This shouldn't happen
    exit(1);
  }

  if (env_heapcheck) env_heapcheck[-2] = 'K';     // last letter in heapchecK
  if (env_ldpreload) env_heapcheck[-2] = 'D';     // last letter in ldpreloaD

  const int kMaxOutputLine = 10000;
  char line[kMaxOutputLine];
  while (fgets(line, sizeof(line), f) != NULL) {
    if (output)
      *output += line;
  }

  return pclose(f);
}

// RAW_LOG 'str' line by line to prevent its truncation in RAW_LOG:
static void RawLogLines(const string& str) {
  int p = 0;
  while (1) {
    int l = str.find('\n', p);
    if (l == string::npos) {
      if (str[p]) {  // print last line if non empty
        RAW_LOG(INFO, "%s", str.c_str() + p);
      }
      break;
    }
    const_cast<string&>(str)[l] = '\0';  // safe for our use case
    RAW_LOG(INFO, "%s", str.c_str() + p);
    const_cast<string&>(str)[l] = '\n';
    p = l + 1;
  }
}

bool HeapLeakChecker::DoNoLeaks(CheckType check_type,
                                CheckFullness fullness,
                                ReportMode report_mode) {
  // The locking also helps us keep the messages
  // for the two checks close together.
  alignment_checker_lock.Lock();
  bool result;
  if (FLAGS_heap_check_test_pointer_alignment) {
    pointer_alignment = 1;
    bool result_wo_align = DoNoLeaksOnce(check_type, fullness, NO_REPORT);
    pointer_alignment = sizeof(void*);
    result = DoNoLeaksOnce(check_type, fullness, report_mode);
    if (!result) {
      if (result_wo_align) {
        RAW_LOG(WARNING, "Found no leaks without pointer alignment: "
                         "something might be placing pointers at "
                         "unaligned addresses! This needs to be fixed.");
      } else {
        RAW_LOG(INFO, "Found leaks without pointer alignment as well: "
                      "unaligned pointers must not be the cause of leaks.");
        RAW_LOG(INFO, "--heap_check_test_pointer_alignment did not help to "
                      "diagnose the leaks.");
      }
    }
  } else {
    result = DoNoLeaksOnce(check_type, fullness, report_mode);
    if (!result) {
      if (!FLAGS_heap_check_identify_leaks) {
        RAW_LOG(INFO, "setenv HEAP_CHECK_IDENTIFY_LEAKS=1 and rerun to identify "
                      "the addresses of all leaked objects; "
                      "will be reported as fake immediate allocation callers");
      }
      RAW_LOG(INFO, "If you are totally puzzled about why the leaks are there, "
                    "try rerunning it with "
                    "setenv HEAP_CHECK_TEST_POINTER_ALIGNMENT=1");
    }
  }
  alignment_checker_lock.Unlock();
  return result;
}

bool HeapLeakChecker::DoNoLeaksOnce(CheckType check_type,
                                    CheckFullness fullness,
                                    ReportMode report_mode) {
  // Heap activity in other threads is paused for this function
  // until we got all profile difference info.
  heap_checker_lock.Lock();
  MemoryRegionMap::Lock();
  if (heap_checker_on) {
    if (name_ == NULL) {
      RAW_LOG(FATAL, "Heap profiling must be not turned on "
                     "after construction of a HeapLeakChecker");
    }
    // Use our stack ptr to make stack data live:
    int a_local_var;
    size_t end_inuse_bytes;
    size_t end_inuse_allocs;
    DumpProfileLocked(END_PROFILE, &a_local_var,
                      &end_inuse_bytes, &end_inuse_allocs);
    const bool use_initial_profile =
      !(FLAGS_heap_check_before_constructors  &&  this == main_heap_checker);
    if (!use_initial_profile) {  // compare against empty initial profile
      start_inuse_bytes_ = 0;
      start_inuse_allocs_ = 0;
    }
    RAW_VLOG(1, "End check \"%s\" profile: %"PRIuS" bytes in %"PRIuS" objects",
                name_, end_inuse_bytes, end_inuse_allocs);
    inuse_bytes_increase_ = static_cast<ssize_t>(end_inuse_bytes -
                                                 start_inuse_bytes_);
    inuse_allocs_increase_ = static_cast<ssize_t>(end_inuse_allocs -
                                                  start_inuse_allocs_);
    has_checked_ = true;
    MemoryRegionMap::Unlock();
    heap_checker_lock.Unlock();
    bool see_leaks =
      check_type == SAME_HEAP
      ? (inuse_bytes_increase_ != 0 || inuse_allocs_increase_ != 0)
      : (inuse_bytes_increase_ > 0 || inuse_allocs_increase_ > 0);
    if (see_leaks || fullness == USE_PPROF) {
      const bool pprof_can_ignore = disabled_regexp != NULL;
      string beg_profile;
      string end_profile;
      string base_command;
      MakeCommand(name_, check_type == NO_LEAKS,
                  use_initial_profile, *profile_prefix,
                  &beg_profile, &end_profile, &base_command);
      // Make the two command lines out of the base command, with
      // appropriate mode options
      string command = base_command + " --text";
      string gv_command;
      gv_command = base_command;
      gv_command +=
        " --edgefraction=1e-10 --nodefraction=1e-10 --heapcheck --gv";

      if (see_leaks) {
        RAW_LOG(ERROR, "Heap memory leaks of %"PRIdS" bytes and/or "
                       "%"PRIdS" allocations detected by check \"%s\".",
                       inuse_bytes_increase_, inuse_allocs_increase_, name_);
        RAW_LOG(ERROR, "TO INVESTIGATE leaks RUN e.g. THIS shell command:\n"
                       "\n%s\n", gv_command.c_str());
      }
      string output;
      bool checked_leaks = true;
      if ((see_leaks  &&  report_mode == PPROF_REPORT)  ||
          fullness == USE_PPROF) {
        if (access(flags_heap_profile_pprof->c_str(), X_OK|R_OK) != 0) {
          RAW_LOG(WARNING, "Skipping pprof check: could not run it at %s",
                           flags_heap_profile_pprof->c_str());
          checked_leaks = false;
        } else {
          // We don't care about pprof's stderr as long as it
          // succeeds with empty report:
          checked_leaks = GetStatusOutput((command + " 2>/dev/null").c_str(),
                                          &output) == 0;
        }
        if (see_leaks && pprof_can_ignore && output.empty() && checked_leaks) {
          RAW_LOG(WARNING, "These must be leaks that we disabled"
                           " (pprof succeeded)! This check WILL FAIL"
                           " if the binary is strip'ped!");
          see_leaks = false;
        }
        // do not fail the check just due to us being a stripped binary
        if (!see_leaks  &&  strstr(output.c_str(), "nm: ") != NULL  &&
            strstr(output.c_str(), ": no symbols") != NULL)  output.clear();
      }
      // Make sure the profiles we created are still there.
      // They can get deleted e.g. if the program forks/executes itself
      // and FLAGS_cleanup_old_heap_profiles was kept as true.
      if (access(end_profile.c_str(), R_OK) != 0  ||
          (!beg_profile.empty()  &&  access(beg_profile.c_str(), R_OK) != 0)) {
        RAW_LOG(FATAL, "One of the heap profiles is gone: %s %s",
                       beg_profile.c_str(), end_profile.c_str());
      }
      if (!(see_leaks  ||  checked_leaks)) {
        // Crash if something went wrong with executing pprof
        // and we rely on pprof to do its work:
        RAW_LOG(FATAL, "The pprof command failed: %s", command.c_str());
      }
      if (see_leaks  &&  use_initial_profile) {
        RAW_LOG(WARNING, "CAVEAT: Some of the reported leaks might have "
                         "occurred before check \"%s\" was started!", name_);
      }
      bool tricky_leaks = !output.empty();
      if (!see_leaks  &&  tricky_leaks) {
        RAW_LOG(WARNING, "Tricky heap memory leaks of"
                         " no bytes and no allocations "
                         "detected by check \"%s\".", name_);
        RAW_LOG(WARNING, "TO INVESTIGATE leaks RUN e.g. THIS shell command:\n"
                         "\n%s\n", gv_command.c_str());
        if (use_initial_profile) {
          RAW_LOG(WARNING, "CAVEAT: Some of the reported leaks might have "
                           "occurred before check \"%s\" was started!", name_);
        }
        see_leaks = true;
      }
      if (see_leaks  &&  report_mode == PPROF_REPORT) {
        if (checked_leaks) {
          RAW_LOG(INFO, "Below is (less informative) textual version "
                        "of this pprof command's output:");
          RawLogLines(output);
        } else {
          RAW_LOG(ERROR, "The pprof command has failed");
        }
      }
    } else {
      RAW_VLOG(0, "No leaks found for check \"%s\" "
                  "(but no 100%% guarantee that there aren't any)", name_);
    }
    return !see_leaks;
  } else {
    if (name_ != NULL) {
      RAW_LOG(FATAL, "Profiling must stay enabled during leak checking");
    }
    MemoryRegionMap::Unlock();
    heap_checker_lock.Unlock();
    return true;
  }
}

HeapLeakChecker::~HeapLeakChecker() {
  if (name_ != NULL) {  // had leak checking enabled when created the checker
    if (!has_checked_) {
      RAW_LOG(FATAL, "Some *NoLeaks|SameHeap method"
                     " must be called on any created checker");
    }
    UnIgnoreObject(name_);
    delete[] name_;
    name_ = NULL;
  }
}

//----------------------------------------------------------------------
// HeapLeakChecker overall heap check components
//----------------------------------------------------------------------

bool HeapLeakChecker::IsActive() {
  return heap_checker_on;
}

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
    if (!FLAGS_heap_check_after_destructors  &&  do_main_heap_check) {
      DoMainHeapCheck();
    }
  }
}

// defined below
static int GetCommandLineFrom(const char* file, char* cmdline, int size);

static bool internal_init_start_has_run = false;

// Called exactly once, before main() (but hopefully just before).
// This picks a good unique name for the dumped leak checking heap profiles.
void HeapLeakChecker::InternalInitStart() {
  RAW_CHECK(!internal_init_start_has_run, "Only one call is expected");
  internal_init_start_has_run = true;

  if (FLAGS_heap_check.empty()) {
    // turns out we do not need checking in the end; can stop profiling
    TurnItselfOff();
    return;
  }

  // Changing this to false can be useful when debugging heap-checker itself:
  if (!FLAGS_heap_check_run_under_gdb) {
    // See if heap checker should turn itself off because we are
    // running under gdb (to avoid conflicts over ptrace-ing rights):
    char name_buf[15+15];
    snprintf(name_buf, sizeof(name_buf), "/proc/%d/cmdline", int(getppid()));
    char cmdline[1024*8];
    int size = GetCommandLineFrom(name_buf, cmdline, sizeof(cmdline)-1);
    cmdline[size] = '\0';
    // look for "gdb" in the executable's name:
    const char* last = strrchr(cmdline, '/');
    if (last)  last += 1;
    else  last = cmdline;
    if (strncmp(last, "gdb", 3) == 0) {
      RAW_LOG(WARNING, "We seem to be running under gdb; will turn itself off");
      TurnItselfOff();
      return;
    }
  }

  if (!constructor_heap_profiling) {
    RAW_LOG(FATAL, "Can not start so late. You have to enable heap checking "
                   "with HEAPCHECK=<mode>.");
  }

  // make an indestructible copy for heap leak checking
  // happening after global variable destruction
  flags_heap_profile_pprof = new string(FLAGS_heap_profile_pprof);

  // Set all flags
  if (FLAGS_heap_check == "minimal") {
    // The least we can check.
    FLAGS_heap_check_before_constructors = false;  // from after main
                                                   // (ignore more)
    FLAGS_heap_check_after_destructors = false;  // to after cleanup
                                                 // (most data is live)
    FLAGS_heap_check_strict_check = false;  // < profile check (ignore more)
    FLAGS_heap_check_ignore_thread_live = true;  // ignore all live
    FLAGS_heap_check_ignore_global_live = true;  // ignore all live
  } else if (FLAGS_heap_check == "normal") {
    // Faster than 'minimal' and not much stricter.
    FLAGS_heap_check_before_constructors = true;  // from no profile (fast)
    FLAGS_heap_check_after_destructors = false;  // to after cleanup
                                                 // (most data is live)
    FLAGS_heap_check_strict_check = true;  // == profile check (fast)
    FLAGS_heap_check_ignore_thread_live = true;  // ignore all live
    FLAGS_heap_check_ignore_global_live = true;  // ignore all live
  } else if (FLAGS_heap_check == "strict") {
    // A bit stricter than 'normal': global destructors must fully clean up
    // after themselves if they are present.
    FLAGS_heap_check_before_constructors = true;  // from no profile (fast)
    FLAGS_heap_check_after_destructors = true;  // to after destructors
                                                // (less data live)
    FLAGS_heap_check_strict_check = true;  // == profile check (fast)
    FLAGS_heap_check_ignore_thread_live = true;  // ignore all live
    FLAGS_heap_check_ignore_global_live = true;  // ignore all live
  } else if (FLAGS_heap_check == "draconian") {
    // Drop not very portable and not very exact live heap flooding.
    FLAGS_heap_check_before_constructors = true;  // from no profile (fast)
    FLAGS_heap_check_after_destructors = true;  // to after destructors
                                                // (need them)
    FLAGS_heap_check_strict_check = true;  // == profile check (fast)
    FLAGS_heap_check_ignore_thread_live = false;  // no live flood (stricter)
    FLAGS_heap_check_ignore_global_live = false;  // no live flood (stricter)
  } else if (FLAGS_heap_check == "as-is") {
    // do nothing: use other flags as is
  } else if (FLAGS_heap_check == "local") {
    // do nothing
  } else {
    RAW_LOG(FATAL, "Unsupported heap_check flag: %s",
                   FLAGS_heap_check.c_str());
  }
  RAW_DCHECK(heap_checker_pid == getpid(), "");
  heap_checker_on = true;
  RAW_DCHECK(heap_profile, "");
  heap_checker_lock.Lock();
  ProcMapsResult pm_result = UseProcMapsLocked(DISABLE_LIBRARY_ALLOCS);
    // might neeed to do this more than once
    // if one later dynamically loads libraries that we want disabled
  heap_checker_lock.Unlock();
  if (pm_result != PROC_MAPS_USED) {  // can't function
    TurnItselfOff();
    return;
  }

  // make a good place and name for heap profile leak dumps
  profile_prefix = new string(FLAGS_heap_check_dump_directory);
  *profile_prefix += "/";
  *profile_prefix += invocation_name();
  HeapProfileTable::CleanupOldProfiles(profile_prefix->c_str());

  // Finalize prefix for dumping leak checking profiles.
  char pid_buf[15];
  if (main_thread_pid == 0)  // possible if we're called before constructors
    main_thread_pid = getpid();
  snprintf(pid_buf, sizeof(pid_buf), ".%d", main_thread_pid);
  *profile_prefix += pid_buf;

  // Make sure new/delete hooks are installed properly
  // and heap profiler is indeed able to keep track
  // of the objects being allocated.
  // We test this to make sure we are indeed checking for leaks.
  char* test_str = new char[5];
  size_t size;
  RAW_CHECK(heap_profile->FindAlloc(test_str, &size),
            "our own new/delete not linked?");
  delete[] test_str;
  RAW_CHECK(!heap_profile->FindAlloc(test_str, &size),
            "our own new/delete not linked?");
  // If we crash in the above code, it probably means that
  // "nm <this_binary> | grep new" will show that tcmalloc's new/delete
  // implementation did not get linked-in into this binary
  // (i.e. nm will list __builtin_new and __builtin_vec_new as undefined).
  // If this happens, it is a BUILD bug to be fixed.

  if (FLAGS_heap_check != "local") {
    // Schedule registered heap cleanup
    atexit(RunHeapCleanups);
    RAW_DCHECK(main_heap_checker == NULL,
               "Repeated creation of main_heap_checker");
    main_heap_checker = new HeapLeakChecker();
    do_main_heap_check = true;
  }

  RAW_CHECK(heap_checker_on  &&  constructor_heap_profiling,
            "Leak checking is expected to be fully turned on now");
}

// We want this to run early as well, but not so early as
// ::BeforeConstructors (we want flag assignments to have already
// happened, for instance).  Initializer-registration does the trick.
REGISTER_MODULE_INITIALIZER(init_start, HeapLeakChecker::InternalInitStart());

void HeapLeakChecker::DoMainHeapCheck() {
  RAW_DCHECK(heap_checker_pid == getpid()  &&  do_main_heap_check, "");
  if (!NoGlobalLeaks()) {
    if (FLAGS_heap_check_identify_leaks) {
      RAW_LOG(FATAL, "Whole-program memory leaks found.");
    }
    RAW_LOG(ERROR, "Exiting with error code (instead of crashing) "
                   "because of whole-program memory leaks");
    _exit(1);    // we don't want to call atexit() routines!
  }
  do_main_heap_check = false;  // just did it
}

HeapLeakChecker* HeapLeakChecker::GlobalChecker() {
  return main_heap_checker;
}

bool HeapLeakChecker::NoGlobalLeaks() {
  bool result = true;
  HeapLeakChecker* main_hc = main_heap_checker;
  if (main_hc) {
    CheckType check_type = FLAGS_heap_check_strict_check ? SAME_HEAP : NO_LEAKS;
    if (FLAGS_heap_check_before_constructors)  check_type = SAME_HEAP;
      // NO_LEAKS here just would make it slower in this case
      // (we don't use the starting profile anyway)
    CheckFullness fullness = check_type == NO_LEAKS ? USE_PPROF : USE_COUNTS;
      // use pprof if it can help ignore false leaks
    ReportMode report_mode = FLAGS_heap_check_report ? PPROF_REPORT : NO_REPORT;
    RAW_VLOG(0, "Checking for whole-program memory leaks");
    result = main_hc->DoNoLeaks(check_type, fullness, report_mode);
  }
  return result;
}

void HeapLeakChecker::CancelGlobalCheck() {
  if (do_main_heap_check) {
    RAW_VLOG(0, "Canceling the automatic at-exit "
                "whole-program memory leak check");
    do_main_heap_check = false;
  }
}

//----------------------------------------------------------------------
// HeapLeakChecker global constructor/destructor ordering components
//----------------------------------------------------------------------

static bool in_initial_malloc_hook = false;

#ifdef HAVE___ATTRIBUTE___   // we need __attribute__((weak)) for this to work
#define INSTALLED_INITIAL_MALLOC_HOOKS

void HeapLeakChecker_BeforeConstructors();  // below

// Helper for InitialMallocHook_* below
static inline void InitHeapLeakCheckerFromMallocHook() {
  RAW_CHECK(!in_initial_malloc_hook,
            "Something did not reset initial MallocHook-s");
  in_initial_malloc_hook = true;
  // Initialize heap checker on the very first allocation/mmap/sbrk call:
  HeapLeakChecker_BeforeConstructors();
  in_initial_malloc_hook = false;
}

// These will owerwrite the weak definitions in malloc_hook.cc:

// Important to have this to catch the first allocation call from the binary:
extern void InitialMallocHook_New(const void* ptr, size_t size) {
  InitHeapLeakCheckerFromMallocHook();
  // record this first allocation as well (if we need to):
  MallocHook::InvokeNewHook(ptr, size);
}

// Important to have this to catch the first mmap call (say from tcmalloc):
extern void InitialMallocHook_MMap(const void* result,
                                   const void* start,
                                   size_t size,
                                   int protection,
                                   int flags,
                                   int fd,
                                   off_t offset) {
  InitHeapLeakCheckerFromMallocHook();
  // record this first mmap as well (if we need to):
  MallocHook::InvokeMmapHook(
    result, start, size, protection, flags, fd, offset);
}

// Important to have this to catch the first sbrk call (say from tcmalloc):
extern void InitialMallocHook_Sbrk(const void* result, ptrdiff_t increment) {
  InitHeapLeakCheckerFromMallocHook();
  // record this first sbrk as well (if we need to):
  MallocHook::InvokeSbrkHook(result, increment);
}

#endif

// Optional silencing, it must be called shortly after leak checker activates
// in HeapLeakChecker::BeforeConstructors not to let logging messages through,
// but it can't be called when BeforeConstructors() is called from within
// the first mmap/sbrk/alloc call (something deadlocks in this case).
// Hence we arrange for this to be called from the first global c-tor
// that calls HeapLeakChecker_BeforeConstructors.
static void HeapLeakChecker_MaybeMakeSilent() {
#if 0  // TODO(csilvers): see if we can get something like this to work
  if (!VLOG_IS_ON(1))          // not on a verbose setting
    FLAGS_verbose = WARNING;   // only log WARNING and ERROR and FATAL
#endif
}

void HeapLeakChecker::BeforeConstructors() {
  RAW_CHECK(!constructor_heap_profiling,
            "BeforeConstructors called multiple times");
  // set hooks early to crash if 'new' gets called before we make heap_profile:
  MallocHook::SetNewHook(NewHook);
  MallocHook::SetDeleteHook(DeleteHook);
  constructor_heap_profiling = true;
  MemoryRegionMap::Init();  // set up MemoryRegionMap
    // (important that it's done before HeapProfileTable creation below)
  Allocator::Init();
  RAW_CHECK(heap_profile == NULL, "");
  heap_checker_lock.Lock();  // Allocator expects it
  heap_profile = new (Allocator::Allocate(sizeof(HeapProfileTable)))
                   HeapProfileTable(&Allocator::Allocate, &Allocator::Free);
  heap_checker_lock.Unlock();
  RAW_VLOG(0, "Starting tracking the heap");
  heap_checker_on = true;
  // Run silencing if we are called from the first global c-tor,
  // not from the first mmap/sbrk/alloc call:
  if (!in_initial_malloc_hook) HeapLeakChecker_MaybeMakeSilent();
}

void HeapLeakChecker::TurnItselfOff() {
  FLAGS_heap_check = "";  // for users who test for it
  if (constructor_heap_profiling) {
    RAW_CHECK(heap_checker_on, "");
    RAW_LOG(INFO, "Turning heap leak checking off");
    heap_checker_on = false;
    MallocHook::SetNewHook(NULL);
    MallocHook::SetDeleteHook(NULL);
    heap_checker_lock.Lock();  // Allocator expects it
    Allocator::DeleteAndNull(&heap_profile);
    // free our optional global data:
    Allocator::DeleteAndNullIfNot(&disabled_regexp);
    Allocator::DeleteAndNullIfNot(&ignored_objects);
    Allocator::DeleteAndNullIfNot(&disabled_addresses);
    Allocator::DeleteAndNullIfNot(&disabled_ranges);
    Allocator::DeleteAndNullIfNot(&global_region_caller_ranges);
    heap_checker_lock.Unlock();
    Allocator::Shutdown();
    MemoryRegionMap::Shutdown();
  }
  RAW_CHECK(!heap_checker_on, "");
}

// Read in the command line from 'file' into 'cmdline' and return the size read
// 'size' is the space available in 'cmdline'
// We need this because we don't yet have argv/argc.
// CAVEAT: 'file' (some /proc/*/cmdline) might contain
// the command line truncated.
// Arguments in cmdline will be '\0'-terminated,
// the first one will be the binary's name.
static int GetCommandLineFrom(const char* file, char* cmdline, int size) {
  // This routine is only used to check if we're running under gdb, so
  // it's ok if this #if fails and the routine is a no-op.
#if defined(HAVE_SYS_SYSCALL_H)
  // This function is called before memory allocation hooks are set up
  // so we must not have any memory allocations in it.  We use syscall
  // versions of open/read/close here because we don't trust the non-syscall
  // versions: they might 'accidentally' cause a memory allocation.
  // Here's a real-life problem scenario we had:
  // 1) A program LD_PRELOADed a library called list_file_used.a
  // 2) list_file_used intercepted open/read/close and called dlsym()
  // 3) dlsym() called pthread_setspecific() which called malloc().
  // This malloced memory is 'hidden' from the heap-checker.  By
  // definition, this thread-local data is live, and everything it points
  // to is live (not a memory leak) as well.  But because this memory
  // was hidden from the heap-checker, everything it points to was
  // taken to be orphaned, and therefore, a memory leak.
  int fd = syscall(SYS_open, file, O_RDONLY);
  int result = 0;
  if (fd >= 0) {
    ssize_t r;
    while ((r = syscall(SYS_read, fd, cmdline + result, size)) > 0) {
      result += r;
      size -= r;
    }
    syscall(SYS_close, fd);
  }
  return result;
#else   // HAVE_SYS_SYSCALL_H
  return 0;
#endif
}

extern bool heap_leak_checker_bcad_variable;  // in heap-checker-bcad.cc

static bool has_called_BeforeConstructors = false;

void HeapLeakChecker_BeforeConstructors() {
  // We can be called from several places: the first mmap/sbrk/alloc call
  // or the first global c-tor from heap-checker-bcad.cc:
  if (has_called_BeforeConstructors) {
    // Make sure silencing is done when we are called from first global c-tor:
    if (heap_checker_on)  HeapLeakChecker_MaybeMakeSilent();
    return;  // do not re-execure initialization
  }
  has_called_BeforeConstructors = true;

  heap_checker_pid = getpid();  // set it always
  heap_leak_checker_bcad_variable = true;
  // just to reference it, so that heap-checker-bcad.o is linked in

  // This function can be called *very* early, before the normal
  // global-constructor that sets FLAGS_verbose.  Set it manually now,
  // so the RAW_LOG messages here are controllable.
  const char* verbose_str = GetenvBeforeMain("PERFTOOLS_VERBOSE");
  if (verbose_str && atoi(verbose_str)) {  // different than the default of 0?
    FLAGS_verbose = atoi(verbose_str);
  }

  bool need_heap_check = true;
  // The user indicates a desire for heap-checking via the HEAPCHECK
  // environment variable.  If it's not set, there's no way to do
  // heap-checking.
  if (!GetenvBeforeMain("HEAPCHECK")) {
    need_heap_check = false;
  }
#ifdef HAVE_GETEUID
  if (need_heap_check && getuid() != geteuid()) {
    // heap-checker writes out files.  Thus, for security reasons, we don't
    // recognize the env. var. to turn on heap-checking if we're setuid.
    RAW_LOG(WARNING, ("HeapChecker: ignoring HEAPCHECK because "
                      "program seems to be setuid\n"));
    need_heap_check = false;
  }
#endif
  if (need_heap_check) {
    HeapLeakChecker::BeforeConstructors();
  } else {  // cancel our initial hooks
#ifdef INSTALLED_INITIAL_MALLOC_HOOKS
    if (MallocHook::GetNewHook() == &InitialMallocHook_New)
      MallocHook::SetNewHook(NULL);
    if (MallocHook::GetMmapHook() == &InitialMallocHook_MMap)
      MallocHook::SetMmapHook(NULL);
    if (MallocHook::GetSbrkHook() == &InitialMallocHook_Sbrk)
      MallocHook::SetSbrkHook(NULL);
#endif
  }
}

// This function is executed after all global object destructors run.
void HeapLeakChecker_AfterDestructors() {
  if (heap_checker_pid == getpid()) {  // can get here (via forks?)
                                       // with other pids
    if (FLAGS_heap_check_after_destructors  &&  do_main_heap_check) {
      HeapLeakChecker::DoMainHeapCheck();
      poll(NULL, 0, 500);
        // Need this hack to wait for other pthreads to exit.
        // Otherwise tcmalloc find errors
        // on a free() call from pthreads.
    }
    RAW_CHECK(!do_main_heap_check, "should have done it");
  }
}

//----------------------------------------------------------------------
// HeapLeakChecker disabling helpers
//----------------------------------------------------------------------

// These functions are at the end of the file to prevent their inlining:

void HeapLeakChecker::DisableChecksInLocked(const char* pattern) {
  // make disabled_regexp
  if (disabled_regexp == NULL) {
    disabled_regexp = new (Allocator::Allocate(sizeof(HCL_string))) HCL_string;
  }
  RAW_VLOG(1, "Disabling leak checking in stack traces "
              "under frames maching \"%s\"", pattern);
  if (disabled_regexp->size())  *disabled_regexp += '|';
  *disabled_regexp += pattern;
}

void HeapLeakChecker::DisableChecksFromToLocked(const void* start_address,
                                                const void* end_address,
                                                int max_depth) {
  RAW_DCHECK(heap_checker_lock.IsHeld(), "");
  RAW_DCHECK(start_address < end_address, "");
  if (disabled_ranges == NULL) {
    disabled_ranges = new (Allocator::Allocate(sizeof(DisabledRangeMap)))
                        DisabledRangeMap;
  }
  RangeValue value;
  value.start_address = reinterpret_cast<uintptr_t>(start_address);
  value.max_depth = max_depth;
  if (disabled_ranges->
        insert(make_pair(reinterpret_cast<uintptr_t>(end_address),
                         value)).second) {
    RAW_VLOG(1, "Disabling leak checking in stack traces "
                "under frame addresses between %p..%p",
                start_address, end_address);
  } else {  // check that this is just a verbatim repetition
    RangeValue const& val =
      disabled_ranges->find(reinterpret_cast<uintptr_t>(end_address))->second;
    if (val.max_depth != value.max_depth  ||
        val.start_address != value.start_address) {
      RAW_LOG(FATAL, "Two DisableChecksToHereFrom calls conflict: "
                     "(%p, %p, %d) vs. (%p, %p, %d)",
                     (void*)value.start_address, end_address,
                     value.max_depth,
                     start_address, end_address, max_depth);
    }
  }
}

void HeapLeakChecker::DisableChecksAtLocked(const void* address) {
  RAW_DCHECK(heap_checker_lock.IsHeld(), "");
  if (disabled_addresses == NULL) {
    disabled_addresses = new (Allocator::Allocate(sizeof(DisabledAddressSet)))
                           DisabledAddressSet;
  }
  // disable the requested address
  if (disabled_addresses->insert(reinterpret_cast<uintptr_t>(address)).second) {
    RAW_VLOG(1, "Disabling leak checking in stack traces "
                "under frame address %p", address);
  }
}

bool HeapLeakChecker::HaveOnHeapLocked(const void** ptr, size_t* object_size) {
  RAW_DCHECK(heap_checker_lock.IsHeld(), "");
  // Size of the C++ object array size integer
  // (potentially compiler dependent; 4 on i386 and gcc; 8 on x86_64 and gcc)
  const int kArraySizeOffset = sizeof(size_t);
  // sizeof(basic_string<...>::_Rep) for C++ library of gcc 3.4
  // (basically three integer counters;
  // library/compiler dependent; 12 on i386 and gcc)
  const int kStringOffset = sizeof(size_t) * 3;
  // Size of refcount used by UnicodeString in third_party/icu.
  const int kUnicodeStringOffset = sizeof(uint32);
  // NOTE: One can add more similar offset cases below
  //       even when they do not happen for the used compiler/library;
  //       all that's impacted is
  //       - HeapLeakChecker's performace during live heap walking
  //       - and a slightly greater chance to mistake random memory bytes
  //         for a pointer and miss a leak in a particular run of a binary.
  bool result = true;
  if (heap_profile->FindAlloc(*ptr, object_size)) {
    // done
  } else if (heap_profile->FindAlloc(reinterpret_cast<const char*>(*ptr)
                                     - kArraySizeOffset,
                                     object_size)  &&
             *object_size > kArraySizeOffset) {
    // this case is to account for the array size stored inside of
    // the memory allocated by new FooClass[size] for classes with destructors
    *ptr = reinterpret_cast<const char*>(*ptr) - kArraySizeOffset;
    RAW_VLOG(7, "Got poiter into %p at +%d", ptr, kArraySizeOffset);
  } else if (heap_profile->FindAlloc(reinterpret_cast<const char*>(*ptr)
                                     - kStringOffset,
                                     object_size)  &&
             *object_size > kStringOffset) {
    // this case is to account for basic_string<> representation in
    // newer C++ library versions when the kept pointer points to inside of
    // the allocated region
    *ptr = reinterpret_cast<const char*>(*ptr) - kStringOffset;
    RAW_VLOG(7, "Got poiter into %p at +%d", ptr, kStringOffset);
  } else if (kUnicodeStringOffset != kArraySizeOffset &&
             heap_profile->FindAlloc(
                 reinterpret_cast<const char*>(*ptr) - kUnicodeStringOffset,
                 object_size)  &&
             *object_size > kUnicodeStringOffset) {
    // this case is to account for third party UnicodeString.
    // UnicodeString stores a 32-bit refcount (in both 32-bit and
    // 64-bit binaries) as the first uint32 in the allocated memory
    // and a pointer points into the second uint32 behind the refcount.
    *ptr = reinterpret_cast<const char*>(*ptr) - kUnicodeStringOffset;
    RAW_VLOG(7, "Got poiter into %p at +%d", ptr, kUnicodeStringOffset);
  } else {
    result = false;
  }
  return result;
}

const void* HeapLeakChecker::GetAllocCaller(void* ptr) {
  // this is used only in unittest, so the heavy checks are fine
  HeapProfileTable::AllocInfo info;
  heap_checker_lock.Lock();
  CHECK(heap_profile->FindAllocDetails(ptr, &info));
  heap_checker_lock.Unlock();
  CHECK(info.stack_depth >= 1);
  return info.call_stack[0];
}
