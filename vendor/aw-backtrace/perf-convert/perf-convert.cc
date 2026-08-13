/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
//
// perf-convert: read a `perf record --call-graph dwarf` capture, unwind every
// sample offline with aw-backtrace's fast path, and write a new perf.data
// whose samples carry a plain PERF_SAMPLE_CALLCHAIN instead of a raw stack
// dump -- i.e. `perf inject --convert-callchain` without libdw.
//
// Assumptions (v1, all asserted or logged):
//   * single evsel, sample_type == IP|TID|TIME|CALLCHAIN|PERIOD|REGS_USER|STACK_USER
//   * we trust the file paths in the MMAP records and that the on-disk files
//     still match -- no build-id checking
//   * fast path only: a frame the fast path can't do truncates the chain

#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/flags/usage.h"
#include "absl/log/absl_check.h"
#include "absl/log/globals.h"
#include "absl/log/initialize.h"
#include "absl/log/log.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_split.h"
//
#include "aw-backtrace-fastpath.h"
#include "elf-module.h"
#include "perf-data.h"
#include "perf-writer.h"
#include "process-table.h"
#include "sample-dump.h"
#include "unwinder.h"

ABSL_FLAG(std::string, input, "", "input perf.data");
ABSL_FLAG(std::string, output, "", "output perf.data");
ABSL_FLAG(int64_t, max_samples, 0, "stop after N samples (0 = all)");
ABSL_FLAG(int, max_frames, 512, "max user frames per sample");
ABSL_FLAG(std::string, selftest_elf, "", "load this ELF and sweep TryFastFrameInfo over its FDEs");
ABSL_FLAG(std::string, replay_sample, "", "unwind one previously dumped .sample file and print the chain");
ABSL_FLAG(std::string, dump_samples, "",
          "comma-separated PID:TID:TIME triples; each matching sample is written to --dump_dir");
ABSL_FLAG(std::string, dump_dir, "perf-convert/testdata", "directory for --dump_samples output");

