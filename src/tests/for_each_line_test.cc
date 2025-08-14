/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
 * Copyright (c) 2025, gperftools Contributors
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

#include "base/for_each_line.h"

#include <stdio.h>

#include <string>
#include <vector>

#include "gtest/gtest.h"

static constexpr const char* kBasicExample[] = {
  "short",
  "562c039e8000-562c039f0000 r--p 00000000 00:00 2354112436 /home/me/src/External/gperftools/proc_maps_iterator_test",
  "562c039f0000-562c03a35000 r-xp 00008000 00:00 2354112436 /home/me/src/External/gperftools/proc_maps_iterator_test",
  "562c03a35000-562c03a4b000 r--p 0004d000 00:00 2354112436 /home/me/src/External/gperftools/proc_maps_iterator_test",
  "562c03a4b000-562c03a4d000 r--p 00062000 00:00 2354112436 /home/me/src/External/gperftools/proc_maps_iterator_test",
  "562c03a4d000-562c03a4e000 rw-p 00064000 00:00 2354112436 /home/me/src/External/gperftools/proc_maps_iterator_test",
  "562c1f8fc000-562c1f91d000 rw-p 00000000 00:00 0 [heap]",
  "7f7987aed000-7f7987b15000 r--p 00000000 00:00 1395652429 /usr/lib/x86_64-linux-gnu/libc.so.6",
  "7f7987b15000-7f7987c7a000 r-xp 00028000 00:00 1395652429 /usr/lib/x86_64-linux-gnu/libc.so.6",
  "7f7987c7a000-7f7987cd0000 r--p 0018d000 00:00 1395652429 /usr/lib/x86_64-linux-gnu/libc.so.6",
  "7f7987cd0000-7f7987cd4000 r--p 001e2000 00:00 1395652429 /usr/lib/x86_64-linux-gnu/libc.so.6",
  "7f7987cd4000-7f7987cd6000 rw-p 001e6000 00:00 1395652429 /usr/lib/x86_64-linux-gnu/libc.so.6",
  "7f7987cd6000-7f7987ce3000 rw-p 00000000 00:00 0",
  "7f7987ce3000-7f7987ce7000 r--p 00000000 00:00 1338727929 /usr/lib/x86_64-linux-gnu/libgcc_s.so.1",
  "7f7987ce7000-7f7987d0a000 r-xp 00004000 00:00 1338727929 /usr/lib/x86_64-linux-gnu/libgcc_s.so.1",
  "7f7987d0a000-7f7987d0e000 r--p 00027000 00:00 1338727929 /usr/lib/x86_64-linux-gnu/libgcc_s.so.1",
  "7f7987d0e000-7f7987d0f000 r--p 0002a000 00:00 1338727929 /usr/lib/x86_64-linux-gnu/libgcc_s.so.1",
  "7f7987d0f000-7f7987d10000 rw-p 0002b000 00:00 1338727929 /usr/lib/x86_64-linux-gnu/libgcc_s.so.1",
  "7f7987d10000-7f7987d21000 r--p 00000000 00:00 1395652475 /usr/lib/x86_64-linux-gnu/libm.so.6",
  "7f7987d21000-7f7987d9e000 r-xp 00011000 00:00 1395652475 /usr/lib/x86_64-linux-gnu/libm.so.6",
  "7f7987d9e000-7f7987dfe000 r--p 0008e000 00:00 1395652475 /usr/lib/x86_64-linux-gnu/libm.so.6",
  "7f7987dfe000-7f7987dff000 r--p 000ed000 00:00 1395652475 /usr/lib/x86_64-linux-gnu/libm.so.6",
  "7f7987dff000-7f7987e00000 rw-p 000ee000 00:00 1395652475 /usr/lib/x86_64-linux-gnu/libm.so.6",
  "7f7987e00000-7f7987ea2000 r--p 00000000 00:00 1338726433 /usr/lib/x86_64-linux-gnu/libstdc++.so.6.0.34",
  "7f7987ea2000-7f7987fd2000 r-xp 000a2000 00:00 1338726433 /usr/lib/x86_64-linux-gnu/libstdc++.so.6.0.34",
  "7f7987fd2000-7f7988060000 r--p 001d2000 00:00 1338726433 /usr/lib/x86_64-linux-gnu/libstdc++.so.6.0.34",
  "7f7988060000-7f798806f000 r--p 0025f000 00:00 1338726433 /usr/lib/x86_64-linux-gnu/libstdc++.so.6.0.34",
  "7f798806f000-7f7988072000 rw-p 0026e000 00:00 1338726433 /usr/lib/x86_64-linux-gnu/libstdc++.so.6.0.34",
  "7f7988072000-7f7988076000 rw-p 00000000 00:00 0",
  "7f7988082000-7f7988087000 rw-p 00000000 00:00 0",
  "7f79880b5000-7f79880b7000 rw-p 00000000 00:00 0",
  "7f79880b7000-7f79880bb000 r--p 00000000 00:00 0 [vvar]",
  "7f79880bb000-7f79880bd000 r--p 00000000 00:00 0 [vvar_vclock]",
  "7f79880bd000-7f79880bf000 r-xp 00000000 00:00 0 [vdso]",
  "7f79880bf000-7f79880c0000 r--p 00000000 00:00 1338729117 /usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2",
  "7f79880c0000-7f79880e8000 r-xp 00001000 00:00 1338729117 /usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2",
  "7f79880e8000-7f79880f3000 r--p 00029000 00:00 1338729117 /usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2",
  "7f79880f3000-7f79880f5000 r--p 00034000 00:00 1338729117 /usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2",
  "7f79880f5000-7f79880f6000 rw-p 00036000 00:00 1338729117 /usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2",
  "7f79880f6000-7f79880f7000 rw-p 00000000 00:00 0",
  "7fff4dd6c000-7fff4dd8d000 rw-p 00000000 00:00 0 [stack]"
};

