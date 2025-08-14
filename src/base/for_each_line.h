/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
 * Copyright (c) 2025, gperftools Contributors
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
#ifndef FOR_EACH_LINE_H_
#define FOR_EACH_LINE_H_
#include "config.h"

#include <limits.h>

#include "base/basictypes.h"
#include "base/function_ref.h"
#include "base/logging.h"

namespace tcmalloc {

// Using buffer of the given size, reads some contents, find lines
// boundaries and calls given body function for each line. We assume
// that BufSize is large enough to hold largest line. If we see line
// that is longer than the buffer, we fail and return false.
template <size_t BufSize = PATH_MAX + 1024>
ATTRIBUTE_VISIBILITY_HIDDEN
bool ForEachLine(FunctionRef<int(void*, int)> reader, FunctionRef<bool(char*, char*)> body) {
  char buf[BufSize];
  char* const buf_end = buf + sizeof(buf) - 1;

  char* sbuf = buf; // note, initial value could be nullptr, but
                    // memmove actually insists to get non-null
                    // arguments (even when count is 0)
  char* ebuf = sbuf;

  bool eof = false;

  for (;;) {
    char* nextline = static_cast<char*>(memchr(sbuf, '\n', ebuf - sbuf));

    if (nextline != nullptr) {
      RAW_CHECK(nextline < ebuf, "BUG");

      *nextline = 0; // Turn newline into '\0'.

      if (!body(sbuf, nextline)) {
        break;
      }

      sbuf = nextline + 1;
      continue;
    }

    int count = ebuf - sbuf;

    if (eof) {
      if (count == 0) {
        break; // done
      }

      // Last read ended up without trailing newline. Lets add
      // it. Note, we left one byte margin above, so we're able to
      // write this and not get past end of buf.
      *ebuf++ = '\n';
      continue;
    }

    if (ebuf == buf_end && sbuf == buf) {
      // Line somehow ended up too long for our buffer. Bail out.
      return false;
    }

    // Move the current text to the start of the buffer
    memmove(buf, sbuf, count);
    sbuf = buf;
    ebuf = sbuf + count;

    int nread = reader(ebuf, buf_end - ebuf);

    // Read failures are not expected, but lets not crash if this
    // happens in non-debug mode.
    DCHECK_GE(nread, 0);
    if (nread < 0) {
      nread = 0;
    }

    if (nread == 0) {
      eof = true;
    }
    ebuf += nread;
    // Retry memchr above.
  }

  return true;
}

}  // namespace tcmalloc

#endif  // FOR_EACH_LINE_H_

