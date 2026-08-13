/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
//
// Originally authored for gperftools and relicensed to 0BSD by the author.
#ifndef BASE_STATIC_STORAGE_H_
#define BASE_STATIC_STORAGE_H_
// #include "config.h"

#include <stdint.h>

#include <utility>

namespace aw_backtrace_internal {

template <typename T>
class StaticStorage {
 public:
  T* get() {
    return reinterpret_cast<T*>(bytes_);
  }
  const T* get() const {
    return reinterpret_cast<const T*>(bytes_);
  }

  template <class... U>
  T* Construct(U&&... u) {
    return new (bytes_) T(std::forward<U>(u)...);
  }

 private:
  alignas(alignof(T)) uint8_t bytes_[sizeof(T)];
};

}  // namespace aw_backtrace_internal

#endif  // BASE_STATIC_STORAGE_H_
