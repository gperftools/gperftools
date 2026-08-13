/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#include "unwinder.h"

#include <string.h>

#include "aw-backtrace-fastpath.h"

namespace perf_convert {

namespace {

using aw_backtrace_internal::Arch;
using aw_backtrace_internal::CfaRule;
using aw_backtrace_internal::FrameInfo;
using aw_backtrace_internal::RegisterRule;
using aw_backtrace_internal::fastpath::FastPathFrame;
using aw_backtrace_internal::fastpath::TryFastFrameInfo;

// Reads 8-byte, 8-aligned words out of the captured stack window. bytes[i] is
// the byte at address (sp0 + i); anything else is a miss.
class StackReader {
 public:
  StackReader(uint64_t sp0, const uint8_t* data, uint64_t dyn) : sp0_(sp0), data_(data), dyn_(dyn) {
  }

  bool Read(uint64_t addr, uint64_t* out) const {
    if ((addr & 7) != 0 || addr < sp0_ || data_ == nullptr) {
      return false;
    }
    uint64_t off = addr - sp0_;
    if (off + 8 > dyn_) {
      return false;
    }
    memcpy(out, data_ + off, 8);
    return true;
  }

 private:
  uint64_t sp0_;
  const uint8_t* data_;
  uint64_t dyn_;
};

}  // namespace

const char* StopReasonName(StopReason r) {
  switch (r) {
    case StopReason::kEndOfChain:
      return "end-of-chain";
    case StopReason::kMaxFrames:
      return "max-frames";
    case StopReason::kNoUserRegs:
      return "no-user-regs";
    case StopReason::kUnknownPid:
      return "unknown-pid";
    case StopReason::kNoRange:
      return "no-range";
    case StopReason::kNoEhFrame:
      return "no-ehframe";
    case StopReason::kNoVaddr:
      return "no-vaddr";
    case StopReason::kFastPath:
      return "fastpath";
    case StopReason::kStack:
      return "stack";
    case StopReason::kBelowSp:
      return "below-sp";
    case StopReason::kFpUnavailable:
      return "fp-unavailable";
  }
  return "?";
}

UnwindResult UnwindUser(const ParsedSample& s, const ProcessTable& procs, int max_frames) {
  UnwindResult res;
  if (s.regs_abi == 0) {
    res.reason = StopReason::kNoUserRegs;
    return res;
  }

  StackReader stack(s.rsp, s.stack, s.stack_dyn_size);
  uint64_t pc = s.rip, sp = s.rsp, fp = s.rbp;
  // FP goes "unknown" once its spill slot falls outside the captured window
  // -- normal in an epilogue, where `pop %rbp` leaves the caller's value
  // below rsp. That only matters if a later frame's CFA is FP-relative.
  bool fp_valid = true;
  res.frames.push_back(pc);

  auto stop = [&](StopReason r) {
    res.reason = r;
    return res;
  };

  bool leaf = true;
  while (static_cast<int>(res.frames.size()) < max_frames) {
    const uint64_t lookup = leaf ? pc : pc - 1;
    ProcessTable::Miss why = ProcessTable::Miss::kNone;
    std::optional<ResolvedFrame> r = procs.Resolve(s.pid, lookup, &why);
    if (!r) {
      res.module = nullptr;
      switch (why) {
        case ProcessTable::Miss::kUnknownPid:
          return stop(StopReason::kUnknownPid);
        case ProcessTable::Miss::kNoRange:
          return stop(StopReason::kNoRange);
        case ProcessTable::Miss::kNoModule:
          return stop(StopReason::kNoEhFrame);
        case ProcessTable::Miss::kNoVaddr:
          return stop(StopReason::kNoVaddr);
        default:
          return stop(StopReason::kFastPath);
      }
    }
    res.module = r->module;
    res.module_vaddr = r->module_vaddr;

    uint64_t lookup_pc = r->module->LookupPc(r->module_vaddr);
    const auto& eh = r->module->eh_frame();
    FastPathFrame ff = TryFastFrameInfo(eh.eh_frame_start, eh.eh_frame_end, eh.eh_frame_hdr, lookup_pc);
    FrameInfo fi;
    if (!ff.ToFrameInfo(&fi)) {
      ElfModule::BoundedPtr bp = r->module->LookupPcToPointer(lookup_pc);
      if (bp.ptr) {
        if (Arch::DetectPLTEntry(bp.ptr, &fi, {bp.bound_start, bp.bound_end})) {
          // success! Fallthrough, we got FrameInfo filled
        } else {
          return stop(StopReason::kFastPath);
        }
      }
    }

    uint64_t cfa;
    if (fi.cfa.kind == CfaRule::Kind::SpRel) {
      cfa = sp + fi.cfa.offset;
    } else if (fi.cfa.kind == CfaRule::Kind::FpRel) {
      if (!fp_valid) {
        return stop(StopReason::kFpUnavailable);
      }
      cfa = fp + fi.cfa.offset;
    } else {
      return stop(StopReason::kFastPath);
    }
    if (cfa <= sp) {  // must move up the stack
      return stop(StopReason::kBelowSp);
    }

    if (fi.ra.kind == RegisterRule::Kind::Undefined) {
      return stop(StopReason::kEndOfChain);
    }
    if (fi.ra.kind != RegisterRule::Kind::MemCfaRel) {
      return stop(StopReason::kFastPath);
    }
    uint64_t new_pc = 0;
    if (!stack.Read(cfa + fi.ra.offset, &new_pc)) {
      return stop(StopReason::kStack);
    }
    if (new_pc == 0) {
      return stop(StopReason::kEndOfChain);
    }

    uint64_t new_fp = fp;
    bool new_fp_valid = fp_valid;
    if (fi.fp.kind == RegisterRule::Kind::MemCfaRel) {
      // A read miss here is not fatal: the spill slot is simply outside the
      // captured stack (epilogue, or very deep). Carry FP as unknown.
      new_fp_valid = stack.Read(cfa + fi.fp.offset, &new_fp);
    } else if (fi.fp.kind != RegisterRule::Kind::SameValue) {
      return stop(StopReason::kFastPath);
    }

    pc = new_pc;
    sp = cfa;
    fp = new_fp;
    fp_valid = new_fp_valid;
    res.frames.push_back(pc);
    leaf = false;
  }
  return stop(StopReason::kMaxFrames);
}

}  // namespace perf_convert
