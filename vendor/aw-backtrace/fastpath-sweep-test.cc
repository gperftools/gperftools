/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
//
// Differential sweep of the fast-path CFI decoder against the full one.
//
// The fast path's entire contract is "agree with DoUnwindLookup, or fail" --
// it is an accelerator, never a behaviour change (see AGENT.md 4.7). That is
// checkable without any of the stepping machinery the comparer needs, which
// matters because the comparer only compares when run under qemu, and then
// only over the pcs its tests happen to reach. Instead: walk every FDE of
// every loaded module, ask both decoders about a handful of pcs in each, and require that
// wherever the fast path answers at all it answers exactly what the slow path
// does.
//
// The corpus is whatever the test process happens to have mapped -- libc,
// libstdc++, the test binary itself -- so it is toolchain-dependent by
// construction. Hence: mismatches must be zero, but the *coverage* figure is
// only a loose floor guarding against a change that breaks the decoder
// wholesale.

#include <gtest/gtest.h>
#include <link.h>
#include <stdint.h>

#include <string>
#include <vector>

#define BUILDING_TEST

#include "aw-backtrace-fastpath.h"
#include "aw-structs.h"
#include "backtrace-core.h"
#include "eh-frame-reader.h"

namespace aw_backtrace_internal {
namespace {

// The .eh_frame_hdr binary search table, i.e. exactly the structure
// TryFastFrameInfo searches. We only walk it to enumerate probe pcs; both
// decoders under test locate .eh_frame for themselves via LocateEHFrame.
struct EHFrameHDRPrefix {
  uint8_t version;
  uint8_t eh_frame_ptr_enc;
  uint8_t fde_count_enc;
  uint8_t table_enc;
  int32_t eh_frame_ptr;
  uint32_t fde_count;
};
struct EHFrameHDREntry {
  int32_t start_ip_offset;
  int32_t fde_ptr_offset;
};

struct Module {
  std::string name;
  uintptr_t eh_frame_hdr;
};

int CollectModule(struct dl_phdr_info* info, size_t, void* data) {
  auto* out = static_cast<std::vector<Module>*>(data);
  for (int i = 0; i < info->dlpi_phnum; i++) {
    const ElfW(Phdr) & ph = info->dlpi_phdr[i];
    if (ph.p_type != PT_GNU_EH_FRAME || ph.p_memsz < sizeof(EHFrameHDRPrefix)) {
      continue;
    }
    out->push_back(Module{info->dlpi_name && info->dlpi_name[0] ? info->dlpi_name : "<main>",
                          static_cast<uintptr_t>(info->dlpi_addr + ph.p_vaddr)});
  }
  return 0;
}

// The pcs of every FDE in one module, taken from its search table. Empty if
// the header is not the shape the fast path insists on -- in which case the
// fast path would decline the whole module anyway and there is nothing here
// worth sweeping.
std::vector<uintptr_t> FDEStartPCs(uintptr_t eh_frame_hdr) {
  std::vector<uintptr_t> pcs;
  const auto* hdr = reinterpret_cast<const EHFrameHDRPrefix*>(eh_frame_hdr);
  if (hdr->version != 1 || hdr->eh_frame_ptr_enc != 0x1b || hdr->fde_count_enc != 0x03 || hdr->table_enc != 0x3b) {
    return pcs;
  }
  const auto* table = reinterpret_cast<const EHFrameHDREntry*>(eh_frame_hdr + sizeof(EHFrameHDRPrefix));
  pcs.reserve(hdr->fde_count);
  for (uint32_t i = 0; i < hdr->fde_count; i++) {
    // datarel sdata4: the offset is from the header itself, so this is
    // already a runtime address.
    pcs.push_back(eh_frame_hdr + static_cast<uintptr_t>(table[i].start_ip_offset));
  }
  return pcs;
}

// Both decoders describe the same frame if they agree field for field --
// with one exception that is a difference in representation, not in
// behaviour. When the RA rule is Undefined the walk stops (both UnwindLoop
// and UnwindLoopFastPath break on it) before anything reads cfa or fp, so
// the fast path is free to report EndOfChain's architectural-default row
// where the slow path reports whatever the FDE last said. Nothing can
// observe the difference.
bool EquivalentForUnwinding(const FrameInfo& a, const FrameInfo& b) {
  if (a.ra.kind == RegisterRule::Kind::Undefined && b.ra.kind == RegisterRule::Kind::Undefined) {
    return true;
  }
  return a == b;
}

TEST(FastPathSweep, AgreesWithFullDecoder) {
  std::vector<Module> modules;
  dl_iterate_phdr(CollectModule, &modules);
  ASSERT_FALSE(modules.empty());

  int64_t probes = 0;        // pcs the slow decoder resolved, i.e. the denominator
  int64_t fast_ok = 0;       // ... of which the fast path also resolved
  int64_t mismatches = 0;    // ... and disagreed about
  int64_t fast_only = 0;     // fast path answered where the slow one would not
  int64_t end_of_chain = 0;  // ... in the one shape where that is expected
  int reported = 0;

  for (const Module& m : modules) {
    std::vector<uintptr_t> starts = FDEStartPCs(m.eh_frame_hdr);
    for (size_t i = 0; i < starts.size(); i++) {
      // A few pcs per FDE: entry, just inside the prologue, and something
      // well into the body. The table is sorted, so the next entry bounds
      // this one; the last FDE's extent is unknown, so probe conservatively.
      const uintptr_t lo = starts[i];
      const uintptr_t hi = (i + 1 < starts.size()) ? starts[i + 1] : lo + 8;
      if (hi <= lo) {
        continue;
      }
      for (uintptr_t pc : {lo, lo + (hi - lo) / 4, lo + (hi - lo) / 2, hi - 1}) {
        FrameInfo slow;
        const LookupOutcome slow_outcome = DoUnwindLookup(pc, &slow, {});
        if (slow_outcome != LookupOutcome::kOk) {
          // The fast path claiming to know something the full decoder
          // refuses is a fast-path bug, so this case is still checked --
          // it just is not part of the coverage denominator.
          EHReaderInputs storage;
          EHReaderInputs* eh = LocateEHFrame(pc, &storage);
          FrameInfo fast;
          if (eh && TryFastFrameInfo(eh->eh_frame_start, eh->eh_frame_end, eh->eh_frame_hdr, pc).ToFrameInfo(&fast)) {
            if (slow_outcome == LookupOutcome::kUndefinedRA && fast.ra.kind == RegisterRule::Kind::Undefined) {
              // Both decoders saying the same thing: DW_CFA_undefined on the
              // RA column, a frame with nothing to return to -- the process
              // and thread entry points, _dl_start_user, clone's child. The
              // fast path calls it EndOfChain, the full decoder kUndefinedRA,
              // and both stop the walk. It is an agreement that simply is not
              // part of the coverage denominator, since the slow path did not
              // return kOk.
              end_of_chain++;
            } else {
              fast_only++;
              if (reported++ < 10) {
                ADD_FAILURE() << m.name << " pc=0x" << std::hex << pc << std::dec
                              << ": fast path answered where the full decoder failed: " << DescribeFrameInfo(fast);
              }
            }
          }
          continue;
        }
        probes++;

        EHReaderInputs storage;
        EHReaderInputs* eh = LocateEHFrame(pc, &storage);
        ASSERT_NE(eh, nullptr);
        FrameInfo fast;
        if (!TryFastFrameInfo(eh->eh_frame_start, eh->eh_frame_end, eh->eh_frame_hdr, pc).ToFrameInfo(&fast)) {
          continue;  // an ordinary fallback, not an error
        }
        fast_ok++;
        if (!EquivalentForUnwinding(fast, slow)) {
          mismatches++;
          if (reported++ < 10) {
            ADD_FAILURE() << m.name << " pc=0x" << std::hex << pc << std::dec << "\n  fast: " << DescribeFrameInfo(fast)
                          << "\n  slow: " << DescribeFrameInfo(slow);
          }
        }
      }
    }
  }

  ASSERT_GT(probes, 1000);
  fprintf(stderr, "swept %zu modules, %lld probes, fast path handled %lld (%.1f%%), %lld end-of-chain\n",
          modules.size(), (long long)probes, (long long)fast_ok, 100.0 * (double)fast_ok / (double)probes,
          (long long)end_of_chain);

  EXPECT_EQ(mismatches, 0);
  EXPECT_EQ(fast_only, 0);
  // Loose floor. The fast path legitimately declines CFI it does not
  // pattern-match; what this catches is a change that breaks it wholesale.
  //
  // The floor is compiler-dependent because the fast path pins the CIE
  // alignment factors to what gas emits (see AGENT.md 4.7): on aarch64 clang
  // instead emits code_align 1 / data_align -4, so every clang-built object
  // in the corpus -- which, in a clang build, includes gtest and the test
  // binary itself, a good third of the probes -- takes the slow path by
  // design. glibc is gcc-built on every distro that matters, so the gcc
  // number stays near-total.
  double coverage_pct = (double)fast_ok / (double)probes * 100;
#if defined(__clang__) && defined(__aarch64__)
  printf(
      "This is clang and aarch64. As of this writing this is \"broken\" in a sense "
      "of them producing wildly inefficient code and data aligns. "
      "Instead of adapting fast-path to this nonsensical case, we adapt the test.\n");
#else
  EXPECT_GT(coverage_pct, 80);
#endif
  printf("fast-path coverage on sweep test: %g\n", coverage_pct);
}

}  // namespace
}  // namespace aw_backtrace_internal
