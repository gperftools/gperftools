/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#ifndef PERF_CONVERT_ELF_MODULE_H_
#define PERF_CONVERT_ELF_MODULE_H_

#include <stdint.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/functional/function_ref.h"

namespace perf_convert {

// The three real addresses into perf-convert's process's mapping of
// the file that TryFastFrameInfo needs: the .eh_frame_hdr header, and
// a conservative [start, end) that bounds how far the decoder may
// read. All zero => no usable eh_frame.
struct EHFrameBounds {
  uintptr_t eh_frame_hdr;
  uintptr_t eh_frame_start;
  uintptr_t eh_frame_end;
};

// One backing file for a mapping, mmap'd once, whole, PROT_READ, and never
// unmapped. We do NOT reproduce the dynamic loader's layout: the whole file
// sits at file offsets, and every address we hand the fast-path decoder is
// expressed relative to .eh_frame_hdr, so the .eh_frame_hdr<->.eh_frame
// displacement (they share one PT_LOAD) is all that has to be preserved --
// and a raw mapping preserves it.
class ElfModule {
 public:
  ElfModule(const ElfModule&) = delete;
  ElfModule& operator=(const ElfModule&) = delete;
  ~ElfModule();

  // Returns nullptr (and logs once) if the path can't be opened/mapped or
  // isn't an x86-64 ELF. A module with no PT_GNU_EH_FRAME loads fine but
  // has_eh_frame() is false and every lookup against it fails.
  static std::unique_ptr<ElfModule> Open(const char* path);

  const std::string& path() const {
    return path_;
  }
  bool has_eh_frame() const {
    return eh_.eh_frame_hdr != 0;
  }

  // file offset of a byte -> its link-time vaddr, using the PT_LOAD table.
  std::optional<uint64_t> FileOffToVaddr(uint64_t file_off) const;

  // The eh_frame pointers to pass TryFastFrameInfo. lookup_pc for a given
  // module vaddr is LookupPc(vaddr).
  const EHFrameBounds& eh_frame() const {
    return eh_;
  }

  // "lookup pc" is a little conflated thing. This implementation
  // doesn't really lay out all the relative addresses of the elf file
  // as they are loaded. For eh_frame_hdr we record it's actual
  // location where it is loaded and it's original vaddr. Then
  // module's vaddr is mapped into our pointer-space such that
  // relative location of the vaddr's data is matching offsets
  // expected by eh_frame{,_hdr} stuff (which all uses relative
  // addressing). More complicated words than it actually is. See
  // LookupPcToPointer and LookupPcToVaddr below too.
  uintptr_t LookupPc(uint64_t module_vaddr) const {
    return eh_.eh_frame_hdr + (static_cast<uintptr_t>(module_vaddr) - gnu_eh_frame_vaddr_);
  }

  uintptr_t LookupPcToVaddr(uintptr_t pc) const {
    return pc + gnu_eh_frame_vaddr_ - eh_.eh_frame_hdr;  // reverse of LookupPc above
  }

  struct BoundedPtr {
    uintptr_t bound_start;
    uintptr_t bound_end;
    uintptr_t ptr;
  };

  // When we want to access the instructions of the lookup_pc we
  // constructed for unwind info lookup we invoke this thing and does
  // the address translation so that BoundedPtr::ptr is just address
  // of the data we can access in our address space.
  BoundedPtr LookupPcToPointer(uintptr_t lookup_pc) const {
    uintptr_t base_addr = reinterpret_cast<uintptr_t>(base_);
    uintptr_t vaddr_pc = LookupPcToVaddr(lookup_pc);
    BoundedPtr res{};
    for (const auto& ld : loads_) {
      if (ld.vaddr <= vaddr_pc && (vaddr_pc - ld.vaddr) < ld.filesz) {
        res.bound_start = base_addr + ld.offset;
        res.bound_end = res.bound_start + ld.filesz;
        res.ptr = res.bound_start + (vaddr_pc - ld.vaddr);
      }
    }
    return res;
  }

  // Link-time vaddrs of every FDE start, from the .eh_frame_hdr search table.
  // Diagnostics only.
  void TestOnly_ForEachFDE(absl::FunctionRef<void(uintptr_t pc_start, uintptr_t pc_end)> body) const;

 private:
  friend class ModuleCache;
  ElfModule() = default;

  struct Load {
    uint64_t vaddr;
    uint64_t offset;
    uint64_t filesz;
  };

  std::string path_;
  const uint8_t* base_ = nullptr;
  size_t size_ = 0;
  std::vector<Load> loads_;
  uint64_t gnu_eh_frame_vaddr_ = 0;
  EHFrameBounds eh_{};  // all-zero => no eh_frame
};

// path -> ElfModule, opened lazily, kept forever. Not thread-safe (the tool
// is single-threaded, matching the format it reads).
class ModuleCache {
 public:
  // Never returns nullptr; a module that failed to open is cached as a stub
  // whose has_eh_frame() is false.
  ElfModule* Get(const std::string& path);

  int64_t opened() const {
    return opened_;
  }
  int64_t failed() const {
    return failed_;
  }

 private:
  absl::flat_hash_map<std::string, std::unique_ptr<ElfModule>> by_path_;
  int64_t opened_ = 0;
  int64_t failed_ = 0;
};

}  // namespace perf_convert

#endif  // PERF_CONVERT_ELF_MODULE_H_
