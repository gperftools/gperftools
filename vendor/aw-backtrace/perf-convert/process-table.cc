/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#include "process-table.h"

#include <sys/mman.h>

namespace perf_convert {

namespace {
// Anonymous / special mappings that have no ELF backing.
bool IsRealFile(std::string_view name) {
  return !name.empty() && name[0] == '/' && !name.starts_with("/memfd:") && name != "//anon" &&
         !name.starts_with("/dev/zero") && !name.starts_with("/SYSV");
}
}  // namespace

void ProcessTable::HandleMmap(uint32_t pid, uint64_t addr, uint64_t len, uint64_t pgoff, uint32_t prot,
                              const std::string& filename) {
  if (len == 0 || (prot & PROT_EXEC) == 0 || !IsRealFile(filename)) {
    return;
  }
  procs_[pid].Insert(addr, addr + len, MappedFile{pgoff, addr, modules_->Get(filename)});
}

void ProcessTable::HandleFork(uint32_t pid, uint32_t ppid) {
  if (pid == ppid) {
    return;  // new thread: shares the address space, already keyed by pid
  }
  auto it = procs_.find(ppid);
  if (it != procs_.end()) {
    procs_[pid] = it->second;  // copy-on-fork
  } else {
    procs_[pid];  // ensure present, empty
  }
}

void ProcessTable::HandleExec(uint32_t pid) {
  procs_[pid].Clear();
}

void ProcessTable::HandleExit(uint32_t pid) {
  procs_.erase(pid);
}

std::vector<ProcessTable::RangeDump> ProcessTable::RangesFor(uint32_t pid) const {
  std::vector<RangeDump> out;
  auto it = procs_.find(pid);
  if (it == procs_.end()) {
    return out;
  }
  for (const auto& [start, e] : it->second) {
    out.push_back({start, e.end, e.value.file_offset, e.value.module ? e.value.module->path() : std::string()});
  }
  return out;
}

std::optional<ResolvedFrame> ProcessTable::Resolve(uint32_t pid, uint64_t addr, Miss* why) const {
  auto set = [&](Miss m) -> std::optional<ResolvedFrame> {
    if (why)
      *why = m;
    return std::nullopt;
  };
  if (why)
    *why = Miss::kNone;

  auto pit = procs_.find(pid);
  if (pit == procs_.end()) {
    return set(Miss::kUnknownPid);
  }
  auto hit = pit->second.Lookup(addr);
  if (!hit) {
    return set(Miss::kNoRange);
  }
  const MappedFile& mf = *hit->value;
  if (mf.module == nullptr || !mf.module->has_eh_frame()) {
    return set(Miss::kNoModule);
  }
  uint64_t file_off = addr - mf.original_addr + mf.file_offset;
  std::optional<uint64_t> vaddr = mf.module->FileOffToVaddr(file_off);
  if (!vaddr) {
    return set(Miss::kNoVaddr);
  }
  return ResolvedFrame{mf.module, *vaddr};
}

}  // namespace perf_convert
