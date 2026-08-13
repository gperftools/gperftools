/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#ifndef PERF_CONVERT_SAMPLE_DUMP_H_
#define PERF_CONVERT_SAMPLE_DUMP_H_

#include <stdint.h>

#include <optional>
#include <string>
#include <vector>

#include "absl/types/span.h"
#include "perf-data.h"
#include "process-table.h"

namespace perf_convert {

// A sample read back from a v1 dump file. Owns its buffers; ToSample()
// returns a ParsedSample whose spans point into this object.
struct LoadedSample {
  uint32_t pid = 0, tid = 0;
  uint64_t time = 0, period = 0;
  uint64_t rip = 0, rsp = 0, rbp = 0;
  std::vector<uint64_t> stack_words;     // stack_words[i] is the word at rsp + 8*i
  std::vector<uint64_t> kernel_chain;    // original PERF_SAMPLE_CALLCHAIN
  std::vector<uint64_t> recorded_chain;  // the "ours" line -- for reference only
  struct Mapping {
    uint64_t start, end, file_offset;
    std::string path;
  };
  std::vector<Mapping> maps;

  ParsedSample ToSample() const;
  void PopulateProcessTable(ProcessTable* procs) const;
};

// Parse a v1 dump. nullopt on I/O or format error.
std::optional<LoadedSample> ReadSampleDump(const std::string& path);

// Writes one sample to `path` as a plain line-based text file (format v1):
// pid/tid/time, all captured user regs, the pid's exec-only mappings at
// sample time (start end pgoff path), the raw stack contents one u64 per
// line, the original kernel callchain prefix, and the chain we produced.
// Enough to replay the unwind offline. Returns false on write error.
bool WriteSampleDump(const std::string& path, const PerfData& pd, const ParsedSample& s,
                     const std::vector<ProcessTable::RangeDump>& ranges, absl::Span<const uint64_t> our_chain);

}  // namespace perf_convert

#endif  // PERF_CONVERT_SAMPLE_DUMP_H_