namespace perf_convert {
namespace {

template <typename T>
T Rd(const uint8_t* p) {
  T v;
  memcpy(&v, p, sizeof(T));
  return v;
}

// Timestamp of a record. SAMPLE carries TIME inline (after ip, pid/tid);
// every other record with sample_id_all gets it as the last u64 of a
// {pid,tid,time} trailer. Records with no trailer (TIME_CONV, THREAD_MAP,
// FINISHED_ROUND, ...) return 0 and sort to the front, keeping file order.
uint64_t RecordTime(const perf_event_header* h) {
  const uint8_t* body = reinterpret_cast<const uint8_t*>(h) + sizeof(perf_event_header);
  switch (h->type) {
    case PERF_RECORD_SAMPLE:
      return Rd<uint64_t>(body + 16);  // ip(8) pid,tid(8) time
    case PERF_RECORD_MMAP:
    case PERF_RECORD_MMAP2:
    case PERF_RECORD_COMM:
    case PERF_RECORD_FORK:
    case PERF_RECORD_EXIT:
    case PERF_RECORD_THROTTLE:
    case PERF_RECORD_UNTHROTTLE:
    case PERF_RECORD_KSYMBOL:
    case PERF_RECORD_BPF_EVENT:
      if (h->size >= sizeof(perf_event_header) + 8) {
        return Rd<uint64_t>(reinterpret_cast<const uint8_t*>(h) + h->size - 8);
      }
      return 0;
    default:
      return 0;
  }
}

int SelfTest(const std::string& path) {
  std::unique_ptr<ElfModule> m = ElfModule::Open(path.c_str());
  if (!m || !m->has_eh_frame()) {
    absl::FPrintF(stderr, "selftest: no module / no eh_frame\n");
    return 1;
  }
  int64_t ok = 0, fail = 0, probes = 0;
  int fde_no = -1;
  m->TestOnly_ForEachFDE([&](uint64_t lo, uint64_t hi) {
    fde_no++;
    ABSL_CHECK(lo < hi);
    const auto& eh = m->eh_frame();
    for (uint64_t v : {lo, lo + 1, lo + (hi - lo) / 2, hi - 1}) {
      probes++;
      auto ff = aw_backtrace_internal::fastpath::TryFastFrameInfo(eh.eh_frame_start, eh.eh_frame_end, eh.eh_frame_hdr,
                                                                  m->LookupPc(v));
      aw_backtrace_internal::FrameInfo fi;
      if (ff.ToFrameInfo(&fi)) {
        ok++;
      } else {
        fail++;
        absl::FPrintF(stderr, "  FAIL fde[%d] vaddr 0x%x\n", fde_no, v);
      }
    }
  });
  absl::FPrintF(stderr, "selftest %s: %d FDEs, %d probes, %d ok, %d fail (%.1f%%)\n", path, fde_no, probes, ok, fail,
                probes ? 100.0 * fail / probes : 0.0);
  return 0;
}

int ReplaySample(const std::string& path) {
  std::optional<LoadedSample> ls = ReadSampleDump(path);
  if (!ls) {
    LOG(ERROR) << "cannot read sample dump: " << path;
    return 1;
  }
  ModuleCache modules;
  ProcessTable procs(&modules);
  ls->PopulateProcessTable(&procs);
  ParsedSample s = ls->ToSample();

  UnwindResult r = UnwindUser(s, procs, absl::GetFlag(FLAGS_max_frames));

  absl::PrintF("sample %d:%d:%d  rip=0x%x rsp=0x%x rbp=0x%x  stack=%d bytes  maps=%d\n", ls->pid, ls->tid, ls->time,
               ls->rip, ls->rsp, ls->rbp, s.stack_dyn_size, ls->maps.size());
  absl::PrintF("stopped: %s", StopReasonName(r.reason));
  if (r.module)
    absl::PrintF("  at %s +0x%x", r.module->path(), r.module_vaddr);
  absl::PrintF("\n\n%-4s %-18s\n", "#", "pc");
  for (size_t i = 0; i < r.frames.size(); i++) {
    absl::PrintF("%-4d 0x%016x\n", i, r.frames[i]);
  }
  if (!ls->recorded_chain.empty() && ls->recorded_chain != r.frames) {
    absl::PrintF("\nNOTE: differs from the 'ours' chain recorded in the dump (%d frames)\n", ls->recorded_chain.size());
  }
  return 0;
}

struct Want {
  uint32_t pid, tid;
  uint64_t time;
};

int Convert() {
  const std::string in = absl::GetFlag(FLAGS_input);
  const std::string out = absl::GetFlag(FLAGS_output);
  if (in.empty() || out.empty()) {
    LOG(ERROR) << "need --input and --output";
    return 2;
  }

  auto pd_or = PerfData::Open(in.c_str());
  if (!pd_or.ok()) {
    LOG(ERROR) << "open input: " << pd_or.status();
    return 1;
  }
  std::unique_ptr<PerfData> pd = std::move(*pd_or);

  std::vector<Want> want;
  for (std::string_view t : absl::StrSplit(absl::GetFlag(FLAGS_dump_samples), ',', absl::SkipEmpty())) {
    std::vector<std::string_view> p = absl::StrSplit(t, ':');
    uint32_t pid, tid;
    uint64_t time;
    if (p.size() != 3 || !absl::SimpleAtoi(p[0], &pid) || !absl::SimpleAtoi(p[1], &tid) ||
        !absl::SimpleAtoi(p[2], &time)) {
      LOG(ERROR) << "--dump_samples entry not PID:TID:TIME: " << t;
      return 2;
    }
    want.push_back({pid, tid, time});
  }
  const std::string dump_dir = absl::GetFlag(FLAGS_dump_dir);

  ModuleCache modules;
  ProcessTable procs(&modules);
  PerfWriter writer(out, *pd);
  if (auto s = writer.Begin(); !s.ok()) {
    LOG(ERROR) << "writer.Begin: " << s;
    return 1;
  }

  // Phase 1: index every record and sort by timestamp. perf.data is only
  // loosely ordered (per-CPU buffers, ~one FINISHED_ROUND per second here),
  // so a strict file-order walk sees samples before the MMAP2 flood that
  // explains them.
  absl::Span<const uint8_t> data = pd->data();
  struct Ev {
    uint64_t time;
    uint32_t off;
  };
  std::vector<Ev> evs;
  evs.reserve(1 << 20);
  for (size_t pos = 0; pos + sizeof(perf_event_header) <= data.size();) {
    const auto* h = reinterpret_cast<const perf_event_header*>(data.data() + pos);
    if (h->size < sizeof(perf_event_header) || pos + h->size > data.size()) {
      LOG(ERROR) << "truncated record at " << pos;
      break;
    }
    evs.push_back({RecordTime(h), static_cast<uint32_t>(pos)});
    pos += h->size;
  }
  std::stable_sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) { return a.time < b.time; });

  // Phase 2: replay in time order -- update the process model and stream out.
  UnwindTally tally;
  const int max_frames = absl::GetFlag(FLAGS_max_frames);
  const int64_t max_samples = absl::GetFlag(FLAGS_max_samples);
  int64_t n_sample = 0, n_mmap = 0, n_other = 0;
  auto fail = [&](const absl::Status& s) {
    LOG(ERROR) << s;
    return 1;
  };

