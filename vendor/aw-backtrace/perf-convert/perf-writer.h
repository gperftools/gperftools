/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#ifndef PERF_CONVERT_PERF_WRITER_H_
#define PERF_CONVERT_PERF_WRITER_H_

#include <stdint.h>

#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/types/span.h"
#include "perf-data.h"

namespace perf_convert {

// Writes a new perf.data that is the input with every SAMPLE's DWARF stack
// dump + user regs replaced by an fp-shaped PERF_SAMPLE_CALLCHAIN. The output
// attr drops STACK_USER/REGS_USER (so perf consumes the callchain as-is
// instead of re-unwinding). EVENT_DESC is dropped from the feature set; perf
// reads attrs from the attrs section.
class PerfWriter {
 public:
  PerfWriter(const std::string& out_path, const PerfData& src);
  ~PerfWriter();

  absl::Status Begin();

  // Copy a non-SAMPLE record unchanged.
  absl::Status WriteVerbatim(const perf_event_header* h);

  // Emit a SAMPLE with `user_frames` (leaf-first runtime VAs) spliced into
  // the callchain after the kernel prefix.
  absl::Status WriteSample(const perf_event_header* orig, const ParsedSample& s,
                           absl::Span<const uint64_t> user_frames);

  absl::Status Finish();

 private:
  absl::Status WriteAll(const void* p, size_t n);

  const PerfData& src_;
  std::string out_path_;
  int fd_ = -1;
  uint64_t off_ = 0;  // current write offset

  uint64_t data_off_ = 0;
  uint64_t data_size_ = 0;
  std::vector<uint64_t> ips_scratch_;
};

}  // namespace perf_convert

#endif  // PERF_CONVERT_PERF_WRITER_H_
