/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#ifndef SIMPLE_COUNTER_H_
#define SIMPLE_COUNTER_H_

#include <stdint.h>

#include <atomic>

namespace aw_backtrace_internal {

class SimpleCounter {
 public:
  constexpr SimpleCounter() = default;

  void Add(uintptr_t by = 1) {
    value.fetch_add(by, std::memory_order_relaxed);
  }
  uintptr_t Load() const {
    return value.load(std::memory_order_relaxed);
  }

 private:
  std::atomic<uintptr_t> value;
};

}  // namespace aw_backtrace_internal

#endif  // SIMPLE_COUNTER_H_
