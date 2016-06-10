// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
// Copyright (c) 2016, gperftools Contributors
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
// Author: Andrey Semashev
//
// A tcmalloc system allocator that uses mmap with MAP_HUGETLB flag.
//
// Since it only exists on linux, we only register this allocator there.

#ifdef __linux

#include <config.h>
#include <errno.h>                      // for errno
#include <stddef.h>                     // for size_t, NULL
#ifdef HAVE_STDINT_H
#include <stdint.h>                     // for uintptr_t
#endif
#include <string.h>                     // for strerror, memmove, memchr, strncmp
#include <sys/mman.h>                   // for mmap, MAP_FAILED, etc

#if defined(MAP_HUGETLB)

#include <sys/types.h>
#include <sys/stat.h>                   // for stat, fstat
#include <fcntl.h>                      // for open
#include <unistd.h>                     // for close, read
#include <new>                          // for operator new
#include <algorithm>                    // for lower_bound

#include <gperftools/malloc_extension.h>
#include "base/basictypes.h"
#include "base/googleinit.h"
#include "internal_logging.h"

DEFINE_bool(hugetlb_mmap_malloc_enable,
            EnvToBool("TCMALLOC_HUGETLB_MMAP_ENABLE", false),
            "Enable hugetlb_mmap system memory allocator.");
#if defined(MAP_HUGE_SHIFT)
DEFINE_uint64(hugetlb_mmap_malloc_page_size_kb,
              EnvToInt64("TCMALLOC_HUGETLB_MMAP_PAGE_SIZE_KB", 0),
              "Specifies the preferred page size in KiB. "
              "The allocator will choose the closest available page size "
              "not greater than the specified value.  0 == use the default "
              "huge page size.");
#endif
DEFINE_uint64(hugetlb_mmap_malloc_limit_mb,
              EnvToInt64("TCMALLOC_HUGETLB_MMAP_LIMIT_MB", 0),
              "Limit total allocation size to the "
              "specified number of MiB.  0 == no limit.");
DEFINE_bool(hugetlb_mmap_malloc_abort_on_fail,
            EnvToBool("TCMALLOC_HUGETLB_MMAP_ABORT_ON_FAIL", false),
            "abort() whenever hugetlb_mmap_malloc fails to satisfy an allocation "
            "for any reason.");
DEFINE_bool(hugetlb_mmap_malloc_ignore_mmap_fail,
            EnvToBool("TCMALLOC_HUGETLB_MMAP_IGNORE_MMAP_FAIL", false),
            "Ignore failures from mmap");

namespace tcmalloc {
namespace {

// Hugetlb+mmap based allocator for tcmalloc
class HugetlbMmapSysAllocator: public SysAllocator {
private:
  struct HugepageInfo {
    const char* sysfs_dir_name;
    size_t page_size;
  };

public:
  explicit HugetlbMmapSysAllocator(SysAllocator* fallback, size_t page_size)
    : fallback_(fallback),
      page_size_(page_size),
      allocated_size_(0u),
      mmap_flags_(MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB
#if defined(MAP_HUGE_SHIFT)
        | (get_log2(page_size) << MAP_HUGE_SHIFT)
#endif
      ),
      alloc_failed_(false) {
  }

  void* Alloc(size_t size, size_t* actual_size, size_t alignment);
  static void Initialize();

private:
  static bool ParseMeminfo(int meminfo_fd, size_t* page_size);

#if defined(MAP_HUGE_SHIFT)
  static unsigned int get_log2(uint64 n) {
    if (n & 0x00000000FFFFFFFFull)
      return get_log2(static_cast< uint32 >(n));
    else
      return get_log2(static_cast< uint32 >(n >> 32)) + 32u;
  }

  static unsigned int get_log2(uint32 n) {
    if (n & 0x0000FFFFu)
      return get_log2(static_cast< uint16 >(n));
    else
      return get_log2(static_cast< uint16 >(n >> 16)) + 16u;
  }

