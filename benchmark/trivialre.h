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
#ifndef TRIVIALRE_H_
#define TRIVIALRE_H_

#include <stdio.h>
#include <stdlib.h>

#include <algorithm>
#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace trivialre {

// Callback for Matcher. See below.
using CB = std::function<bool(std::string_view, bool)>;
// Matcher is a function that gets string and invokes given callback
// with remaining text (i.e. suffix of parsed part) for each
// successful parsing. We're able to express arbitrary trees of regexp
// expressions with this simple abstraction.
using Matcher = std::function<bool(std::string_view str, bool line_start, const CB& cb)>;

// MatchSubstring returns true iff there is substring of `str' that
// matches given matcher.
inline bool MatchSubstring(const Matcher& m, std::string_view str) {
  size_t sz = str.size();
  CB succeed = [](std::string_view str, bool line_start) { return true; };
  bool line_start = true;
  for (size_t i = 0; i < sz; i++) {
    if (m(str, line_start, succeed)) {
      return true;
    }
    line_start = (str[0] == '\n');
    str.remove_prefix(1);
  }
  return m("", line_start, succeed);
}

Matcher CompileREOrDie(std::string_view str);

// --- implementation ---

namespace matchers {

// MatcherBuilder is a collection of functions that combine Matchers
// according to various kinds of regex structures (sequence,
// alternatives, '*' etc).
struct MatcherBuilder {
  using Matcher = trivialre::Matcher;

  // Returns Matcher that parses given literal.
  static Matcher Lit(std::string_view lit);
  // Returns Matcher that left then right.
  static Matcher Seq(Matcher left, Matcher right);
  // Returns Matcher that given parses given sequence of matchers
  // (folding from right for efficiency).
  static Matcher SeqMany(std::initializer_list<Matcher> list);
  // Returns Matcher that parses either left or right.
  static Matcher Alt(Matcher left, Matcher right);
  // Returns Matcher that matches 0 or more parsings of given nested
  // matcher. I.e. implements '*' operator of regexps.
  static Matcher Star(Matcher nested);
  static Matcher LineStart();
  static Matcher LineEnd();

  // Returns Matcher that parses one character iff pred(character) is
  // true.
  template <typename Predicate>
  static Matcher CharP(Predicate pred);

  // Dot matcher implements '.' operator of regexps. I.e. matches
  // exactly one non-newline character.
  static Matcher Dot() {
    return CharP([](char ch) { return ch != '\n'; });
  }
  // Any matcher immediately suceeds consuming no text at all.
  static Matcher Any() {
    return [](std::string_view str, bool line_start, const CB& cb) { return cb(str, line_start); };
  }
};

inline Matcher MatcherBuilder::Lit(std::string_view lit) {
  return [=](std::string_view str, bool line_start, const CB& cb) -> bool {
    auto sz = lit.size();
    if (str.substr(0, sz) != lit) {
      return false;
    }
    line_start = (sz == 0) ? line_start : (str[sz - 1] == '\n');
    str.remove_prefix(sz);
    // printf("Matched prefix %.*s (rest: %.*s)\n",
    //        static_cast<int>(lit.size()), lit.data(),
    //        std::max<int>(str.size(), 6), str.data());
    return cb(str, line_start);
  };
}

inline Matcher MatcherBuilder::Seq(Matcher left, Matcher right) {
  return [left = std::move(left), right = std::move(right)](std::string_view str, bool line_start, const CB& cb) -> bool {
    return left(str, line_start, [=](std::string_view str, bool line_start) { return right(str, line_start, cb); });
  };
}

inline Matcher MatcherBuilder::SeqMany(std::initializer_list<Matcher> list) {
  if (std::empty(list)) {
    return Any();
  }
  auto it = std::rbegin(list);
  Matcher rv = *it++;
  while (it != std::rend(list)) {
    rv = Seq(*it++, std::move(rv));
  }
  return rv;
}

inline Matcher MatcherBuilder::Alt(Matcher left, Matcher right) {
  return [left = std::move(left), right = std::move(right)](std::string_view str, bool line_start, const CB& cb) -> bool {
    if (left(str, line_start, cb)) {
      return true;
    }
    return right(str, line_start, cb);
  };
}

inline Matcher MatcherBuilder::Star(Matcher nested) {
  return [nested = std::move(nested)](std::string_view str, bool line_start, const CB& cb) -> bool {
    CB rec;
    rec = [&](std::string_view str, bool line_start) -> bool {
      if (cb(str, line_start)) {
        return true;
      }
      return nested(str, line_start, rec);
    };
    return rec(str, line_start);
  };
}

template <typename Predicate>
Matcher MatcherBuilder::CharP(Predicate pred) {
  return [pred = std::move(pred)](std::string_view str, bool line_start, const CB& cb) -> bool {
    if (str.size() && pred(str[0])) {
      bool line_start = (str[0] == '\n');
      str.remove_prefix(1);
      return cb(str, line_start);
    }
    return false;
  };
}

inline Matcher MatcherBuilder::LineStart() {
  return [](std::string_view str, bool line_start, const CB& cb) {
    if (!line_start) return false;
    return cb(str, line_start);
  };
}

inline Matcher MatcherBuilder::LineEnd() {
  return [](std::string_view str, bool line_start, const CB& cb) {
    if (str.size() && str[0] != '\n') {
      return false;
    }
    // Yes, line-end doesn't consume the \n character.
    return cb(str, line_start);
  };
}

}  // namespace matchers

