/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#ifndef PERF_CONVERT_UNWINDER_H_
#define PERF_CONVERT_UNWINDER_H_

#include <stdint.h>

#include <vector>

#include "elf-module.h"
#include "perf-data.h"
#include "process-table.h"

namespace perf_convert {

// Why UnwindUser stopped. Only kEndOfChain / kMaxFrames are "clean".
enum class StopReason {
  kEndOfChain,     // RA undefined / null / architectural end of chain
  kMaxFrames,      // hit the caller's frame cap
  kNoUserRegs,     // sample taken in the kernel: no user state to unwind
  kUnknownPid,     // sample's pid never seen in FORK/COMM/MMAP
  kNoRange,        // PC in no mapping for a known pid
  kNoEhFrame,      // mapping's file has no PT_GNU_EH_FRAME
  kNoVaddr,        // file offset in no PT_LOAD
  kFastPath,       // TryFastFrameInfo / ToFrameInfo bailed -- needs the slow path
  kStack,          // needed the return address outside the captured window
  kBelowSp,        // CFA didn't advance up the stack -- corruption, not depth
  kFpUnavailable,  // a frame's CFA is FP-relative but FP was spilled out of
                   // the captured window earlier (typically an epilogue pop)
};
inline constexpr int kNumStopReason = 12;
const char* StopReasonName(StopReason r);

struct UnwindResult {
  // Leaf-first runtime virtual addresses; front() == sample->rip. Never
  // contains a PERF_CONTEXT_* marker. Empty iff reason == kNoUserRegs.
  std::vector<uint64_t> frames;
  StopReason reason = StopReason::kEndOfChain;
  // The frame the walk stopped at (its module, if one was resolved). vaddr
  // is the module-relative PC there. Useful for triaging kFastPath stops.
  const ElfModule* module = nullptr;
  uint64_t module_vaddr = 0;
};

// Unwind one sample's user stack. `procs` must reflect the address space as
// of the sample's timestamp.
UnwindResult UnwindUser(const ParsedSample& s, const ProcessTable& procs, int max_frames);

// Optional caller-side aggregation over many UnwindResults.
struct UnwindTally {
  int64_t samples = 0;
  int64_t no_user_regs = 0;
  int64_t frames = 0;
  int64_t stop[kNumStopReason] = {};  // indexed by static_cast<int>(StopReason)

  void Add(const UnwindResult& r) {
    samples++;
    if (r.reason == StopReason::kNoUserRegs) {
      no_user_regs++;
    } else {
      frames += static_cast<int64_t>(r.frames.size());
    }
    stop[static_cast<int>(r.reason)]++;
  }
};

}  // namespace perf_convert

#endif  // PERF_CONVERT_UNWINDER_H_
