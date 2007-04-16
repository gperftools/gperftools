/* Copyright (c) 2006, Google Inc.
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
 *
 * ---
 * Author: Sanjay Ghemawat
 */

#include "base/logging.h"
#include "base/atomicops.h"

template <class AtomicType>
static void TestAtomicIncrement() {
  // For now, we just test single threaded execution

  // use a guard value to make sure the AtomicIncrement doesn't go
  // outside the expected address bounds.  This is in particular to
  // test that some future change to the asm code doesn't cause the
  // 32-bit AtomicIncrement doesn't do the wrong thing on 64-bit
  // machines.
  struct {
    AtomicType prev_word;
    AtomicType count;
    AtomicType next_word;
  } s;

  AtomicType prev_word_value, next_word_value;
  memset(&prev_word_value, 0xFF, sizeof(AtomicType));
  memset(&next_word_value, 0xEE, sizeof(AtomicType));

  s.prev_word = prev_word_value;
  s.count = 0;
  s.next_word = next_word_value;

  CHECK_EQ(AtomicIncrement(&s.count, 1), 1);
  CHECK_EQ(s.count, 1);
  CHECK_EQ(s.prev_word, prev_word_value);
  CHECK_EQ(s.next_word, next_word_value);

  CHECK_EQ(AtomicIncrement(&s.count, 2), 3);
  CHECK_EQ(s.count, 3);
  CHECK_EQ(s.prev_word, prev_word_value);
  CHECK_EQ(s.next_word, next_word_value);

  CHECK_EQ(AtomicIncrement(&s.count, 3), 6);
  CHECK_EQ(s.count, 6);
  CHECK_EQ(s.prev_word, prev_word_value);
  CHECK_EQ(s.next_word, next_word_value);

  CHECK_EQ(AtomicIncrement(&s.count, -3), 3);
  CHECK_EQ(s.count, 3);
  CHECK_EQ(s.prev_word, prev_word_value);
  CHECK_EQ(s.next_word, next_word_value);

  CHECK_EQ(AtomicIncrement(&s.count, -2), 1);
  CHECK_EQ(s.count, 1);
  CHECK_EQ(s.prev_word, prev_word_value);
  CHECK_EQ(s.next_word, next_word_value);

  CHECK_EQ(AtomicIncrement(&s.count, -1), 0);
  CHECK_EQ(s.count, 0);
  CHECK_EQ(s.prev_word, prev_word_value);
  CHECK_EQ(s.next_word, next_word_value);

  CHECK_EQ(AtomicIncrement(&s.count, -1), -1);
  CHECK_EQ(s.count, -1);
  CHECK_EQ(s.prev_word, prev_word_value);
  CHECK_EQ(s.next_word, next_word_value);

  CHECK_EQ(AtomicIncrement(&s.count, -4), -5);
  CHECK_EQ(s.count, -5);
  CHECK_EQ(s.prev_word, prev_word_value);
  CHECK_EQ(s.next_word, next_word_value);

  CHECK_EQ(AtomicIncrement(&s.count, 5), 0);
  CHECK_EQ(s.count, 0);
  CHECK_EQ(s.prev_word, prev_word_value);
  CHECK_EQ(s.next_word, next_word_value);
}

int main(int argc, char** argv) {
  TestAtomicIncrement<AtomicWord>();
  TestAtomicIncrement<Atomic32>();
  printf("PASS\n");
  return 0;
}
