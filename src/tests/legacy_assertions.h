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
#ifndef TESTS_LEGACY_ASSERTIONS_H_
#define TESTS_LEGACY_ASSERTIONS_H_
#include "base/logging.h"

// TODO: remove after gtest adoption is complete
// Synonyms for CHECK_* that are used in some unittests.
#define EXPECT_EQ(val1, val2) CHECK_EQ(val1, val2)
#define EXPECT_NE(val1, val2) CHECK_NE(val1, val2)
#define EXPECT_LE(val1, val2) CHECK_LE(val1, val2)
#define EXPECT_LT(val1, val2) CHECK_LT(val1, val2)
#define EXPECT_GE(val1, val2) CHECK_GE(val1, val2)
#define EXPECT_GT(val1, val2) CHECK_GT(val1, val2)
#define ASSERT_EQ(val1, val2) EXPECT_EQ(val1, val2)
#define ASSERT_NE(val1, val2) EXPECT_NE(val1, val2)
#define ASSERT_LE(val1, val2) EXPECT_LE(val1, val2)
#define ASSERT_LT(val1, val2) EXPECT_LT(val1, val2)
#define ASSERT_GE(val1, val2) EXPECT_GE(val1, val2)
#define ASSERT_GT(val1, val2) EXPECT_GT(val1, val2)
// As are these variants.
#define EXPECT_TRUE(cond)     CHECK(cond)
#define EXPECT_FALSE(cond)    CHECK(!(cond))
#define EXPECT_STREQ(a, b)    CHECK(strcmp(a, b) == 0)
#define ASSERT_TRUE(cond)     EXPECT_TRUE(cond)
#define ASSERT_FALSE(cond)    EXPECT_FALSE(cond)
#define ASSERT_STREQ(a, b)    EXPECT_STREQ(a, b)

#endif  // TESTS_LEGACY_ASSERTIONS_H_
