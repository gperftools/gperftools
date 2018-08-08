#!/bin/sh

# Copyright (c) 2018, Google Inc.
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met:
#
#     * Redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above
# copyright notice, this list of conditions and the following disclaimer
# in the documentation and/or other materials provided with the
# distribution.
#     * Neither the name of Google Inc. nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

# ---
# Author: Matt Morehouse

BINDIR="${BINDIR:-.}"
# We expect PPROF_PATH to be set in the environment.
# If not, we set it to some reasonable value
export PPROF_PATH="${PPROF_PATH:-$BINDIR/src/pprof}"
export TCMALLOC_GUARDED_SAMPLE_PARAMETER=100  # Enable GWP-ASan

GUARDED_PAGE_ALLOCATOR_TEST="${1:-$BINDIR/guarded_page_allocator_test}"

PrintRemainingOutput() {
  while read line; do echo "$line"; done
}

# Returns 0 if my_regex matches against my_output.  Otherwise returns 1.
#
# If my_regex has the form "regex1{{num_lines}}regex2", then returns 0 only if
# regex1 matches against my_output AND regex2 also matches my_output within
# num_lines after the regex1 match.
MultilineGrep() {
  my_regex="$1"
  my_output="$2"
  if echo "$my_regex" | grep "{{[0-9]*}}"; then
    # Multiline regex
    first_regex="$(echo "$my_regex" | grep -o "^[^{]*")"
    num_lines="$(echo "$my_regex" | grep -o "{{[0-9]*}}" | grep -o "[0-9]*")"
    second_regex="$(echo "$my_regex" | grep -o "}}.*$" | grep -o "[^}]*$")"
    echo "$my_output" \
      | grep -A"$num_lines" "$first_regex" \
      | grep -o "$second_regex"
  else
    # Single line regex
    echo "$my_output" | grep -o "$my_regex"
  fi
}

# Return 0 if death test crashes with expected regex
# Return 1 if death test crashes with wrong regex
# Return 2 if death test doesn't crash
# Return 3 if not a death test
RunDeathTest() {
  "$GUARDED_PAGE_ALLOCATOR_TEST" "$1" 2>&1 | {
    is_death_test=false
    has_regex=false
    while read line; do
      if echo "$line" | grep "EXPECT_DEATH"; then
        is_death_test=true
        read regex_line
        if echo "$regex_line" | grep "Expected regex:"; then
          has_regex=true
          regex="$(expr "$regex_line" : "Expected regex:\(.*\)")"
        fi
        break
      fi
    done
    [ "$is_death_test" = false ] && return 3
    remaining_output="$(PrintRemainingOutput)"
    echo "$remaining_output" | tail -n1 | grep "DONE" && return 2
    MultilineGrep "$regex" "$remaining_output"
  }
}

num_failures=0
death_test_num=0
while :; do
  echo -n "Running death test $death_test_num..."
  RunDeathTest $death_test_num
  death_test_status=$?
  if [ "$death_test_status" = 0 ]; then
    echo "OK"
  elif [ "$death_test_status" = 1 ]; then
    echo "FAILED. Output mismatch"
    num_failures=$(expr $num_failures + 1)
  elif [ "$death_test_status" = 2 ]; then
    echo "FAILED. No crash"
    num_failures=$(expr $num_failures + 1)
  elif [ "$death_test_status" = 3 ]; then
    echo "done with death tests"
    break
  else
    echo "FAILED. Unknown error"
    num_failures=$(expr $num_failures + 1)
  fi
  death_test_num=$(expr $death_test_num + 1)
done

if [ "$num_failures" = 0 ]; then
  echo "PASS"
else
  echo "Failed with $num_failures failures"
fi
exit $num_failures
