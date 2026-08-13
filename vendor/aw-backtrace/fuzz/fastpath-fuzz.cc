/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
//
// libFuzzer target for the fast-path .eh_frame decoder
// (aw-backtrace-fastpath.h :: TryFastFrameInfo).
//

#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "aw-backtrace-fastpath.h"
#include "check.h"
#include "fuzz-common.h"
#include "v/mini-x86-int/base/cleanup.h"

using aw_backtrace_internal::EHReaderInputs;
using aw_backtrace_internal::FrameInfo;
using aw_backtrace_internal::fastpath::FastPathFrame;
using aw_backtrace_internal::fastpath::IdentityXlate;
using aw_backtrace_internal::fastpath::TryFastFrameInfo;

void CheckExtraFail(const char* cond_str, const char* extra, const char* file, int line) {
  aw_backtrace_internal::CheckFail::PrintfAndDie("Check fail at %s:%d: cond: %s, %s: %s\n", file, line, cond_str, extra,
                                                 strerror(errno));
}

#define CHECK_PERROR(cond, extra)                         \
  do {                                                    \
    if (!(cond)) {                                        \
      CheckExtraFail(#cond, (extra), __FILE__, __LINE__); \
    }                                                     \
  } while (0)

const uint8_t* MustMMAP(void* hint, size_t len, int prot, int flags, int fd, off_t offset) {
  void* res = mmap(hint, len, prot, flags, fd, offset);
  CHECK_PERROR(res != MAP_FAILED, "mmap");
  return static_cast<const uint8_t*>(res);
}

uint8_t* LeakAllocation(size_t amount) {
  struct R {
    uint8_t* ptr;
    R* next;
  };
  // Keep asan happy about our leak
  static R* r;

  uint8_t* ret = new uint8_t[amount];
  r = new R{ret, r};
  return ret;
}

template <typename R>
R OpenAndStat(std::string path, const std::function<R(int fd, struct stat* st)>& body) {
  int fd = open(path.c_str(), O_RDONLY);
  CHECK_PERROR(fd >= 0, "open");
  tcmalloc::Cleanup close_fd{[fd]() { close(fd); }};

  struct stat st;
  CHECK_PERROR(fstat(fd, &st) == 0, path.c_str());

  return body(fd, &st);
}

void FillPCOffsets(XlateData* data) {
  struct EHFrameHDR {
    uint8_t ver;
    uint8_t enc1;
    uint8_t enc2;
    uint8_t enc3;
    int32_t table_offset;
    uint32_t fde_count;
    struct {
      int32_t pc_offset;
      int32_t fde_offset;
    } table[1];
  };
  const auto& eh = *reinterpret_cast<const EHFrameHDR*>(data->data);
  CHECK(eh.ver == 1 && eh.enc1 == 0x1b && eh.enc2 == 0x03 && eh.enc3 == 0x3b);

  uint32_t fde_count = eh.fde_count;
  CHECK(fde_count > 0);

  // auto size = (const uint8_t*)(&table[2 * fde_count]) - &h->version;
  // CHECK((size_t)size <= (size_t)eh_f_hdr_s->sh_size);
  uintptr_t lo_off = eh.table[0].pc_offset + offsetof(EHFrameHDR, table);
  auto end_idx = fde_count - 1;
  uintptr_t hi_off = eh.table[end_idx].pc_offset + offsetof(EHFrameHDR, table) + sizeof(eh.table) * end_idx;

  // Offsets we store are from the start of eh_frame_hdr
  data->low_pc_offset = lo_off;
  data->high_pc_offset = hi_off;
}

// Load .eh_frame{,_hdr} bits from given ELF file. We make sure there
// is some guards around actual data to trap any out of bounds
// accesses.
XlateData LoadELF(std::string path) {
  return OpenAndStat<XlateData>(path, [](int fd, struct stat* st) -> XlateData {
    const uint8_t* everything = MustMMAP(nullptr, st->st_size, PROT_READ, MAP_SHARED, fd, 0);

    const auto& elf_hdr = *reinterpret_cast<const Elf64_Ehdr*>(everything);
    std::string_view magic{(const char*)&elf_hdr.e_ident, SELFMAG};
    CHECK(magic == ELFMAG || elf_hdr.e_ident[EI_CLASS] == ELFCLASS64);

    const Elf64_Shdr* eh_f_s = nullptr;
    const Elf64_Shdr* eh_f_hdr_s = nullptr;
    {
      const auto* sh = reinterpret_cast<const Elf64_Shdr*>(everything + elf_hdr.e_shoff);
      const char* shstr = reinterpret_cast<const char*>(everything + sh[elf_hdr.e_shstrndx].sh_offset);
      for (unsigned i = 0; i < elf_hdr.e_shnum; i++) {
        const char* name = shstr + sh[i].sh_name;
        if (name == std::string_view{".eh_frame_hdr"}) {
          eh_f_hdr_s = sh + i;
        } else if (name == std::string_view{".eh_frame"}) {
          eh_f_s = sh + i;
        }
      }
    }
    CHECK(eh_f_hdr_s && eh_f_s);
    CHECK(eh_f_hdr_s->sh_addr < eh_f_s->sh_addr);

    // size that covers both sections at whatever addresses they have
    // (usually just back to back, but sometimes with a small alignment
    // gap).
    size_t total_size = (eh_f_s->sh_addr - eh_f_hdr_s->sh_addr) + eh_f_s->sh_size;

    CHECK(total_size < size_t{1} << 30);

    const uint8_t* mapped_hdr;

    if (false) {
      size_t kMappingSize = size_t{2} << 40;
      const uint8_t* mapping =
          MustMMAP(nullptr, kMappingSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);

      size_t offset = (kMappingSize - total_size) / 2;
      offset &= ~size_t{4095};
      size_t rounding = eh_f_hdr_s->sh_addr & 4095;

      mapped_hdr = mapping + offset + rounding;

      (void)MustMMAP((void*)(mapping + offset), total_size + rounding, PROT_READ, MAP_SHARED | MAP_FIXED, fd,
                     eh_f_hdr_s->sh_offset - rounding);
    } else {
      auto data = LeakAllocation(total_size);
      memcpy(data, everything + eh_f_hdr_s->sh_offset, total_size);
      mapped_hdr = data;
    }

    XlateData ret;
    ret.data = mapped_hdr;
    ret.data_size = total_size;
    FillPCOffsets(&ret);
    return ret;
  });
}

XlateData LoadEHFrameData(std::string path) {
  return OpenAndStat<XlateData>(std::move(path), [](int fd, struct stat* st) {
    size_t size = st->st_size;
    uint8_t* bytes = LeakAllocation(size);
    CHECK((int)size == read(fd, bytes, size));
    XlateData xd;
    xd.data = bytes;
    xd.data_size = size;
    FillPCOffsets(&xd);
    return xd;
  });
}

std::vector<XlateData> all_datas;

// poor-man's --flag=value parser. If the arg is in the position idx
// then we parse the value and return it.
std::optional<std::string> EatLongArg(std::string_view prefix, int idx, int* pargc, char** argv) {
  std::string_view arg = argv[idx];
  if (!arg.starts_with(prefix))
    return {};

  std::optional<std::string> maybe_res = {std::string{arg.substr(prefix.size())}};
  if (!maybe_res)
    return maybe_res;
  for (; idx + 1 < *pargc; idx++) {
    argv[idx] = argv[idx + 1];
  }
  argv[idx] = nullptr;
  (*pargc)--;
  return maybe_res;
}

extern "C" int LLVMFuzzerInitialize(int* pargc, char*** pargv) {
  std::optional<std::string> first_loaded_path;
  auto report_loading = [&](const char* what, const std::string& s) {
    if (!first_loaded_path)
      first_loaded_path = s;
    fprintf(stderr, "Loading .eh_frame{,_hdr} from %s at %s\n", what, s.c_str());
  };
  {
    int i = 1;
    while (i < *pargc) {
      if (auto maybe_elf = EatLongArg("--load-elf=", i, pargc, *pargv)) {
        report_loading("elf file", maybe_elf.value());
        all_datas.emplace_back(LoadELF(std::move(maybe_elf).value()));
        continue;
      }
      if (auto maybe_eh = EatLongArg("--load-data=", i, pargc, *pargv)) {
        report_loading("raw sections", maybe_eh.value());
        all_datas.emplace_back(LoadEHFrameData(std::move(maybe_eh).value()));
        continue;
      }
      i++;
    }
  }

  // if no --load-xyz args given, then we use our "golden" set
  // captured from my system's libraries.
  if (!first_loaded_path) {
    std::string_view stock_data[] = {"fuzz/data/libc", "fuzz/data/libc++", "fuzz/data/libstdc++"};
    for (std::string_view path : stock_data) {
      std::string s{path};
      report_loading("raw sections", s);
      all_datas.emplace_back(LoadEHFrameData(std::move(s)));
    }
  }

  FastPathFrame ff;

  auto good_info = [](FastPathFrame ff) {
    FrameInfo fi;
    return ff != FastPathFrame::Failure() && ff.ToFrameInfo(&fi);
  };

  XlateData& first_data = all_datas[0];

#ifndef FUZZ_SKIP_IDENTITY
  {
    // Quick "smoke test" that the we're to read some frame info using normal logic.
    auto addr = reinterpret_cast<uintptr_t>(first_data.data);
    EHReaderInputs i = MakeInputs(addr, first_data.data_size);
    ff = TryFastFrameInfo(i.eh_frame_start, i.eh_frame_end, i.eh_frame_hdr,
                          reinterpret_cast<uintptr_t>(first_data.data) + first_data.low_pc_offset, IdentityXlate{});
    CHECK(good_info(ff));
  }
#endif

  ff = Run(&first_data, first_data.low_pc_offset);
  CHECK(good_info(ff));

  // ff = Run(&g_data, (g_data.low_pc_offset + g_data.high_pc_offset) / 2);
  // CHECK(good_info(ff));

  fprintf(stderr, "smoke test cleanups size: %zu\n", first_data.cleanups.size());
  fprintf(stderr, "fastpath-fuzz: smoke ok (%s) lo_off=%zu hi_off=%zu size=%zu\n", first_loaded_path->c_str(),
          first_data.low_pc_offset, first_data.high_pc_offset, first_data.data_size);

  return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* _data, size_t size) {
  struct __attribute__((packed)) FuzzData {
    uint32_t fuzz_pc;
    uint32_t data_set;
    Mut mutations[];
  };
  const FuzzData* data = reinterpret_cast<const FuzzData*>(_data);

  if (size < sizeof(FuzzData))
    return 0;

  XlateData& xd = all_datas[data->data_set % all_datas.size()];

  xd.cleanups.clear();
  xd.mutations.clear();

  size_t i = 0;
  while (offsetof(FuzzData, mutations[i + 1]) <= size) {
    if (data->mutations[i].off < xd.data_size)
      xd.mutations.push_back(data->mutations[i]);
    // g_data.mutations.back().off %= g_data.data_size;
    i++;
  }

  uint64_t pc_off = data->fuzz_pc % (xd.high_pc_offset + 16384);
  FastPathFrame ff = Run(&xd, pc_off);

  if (!(ff == FastPathFrame::Failure())) {
    FrameInfo fi;
    (void)ff.ToFrameInfo(&fi);
  }
  return 0;
}
