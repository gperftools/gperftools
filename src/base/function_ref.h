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
#ifndef BASE_FUNCTION_REF_H_
#define BASE_FUNCTION_REF_H_
#include "config.h"

#include <type_traits>
#include <utility>

namespace tcmalloc {

template <typename T>
struct FunctionRef;

// FunctionRef is to std::function what std::string_view is to
// std::string. That is non-owning, trivially copyable reference-like
// type to anything that can be invoked. It is mostly equivalent to
// absl::FunctionRef. Just like std::string_view we encourage people
// to pass it by value and in most most modern ABIs it will be
// efficiently passed in two registers.
//
// It avoids any memory allocation (unlike std::function). So good to
// use everywhere even inside tightest guts of malloc or
// super-early. Of course, since it doesn't own it's invokable, it
// must not outlive it.
//
// Our version also explicitly exposes pure C-style function and data
// arguments to be used by legacy C-style API's that pass callback
// function and callback argument in order to implement closure-like
// features.
template <typename R, typename... Args>
struct FunctionRef<R(Args...)> {
  template <typename F,
            typename FR = std::invoke_result_t<F, Args&&...>>
  using EnableIfCompatible =
      typename std::enable_if<std::is_void<R>::value ||
                              std::is_convertible<FR, R>::value>::type;

  explicit FunctionRef(R (*fn)(Args..., void*), void* data)
    : fn(fn), data(data) {}

  template <typename Body, typename = EnableIfCompatible<const Body&>>
  FunctionRef(const Body& body) : FunctionRef(
    [] (Args... args, void* data) {
      const Body& b = *(reinterpret_cast<const Body*>(data));
      return b(std::forward<Args>(args)...);
    },
    const_cast<void*>(reinterpret_cast<const void*>(&body))) {}

  R operator()(Args... args) const {
    return fn(std::forward<Args>(args)..., data);
  }

  R (*const fn)(Args... args, void* data);
  void* const data;
};

template <typename T>
struct FunctionRefFirstDataArg;

// FunctionRefFirstDataArg is same as FunctionRef except it's fn's
// function type accepts data argument in first position.
template <typename R, typename... Args>
struct FunctionRefFirstDataArg<R(Args...)> {
  template <typename F,
            typename FR = std::invoke_result_t<F, Args&&...>>
  using EnableIfCompatible =
      typename std::enable_if<std::is_void<R>::value ||
                              std::is_convertible<FR, R>::value>::type;

  explicit FunctionRefFirstDataArg(R (*fn)(void*, Args...), void* data)
    : fn(fn), data(data) {}

  template <typename Body>
  FunctionRefFirstDataArg(const Body& body) : FunctionRefFirstDataArg(
    [] (void* data, Args... args) {
      const Body& b = *(reinterpret_cast<const Body*>(data));
      return b(std::forward<Args>(args)...);
    },
    const_cast<void*>(reinterpret_cast<const void*>(&body))) {}

  R operator()(Args... args) const {
    return fn(data, std::forward<Args>(args)...);
  }

  R (*const fn)(void* data, Args... args);
  void* const data;
};

}  // namespace tcmalloc

#endif  // BASE_FUNCTION_REF_H_