  for (const Ev& ev : evs) {
    const auto* h = reinterpret_cast<const perf_event_header*>(data.data() + ev.off);
    const uint8_t* body = reinterpret_cast<const uint8_t*>(h) + sizeof(perf_event_header);

    switch (h->type) {
      case PERF_RECORD_MMAP2:
        procs.HandleMmap(Rd<uint32_t>(body), Rd<uint64_t>(body + 8), Rd<uint64_t>(body + 16), Rd<uint64_t>(body + 24),
                         Rd<uint32_t>(body + 56), reinterpret_cast<const char*>(body + 64));
        n_mmap++;
        if (auto s = writer.WriteVerbatim(h); !s.ok())
          return fail(s);
        break;
      case PERF_RECORD_MMAP: {
        uint32_t prot = (h->misc & PERF_RECORD_MISC_MMAP_DATA) ? 0 : PROT_EXEC;
        procs.HandleMmap(Rd<uint32_t>(body), Rd<uint64_t>(body + 8), Rd<uint64_t>(body + 16), Rd<uint64_t>(body + 24),
                         prot, reinterpret_cast<const char*>(body + 32));
        n_mmap++;
        if (auto s = writer.WriteVerbatim(h); !s.ok())
          return fail(s);
        break;
      }
      case PERF_RECORD_FORK:
        procs.HandleFork(Rd<uint32_t>(body), Rd<uint32_t>(body + 4));
        n_other++;
        if (auto s = writer.WriteVerbatim(h); !s.ok())
          return fail(s);
        break;
      case PERF_RECORD_EXIT:
        procs.HandleExit(Rd<uint32_t>(body));
        n_other++;
        if (auto s = writer.WriteVerbatim(h); !s.ok())
          return fail(s);
        break;
      case PERF_RECORD_COMM:
        if (h->misc & PERF_RECORD_MISC_COMM_EXEC)
          procs.HandleExec(Rd<uint32_t>(body));
        n_other++;
        if (auto s = writer.WriteVerbatim(h); !s.ok())
          return fail(s);
        break;
      case PERF_RECORD_SAMPLE: {
        if (max_samples > 0 && n_sample >= max_samples)
          break;
        ParsedSample s = pd->ParseSample(h);
        UnwindResult r = UnwindUser(s, procs, max_frames);
        tally.Add(r);
        if (auto w = writer.WriteSample(h, s, r.frames); !w.ok())
          return fail(w);
        n_sample++;
        for (const Want& w : want) {
          if (w.pid == s.pid && w.tid == s.tid && w.time == s.time) {
            std::string p = absl::StrCat(dump_dir, "/", s.pid, "-", s.tid, "-", s.time, ".sample");
            if (!WriteSampleDump(p, *pd, s, procs.RangesFor(s.pid), r.frames)) {
              LOG(ERROR) << "failed writing " << p;
            } else {
              absl::FPrintF(stderr, "wrote %s (%d frames, stop: %s)\n", p, r.frames.size(), StopReasonName(r.reason));
            }
          }
        }
        break;
      }
      default:
        n_other++;
        if (auto s = writer.WriteVerbatim(h); !s.ok())
          return fail(s);
        break;
    }
  }

  if (auto s = writer.Finish(); !s.ok()) {
    LOG(ERROR) << "writer.Finish: " << s;
    return 1;
  }

  absl::FPrintF(stderr, "records: %d sample, %d mmap, %d other; %d pids\n", n_sample, n_mmap, n_other,
                procs.known_pids());
  absl::FPrintF(stderr, "modules: %d opened, %d failed\n", modules.opened(), modules.failed());
  absl::FPrintF(stderr, "unwind:  %d samples (%d no-user-regs), %d frames (%.1f/sample)\n", tally.samples,
                tally.no_user_regs, tally.frames, tally.samples ? double(tally.frames) / tally.samples : 0.0);
  absl::FPrintF(stderr, "  stop:");
  for (int i = 0; i < kNumStopReason; i++) {
    if (tally.stop[i]) {
      absl::FPrintF(stderr, "  %s=%d", StopReasonName(static_cast<StopReason>(i)), tally.stop[i]);
    }
  }
  absl::FPrintF(stderr, "\n");
  return 0;
}

int Run() {
  if (std::string p = absl::GetFlag(FLAGS_selftest_elf); !p.empty())
    return SelfTest(p);
  if (std::string p = absl::GetFlag(FLAGS_replay_sample); !p.empty())
    return ReplaySample(p);
  return Convert();
}

}  // namespace
}  // namespace perf_convert

int main(int argc, char** argv) {
  absl::SetProgramUsageMessage(
      "Rewrites a `perf record --call-graph dwarf` recording, replacing the raw\n"
      "stack dumps with plain callchains.\n\n"
      "Usage: perf-convert --input perf.data --output out.perf.data");
  absl::ParseCommandLine(argc, argv);
  absl::InitializeLog();
  absl::SetStderrThreshold(absl::LogSeverity::kWarning);
  return perf_convert::Run();
}
