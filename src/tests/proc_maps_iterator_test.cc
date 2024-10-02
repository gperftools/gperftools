/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
 * Copyright (c) 2024, gperftools Contributors
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "config_for_unittests.h"

#include "base/proc_maps_iterator.h"

#include <stdio.h>

#include <string>

#include "base/generic_writer.h"
#include "gtest/gtest.h"

#if defined __ELF__
#include <link.h>
#define PRINT_DL_PHDRS
#endif

int variable;

// There is not much we can thoroughly test. But it is easy to test
// that we're seeing at least .bss bits. We can also check that we saw
// at least one executable mapping.
TEST(ProcMapsIteratorTest, ForEachMapping) {
  bool seen_variable = false;
  bool seen_executable = false;
  bool ok = tcmalloc::ForEachProcMapping([&] (const tcmalloc::ProcMapping& mapping) {
    const uintptr_t variable_addr = reinterpret_cast<uintptr_t>(&variable);
    if (mapping.start <= variable_addr && variable_addr <= mapping.end) {
      seen_variable = true;
    }
    if (std::string(mapping.flags).find_first_of('x') != std::string::npos) {
      seen_executable = true;
    }
  });
  ASSERT_TRUE(ok) << "failed to open proc/self/maps";
  ASSERT_TRUE(seen_variable);
  ASSERT_TRUE(seen_executable);
}

#ifdef PRINT_DL_PHDRS

std::string MapFlags(uintptr_t flags) {
  std::string ret;
  ret += (flags & PF_R) ? "r" : "-";
  ret += (flags & PF_W) ? "w" : "-";
  ret += (flags & PF_X) ? "x" : "-";
  if ((flags & ~uintptr_t{PF_R | PF_W | PF_X})) {
    ret += " + junk";
  }
  return ret;
}

std::string MapType(uintptr_t p_type) {
#define PT(c) do { if (p_type == c) return (#c); } while (false)
#ifdef PT_NULL
  PT(PT_NULL);
#endif
#ifdef PT_LOAD
  PT(PT_LOAD);
#endif
#ifdef PT_DYNAMIC
  PT(PT_DYNAMIC);
#endif
#ifdef PT_INTERP
  PT(PT_INTERP);
#endif
#ifdef PT_NOTE
  PT(PT_NOTE);
#endif
#ifdef PT_SHLIB
  PT(PT_SHLIB);
#endif
#ifdef PT_PHDR
  PT(PT_PHDR);
#endif
#ifdef PT_TLS
  PT(PT_TLS);
#endif
#ifdef PT_NUM
  PT(PT_NUM);
#endif
#ifdef PT_LOOS
  PT(PT_LOOS);
#endif
#ifdef PT_GNU_EH_FRAME
  PT(PT_GNU_EH_FRAME);
#endif
#ifdef PT_GNU_STACK
  PT(PT_GNU_STACK);
#endif
#ifdef PT_GNU_RELRO
  PT(PT_GNU_RELRO);
#endif
#ifdef PT_GNU_PROPERTY
  PT(PT_GNU_PROPERTY);
#endif
#ifdef PT_GNU_SFRAME
  PT(PT_GNU_SFRAME);
#endif
#ifdef PT_LOSUNW
  PT(PT_LOSUNW);
#endif
#ifdef PT_SUNWBSS
  PT(PT_SUNWBSS);
#endif
#ifdef PT_SUNWSTACK
  PT(PT_SUNWSTACK);
#endif
#ifdef PT_HISUNW
  PT(PT_HISUNW);
#endif
#ifdef PT_HIOS
  PT(PT_HIOS);
#endif
#ifdef PT_LOPROC
  PT(PT_LOPROC);
#endif
#ifdef PT_HIPROC
  PT(PT_HIPROC);
#endif
#undef PT
  return "(UNKNOWN)";
}

void DoPrintPHDRs() {
  printf("iterating phdrs:\n");
  int rv = dl_iterate_phdr([] (struct dl_phdr_info *info, size_t _size, void* _bogus) -> int {
    printf("Got info. at = %p, path = '%s', num_phdrs = %d\n",
           reinterpret_cast<void*>(static_cast<uintptr_t>(info->dlpi_addr)),
           info->dlpi_name,
           (int)info->dlpi_phnum);
    for (int i = 0; i < info->dlpi_phnum; i++) {
      printf(" phdr %d: type = 0x%zx (%s), offset = 0x%zx, vaddr = 0x%zx - 0x%zx, filesz = %zu, memsz = %zu, flags = 0x%zx (%s), align = 0x%zx\n",
             i,
             (uintptr_t)info->dlpi_phdr[i].p_type, MapType(info->dlpi_phdr[i].p_type).c_str(),
             (uintptr_t)info->dlpi_phdr[i].p_offset,
             (uintptr_t)info->dlpi_phdr[i].p_vaddr, (uintptr_t)info->dlpi_phdr[i].p_vaddr + info->dlpi_phdr[i].p_memsz,
             (uintptr_t)info->dlpi_phdr[i].p_filesz,
             (uintptr_t)info->dlpi_phdr[i].p_memsz,
             (uintptr_t)info->dlpi_phdr[i].p_flags, MapFlags(info->dlpi_phdr[i].p_flags).c_str(),
             (uintptr_t)info->dlpi_phdr[i].p_align);
    }
    return 0;
  }, nullptr);
  printf("dl_iterate rv = %d\n", rv);
}

#else
void DoPrintPHDRs() {}
#endif

TEST(ProcMapsIteratorTest, SaveMappingNonEmpty) {
  std::string s;
  {
    tcmalloc::StringGenericWriter writer(&s);
    tcmalloc::SaveProcSelfMaps(&writer);
  }
  // Lets at least ensure we got something
  ASSERT_NE(s.size(), 0);
  printf("Got the following:\n%s\n---\n", s.c_str());

  DoPrintPHDRs();
}
