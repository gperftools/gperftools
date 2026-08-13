/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#include "sample-dump.h"

#include <asm/perf_regs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include <fstream>
#include <sstream>
#include <string>

#include "absl/strings/str_format.h"

namespace perf_convert {

namespace {
const char* kX86RegName[] = {"ax", "bx", "cx", "dx", "si", "di", "bp",  "sp",  "ip",  "flags", "cs",  "ss",
                             "ds", "es", "fs", "gs", "r8", "r9", "r10", "r11", "r12", "r13",   "r14", "r15"};
constexpr int kNumX86Reg = sizeof(kX86RegName) / sizeof(kX86RegName[0]);
}  // namespace

ParsedSample LoadedSample::ToSample() const {
  ParsedSample s;
  s.pid = pid;
  s.tid = tid;
  s.time = time;
  s.period = period;
  s.regs_abi = 2;  // PERF_SAMPLE_REGS_ABI_64
  s.rip = rip;
  s.rsp = rsp;
  s.rbp = rbp;
  s.stack = reinterpret_cast<const uint8_t*>(stack_words.data());
  s.stack_dyn_size = stack_words.size() * 8;
  s.callchain = absl::MakeConstSpan(kernel_chain);
  return s;
}

void LoadedSample::PopulateProcessTable(ProcessTable* procs) const {
  for (const Mapping& m : maps) {
    procs->HandleMmap(pid, m.start, m.end - m.start, m.file_offset, PROT_EXEC, m.path);
  }
}

std::optional<LoadedSample> ReadSampleDump(const std::string& path) {
  std::ifstream in(path);
  if (!in)
    return std::nullopt;
  LoadedSample ls;
  std::string line;
  bool magic = false;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') {
      if (line.rfind("# perf-convert sample v1", 0) == 0)
        magic = true;
      continue;
    }
    std::istringstream ss(line);
    std::string tok;
    ss >> tok;
    auto hex = [&](const std::string& s) { return static_cast<uint64_t>(strtoull(s.c_str(), nullptr, 0)); };
    if (tok == "pid") {
      ss >> ls.pid;
    } else if (tok == "tid") {
      ss >> ls.tid;
    } else if (tok == "time") {
      ss >> ls.time;
    } else if (tok == "period") {
      ss >> ls.period;
    } else if (tok == "reg") {
      std::string name, val;
      ss >> name >> val;
      if (name == "ip")
        ls.rip = hex(val);
      else if (name == "sp")
        ls.rsp = hex(val);
      else if (name == "bp")
        ls.rbp = hex(val);
    } else if (tok == "map") {
      std::string a, b, c, p;
      ss >> a >> b >> c >> p;
      ls.maps.push_back({hex(a), hex(b), hex(c), p});
    } else if (tok == "stack") {
      std::string base, size;
      ss >> base >> size;  // base == rsp (already have it); size informational
    } else if (!tok.empty() && tok[0] == '+') {
      std::string val;
      ss >> val;
      ls.stack_words.push_back(hex(val));
    } else if (tok == "kernel") {
      std::string a;
      while (ss >> a) ls.kernel_chain.push_back(hex(a));
    } else if (tok == "ours") {
      std::string a;
      while (ss >> a) ls.recorded_chain.push_back(hex(a));
    }
  }
  if (!magic || ls.rsp == 0)
    return std::nullopt;
  return ls;
}

bool WriteSampleDump(const std::string& path, const PerfData& pd, const ParsedSample& s,
                     const std::vector<ProcessTable::RangeDump>& ranges, absl::Span<const uint64_t> our_chain) {
  FILE* f = fopen(path.c_str(), "w");
  if (!f)
    return false;

  absl::FPrintF(f, "# perf-convert sample v1\n");
  absl::FPrintF(f, "pid %d\n", s.pid);
  absl::FPrintF(f, "tid %d\n", s.tid);
  absl::FPrintF(f, "time %d\n", s.time);
  absl::FPrintF(f, "period %d\n", s.period);

  // Registers, in sample_regs_user mask-bit order (same order as s.regs[]).
  const uint64_t mask = pd.sample_regs_user();
  if (s.regs && s.regs_abi != 0) {
    int i = 0;
    for (int r = 0; r < 64 && i < s.n_regs; r++) {
      if (!(mask & (uint64_t{1} << r)))
        continue;
      absl::FPrintF(f, "reg %-5s 0x%016x\n", (r < kNumX86Reg) ? kX86RegName[r] : "?", s.regs[i]);
      i++;
    }
  }

  for (const auto& rg : ranges) {
    absl::FPrintF(f, "map 0x%012x 0x%012x 0x%012x %s\n", rg.start, rg.end, rg.file_offset, rg.path);
  }

  absl::FPrintF(f, "stack 0x%016x %d\n", s.rsp, s.stack_dyn_size);
  for (uint64_t off = 0; off + 8 <= s.stack_dyn_size; off += 8) {
    uint64_t w;
    memcpy(&w, s.stack + off, 8);
    absl::FPrintF(f, "+%04x 0x%016x\n", off, w);
  }

  absl::FPrintF(f, "kernel");
  for (uint64_t ip : s.callchain) absl::FPrintF(f, " 0x%x", ip);
  absl::FPrintF(f, "\n");

  absl::FPrintF(f, "ours");
  for (uint64_t ip : our_chain) absl::FPrintF(f, " 0x%x", ip);
  absl::FPrintF(f, "\n");

  bool ok = ferror(f) == 0;
  return fclose(f) == 0 && ok;
}

}  // namespace perf_convert
