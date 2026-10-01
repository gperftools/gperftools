// SPDX-License-Identifier: 0BSD
#ifndef SYM_HELPER_PROTOCOL_H_
#define SYM_HELPER_PROTOCOL_H_
#include <stddef.h>
#include <stdint.h>

// Wire format for the response we write back into the request's own
// memfd.  Shared between symbolize-backtrace.cc and
// sym-helper.cc. See that file's copy for the full rationale, in
// particular for why `magic` is committed via a separate, final write
// rather than being part of the same write as the rest of the header.
inline constexpr uint32_t kSymResponseMagic = 0x53594d31;  // "SYM1"

struct SymResponseHeader {
  uint32_t magic;
  uint32_t entry_count;
};

struct SymRequest {
  size_t count;
  uint64_t vaddrs[];
  // char proc_maps[];
};

struct SymResponseEntry {
  uint32_t addr_index;
  uint32_t lineno;
  uint32_t inlined;  // 0 or 1 -- a plain uint32_t keeps the layout trivial
  uint32_t function_off, function_len;
  uint32_t filename_off, filename_len;
  uint32_t module_off, module_len;
  uint64_t vaddr;
};

#endif  // SYM_HELPER_PROTOCOL_H_
