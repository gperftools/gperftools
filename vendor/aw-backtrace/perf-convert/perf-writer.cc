/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#include "perf-writer.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "absl/strings/str_format.h"

namespace perf_convert {

namespace {
constexpr uint64_t kUserContext = static_cast<uint64_t>(PERF_CONTEXT_USER);

// exclude_callchain_user lives at bit 39 of the perf_event_attr bitfield word
// that starts at offsetof(perf_event_attr, read_format)+8. Rather than depend
// on the layout, flip it through a struct copy.
perf_event_attr PatchAttr(perf_event_attr a) {
  a.sample_type &= ~(uint64_t{PERF_SAMPLE_STACK_USER} | uint64_t{PERF_SAMPLE_REGS_USER});
  a.sample_regs_user = 0;
  a.sample_stack_user = 0;
  a.exclude_callchain_user = 0;
  return a;
}
}  // namespace

PerfWriter::PerfWriter(const std::string& out_path, const PerfData& src) : src_(src), out_path_(out_path) {
}

PerfWriter::~PerfWriter() {
  if (fd_ >= 0)
    close(fd_);
}

absl::Status PerfWriter::WriteAll(const void* p, size_t n) {
  const char* c = static_cast<const char*>(p);
  while (n > 0) {
    ssize_t w = write(fd_, c, n);
    if (w < 0) {
      if (errno == EINTR)
        continue;
      return absl::InternalError(absl::StrFormat("write: %s", strerror(errno)));
    }
    c += w;
    n -= w;
    off_ += w;
  }
  return absl::OkStatus();
}

absl::Status PerfWriter::Begin() {
  fd_ = open(out_path_.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd_ < 0) {
    return absl::InternalError(absl::StrFormat("open %s: %s", out_path_, strerror(errno)));
  }

  const PerfFileHeader& sh = src_.header();

  // [header placeholder][attr(attr_size) + ids_section][id array][data...]
  std::vector<uint8_t> head(sizeof(PerfFileHeader), 0);
  if (auto s = WriteAll(head.data(), head.size()); !s.ok())
    return s;

  uint64_t attrs_off = off_;
  perf_event_attr pa = PatchAttr(src_.attr());
  std::vector<uint8_t> attr_bytes(src_.event_attr_size(), 0);
  memcpy(attr_bytes.data(), &pa, std::min<size_t>(src_.event_attr_size(), sizeof(pa)));
  if (auto s = WriteAll(attr_bytes.data(), attr_bytes.size()); !s.ok())
    return s;

  // ids section points at the id array we write right after it.
  uint64_t ids_off = attrs_off + src_.event_attr_size() + sizeof(PerfFileSection);
  absl::Span<const uint8_t> id_bytes = src_.attr_ids_bytes();
  PerfFileSection ids_sec{ids_off, static_cast<uint64_t>(id_bytes.size())};
  if (auto s = WriteAll(&ids_sec, sizeof(ids_sec)); !s.ok())
    return s;
  if (!id_bytes.empty()) {
    if (auto s = WriteAll(id_bytes.data(), id_bytes.size()); !s.ok())
      return s;
  }
  (void)sh;

  data_off_ = off_;
  return absl::OkStatus();
}

absl::Status PerfWriter::WriteVerbatim(const perf_event_header* h) {
  return WriteAll(h, h->size);
}

absl::Status PerfWriter::WriteSample(const perf_event_header* orig, const ParsedSample& s,
                                     absl::Span<const uint64_t> user_frames) {
  ips_scratch_.clear();
  bool saw_user = false;
  for (uint64_t ip : s.callchain) {
    ips_scratch_.push_back(ip);
    if (ip == kUserContext) {
      saw_user = true;
      break;
    }
  }
  if (!saw_user)
    ips_scratch_.push_back(kUserContext);
  for (uint64_t f : user_frames) ips_scratch_.push_back(f);

  // body: ip | pid,tid | time | period | (nr, ips[nr])   (kernel field order)
  uint64_t nr = ips_scratch_.size();
  uint64_t body = 8 + 8 + 8 + 8 + 8 + nr * 8;
  perf_event_header nh{orig->type, orig->misc, static_cast<uint16_t>(sizeof(perf_event_header) + body)};

  std::vector<uint8_t> buf;
  buf.reserve(nh.size);
  auto put = [&](const void* p, size_t n) {
    const uint8_t* c = static_cast<const uint8_t*>(p);
    buf.insert(buf.end(), c, c + n);
  };
  put(&nh, sizeof(nh));
  put(&s.ip, 8);
  put(&s.pid, 4);
  put(&s.tid, 4);
  put(&s.time, 8);
  put(&s.period, 8);
  put(&nr, 8);
  put(ips_scratch_.data(), nr * 8);
  return WriteAll(buf.data(), buf.size());
}

absl::Status PerfWriter::Finish() {
  data_size_ = off_ - data_off_;

  // Feature section table then payloads, right after the data section.
  // Drop EVENT_DESC; perf falls back to the attrs section.
  std::vector<std::pair<int, PerfFileSection>> keep;
  for (auto& [bit, sec] : src_.features()) {
    if (bit == kHeaderEventDesc)
      continue;
    if (sec.offset + sec.size > src_.size())
      continue;  // skip corrupt
    keep.push_back({bit, sec});
  }

  uint64_t table_off = off_;
  std::vector<PerfFileSection> table(keep.size());
  std::vector<uint8_t> zeros(sizeof(PerfFileSection) * keep.size(), 0);
  if (auto st = WriteAll(zeros.data(), zeros.size()); !st.ok())
    return st;

  for (size_t i = 0; i < keep.size(); i++) {
    uint64_t payload_off = off_;
    if (auto st = WriteAll(src_.base() + keep[i].second.offset, keep[i].second.size); !st.ok())
      return st;
    table[i] = PerfFileSection{payload_off, keep[i].second.size};
  }

  if (pwrite(fd_, table.data(), sizeof(PerfFileSection) * table.size(), table_off) !=
      static_cast<ssize_t>(sizeof(PerfFileSection) * table.size())) {
    return absl::InternalError("pwrite feature table");
  }

  PerfFileHeader h = src_.header();
  h.attrs.offset = sizeof(PerfFileHeader);
  h.attrs.size = h.attr_size + sizeof(PerfFileSection);
  h.data.offset = data_off_;
  h.data.size = data_size_;
  h.event_types = PerfFileSection{0, 0};
  for (int b = 0; b < 4; b++) h.flags[b] = 0;
  for (auto& [bit, sec] : keep) h.flags[bit / 64] |= (uint64_t{1} << (bit % 64));

  if (pwrite(fd_, &h, sizeof(h), 0) != static_cast<ssize_t>(sizeof(h))) {
    return absl::InternalError("pwrite header");
  }
  if (close(fd_) != 0) {
    fd_ = -1;
    return absl::InternalError(absl::StrFormat("close: %s", strerror(errno)));
  }
  fd_ = -1;
  return absl::OkStatus();
}

}  // namespace perf_convert
