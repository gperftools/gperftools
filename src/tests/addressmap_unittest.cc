// Copyright (c) 2005, Google Inc.
// All rights reserved.
// 
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
// 
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// ---
// Author: Sanjay Ghemawat

#include <vector>
#include <set>
#include <algorithm>
#include "addressmap-inl.h"
#include "base/logging.h"
#include "base/commandlineflags.h"

DEFINE_int32(iters, 20, "Number of test iterations");
DEFINE_int32(N, 100000,  "Number of elements to test per iteration");

using std::pair;
using std::make_pair;
using std::vector;
using std::set;
using std::random_shuffle;

static void SetCheckCallback(void* ptr, int val,
                             set<pair<void*, int> >* check_set) {
  check_set->insert(make_pair(ptr, val));
}

int main(int argc, char** argv) {
  // Get a bunch of pointers
  const int N = FLAGS_N;
  static const int kObjectLength = 19;
  vector<char*> ptrs;
  for (int i = 0; i < N; ++i) {
    ptrs.push_back(new char[kObjectLength]);
  }

  for (int x = 0; x < FLAGS_iters; ++x) {
    // Permute pointers to get rid of allocation order issues
    random_shuffle(ptrs.begin(), ptrs.end());

    AddressMap<int> map(malloc, free);
    int result;

    // Insert a bunch of entries
    for (int i = 0; i < N; ++i) {
      void* p = ptrs[i];
      CHECK(!map.Find(p, &result));
      map.Insert(p, i);
      CHECK(map.Find(p, &result));
      CHECK_EQ(result, i);
      map.Insert(p, i + N);
      CHECK(map.Find(p, &result));
      CHECK_EQ(result, i + N);
    }

    // Delete the even entries
    for (int i = 0; i < N; i += 2) {
      void* p = ptrs[i];
      CHECK(map.FindAndRemove(p, &result));
      CHECK_EQ(result, i + N);
    }

    // Lookup the odd entries and adjust them
    for (int i = 1; i < N; i += 2) {
      void* p = ptrs[i];
      CHECK(map.Find(p, &result));
      CHECK_EQ(result, i + N);
      map.Insert(p, i + 2*N);
      CHECK(map.Find(p, &result));
      CHECK_EQ(result, i + 2*N);
    }

    // Insert even entries back
    for (int i = 0; i < N; i += 2) {
      void* p = ptrs[i];
      map.Insert(p, i + 2*N);
      CHECK(map.Find(p, &result));
      CHECK_EQ(result, i + 2*N);
    }

    // Check all entries
    set<pair<void*, int> > check_set;
    map.Iterate(SetCheckCallback, &check_set);
    CHECK_EQ(check_set.size(), N);
    for (int i = 0; i < N; ++i) {
      void* p = ptrs[i];
      check_set.erase(make_pair(p, i + 2*N));
      CHECK(map.Find(p, &result));
      CHECK_EQ(result, i + 2*N);
    }
    CHECK_EQ(check_set.size(), 0);

  }

  for (int i = 0; i < N; ++i) {
    delete[] ptrs[i];
  }

  printf("PASS\n");
  return 0;
}
