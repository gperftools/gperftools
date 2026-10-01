/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
//
// frame-info-dump: given an ELF module and a link-time (module-relative)
// vaddr in it, prints the FrameInfo the fast path and the full CFI decoder
// each derive for that address -- no live process required. The module is
// mmap'd read-only via ElfModule and an EHReaderInputs is built straight
// from its program headers, so the slow path runs through
// DoUnwindLookupFromInputs rather than DoUnwindLookup, which would otherwise
// insist on _dl_find_object seeing the address in *this* process.
#define BUILDING_TEST  // pulls in aw-structs.h's DescribeFrameInfo

#include <stdint.h>

#include <memory>
#include <string>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/flags/usage.h"
#include "absl/log/initialize.h"
#include "absl/log/log.h"
#include "absl/strings/str_format.h"
//
#include "aw-backtrace-fastpath.h"
#include "backtrace-core.h"
#include "elf-module.h"

ABSL_FLAG(std::string, elf, "", "Path to the ELF module to inspect (required).");
// Abseil's own integer flag parsing already understands a leading "0x" as
// base 16 (and does not treat a leading "0" as octal), so this needs no
// hand-rolled parsing.
ABSL_FLAG(uint64_t, vaddr, 0, "Link-time, module-relative vaddr to look up: decimal or 0x-hex.");
ABSL_FLAG(bool, fastpath, true, "Print the fast path's (TryFastFrameInfo) answer.");
ABSL_FLAG(bool, slowpath, true, "Print the full CFI decoder's (DoUnwindLookupFromInputs) answer.");
ABSL_FLAG(bool, diag, true, "On a slow-path failure, have the decoder report why (to stderr).");

namespace {

using aw_backtrace_internal::DescribeFrameInfo;
using aw_backtrace_internal::DiagFlags;
using aw_backtrace_internal::DoUnwindLookupFromInputs;
using aw_backtrace_internal::EHReaderInputs;
using aw_backtrace_internal::FrameInfo;
using aw_backtrace_internal::LookupOutcome;
using aw_backtrace_internal::fastpath::TryFastFrameInfo;
using perf_convert::ElfModule;

const char* OutcomeName(LookupOutcome o) {
  switch (o) {
    case LookupOutcome::kOk:
      return "ok";
    case LookupOutcome::kUndefinedRA:
      return "undefined-ra (end of chain)";
    case LookupOutcome::kFail:
      return "fail";
    case LookupOutcome::kFailExpression:
      return "fail (unsupported dwarf expression)";
  }
  return "?";
}

}  // namespace

int main(int argc, char** argv) {
  absl::SetProgramUsageMessage(
      "Print the FrameInfo the fast path and/or the full CFI decoder derive for one\n"
      "(ELF module, vaddr) pair -- no live process needed.\n\n"
      "Usage: frame-info-dump --elf=/path/to/module --vaddr=0x1a2b0");
  absl::ParseCommandLine(argc, argv);
  absl::InitializeLog();

  const std::string elf_path = absl::GetFlag(FLAGS_elf);
  std::unique_ptr<ElfModule> mod = ElfModule::Open(elf_path.c_str());
  if (!mod) {
    LOG(ERROR) << "could not open/parse " << elf_path;
    return 1;
  }
  if (!mod->has_eh_frame()) {
    LOG(ERROR) << elf_path << ": no usable PT_GNU_EH_FRAME";
    return 1;
  }

  const uint64_t vaddr = absl::GetFlag(FLAGS_vaddr);
  const auto& eh = mod->eh_frame();
  const uintptr_t lookup_pc = mod->LookupPc(vaddr);
  absl::PrintF("%s vaddr=0x%x -> lookup_pc=0x%x (eh_frame_hdr=0x%x)\n", elf_path, vaddr, lookup_pc, eh.eh_frame_hdr);

  bool any_ok = false;

  if (absl::GetFlag(FLAGS_fastpath)) {
    FrameInfo fast;
    if (TryFastFrameInfo(eh.eh_frame_start, eh.eh_frame_end, eh.eh_frame_hdr, lookup_pc).ToFrameInfo(&fast)) {
      absl::PrintF("fast:  %s\n", DescribeFrameInfo(fast));
      any_ok = true;
    } else {
      absl::PrintF("fast:  <declined>\n");
    }
  }

  if (absl::GetFlag(FLAGS_slowpath)) {
    EHReaderInputs inputs{
        .lookup_pc = lookup_pc,
        .eh_frame_hdr = eh.eh_frame_hdr,
        .map_start = eh.eh_frame_start,
        .map_end = eh.eh_frame_end,
        .eh_frame_start = eh.eh_frame_start,
        .eh_frame_end = eh.eh_frame_end,
    };
    FrameInfo slow;
    LookupOutcome outcome = DoUnwindLookupFromInputs(inputs, &slow, DiagFlags{.report = absl::GetFlag(FLAGS_diag)});
    if (outcome == LookupOutcome::kOk || outcome == LookupOutcome::kUndefinedRA) {
      absl::PrintF("slow:  %s (%s)\n", OutcomeName(outcome), DescribeFrameInfo(slow));
      any_ok = true;
    } else {
      absl::PrintF("slow:  %s\n", OutcomeName(outcome));
    }
  }

  return any_ok ? 0 : 1;
}
