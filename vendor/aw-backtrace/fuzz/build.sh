#!/bin/sh
# Build the fast-path .eh_frame decoder fuzzer.
#
#   fuzz/build.sh                       # -> fuzz/fastpath-fuzz
#   CXX=clang++ "CXXFLAGS=-O1 -ggdb3" fuzz/build.sh
set -e
set -x

cd "$(dirname "$0")/.."

CXX="${CXX:-clang++}"
CXXFLAGS="${CXXFLAGS:--ggdb3 -O1 -Wall -Wextra}"

"$CXX" \
  -std=c++20 $CXXFLAGS \
  -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined \
  -I. -Iinclude \
  -o fuzz/fastpath-fuzz \
  fuzz/fastpath-fuzz.cc

"$CXX" \
  -std=c++20 $CXXFLAGS \
  -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined \
  -I. -Iinclude \
  -o fuzz/fastpath-fuzz-naive \
  fuzz/fastpath-fuzz-naive.cc

echo "run: $ fuzz/fastpath-fuzz -use_value_profile=1 -dict=fuzz/dwarf.dict fuzz/corpus"
echo "or: $ fuzz/fastpath-fuzz-naive fuzz/corpus fuzz/data"
