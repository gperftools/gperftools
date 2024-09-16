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
#include "trivialre.h"

#include <gtest/gtest.h>

#include <assert.h>

#include <string>

using trivialre::CompileREOrDie;
using trivialre::Matcher;
using trivialre::MatchSubstring;

// This is matcher builder that build diagnostic string representation
// of regexp matcher expression tree.
struct StringTestingBuilder {
  using Matcher = std::string;

  static Matcher Lit(std::string_view lit) { return std::string("'") + std::string(lit) + "'"; }
  static Matcher Seq(Matcher left, Matcher right) {
    if (right.substr(0, 5) == "(seq ") {
      right = right.substr(5, right.size() - 1 - 5);
    }
    return std::string("(seq ") + left + " " + right + ")";
  }
  static Matcher Alt(Matcher left, Matcher right) {
    if (right.substr(0, 5) == "(alt ") {
      right = right.substr(5, right.size() - 1 - 5);
    }
    return std::string("(alt ") + left + " " + right + ")";
  }
  static Matcher Star(Matcher nested) { return std::string("(star ") + nested + ")"; }
  static Matcher LineStart() { return "^"; }
  static Matcher LineEnd() { return "$"; }

  template <typename Predicate>
  static Matcher CharP(Predicate pred) {
    return "<pred>";
  }

  static Matcher Dot() { return "<dot>"; }
  static Matcher Any() { return "<any>"; }
};

TEST(TrivialRETest, ConstructedMatchers) {
  using B = trivialre::matchers::MatcherBuilder;
  auto m = B::SeqMany({B::Lit("mismatch"), B::Star(B::Dot()), B::Lit("being dealloc"), B::Star(B::Dot()), B::Lit("free")});

  EXPECT_TRUE(MatchSubstring(m, "crap-mismatch-sd-being dealloc-sd-free-junk"));
  EXPECT_FALSE(MatchSubstring(m, "crap-mismatch-sd-being dealloc-sd-fee-junk"));
}

TEST(TrivialRETest, Minimal) {
  auto m = CompileREOrDie("mismatch.*being dealloc.*free");
  EXPECT_TRUE(MatchSubstring(m, "crap-mismatch-sd-being dealloc-sd-free-junk"));
  EXPECT_FALSE(MatchSubstring(m, "crap-mismatch-sd-being dealloc-sd-fee-junk"));
}

TEST(TrivialRETest, Compilations) {
  // format is {regex, golden-parsing}
  std::vector<std::pair<std::string_view, std::string_view>> cases = {
      {"mis.*being deal.*free", "(seq 'mis' (star <dot>) 'being deal' (star <dot>) 'free')"},
      {"mis.*(being|deal).*free", "(seq 'mis' (star <dot>) (alt 'being' 'deal') (star <dot>) 'free')"},
      {"mis.*(being|deal)*fre*e", "(seq 'mis' (star <dot>) (star (alt 'being' 'deal')) 'fr' (star 'e') 'e')"},
      {"mis.*(being|deal)+?free", "(seq 'mis' (star <dot>) (seq (alt 'being' 'deal') (star (alt 'being' 'deal'))) 'free')"},
      {"mis.*(being|deal)?fre*e", "(seq 'mis' (star <dot>) (alt <any> 'being' 'deal') 'fr' (star 'e') 'e')"},
      {"mis.*being|deal.*free",
       "(alt (seq 'mis' (star <dot>) 'being') (seq 'deal' (star <dot>) "
       "'free'))"},
      {"mis.*?being|deal.*free", "(alt (seq 'mis' (star <dot>) 'being') (seq 'deal' (star <dot>) 'free'))"},
      {"\\*", "'*'"},
      {"\\|", "'|'"},
      {"|", "(alt <any> <any>)"},
      {"(|)|", "(alt (alt <any> <any>) <any>)"},
  };

  printf("--- test cases ---\n");
  for (auto [re, expected] : cases) {
    std::string got = trivialre::re_compiler::C<StringTestingBuilder>{StringTestingBuilder{}}.CompileOrDie(re);
    printf("test: /%.*s/ -> %s\n", int(re.size()), re.data(), got.c_str());
    EXPECT_EQ(expected, got) << "re: " << re;
  }
}

bool CompilationFails(std::string_view str) {
  struct Policy {
    bool failed{};
    void NoteError(std::string_view msg, std::string_view at) { failed = true; }
    void StartedParsing(std::string_view str) {}
  };
  trivialre::re_compiler::C<StringTestingBuilder, Policy> compiler({});
  std::string result = compiler.CompileOrDie(str);
  printf("for failing: %s -> %s\n", std::string(str).c_str(), std::string(result).c_str());
  return compiler.failed;
}

TEST(TrivialRETest, CompileFailings) {
  std::vector<std::string_view> examples = {"[", "(", "{}", "((", "\\A", "\\b", "\\S", "\\s", "\\w"};
  for (auto s : examples) {
    EXPECT_TRUE(CompilationFails(s)) << "s: " << s;
  }
}

TEST(TrivialRETest, Runnings) {
  // Format is {re, example...}. Each example is prefixed with '+' for
  // must match or '-' for must not.
  std::vector<std::vector<std::string_view>> cases2 = {
      {"a*", "+a", "+", "+not"},
      {"aa*", "+a", "+aaa", "+ba", "-b"},
      {"a+", "+a", "+aa", "+aaa", "-", "-b"},
      {".", "-\n", "+a", "-"},

      {"[a-f]", "+a", "-z", "-", "+f", "--"},
      {"[a-f-]", "+a", "-z", "-", "+f", "+-"},
      {"[az]", "+a", "-b", "+z"},
      {"[^a-f]", "-a", "+z", "-", "-f"},
      {"[^a-f-]", "-a", "+z", "-", "-f", "--"},
      {"[a-f0-9]", "+a", "-z", "+0", "+9"},
      {"[^]", "+a", "+\n"},
      {"", "+", "+asdasd"},

      {"a(b|c+)d", "+abd", "-ab", "-abcd", "+accd", "-ad"},
      {"a(b|c+)?d", "+abd", "-ab", "-abcd", "+accd", "+ad"},

      {"^a", "+a", "-ba", "+b\na"},
      {"a$", "+a\nb", "+ba", "+b\na"},
      {"a$\\nb", "+a\nb"},
      {"$", "+", "+aaa"},
      {"^$", "+", "-aaa", "+aaa\n"},
  };

  for (const auto& vec : cases2) {
    Matcher m = CompileREOrDie(vec[0]);
    printf("testing /%s/ re: %s\n", std::string(vec[0]).c_str(),
           trivialre::re_compiler::C<StringTestingBuilder>({}).CompileOrDie(vec[0])
               .c_str());
    for (size_t i = 1; i < vec.size(); i++) {
      std::string_view s = vec[i];
      printf("trying: %.*s\n", int(s.size()), s.data());
      if (s[0] == '+') {
        EXPECT_TRUE(MatchSubstring(m, s.substr(1))) << "re: " << vec[0] << " s: " << s;
      } else {
        assert(s[0] == '-');
        EXPECT_FALSE(MatchSubstring(m, s.substr(1))) << "re: " << vec[0] << " s: " << s;
      }
    }
  }
}
