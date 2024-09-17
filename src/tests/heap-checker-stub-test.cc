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
#include "config.h"

#include "gperftools/heap-checker.h"

#include <assert.h>

#include <string>

#if defined __has_attribute
#  if __has_attribute(noinline)
#    define ATTR_NOINLINE __attribute__ ((noinline))
#  endif
#endif

ATTR_NOINLINE void partial() {
  std::string str;
  for (int i = 0; i < 1024; i++) {
    str.append("-");
  }
  printf("the thing: '%.10s\n", str.c_str());

  static std::string* staticted = new std::string("something");
  printf("staticted: %s\n", staticted->c_str());

  {
    HeapLeakChecker::Disabler disabled;
    std::string* leaked2 = new std::string("leaked2");
    printf("leaked2 address: %p\n", leaked2);
    printf("leaked2: %s\n", leaked2->c_str());
  }

  std::string* leaked3 = new std::string("leaked3");
  printf("leaked3 address: %p\n", leaked3);
  HeapLeakChecker::IgnoreObject(leaked3);
  printf("leaked3: %s\n", leaked3->c_str());
}

int main() {
  HeapLeakChecker heap_checker("test_foo");
  printf("sizeof(HeapLeakChecker) = %zu\n", sizeof(heap_checker));
  partial();
  if (!heap_checker.NoLeaks()) assert(nullptr == "heap memory leak");
}
