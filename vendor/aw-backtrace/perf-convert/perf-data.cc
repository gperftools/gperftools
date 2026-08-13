/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#include "perf-data.h"

#include <asm/perf_regs.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "absl/strings/str_format.h"

namespace perf_convert {

namespace {
constexpr uint64_t kExpectedSampleType = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_CALLCHAIN |
                                         PERF_SAMPLE_PERIOD | PERF_SAMPLE_REGS_USER | PERF_SAMPLE_STACK_USER;

template <typename T>
T Read(const uint8_t* p) {
  T v;
  memcpy(&v, p, sizeof(T));
  return v;
}
}  // namespace

PerfData::~PerfData() {
  if (base_ != nullptr) {
    munmap(const_cast<uint8_t*>(base_), size_);
  }
}

absl::StatusOr<std::unique_ptr<PerfData>> PerfData::Open(const std::string& path) {
  int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return absl::NotFoundError(absl::StrFormat("open %s: %s", path, strerror(errno)));
  }
  struct stat st;
  if (fstat(fd, &st) != 0) {
    close(fd);
    return absl::InternalError("fstat failed");
  }
  void* m = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (m == MAP_FAILED) {
    return absl::InternalError(absl::StrFormat("mmap %s: %s", path, strerror(errno)));
  }

  auto pd = std::unique_ptr<PerfData>(new PerfData);
  pd->base_ = static_cast<const uint8_t*>(m);
  pd->size_ = st.st_size;

  if (pd->size_ < sizeof(PerfFileHeader)) {
    return absl::InvalidArgumentError("file smaller than perf header");
  }
  pd->hdr_ = Read<PerfFileHeader>(pd->base_);
  if (pd->hdr_.magic != kPerfMagic2) {
    return absl::InvalidArgumentError("not a PERFILE2 perf.data (compressed or v1?)");
  }
  const PerfFileHeader& h = pd->hdr_;
  if (h.attrs.offset + h.attrs.size > pd->size_ || h.data.offset + h.data.size > pd->size_) {
    return absl::InvalidArgumentError("attrs/data section out of bounds");
  }

  // Attrs. h.attr_size is the size of a whole perf_file_attr entry
  // (perf_event_attr + the trailing ids PerfFileSection). We support exactly
  // one evsel.
  if (h.attr_size <= sizeof(PerfFileSection) || h.attrs.size != h.attr_size) {
    return absl::UnimplementedError(
        absl::StrFormat("expected exactly one evsel (attrs.size=%d, attr_size=%d)", h.attrs.size, h.attr_size));
  }
  pd->event_attr_size_ = h.attr_size - sizeof(PerfFileSection);
  if (pd->event_attr_size_ < PERF_ATTR_SIZE_VER3) {
    return absl::UnimplementedError(absl::StrFormat("perf_event_attr too old: %d bytes", pd->event_attr_size_));
  }
  uint32_t copy = std::min<uint32_t>(pd->event_attr_size_, sizeof(perf_event_attr));
  memcpy(&pd->attr_, pd->base_ + h.attrs.offset, copy);
  pd->ids_section_ = Read<PerfFileSection>(pd->base_ + h.attrs.offset + pd->event_attr_size_);
  pd->ids_off_ = pd->ids_section_.offset;
  pd->ids_len_ = pd->ids_section_.size;

  if (pd->attr_.sample_type != kExpectedSampleType) {
    return absl::UnimplementedError(
        absl::StrFormat("sample_type 0x%x unsupported (want 0x%x: IP|TID|TIME|CALLCHAIN|PERIOD|REGS_USER|STACK_USER)",
                        pd->attr_.sample_type, kExpectedSampleType));
  }

  // Feature section table sits right after the data section, one
  // PerfFileSection per set HEADER_* bit, ascending.
  const uint8_t* ft = pd->base_ + h.data.offset + h.data.size;
  for (int bit = 0; bit < kHeaderFeatBits; bit++) {
    if (!(h.flags[bit / 64] & (uint64_t{1} << (bit % 64)))) {
      continue;
    }
    if (ft + sizeof(PerfFileSection) > pd->base_ + pd->size_) {
      return absl::InvalidArgumentError("feature section table out of bounds");
    }
    pd->features_.emplace_back(bit, Read<PerfFileSection>(ft));
    ft += sizeof(PerfFileSection);
  }

  return pd;
}

absl::Span<const uint8_t> PerfData::data() const {
  return {base_ + hdr_.data.offset, hdr_.data.size};
}

ParsedSample PerfData::ParseSample(const perf_event_header* hh) const {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(hh) + sizeof(perf_event_header);
  const uint8_t* end = reinterpret_cast<const uint8_t*>(hh) + hh->size;
  ParsedSample s;

  s.ip = Read<uint64_t>(p);
  p += 8;
  s.pid = Read<uint32_t>(p);
  s.tid = Read<uint32_t>(p + 4);
  p += 8;
  s.time = Read<uint64_t>(p);
  p += 8;

  // PERF_SAMPLE_PERIOD precedes PERF_SAMPLE_CALLCHAIN in the kernel's
  // perf_output_sample() order.
  s.period = Read<uint64_t>(p);
  p += 8;

  uint64_t nr = Read<uint64_t>(p);
  p += 8;
  s.callchain = absl::MakeConstSpan(reinterpret_cast<const uint64_t*>(p), nr);
  p += 8 * nr;

  s.regs_abi = Read<uint64_t>(p);
  p += 8;
  if (s.regs_abi != 0) {
    const uint64_t mask = attr_.sample_regs_user;
    auto idx = [&](int reg) { return __builtin_popcountll(mask & ((uint64_t{1} << reg) - 1)); };
    const auto* r = reinterpret_cast<const uint64_t*>(p);
    s.regs = r;
    s.n_regs = n_user_regs();
    s.rbp = r[idx(PERF_REG_X86_BP)];
    s.rsp = r[idx(PERF_REG_X86_SP)];
    s.rip = r[idx(PERF_REG_X86_IP)];
    p += 8 * n_user_regs();
  }

  uint64_t ssize = Read<uint64_t>(p);
  p += 8;
  if (ssize != 0) {
    s.stack = p;
    p += ssize;
    s.stack_dyn_size = Read<uint64_t>(p);
    p += 8;
  }
  (void)end;  // p should == end; trusted input.
  return s;
}

}  // namespace perf_convert