struct StringReader {
  mutable std::string_view sv;
  explicit StringReader(std::string_view sv) : sv{sv} {}
  int operator()(void* buf, int count) const {
    count = std::min<int>(count, sv.size());
    memcpy(buf, sv.data(), count);
    sv.remove_prefix(count);
    return count;
  }
};

class ForEachLineTest : public testing::Test {
protected:
  ForEachLineTest() : lines_{kBasicExample, kBasicExample + arraysize(kBasicExample)} {
    for (const std::string& l : lines_) {
      example_.append(l).push_back('\n');
    }
  }

  void CompareEachLine() {
    int i = 0;
    tcmalloc::ForEachLine<120>(StringReader{example_},
                               [&] (char* begin, char* end) -> bool {
                                 std::string_view l{begin, static_cast<size_t>(end - begin)};
                                 // printf("l: %.*s\n", (int)l.size(), l.data());
                                 EXPECT_EQ(l, lines_[i++]);
                                 return true;
                               });
    // printf("checked %d lines\n", i);
    EXPECT_EQ(i, 41);
  }

  const std::vector<std::string> lines_;
  std::string example_;
};

TEST_F(ForEachLineTest, Basic) {
  CompareEachLine();
}

TEST_F(ForEachLineTest, NoLastEOL) {
  EXPECT_EQ(*example_.rbegin(), '\n');
  example_.pop_back(); // remove last \n
  EXPECT_NE(*example_.rbegin(), '\n');

  CompareEachLine();
}

TEST_F(ForEachLineTest, ShortBuffer) {
  std::vector<std::string_view> got;
  bool ok = tcmalloc::ForEachLine<20>(
    StringReader{example_},
    [&] (char* begin, char* end) -> bool {
      got.emplace_back(begin, static_cast<size_t>(end - begin));
      return true;
    });
  EXPECT_EQ(ok, false);
  EXPECT_EQ(got.size(), 1);
}