namespace re_compiler {

struct ErrorPolicy {
  std::string_view original_str;

  void NoteError(std::string_view msg, std::string_view at) {
    // For our trivial implementation we're only able to crash
    fprintf(stderr, "parse error %.*s, at: %.*s\n", int(msg.size()), msg.data(), int(at.size()), at.data());
    fprintf(stderr, "expression we were parsing:\n%.*s\n", int(original_str.size()), original_str.data());
    if (size_t diff = at.data() - original_str.data(); diff < 120) {
      fprintf(stderr, "%s^\n", std::string{}.append(diff, '-').c_str());
    }
    fflush(stderr);
    abort();
  }

  void StartedParsing(std::string_view str) { original_str = str; }
};

// C is our regexp compiler. It assembles matcher tree from string
// regexp representation. Given builder is used to construct concrete
// matchers, allowing flexibility (see StringTestingBuilder).
template <typename Builder, typename ErrorPolicy = re_compiler::ErrorPolicy>
struct C : public ErrorPolicy {
  using Matcher = typename Builder::Matcher;
  // ParseResult is Matcher (or nothing if we parsed empty string) and
  // remaining text.
  using ParseResult = std::pair<std::optional<Matcher>, std::string_view>;

  const Builder& builder;
  explicit C(const Builder& builder) : builder(builder) {}

  bool IsCharAt(std::string_view str, size_t index, char ch) { return index < str.size() && str[index] == ch; }

  // This is top level parser. It parses alternatives of regex runs.
  ParseResult ParseAlt(std::string_view str) {
    auto [maybe_left, str_l] = ParseRun(str);
    if (IsCharAt(str_l, 0, '|')) {
      if (!maybe_left) {
        maybe_left.emplace(builder.Any());
      }
      auto [maybe_right, str_r] = ParseAlt(str_l.substr(1));
      if (!maybe_right) {
        maybe_right.emplace(builder.Any());
      }
      return {builder.Alt(std::move(maybe_left.value()), std::move(maybe_right.value())), str_r};
    }
    return {std::move(maybe_left), str_l};
  }

  using FnPred = std::function<bool(char)>;
  template <typename Body>
  void AddPred(FnPred* pred, Body body) {
    if (!*pred) {
      *pred = body;
    } else {
      *pred = [old = std::move(*pred), body = std::move(body)](char ch) { return old(ch) || body(ch); };
    }
  }

  // Parses [<set-of-chars>] expression. Note: str is just past
  // opening '[' character)
  ParseResult CompileCharSet(std::string_view str) {
    bool negated = false;
    if (IsCharAt(str, 0, '^')) {
      negated = true;
      str.remove_prefix(1);
    }
    FnPred pred;

    while (str.size() > 0 && str[0] != ']') {
      if (str.size() > 2 && str[1] == '-' && str[2] != ']') {
        // range
        AddPred(&pred, [a = str[0], b = str[2]](char ch) { return a <= ch && ch <= b; });
        str.remove_prefix(3);
        continue;
      }

      char ch = str[0];

      if (ch == '\\') {
        if (str.size() == 1) {
          break;
        }
        str.remove_prefix(1);
        ch = str[0];
      }

      AddPred(&pred, [ch](char candidate) { return ch == candidate; });

      str.remove_prefix(1);
    }

    if (!IsCharAt(str, 0, ']')) {
      ErrorPolicy::NoteError("failed to spot ] at the end of char-set term", str);
      return {{}, ""};
    }

    if (!pred) {
      pred = [negated](char candidate) { return negated; };
    } else if (negated) {
      pred = [pred = std::move(pred)](char candidate) { return !pred(candidate); };
    }
    return {builder.CharP(std::move(pred)), str.substr(1)};
  }

