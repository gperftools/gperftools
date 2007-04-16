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
# Runs the 4 profiler unittests and makes sure their profiles look
# appropriate.  We expect two commandline args, as described below.
#
# We run under the assumption that if $PROFILER1 is run with no
# arguments, it prints a usage line of the form
#   USAGE: <actual executable being run> [...]
#
# This is because libtool sometimes turns the 'executable' into a
# shell script which runs an actual binary somewhere else.

# We expect BINDIR and PPROF_PATH to be set in the environment.
# If not, we set them to some reasonable values
BINDIR="${BINDIR:-.}"
PPROF_PATH="${PPROF_PATH:-$BINDIR/src/pprof}"

if [ "x$1" = "x-h" -o "x$1" = "x--help" ]; then
  echo "USAGE: $0 [unittest dir] [path to pprof]"
  echo "       By default, unittest_dir=$BINDIR, pprof_path=$PPROF_PATH"
  exit 1
fi

UNITTEST_DIR=${1:-$BINDIR}
PPROF=${2:-$PPROF_PATH}

PROFILER1=$UNITTEST_DIR/profiler1_unittest
PROFILER2=$UNITTEST_DIR/profiler2_unittest
PROFILER3=$UNITTEST_DIR/profiler3_unittest
PROFILER4=$UNITTEST_DIR/profiler4_unittest

TMPDIR=/tmp/profile_info

# It's meaningful to the profiler, so make sure we know its state
unset CPUPROFILE

rm -rf $TMPDIR
mkdir $TMPDIR || exit 2

num_failures=0

RegisterFailure() {
  num_failures=`expr $num_failures + 1`
}

# Takes two filenames representing profiles, with their executable scripts,
# and a multiplier, and verifies that the 'contentful' functions in
# each profile take the same time (possibly scaled by the given
# multiplier).  It used to be "same" meant within 50%, after adding an 
# noise-reducing X units to each value.  But even that would often
# spuriously fail, so now it's "both non-zero".  We're pretty forgiving.
VerifySimilar() {
  prof1=$TMPDIR/$1
  # We need to run the script with no args to get the actual exe name
  exec1=`$2 2>&1 | awk '{print $2; exit;}'`
  prof2=$TMPDIR/$3
  exec2=`$4 2>&1 | awk '{print $2; exit;}'`
  mult=$5

  mthread1=`$PPROF $exec1 $prof1 | grep test_main_thread | awk '{print $1}'`
  mthread2=`$PPROF $exec2 $prof2 | grep test_main_thread | awk '{print $1}'`
  mthread1_plus=`expr $mthread1 + 5`
  mthread2_plus=`expr $mthread2 + 5`
  if [ -z "$mthread1" ] || [ -z "$mthread2" ] || \
     [ "$mthread1" -le 0 -o "$mthread2" -le 0 ]
#    || [ `expr $mthread1_plus \* $mult` -gt `expr $mthread2_plus \* 2` -o \
#         `expr $mthread1_plus \* $mult \* 2` -lt `expr $mthread2_plus` ]
  then
    echo
    echo ">>> profile on $exec1 vs $exec2 with multiplier $mult failed:"
    echo "Actual times (in profiling units) were '$mthread1' vs. '$mthread2'"
    echo
    RegisterFailure
  fi
}

