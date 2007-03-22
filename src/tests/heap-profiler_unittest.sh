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
# We run under the assumption that if $HEAP_PROFILER is run with --help,
# it prints a usage line of the form
#   USAGE: <actual executable being run> [...]
#
# This is because libtool sometimes turns the 'executable' into a
# shell script which runs an actual binary somewhere else.

if [ -z "$2" ]
then
    echo "USAGE: $0 <unittest dir> <pprof dir>"
    exit 1
fi

UNITTEST_DIR=$1
PPROF=$2/pprof

HEAP_PROFILER=$UNITTEST_DIR/heap-profiler_unittest

TMPDIR=/tmp/heap_profile_info

# It's meaningful to the profiler, so make sure we know its state
unset HEAPPROFILE

rm -rf $TMPDIR
mkdir $TMPDIR || exit 2

num_failures=0

# Given one profile (to check the contents of that profile) or two
# profiles (to check the diff between the profiles), and a function
# name, verify that the function name takes up at least 90% of the
# allocated memory.  The function name is actually specified first.
VerifyMemFunction() {
    function=$1
    shift

    # Getting the 'real' name is annoying, since running HEAP_PROFILER
    # at all tends to destroy the old profiles if we're not careful
    HEAPPROFILE_SAVED="$HEAPPROFILE"
    unset HEAPPROFILE
    exec=`$HEAP_PROFILER --help | awk '{print $2; exit;}'` # get program name
    export HEAPPROFILE="$HEAPPROFILE_SAVED"

    if [ $# = 2 ]; then
	[ -e "$1" -a -e "$2" ] || { echo "Profile not found: $1 or $2"; exit 1; }
	$PPROF --base="$1" $exec "$2"
    else
	[ -e "$1" ] || { echo "Profile not found: $1"; exit 1; }
	$PPROF $exec "$1"
    fi | tr -d % | awk '$6 ~ /^'$function'$/ && $2 > 90 {exit 1;}'

    if [ $? != 1 ]; then
	echo
	echo ">>> Test failed for $function: didn't use 90% of cpu"
	echo
	num_failures=`expr $num_failures + 1`
    fi
}

export HEAPPROFILE=$TMPDIR/test
$HEAP_PROFILER 1              # actually run the program, with a child process

VerifyMemFunction Allocate2 $HEAPPROFILE.0723.heap
VerifyMemFunction Allocate $HEAPPROFILE.0700.heap $HEAPPROFILE.0760.heap

# Check the child process too
VerifyMemFunction Allocate2 ${HEAPPROFILE}_*.0723.heap
VerifyMemFunction Allocate ${HEAPPROFILE}_*.0700.heap ${HEAPPROFILE}_*.0760.heap

rm -rf $TMPDIR      # clean up

echo "Tests finished with $num_failures failures"
exit $num_failures