  // Parses sequence of literals and groups and groups of '*' and '+'
  // expressions.
  ParseResult ParseRun(std::string_view str) {
    if (str.size() == 0) {
      return {{}, str};
    }

    static constexpr char kSpecials[] = "()[]{}.*|\\?+^$";
    static constexpr const char* kSpecialsEnd = kSpecials + sizeof(kSpecials) - 1;

    size_t i;
    for (i = 0; i < str.size(); i++) {
      char ch = str[i];
      if (std::find(kSpecials, kSpecialsEnd, ch) != kSpecialsEnd) {
        break;
      }
    }

    if (i) {
      // we got literal
      if (i > 1 && (IsCharAt(str, i, '*') || IsCharAt(str, i, '+') || IsCharAt(str, i, '?'))) {
        // only last char of literal char runs will be '*'-ed. So lets
        // be careful
        i--;
      }
      // we got literal. Lets try to concat it with possible '*' and next run
      return MaybeStar(builder.Lit(str.substr(0, i)), str.substr(i));
    }

    char first = str[0];
    if (first == '\\' && str.size() > 1) {
      std::string_view literal;
      if (str[1] == 'n') {
        literal = "\n";
      } else if (str[1] == 't') {
        literal = "\t";
      } else if (str[1] == ' ') {
        literal = " ";
      } else if (auto place = std::find(kSpecials, kSpecialsEnd, str[1]); place != kSpecialsEnd) {
        literal = {place, 1};
      } else {
        // Failure to parse
        return {{}, str};
      }
      return MaybeStar(builder.Lit(literal), str.substr(2));
    }
    if (first == '^') {
      return MaybeStar(builder.LineStart(), str.substr(1));
    }
    if (first == '$') {
      return MaybeStar(builder.LineEnd(), str.substr(1));
    }
    if (first == '.') {
      return MaybeStar(builder.Dot(), str.substr(1));
    }
    if (first == '[') {
      return CompileCharSet(str.substr(1));
    }

    if (first == '(') {
      auto [maybe_nested, new_str] = ParseAlt(str.substr(1));

      if (!IsCharAt(new_str, 0, ')')) {
        ErrorPolicy::NoteError("failed to spot ) at the end of group term", new_str);
        return {{}, ""};
      }

      if (maybe_nested) {
        return MaybeStar(std::move(maybe_nested.value()), new_str.substr(1));
      }

      // empty group. We just ignore it. But lets also handle possible
      // '*' after it (which we also eat)
      if (IsCharAt(new_str, 1, '*')) {
        new_str.remove_prefix(1);
      }
      return ParseRun(new_str.substr(1));
    }

    // Likely '|', ')' or parse error
    return {{}, str};
  }

  // Sequences left then right or just left if right is missing).
  Matcher MaybeSeq(Matcher left, std::optional<Matcher> right) {
    if (right) {
      return builder.Seq(std::move(left), std::move(right.value()));
    }
    return left;
  }

  // Builds matcher for '+' expression.
  Matcher MakePlus(Matcher nested) { return builder.Seq(nested, builder.Star(nested)); }

  // Given regex matcher, check if it is followed by '*' or '+' and
  // wrap it if needed, then continue gathering sequence of matches
  // (see ParseRun)
  ParseResult MaybeStar(Matcher left, std::string_view str) {
    if (IsCharAt(str, 0, '*')) {
      left = builder.Star(std::move(left));
      str.remove_prefix(1);
      if (IsCharAt(str, 0, '?')) {
        // We don't produce actual matching, so there is not
        // difference between lazy and eager matching. But lets
        // support the syntax anyways, by ignoring lazyness marker
        str.remove_prefix(1);
      }
    }
    if (IsCharAt(str, 0, '+')) {
      left = MakePlus(std::move(left));
      str.remove_prefix(1);
      if (IsCharAt(str, 0, '?')) {
        // We don't produce actual matching, so there is not
        // difference between lazy and eager matching. But lets
        // support the syntax anyways, by ignoring lazyness marker
        str.remove_prefix(1);
      }
    }
    if (IsCharAt(str, 0, '?')) {
      left = builder.Alt(builder.Any(), std::move(left));
      str.remove_prefix(1);
    }
    auto [maybe_right, new_str] = ParseRun(str);
    return {MaybeSeq(left, std::move(maybe_right)), new_str};
  }

  Matcher CompileOrDie(std::string_view str) {
    ErrorPolicy::StartedParsing(str);
    auto [maybe_m, new_str] = ParseAlt(str);
    if (!new_str.empty()) {
      ErrorPolicy::NoteError("failed to parse entire re string", new_str);
    }
    if (!maybe_m) {
      return builder.Any();
    }
    return maybe_m.value();
  }
};

}  // namespace re_compiler

inline Matcher CompileREOrDie(std::string_view str) { return re_compiler::C<matchers::MatcherBuilder>({}).CompileOrDie(str); }

}  // namespace trivialre

#endif  // TRIVIALRE_H_
