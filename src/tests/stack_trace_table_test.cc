// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
// Copyright 2009 Google Inc. All Rights Reserved.
// Author: fikes@google.com (Andrew Fikes)
//
// Use of this source code is governed by a BSD-style license that can
// be found in the LICENSE file.


#include "config_for_unittests.h"

#include "stack_trace_table.h"

#include <stdio.h>   // for puts()

#include <vector>

#include "base/logging.h"

class StackTraceTableTestHelper {
public:
  using StackTrace = tcmalloc::StackTrace;

  struct Entry {
    const StackTrace trace;
    std::unique_ptr<Entry> next{};
    Entry(const StackTrace& t) : trace(t) {}
  };
  using EntryPtr = std::unique_ptr<Entry>;

  void AddTrace(const StackTrace& t) {
    EntryPtr e{new Entry{t}};
    head_.swap(e->next);
    head_.swap(e);
  }

  std::unique_ptr<void*[]> DumpTraces() {
    auto retval = ProduceStackTracesDump(
      +[] (const void** current_head) {
        const Entry* head = static_cast<const Entry*>(*current_head);
        *current_head = head->next.get();
        return &head->trace;
      }, head_.get());

    head_.reset();
    return retval;
  }

  void CheckTracesAndReset(const uintptr_t* expected, int len) {
    std::unique_ptr<void*[]> entries = DumpTraces();
    for (int i = 0; i < len; i++) {
      CHECK_EQ(reinterpret_cast<uintptr_t>(entries[i]), expected[i]);
    }
  }
private:
  EntryPtr head_;
};

int main() {
  StackTraceTableTestHelper h;

  // Empty table
  static const uintptr_t k1[] = {0};
  h.CheckTracesAndReset(k1, arraysize(k1));

  tcmalloc::StackTrace t1;
  t1.size = static_cast<uintptr_t>(1024);
  t1.depth = static_cast<uintptr_t>(2);
  t1.stack[0] = reinterpret_cast<void*>(1);
  t1.stack[1] = reinterpret_cast<void*>(2);

  tcmalloc::StackTrace t2;
  t2.size = static_cast<uintptr_t>(512);
  t2.depth = static_cast<uintptr_t>(2);
  t2.stack[0] = reinterpret_cast<void*>(2);
  t2.stack[1] = reinterpret_cast<void*>(1);

  // Table w/ just t1
  h.AddTrace(t1);
  static const uintptr_t k2[] = {1, 1024, 2, 1, 2, 0};
  h.CheckTracesAndReset(k2, arraysize(k2));

  // Table w/ t1, t2
  h.AddTrace(t1);
  h.AddTrace(t2);
  static const uintptr_t k3[] = {1, 512, 2, 2, 1, 1, 1024, 2, 1, 2, 0};
  h.CheckTracesAndReset(k3, arraysize(k3));

  // Table w/ t1, t3
  // Same stack as t1, but w/ different size
  tcmalloc::StackTrace t3;
  t3.size = static_cast<uintptr_t>(2);
  t3.depth = static_cast<uintptr_t>(2);
  t3.stack[0] = reinterpret_cast<void*>(1);
  t3.stack[1] = reinterpret_cast<void*>(2);

  h.AddTrace(t1);
  h.AddTrace(t3);
  static const uintptr_t k5[] = {1, 2, 2, 1, 2, 1, 1024, 2, 1, 2, 0};
  h.CheckTracesAndReset(k5, arraysize(k5));

  puts("PASS");
}
