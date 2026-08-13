/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
//
//
// Originally authored for gperftools and relicensed to 0BSD by the author.
#ifndef BASE_CLEANUP_H_
#define BASE_CLEANUP_H_
// #include "config.h"

#include <type_traits>
#include <utility>

namespace tcmalloc {

// Cleanup represents a piece of work (like closing file descriptor)
// when it's scope ends. Anything that can be invoked, and returns
// null can be used. Most typical callback is lambda. This is somewhat
// similar to Go's defer statement.
//
// This is direct equivalent of abseil's absl::Cleanup, except ours
// cannot be moved from (use std::optional if you need this) and
// cannot be canceled. And is much simpler as a result.
//
// Note, we don't offer equivalent of absl::MakeCleanup. Instead, we
// encourage use of C++17 class template argument deduction. I.e. use
// like this:
//
// tcmalloc::Cleanup cleanup([&] () { fclose(something); });
template <typename Callback>
class Cleanup {
 public:
  static_assert(std::is_same<std::invoke_result_t<Callback>, void>::value, "Cleanup callback must return void");

  explicit Cleanup(Callback callback) : callback_(std::move(callback)) {
  }

  // We don't support copying or moving those
  Cleanup(const Cleanup& other) = delete;
  Cleanup& operator=(const Cleanup& other) = delete;

  ~Cleanup() {
    callback_();
  }

 private:
  Callback callback_;
};

}  // namespace tcmalloc

#endif  // BASE_CLEANUP_H_
