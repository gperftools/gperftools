/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#ifndef PERF_CONVERT_PERF_DATA_H_
#define PERF_CONVERT_PERF_DATA_H_

#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "perf-format.h"

namespace perf_convert {

// A SAMPLE record decoded far enough to unwind and rewrite it. Spans point
// into the mmap'd input file.
struct ParsedSample {
  uint64_t ip = 0;
  uint32_t pid = 0;
  uint32_t tid = 0;
  uint64_t time = 0;
  uint64_t period = 0;

  // The kernel-side callchain as recorded (with exclude_callchain_user=1 this
  // is [PERF_CONTEXT_KERNEL, k..., PERF_CONTEXT_USER] or just a marker).
  absl::Span<const uint64_t> callchain;

  // User register file. abi==0 means the task was in the kernel with no user
  // regs: userspace can't be unwound, emit the kernel prefix only.
  uint64_t regs_abi = 0;
  uint64_t rip = 0, rsp = 0, rbp = 0;
  // Raw regs array (in sample_regs_user mask-bit order) and its length, for
  // dumping. Points into the mmap'd file; null when regs_abi == 0.
  const uint64_t* regs = nullptr;
  int n_regs = 0;

  // Captured stack: bytes[i] is the byte at address (rsp + i), valid for
  // i < dyn_size.
  const uint8_t* stack = nullptr;
  uint64_t stack_dyn_size = 0;
};

// Reader over an mmap'd perf.data. Assumes a single evsel and a sample_type
// with no fields after STACK_USER -- both asserted at Open().
class PerfData {
 public:
  static absl::StatusOr<std::unique_ptr<PerfData>> Open(const std::string& path);
  ~PerfData();

  const perf_event_attr& attr() const {
    return attr_;
  }
  uint64_t sample_type() const {
    return attr_.sample_type;
  }
  uint64_t sample_regs_user() const {
    return attr_.sample_regs_user;
  }
  int n_user_regs() const {
    return __builtin_popcountll(attr_.sample_regs_user);
  }
  // Size of the perf_event_attr proper (whole perf_file_attr minus its
  // trailing ids section).
  uint32_t event_attr_size() const {
    return event_attr_size_;
  }

  const uint8_t* base() const {
    return base_;
  }
  size_t size() const {
    return size_;
  }
  const PerfFileHeader& header() const {
    return hdr_;
  }
  absl::Span<const uint8_t> attr_ids_bytes() const {
    return {base_ + ids_off_, ids_len_};
  }
  const PerfFileSection& ids_section() const {
    return ids_section_;
  }

  // [data.offset, data.offset+data.size) as record bytes.
  absl::Span<const uint8_t> data() const;

  // Present feature sections, ascending bit order, as (bit, section).
  const std::vector<std::pair<int, PerfFileSection>>& features() const {
    return features_;
  }

  ParsedSample ParseSample(const perf_event_header* h) const;

 private:
  PerfData() = default;

  const uint8_t* base_ = nullptr;
  size_t size_ = 0;
  PerfFileHeader hdr_{};
  perf_event_attr attr_{};
  PerfFileSection ids_section_{};
  uint64_t ids_off_ = 0, ids_len_ = 0;
  uint32_t event_attr_size_ = 0;
  std::vector<std::pair<int, PerfFileSection>> features_;
};

}  // namespace perf_convert

#endif  // PERF_CONVERT_PERF_DATA_H_
