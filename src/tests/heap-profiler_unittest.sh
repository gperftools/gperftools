#!/bin/sh

# Copyright (c) 2005, Google Inc.
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
# Author: Craig Silverstein
#
# Runs the heap-profiler unittest and makes sure the profile looks appropriate.
#

# This script runs either as heap-profiler_unittest.sh or as
# heap-profiler_debug_unittest.sh. And we want to run matching C
# program (i.e. linked to "normal" libtcmalloc or debug version).
DEFAULT_HEAP_PROFILER=`echo $0 | sed 's/.sh//'`
HEAP_PROFILER="${1:-$DEFAULT_HEAP_PROFILER}"
PPROF=pprof
TEST_TMPDIR=`mktemp -d /tmp/heap-profiler_unittest.XXXXXX`

# It's meaningful to the profiler, so make sure we know its state
unset HEAPPROFILE

num_failures=0

# Given one profile (to check the contents of that profile) or two
# profiles (to check the diff between the profiles), and a function
# name, verify that the function name takes up at least 90% of the
# allocated memory.  The function name is actually specified first.
VerifyMemFunction() {
  function="$1"
  shift

  if [ $# = 2 ]; then
    [ -f "$1" ] || { echo "Profile not found: $1"; exit 1; }
    [ -f "$2" ] || { echo "Profile not found: $2"; exit 1; }
    $PPROF --text --base="$1" "$2" >"$TEST_TMPDIR/output.pprof" 2>&1
  else
    [ -f "$1" ] || { echo "Profile not found: $1"; exit 1; }
    $PPROF --text "$1" >"$TEST_TMPDIR/output.pprof" 2>&1
  fi

  cat "$TEST_TMPDIR/output.pprof" \
      | tr -d % | awk '$6 ~ /^'$function'$/ && ($2+0) > 90 {exit 1;}'
  if [ $? != 1 ]; then
    echo
    echo "--- Test failed for $function: didn't account for 90% of executable memory"
    echo "--- Program output:"
    cat "$TEST_TMPDIR/output"
    echo "--- pprof output:"
    cat "$TEST_TMPDIR/output.pprof"
    echo "---"
    num_failures=`expr $num_failures + 1`
  fi
}

VerifyOutputContains() {
  text="$1"

  if ! grep "$text" "$TEST_TMPDIR/output" >/dev/null 2>&1; then
    echo "--- Test failed: output does not contain '$text'"
    echo "--- Program output:"
    cat "$TEST_TMPDIR/output"
    echo "---"
    num_failures=`expr $num_failures + 1`
  fi
}

HEAPPROFILE="$TEST_TMPDIR/test"
HEAP_PROFILE_INUSE_INTERVAL="10240"   # need this to be 10Kb
HEAP_PROFILE_ALLOCATION_INTERVAL="$HEAP_PROFILE_INUSE_INTERVAL"
HEAP_PROFILE_DEALLOCATION_INTERVAL="$HEAP_PROFILE_INUSE_INTERVAL"
export HEAPPROFILE
export HEAP_PROFILE_INUSE_INTERVAL
export HEAP_PROFILE_ALLOCATION_INTERVAL
export HEAP_PROFILE_DEALLOCATION_INTERVAL

# We make the unittest run a child process, to test that the child
# process doesn't try to write a heap profile as well and step on the
# parent's toes.  If it does, we expect the parent-test to fail.
$HEAP_PROFILER 1 >$TEST_TMPDIR/output 2>&1     # run program, with 1 child proc

VerifyMemFunction Allocate2 "$HEAPPROFILE.1329.heap"
VerifyMemFunction Allocate "$HEAPPROFILE.1448.heap" "$HEAPPROFILE.1548.heap"

# Check the child process got to emit its own profile as well.
VerifyMemFunction Allocate2 "$HEAPPROFILE"_*.1329.heap
VerifyMemFunction Allocate "$HEAPPROFILE"_*.1448.heap "$HEAPPROFILE"_*.1548.heap

# Make sure we logged both about allocating and deallocating memory
VerifyOutputContains "62 MB allocated"
VerifyOutputContains "62 MB freed"

# Now try running without --heap_profile specified, to allow
# testing of the HeapProfileStart/Stop functionality.
$HEAP_PROFILER >"$TEST_TMPDIR/output2" 2>&1

rm -rf $TEST_TMPDIR      # clean up

if [ $num_failures = 0 ]; then
  echo "PASS"
else
  echo "Tests finished with $num_failures failures"
fi
exit $num_failures
