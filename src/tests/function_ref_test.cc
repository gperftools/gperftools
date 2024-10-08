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

#include "base/function_ref.h"

#include <functional>
#include <memory>

#include "gtest/gtest.h"

TEST(FunctionRef, Basic) {
  int fn_ref_invoked = 0;
  int fn_result = -1;
  int fn_arg = 42;

  auto fn = [&] (tcmalloc::FunctionRef<int(int)> ref) {
    fn_result = ref(fn_arg);
    fn_ref_invoked++;
  };

  fn([] (int arg) -> int {
    return arg;
  });

  ASSERT_EQ(fn_result, 42);
  ASSERT_EQ(fn_ref_invoked, 1);

  fn_arg = 13;

  fn([fn_ref_invoked, no_copy = std::make_unique<int>(1)] (int arg) {
    return fn_ref_invoked + arg;
  });

  ASSERT_EQ(fn_result, 14);
  ASSERT_EQ(fn_ref_invoked, 2);

  auto body = [fn_ref_invoked, no_copy = std::make_unique<int>(1)] (int arg) {
    return fn_ref_invoked + arg;
  };

  fn(body);

  ASSERT_EQ(fn_result, 15);
  ASSERT_EQ(fn_ref_invoked, 3);

  std::function<int(int)> f = [&] (int arg) { return fn_ref_invoked + arg; };

  fn(f);

  ASSERT_EQ(fn_result, 16);
  ASSERT_EQ(fn_ref_invoked, 4);
}

TEST(FunctionRef, BasicFirstDataArg) {
  int fn_ref_invoked = 0;
  int fn_result = -1;
  int fn_arg = 42;

  auto fn = [&] (tcmalloc::FunctionRefFirstDataArg<int(int)> ref) {
    fn_result = ref(fn_arg);
    fn_ref_invoked++;
  };

  fn([] (int arg) -> int {
    return arg;
  });

  ASSERT_EQ(fn_result, 42);
  ASSERT_EQ(fn_ref_invoked, 1);

  fn_arg = 13;

  fn([fn_ref_invoked, no_copy = std::make_unique<int>(1)] (int arg) {
    return fn_ref_invoked + arg;
  });

  ASSERT_EQ(fn_result, 14);
  ASSERT_EQ(fn_ref_invoked, 2);

  auto body = [fn_ref_invoked, no_copy = std::make_unique<int>(1)] (int arg) {
    return fn_ref_invoked + arg;
  };

  fn(body);

  ASSERT_EQ(fn_result, 15);
  ASSERT_EQ(fn_ref_invoked, 3);

  std::function<int(int)> f = [&] (int arg) { return fn_ref_invoked + arg; };

  fn(f);

  ASSERT_EQ(fn_result, 16);
  ASSERT_EQ(fn_ref_invoked, 4);
}