  static unsigned int get_log2(uint16 n) {
    if (n & 0x00FFu)
      return get_log2(static_cast< uint8 >(n));
    else
      return get_log2(static_cast< uint8 >(n >> 8)) + 8u;
  }

  static unsigned int get_log2(uint8 n) {
    unsigned int res = 0u;
    if ((n & 0x0Fu) == 0u) {
      res += 4u;
      n >>= 4;
    }
    if ((n & 0x03u) == 0u) {
      res += 2u;
      n >>= 2;
    }
    if ((n & 0x01u) == 0u) {
      res += 1u;
    }

    return res;
  }
#endif // defined(MAP_HUGE_SHIFT)

  SysAllocator* const fallback_;  // Default system allocator to fall back to.
  const size_t page_size_;        // Huge page size. Must always be a power of 2.
  size_t allocated_size_;         // Total allocated size, in bytes.
  const unsigned int mmap_flags_; // Pre-computed flags for mmap().
  bool alloc_failed_;             // Whether failed to allocate memory.

  static const HugepageInfo hugepage_infos_[];
};

static union {
  char buf[sizeof(HugetlbMmapSysAllocator)];
  void *ptr;
} hugetlb_mmap_storage;

// No locking needed here since we assume that tcmalloc calls
// us with an internal lock held (see tcmalloc/system-alloc.cc).
void* HugetlbMmapSysAllocator::Alloc(size_t size, size_t* actual_size,
                                     size_t alignment) {
  if (!alloc_failed_) {
    size_t adjusted_size = size;
    const size_t page_size = page_size_, page_size_mask = page_size - 1u;
    if (alignment > page_size)
      adjusted_size += alignment - page_size;
    // Round up to the whole number of pages
    adjusted_size = (adjusted_size + page_size_mask) & ~page_size_mask;

    // Check for overflow. Also, if the allocation size is too small, make sure the caller is interested in the actual allocated size.
    if (adjusted_size >= size && (adjusted_size >= page_size || actual_size != NULL)) {
      if (FLAGS_hugetlb_mmap_malloc_limit_mb == 0 || allocated_size_ + adjusted_size <= FLAGS_hugetlb_mmap_malloc_limit_mb * 1024u * 1024u) {
        void* res = ::mmap(NULL, adjusted_size, PROT_WRITE | PROT_READ, mmap_flags_, -1, 0);
        if (res != reinterpret_cast< void* >(MAP_FAILED)) {
          allocated_size_ += adjusted_size;

          // Ensure the alignment
          uintptr_t ptr = reinterpret_cast< uintptr_t >(res);
          if (ptr & (alignment - 1u)) {
            const size_t alignment_size = alignment - (ptr & (alignment - 1u));
            ptr += alignment_size;
            adjusted_size -= alignment_size;
          }

          if (actual_size)
            *actual_size = adjusted_size;

          return reinterpret_cast< void* >(ptr);
        }

        if (FLAGS_hugetlb_mmap_malloc_abort_on_fail) {
          const int err = errno;
          Log(kCrash, __FILE__, __LINE__,
              "hugetlb_mmap_malloc_abort_on_fail is set, mmap failed (size, error)",
              adjusted_size, strerror(err));
        }
        if (!FLAGS_hugetlb_mmap_malloc_ignore_mmap_fail) {
          const int err = errno;
          Log(kLog, __FILE__, __LINE__,
              "mmap failed (size, error)", adjusted_size, strerror(err));
          alloc_failed_ = true;
        }
      }
      else {
        Log(kLog, __FILE__, __LINE__, "reached the hugetlb_mmap_malloc_limit_mb limit");
        alloc_failed_ = true;
      }
    }
  }

  return fallback_->Alloc(size, actual_size, alignment);
}

#define TCMALLOC_HUGETLB_MMAP_SYSFS(x) "/sys/kernel/mm/hugepages/" x

// Supported huge page sizes and related directories in sysfs. Some of the known sizes are listed here:
//
// https://www.kernel.org/doc/Documentation/vm/hugetlbpage.txt
// https://wiki.debian.org/Hugepages
//
// NOTE: Must be listed in the ascending order. Every supported page size must be a power of 2.
const HugetlbMmapSysAllocator::HugepageInfo HugetlbMmapSysAllocator::hugepage_infos_[] = {
  { TCMALLOC_HUGETLB_MMAP_SYSFS("hugepages-8kB"), 8u * 1024u },
  { TCMALLOC_HUGETLB_MMAP_SYSFS("hugepages-64kB"), 64u * 1024u },
  { TCMALLOC_HUGETLB_MMAP_SYSFS("hugepages-256kB"), 256u * 1024u },
  { TCMALLOC_HUGETLB_MMAP_SYSFS("hugepages-1024kB"), 1024u * 1024u },
  { TCMALLOC_HUGETLB_MMAP_SYSFS("hugepages-2048kB"), 2048u * 1024u },
  { TCMALLOC_HUGETLB_MMAP_SYSFS("hugepages-4096kB"), 4096u * 1024u },
  { TCMALLOC_HUGETLB_MMAP_SYSFS("hugepages-16384kB"), 16384u * 1024u },
  { TCMALLOC_HUGETLB_MMAP_SYSFS("hugepages-262144kB"), 262144u * 1024u },
  { TCMALLOC_HUGETLB_MMAP_SYSFS("hugepages-524288kB"), 524288u * 1024u },
  { TCMALLOC_HUGETLB_MMAP_SYSFS("hugepages-1048576kB"), 1048576u * 1024u }
};

#undef TCMALLOC_HUGETLB_MMAP_SYSFS

void HugetlbMmapSysAllocator::Initialize() {
  // See what huge page sizes are supported on the current system.
  // Note: avoid allocating any dynamic memory here - this means no opendir() & co.
  // So we repeatedly test for the hugepage directory in the sysfs tree to see
  // if a certain page size is supported.
  size_t page_sizes[sizeof(hugepage_infos_) / sizeof(*hugepage_infos_)] = {};
  size_t* page_size_it = page_sizes;

  for (size_t i = 0u, n = sizeof(hugepage_infos_) / sizeof(*hugepage_infos_); i < n; ++i) {
    struct ::stat st = {};
    // Verify that the hugepages entry is a directory owned by root (the latter is a simple security measure
    // against a possible attack by creating a directory instead of a sysfs entry).
    if (::lstat(hugepage_infos_[i].sysfs_dir_name, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == 0) {
      *page_size_it = hugepage_infos_[i].page_size;
      ++page_size_it;
    }
  }

  if (page_size_it == page_sizes) {
    Log(kLog, __FILE__, __LINE__, "no supported huge pages found");
    return;
  }

  size_t preferred_page_size = 0u;
#if defined(MAP_HUGE_SHIFT)
  preferred_page_size = FLAGS_hugetlb_mmap_malloc_page_size_kb * 1024u;
  if (preferred_page_size == 0u)
#endif
  {
    // See what huge page size is the default
    int meminfo_fd = ::open("/proc/meminfo", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (meminfo_fd < 0) {
      Log(kLog, __FILE__, __LINE__, "failed to discover the default huge page size: "
        "/proc/meminfo unavailable");
      return;
    }

    // Check that the meminfo file is a regular file owned by root. This is a basic protection against
    // someone forging /proc/meminfo.
    struct ::stat st = {};
    if (::fstat(meminfo_fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == 0) {
      ParseMeminfo(meminfo_fd, &preferred_page_size);
    }
    else {
      Log(kLog, __FILE__, __LINE__, "/proc/meminfo does not appear to be part of procfs");
    }

    ::close(meminfo_fd);

    if (preferred_page_size == 0u) {
      Log(kLog, __FILE__, __LINE__, "failed to discover the default huge page size: "
        "could not extract the page size from /proc/meminfo");
      return;
    }
  }

  size_t* selected_page_size = std::lower_bound(static_cast< size_t* >(page_sizes), page_size_it, preferred_page_size);
#if defined(MAP_HUGE_SHIFT)
  // Select the page size not greater than the one specified by user
  if (selected_page_size == page_sizes && *selected_page_size > preferred_page_size) {
    Log(kLog, __FILE__, __LINE__, "no available huge pages suit the page size limit");
    return;
  }
  if (selected_page_size == page_size_it)
    preferred_page_size = *(selected_page_size - 1);
  else
    preferred_page_size = *selected_page_size;
#else
  // The default page size should be enabled
  if (selected_page_size == page_size_it || *selected_page_size != preferred_page_size) {
    Log(kLog, __FILE__, __LINE__, "no available huge pages match the default page size");
    return;
  }
#endif

  // Create the allocator
  SysAllocator* fallback = MallocExtension::instance()->GetSystemAllocator();
  HugetlbMmapSysAllocator* alloc = new (hugetlb_mmap_storage.buf) HugetlbMmapSysAllocator(fallback, preferred_page_size);
  MallocExtension::instance()->SetSystemAllocator(alloc);
}

bool HugetlbMmapSysAllocator::ParseMeminfo(int meminfo_fd, size_t* page_size) {
  char meminfo_buf[4096];
  char* meminfo_end = meminfo_buf;
  while (true) {
    ssize_t sz = ::read(meminfo_fd, meminfo_end, sizeof(meminfo_buf) - (meminfo_end - meminfo_buf));
    if (sz < 0)
      break;

    meminfo_end += sz;
    const char* p = meminfo_buf;
    while (p < meminfo_end) {
      // Find the end of line
      const char* eol = static_cast< const char* >(memchr(p, '\n', meminfo_end - p));
      if (eol == NULL)
        break;

      // See if this is the line we're looking for
      if (eol - p > sizeof("Hugepagesize:") - 1 && strncmp(p, "Hugepagesize:", sizeof("Hugepagesize:") - 1) == 0) {
        p += sizeof("Hugepagesize:") - 1;
        // Note: intentionally not using libc functions below to avoid involving locale and possibly invoke memory allocation
        // Skip spaces
        while (p < eol) {
          const char c = *p;
          if (c != ' ' && c != '\t')
            break;
          ++p;
        }

        // Parse size
        size_t pg_size = 0u;
        while (p < eol) {
          const char c = *p;
          if (c < '0' || c > '9')
            break;
          pg_size = pg_size * 10u + (c - '0');
          ++p;
        }

        // Skip spaces
        while (p < eol) {
          const char c = *p;
          if (c != ' ' && c != '\t')
            break;
          ++p;
        }

        // Parse units. Note: it's safe to dereference eol as it points at '\n'.
        switch (*p)
        {
        case 'B':
        case 'b':
          break;
        case 'K':
        case 'k':
          pg_size *= 1024u;
          break;
        case 'M':
        case 'm':
          pg_size *= 1024u * 1024u;
          break;
        case 'G':
        case 'g':
          pg_size *= 1024u * 1024u * 1024u;
          break;
        default:
          {
            Log(kLog, __FILE__, __LINE__, "failed to discover the default huge page size: "
              "could not recognize the page size units in /proc/meminfo");
            return false;
          }
        }

        *page_size = pg_size;

        return true;
      }

      p = eol + 1;
    }

    // Move the unparsed part of the line to the beginning of the buffer
    memmove(meminfo_buf, p, meminfo_end - p);
    meminfo_end = meminfo_buf + (meminfo_end - p);
  }

  return false;
}


REGISTER_MODULE_INITIALIZER(hugetlb_mmap_malloc, {
  if (FLAGS_hugetlb_mmap_malloc_enable)
    HugetlbMmapSysAllocator::Initialize();
});

} // namespace
} // namespace tcmalloc

#endif   /* ifdef MAP_HUGETLB */
#endif   /* ifdef __linux */
