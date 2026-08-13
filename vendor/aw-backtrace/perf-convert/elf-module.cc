/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#include "elf-module.h"

#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "absl/log/absl_check.h"
#include "absl/log/log.h"

namespace perf_convert {

ElfModule::~ElfModule() {
  if (base_ != nullptr) {
    munmap(const_cast<uint8_t*>(base_), size_);
  }
}

std::unique_ptr<ElfModule> ElfModule::Open(const char* path) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    LOG(WARNING) << "perf-convert: cannot open " << path << " (" << strerror(errno) << ")";
    return nullptr;
  }
  struct stat st;
  if (fstat(fd, &st) != 0 || st.st_size < static_cast<off_t>(sizeof(Elf64_Ehdr))) {
    close(fd);
    LOG(WARNING) << "perf-convert: cannot stat / too small: " << path;
    return nullptr;
  }
  void* m = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (m == MAP_FAILED) {
    LOG(WARNING) << "perf-convert: mmap failed for " << path << " (" << strerror(errno) << ")";
    return nullptr;
  }

  auto mod = std::unique_ptr<ElfModule>(new ElfModule);
  mod->path_ = path;
  mod->base_ = static_cast<const uint8_t*>(m);
  mod->size_ = st.st_size;

  const auto* eh = reinterpret_cast<const Elf64_Ehdr*>(mod->base_);
  if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 || eh->e_ident[EI_CLASS] != ELFCLASS64 || eh->e_machine != EM_X86_64) {
    LOG(WARNING) << "perf-convert: not an x86-64 ELF64: " << path;
    return nullptr;
  }
  if (eh->e_phoff == 0 || eh->e_phnum == 0 ||
      eh->e_phoff + static_cast<uint64_t>(eh->e_phnum) * sizeof(Elf64_Phdr) > mod->size_) {
    LOG(WARNING) << "perf-convert: bad program header table: " << path;
    return nullptr;
  }

  const auto* ph = reinterpret_cast<const Elf64_Phdr*>(mod->base_ + eh->e_phoff);
  uint64_t gnu_off = 0, gnu_vaddr = 0;
  bool have_gnu = false;
  for (unsigned i = 0; i < eh->e_phnum; i++) {
    if (ph[i].p_type == PT_LOAD && ph[i].p_filesz > 0) {
      mod->loads_.push_back({ph[i].p_vaddr, ph[i].p_offset, ph[i].p_filesz});
    } else if (ph[i].p_type == PT_GNU_EH_FRAME) {
      gnu_off = ph[i].p_offset;
      gnu_vaddr = ph[i].p_vaddr;
      have_gnu = true;
    }
  }

  if (have_gnu) {
    // The PT_LOAD holding .eh_frame_hdr (its file bytes bound how far the
    // decoder may read; .eh_frame ends before this segment does).
    for (const auto& l : mod->loads_) {
      if (gnu_vaddr >= l.vaddr && gnu_vaddr < l.vaddr + l.filesz && gnu_off + 8 <= mod->size_) {
        uintptr_t base_addr = reinterpret_cast<uintptr_t>(mod->base_);
        uintptr_t hdr = base_addr + gnu_off;
        mod->gnu_eh_frame_vaddr_ = gnu_vaddr;
        mod->eh_ = EHFrameBounds{
            .eh_frame_hdr = hdr,
            .eh_frame_start = base_addr + l.offset,
            .eh_frame_end = base_addr + l.offset + l.filesz,
        };
        break;
      }
    }
  }
  if (!mod->has_eh_frame()) {
    LOG(INFO) << "perf-convert: no usable PT_GNU_EH_FRAME in " << path;
  }
  return mod;
}

void ElfModule::TestOnly_ForEachFDE(absl::FunctionRef<void(uintptr_t pc_start, uintptr_t pc_end)> body) const {
  if (!has_eh_frame())
    return;
  const auto* h = reinterpret_cast<const uint8_t*>(eh_.eh_frame_hdr);
  uint32_t fde_count;
  memcpy(&fde_count, h + 8, 4);
  const uint8_t* tab = h + 12;
  for (uint32_t i = 0; i < fde_count; i++) {
    struct E {
      int32_t start_rel;
      int32_t fde_rel;
    };
    E entry;
    memcpy(&entry, tab + i * 8, sizeof(E));
    uint64_t pc = gnu_eh_frame_vaddr_ + static_cast<int64_t>(entry.start_rel);
    uint64_t fde_addr = eh_.eh_frame_hdr + static_cast<int64_t>(entry.fde_rel);
    struct FDE {
      uint32_t len;
      uint32_t cie_offset;
      int32_t assumed_start_pc;
      uint32_t assumed_size;
    };
    ABSL_CHECK((fde_addr & 3) == 0);
    const FDE* fde = reinterpret_cast<const FDE*>(fde_addr);
    body(pc, pc + fde->assumed_size);
  }
}

std::optional<uint64_t> ElfModule::FileOffToVaddr(uint64_t file_off) const {
  for (const auto& l : loads_) {
    if (file_off >= l.offset && file_off < l.offset + l.filesz) {
      return file_off - l.offset + l.vaddr;
    }
  }
  return std::nullopt;
}

ElfModule* ModuleCache::Get(const std::string& path) {
  if (auto it = by_path_.find(path); it != by_path_.end()) {
    return it->second.get();
  }
  std::unique_ptr<ElfModule> mod = ElfModule::Open(path.c_str());
  if (mod == nullptr) {
    failed_++;
    // Cache a stub (has_eh_frame() == false) so we neither retry the bad
    // path every sample nor hand callers a null.
    mod = std::unique_ptr<ElfModule>(new ElfModule);
    mod->path_ = std::string(path);
  } else {
    opened_++;
  }
  auto [i, _] = by_path_.emplace(std::string(path), std::move(mod));
  return i->second.get();
}

}  // namespace perf_convert
