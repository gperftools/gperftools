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
#ifndef BASE_CLEANUP_H_
#define BASE_CLEANUP_H_
#include "config.h"

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

  explicit Cleanup(Callback callback) : callback_(std::move(callback)) {}

  // We don't support copying or moving those
  Cleanup(const Cleanup& other) = delete;
  Cleanup& operator=(const Cleanup& other) = delete;

  ~Cleanup() { callback_(); }

 private:
  Callback callback_;
};

}  // namespace tcmalloc

#endif  // BASE_CLEANUP_H_
