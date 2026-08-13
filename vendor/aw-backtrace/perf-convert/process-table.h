/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#ifndef PERF_CONVERT_PROCESS_TABLE_H_
#define PERF_CONVERT_PROCESS_TABLE_H_

#include <stdint.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "elf-module.h"
#include "interval-map.h"

namespace perf_convert {

// What a MMAP record establishes for one address range: the byte at
// `original_addr` is at file offset `file_offset` within `module`. The
// interval this value is attached to may later be clipped or split by an
// overlapping mmap, so its start can drift away from `original_addr` --
// which is why the anchor is stored here rather than taken from the interval.
struct MappedFile {
  uint64_t file_offset;    // pgoff from the mmap record
  uint64_t original_addr;  // mmap addr that `file_offset` refers to
  ElfModule* module;
};

struct ResolvedFrame {
  ElfModule* module;
  uint64_t module_vaddr;
};

// Per-pid view of the address space, rebuilt from the event stream (MMAP2 /
// FORK / COMM-exec / EXIT). The caller replays events in timestamp order; a
// fresh mapping overrides whatever it overlaps (see IntervalMap).
class ProcessTable {
 public:
  explicit ProcessTable(ModuleCache* modules) : modules_(modules) {
  }

  // Only executable file mappings are tracked; `prot` is the mmap prot bits
  // (PROT_*). Non-exec / anonymous / special mappings are ignored.
  void HandleMmap(uint32_t pid, uint64_t addr, uint64_t len, uint64_t pgoff, uint32_t prot,
                  const std::string& filename);
  void HandleFork(uint32_t pid, uint32_t ppid);
  void HandleExec(uint32_t pid);
  void HandleExit(uint32_t pid);

  enum class Miss { kNone, kUnknownPid, kNoRange, kNoModule, kNoVaddr };

  // nullopt if `pid` is unknown or `addr` is in no mapping or the mapping's
  // file has no PT_LOAD covering it. `why` (optional) gets the reason.
  std::optional<ResolvedFrame> Resolve(uint32_t pid, uint64_t addr, Miss* why = nullptr) const;

  size_t known_pids() const {
    return procs_.size();
  }

  struct RangeDump {
    uint64_t start, end, file_offset;
    std::string path;
  };
  // Executable mappings currently live for `pid`, sorted by start. Empty if
  // the pid is unknown.
  std::vector<RangeDump> RangesFor(uint32_t pid) const;

 private:
  using Space = IntervalMap<MappedFile>;

  ModuleCache* modules_;
  absl::flat_hash_map<uint32_t, Space> procs_;
};

}  // namespace perf_convert

#endif  // PERF_CONVERT_PROCESS_TABLE_H_
