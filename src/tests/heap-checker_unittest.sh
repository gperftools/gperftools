#!/bin/sh -e

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
# Runs the heap-checker unittest with various environment variables.
# This is necessary because we turn on features like the heap profiler
# and heap checker via environment variables.  This test makes sure
# they all play well together.
#
# Notice that we run this script with -e, so *any* error is fatal.

if [ -z "$2" ]
then
    echo "USAGE: $0 <unittest dir> <pprof dir>"
    exit 1
fi

UNITTEST_DIR=$1
PPROF=$2/pprof

HEAP_CHECKER="$1/heap-checker_unittest"

TMPDIR=/tmp/heap_check_info

rm -rf $TMPDIR
mkdir $TMPDIR

# $1: value of heap-profile env. var.  $2: value of heap-check env. var.
run_check() {
    export PPROF_PATH="$PPROF"
    [ -n "$1" ] && export HEAPPROFILE="$1" || unset HEAPPROFILE
    [ -n "$2" ] && export HEAPCHECK="$2" || unset HEAPCHECK

    echo ""
    echo ">>> TESTING $HEAP_CHECKER with HEAPPROFILE=$1 and HEAPCHECK=$2"
    $HEAP_CHECKER
    echo ">>> DONE testing $HEAP_CHECKER with HEAPPROFILE=$1 and HEAPCHECK=$2"

    # If we set HEAPPROFILE, then we expect it to actually have emitted
    # a profile.  Check that it did.
    if [ -n "$HEAPPROFILE" ]; then
      [ -e "$HEAPPROFILE.0001.heap" ] || exit 1
    fi
}

run_check "" ""
run_check "" "local"
run_check "" "normal"
run_check "" "strict"
run_check "$TMPDIR/profile" ""
run_check "$TMPDIR/profile" "local"
run_check "$TMPDIR/profile" "normal"
run_check "$TMPDIR/profile" "strict"

rm -rf $TMPDIR      # clean up

echo "ALL TESTS PASSED"
