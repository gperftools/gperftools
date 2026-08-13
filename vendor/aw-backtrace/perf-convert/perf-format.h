/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#ifndef PERF_CONVERT_PERF_FORMAT_H_
#define PERF_CONVERT_PERF_FORMAT_H_

// The perf.data container format. The per-event structs (perf_event_attr,
// perf_event_header, the PERF_RECORD_* bodies, PERF_SAMPLE_* / PERF_CONTEXT_*
// constants) come from <linux/perf_event.h>; only the file-level wrappers,
// which live in perf's own util/header.h and are not in any uapi header, are
// redeclared here.

#include <linux/perf_event.h>
#include <stdint.h>

namespace perf_convert {

// "PERFILE2"
inline constexpr uint64_t kPerfMagic2 = 0x32454c4946524550ULL;

struct PerfFileSection {
  uint64_t offset;
  uint64_t size;
} __attribute__((packed));

struct PerfFileHeader {
  uint64_t magic;
  uint64_t size;       // sizeof(PerfFileHeader) == 104
  uint64_t attr_size;  // sizeof(one perf_file_attr) as written by the producer
  PerfFileSection attrs;
  PerfFileSection data;
  PerfFileSection event_types;  // unused in v2
  uint64_t flags[4];            // 256-bit HEADER_* feature bitmap
} __attribute__((packed));
static_assert(sizeof(PerfFileHeader) == 104);

// perf_file_attr: a perf_event_attr (attr_size bytes) followed by a section
// pointing at this event's u64 id array. We read attr separately since its
// size is taken from the file header, so this only names the trailing part.
struct PerfFileAttrIds {
  PerfFileSection ids;
} __attribute__((packed));

// HEADER_* feature bits (index into PerfFileHeader::flags). Only the ones we
// touch or skip past are named.
enum PerfHeaderBit {
  kHeaderTracingData = 1,
  kHeaderBuildId = 2,
  kHeaderHostname = 3,
  kHeaderOsRelease = 4,
  kHeaderVersion = 5,
  kHeaderArch = 6,
  kHeaderNrCpus = 7,
  kHeaderCpuDesc = 8,
  kHeaderCpuId = 9,
  kHeaderTotalMem = 10,
  kHeaderCmdline = 11,
  kHeaderEventDesc = 12,
  kHeaderCpuTopology = 13,
  kHeaderNumaTopology = 14,
  kHeaderBranchStack = 15,
  kHeaderPmuMappings = 16,
  kHeaderGroupDesc = 17,
  kHeaderLastFeature = 32,
  kHeaderFeatBits = 256,
};

// A context marker in a PERF_SAMPLE_CALLCHAIN array (values >= PERF_CONTEXT_MAX
// when read as unsigned).
inline bool IsContextMarker(uint64_t ip) {
  return ip >= static_cast<uint64_t>(PERF_CONTEXT_MAX);
}

}  // namespace perf_convert

#endif  // PERF_CONVERT_PERF_FORMAT_H_
