/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#include <gtest/gtest.h>
#include <sys/mman.h>

#include "aw-backtrace-fastpath.h"
#include "elf-module.h"
#include "perf-format.h"
#include "process-table.h"

namespace perf_convert {
namespace {

TEST(PerfFormat, ContextMarker) {
  EXPECT_TRUE(IsContextMarker(static_cast<uint64_t>(PERF_CONTEXT_USER)));
  EXPECT_TRUE(IsContextMarker(static_cast<uint64_t>(PERF_CONTEXT_KERNEL)));
  EXPECT_FALSE(IsContextMarker(0x555555554000ULL));
  EXPECT_FALSE(IsContextMarker(0x7fffffffffffULL));
}

// A mapping that is later split by an overlapping mmap in its middle keeps
// its surviving tail re-keyed at a new start address, but the file offset for
// that tail must still be computed from the *original* mapping base, not the
// clipped interval start. Regression test for that.
TEST(ProcessTable, SplitMappingResolvesFileOffsetFromOriginalBase) {
  ModuleCache mc;
  ProcessTable pt(&mc);

  const char* self = "/proc/self/exe";
  const uint64_t base = 0x400000000ULL;
  const uint64_t len = 0x40000;  // covers the file's first PT_LOAD
  const uint64_t probe_off = 0x200;

  // Reference pid: one clean mapping of the whole file at `base`, pgoff 0.
  pt.HandleMmap(1, base, len, 0, PROT_READ | PROT_EXEC, self);
  auto ref = pt.Resolve(1, base + probe_off);
  if (!ref.has_value()) {
    GTEST_SKIP() << "test binary has no usable .eh_frame";
  }

  // Split pid: same clean mapping, then a fresh exec mapping lands in the
  // middle *below* the probe, so the probe ends up in the re-keyed tail.
  pt.HandleMmap(2, base, len, 0, PROT_READ | PROT_EXEC, self);
  pt.HandleMmap(2, base + 0x40, 0x40, 0x40, PROT_READ | PROT_EXEC, self);
  auto split = pt.Resolve(2, base + probe_off);

  ASSERT_TRUE(split.has_value());
  EXPECT_EQ(split->module, ref->module);
  EXPECT_EQ(split->module_vaddr, ref->module_vaddr);
}

// Sweep the fast-path CFI decoder over every FDE in this test's own
// binary (built with the same toolchain as perf-convert). A few FDEs
// carry CFI the fast path legitimately can't pattern-match, so this
// is a threshold check, not all-or-nothing.  Mostly a guard against a
// change that breaks the decoder wholesale.
TEST(SelfTest, SweepOwnFDEs) {
  std::unique_ptr<ElfModule> m = ElfModule::Open("/proc/self/exe");
  ASSERT_TRUE(m && m->has_eh_frame()) << "test binary has no usable .eh_frame";

  const EHFrameBounds& eh = m->eh_frame();
  int64_t probes = 0, fail = 0;
  m->TestOnly_ForEachFDE([&](uint64_t lo, uint64_t hi) {
    ASSERT_LT(lo, hi);
    for (uint64_t v : {lo, lo + 1, lo + (hi - lo) / 2, hi - 1}) {
      probes++;
      auto ff = aw_backtrace_internal::fastpath::TryFastFrameInfo(eh.eh_frame_start, eh.eh_frame_end, eh.eh_frame_hdr,
                                                                  m->LookupPc(v));
      aw_backtrace_internal::FrameInfo fi;
      if (!ff.ToFrameInfo(&fi)) {
        fail++;
      }
    }
  });

  ASSERT_GT(probes, 0);
  double fail_pct = 100.0 * fail / probes;
  EXPECT_LT(fail_pct, 3.0);
}

TEST(ProcessTable, ForkCopiesThenDiverges) {
  ModuleCache mc;
  ProcessTable pt(&mc);
  pt.HandleMmap(100, 0x1000, 0x1000, 0, PROT_READ | PROT_EXEC, "/no/such/file");
  pt.HandleFork(200, 100);
  // Unknown addr in a child: no crash, nullopt.
  EXPECT_FALSE(pt.Resolve(200, 0x1500).has_value());
  EXPECT_FALSE(pt.Resolve(999, 0x1500).has_value());
}

}  // namespace
}  // namespace perf_convert