# Takes a filenames representing a profile, with its executables,
# and a multiplier, and verifies that the main-thread function takes
# the same amount of time as the other-threads function (possibly scaled
# by the given multiplier).  Figuring out the multiplier can be tricky,
# since by design the main thread runs twice as long as each of the
# 'other' threads!  It used to be "same" meant within 50%, after adding an 
# noise-reducing X units to each value.  But even that would often
# spuriously fail, so now it's "both non-zero".  We're pretty forgiving.
VerifyAcrossThreads() {
  prof1=$TMPDIR/$1
  # We need to run the script with no args to get the actual exe name
  exec1=`$2 2>&1 | awk '{print $2; exit;}'`
  mult=$3

  mthread=`$PPROF $exec1 $prof1 | grep test_main_thread | awk '{print $1}'`
  othread=`$PPROF $exec2 $prof2 | grep test_other_thread | awk '{print $1}'`
  if [ -z "$mthread" ] || [ -z "$othread" ] || \
     [ "$mthread" -le 0 -o "$othread" -le 0 ]
#    || [ `expr $mthread \* $mult \* 3` -gt `expr $othread \* 10` -o \
#         `expr $mthread \* $mult \* 10` -lt `expr $othread \* 3` ]
  then
    echo
    echo ">>> profile on $exec1 vs $exec2 with multiplier $mult failed:"
    echo "Actual times (in profiling units) were '$mthread1' vs. '$mthread2'"
    echo
    RegisterFailure
  fi
}

echo
echo ">>> WARNING <<<"
echo "This test looks at timing information to determine correctness."
echo "If your system is loaded, the test may spuriously fail."
echo "If the test does fail with an 'Actual times' error, try running again."
echo

# profiler1 is a non-threaded version
$PROFILER1 50 1 $TMPDIR/p1 || RegisterFailure
$PROFILER1 100 1 $TMPDIR/p2 || RegisterFailure
VerifySimilar p1 $PROFILER1 p2 $PROFILER1 2

# Verify the same thing works if we statically link
$PROFILER2 50 1 $TMPDIR/p3 || RegisterFailure
$PROFILER2 100 1 $TMPDIR/p4 || RegisterFailure
VerifySimilar p3 $PROFILER2 p4 $PROFILER2 2

# Verify the same thing works if we specify via CPUPROFILE
CPUPROFILE=$TMPDIR/p5 $PROFILER2 50 || RegisterFailure
CPUPROFILE=$TMPDIR/p6 $PROFILER2 100 || RegisterFailure
VerifySimilar p5 $PROFILER2 p6 $PROFILER2 2

# When we compile with threads, things take a lot longer even when we only use 1
CPUPROFILE=$TMPDIR/p5b $PROFILER3 10 || RegisterFailure
CPUPROFILE=$TMPDIR/p5c $PROFILER3 20 || RegisterFailure
VerifySimilar p5b $PROFILER3 p5c $PROFILER3 2

# Now try what happens when we use threads
$PROFILER3 5 2 $TMPDIR/p7 || RegisterFailure
$PROFILER3 10 2 $TMPDIR/p8 || RegisterFailure
VerifySimilar p7 $PROFILER3 p8 $PROFILER3 2

$PROFILER4 5 2 $TMPDIR/p9 || RegisterFailure
$PROFILER4 10 2 $TMPDIR/p10 || RegisterFailure
VerifySimilar p9 $PROFILER4 p10 $PROFILER4 2

# More threads!
$PROFILER4 2 3 $TMPDIR/p9 || RegisterFailure
$PROFILER4 4 3 $TMPDIR/p10 || RegisterFailure
VerifySimilar p9 $PROFILER4 p10 $PROFILER4 2

# Compare how much time the main thread takes compared to the other threads
# Recall the main thread runs twice as long as the other threads, by design.
$PROFILER4 2 4 $TMPDIR/p11 || RegisterFailure
VerifyAcrossThreads p11 $PROFILER4 2

# Make sure that when we have a process with a fork, the profiles don't
# clobber each other
CPUPROFILE=$TMPDIR/p6 $PROFILER1 1 -2 || RegisterFailure
n=`ls $TMPDIR/p6* | wc -l`
if [ $n != 3 ]; then
  echo "FORK test FAILED: expected 3 profiles (for main + 2 children), found $n"
  num_failures=`expr $num_failures + 1`
fi

rm -rf $TMPDIR      # clean up

echo "Tests finished with $num_failures failures"
exit $num_failures
