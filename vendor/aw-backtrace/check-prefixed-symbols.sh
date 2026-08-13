#!/bin/sh
# Fails (prints leaked symbols to stderr, exit 1) if any of the given static
# libraries -- built with AW_RENAME_PREFIX set to $1 -- defines an
# external-linkage symbol that doesn't carry that prefix. Catches
# renamings.h falling out of sync with the library's real symbol surface.
# See renamings.h for why this renaming exists at all.
#
# Uses whatever `nm` is on PATH rather than a Bazel-toolchain-resolved one:
# a modern binutils nm reads foreign-arch ELF/archives fine, so there's no
# real need to match it to the target toolchain just to list symbol names.
#
# Usage: check-prefixed-symbols.sh PREFIX FILE [FILE ...]
# FILEs that aren't a plain (non-PIC, non-.so) .a are ignored, so callers can
# just pass a cc_library's whole set of outputs ($(SRCS) in a genrule)
# without pre-filtering.
set -eu

prefix=$1
shift

libs=""
for f in "$@"; do
  case "$f" in
    *.pic.a | *.so) ;;
    *.a) libs="$libs $f" ;;
  esac
done
if [ -z "$libs" ]; then
  echo "check-prefixed-symbols.sh: no plain .a file among: $*" >&2
  exit 1
fi

# --format=sysv is pipe-delimited, so a demangled name containing spaces
# (e.g. "unsigned long std::optional<unsigned long>::value_or(...) const &")
# doesn't shift whitespace-based column parsing the way the default or -P
# output would.
#
# The allowed-unprefixed list covers things that are legitimately never
# going to carry our prefix: standard-library template instantiations
# pulled in as weak COMDATs (ABI-required to be identical across every
# independently-built copy that instantiates them, so the linker already
# merges them safely -- not the ODR-collision risk our own names are), and
# the global-namespace operator new/delete overloads. The raw `_ZNSt` /
# `_ZSt` / `_ZN9__gnu_cxx` fallbacks exist because this nm's demangler
# doesn't fully handle C++20 constrained-template (`requires`-clause)
# mangling and passes those through as raw mangled text instead of
# "std::..." -- without the fallback, that demangler gap would misreport
# genuine std:: symbols as leaks.
leaked=$(nm --defined-only -g --format=sysv -C $libs \
  | awk -F'|' '{cls=$3; gsub(/[ \t]/,"",cls); if (cls ~ /^[A-Z]$/) print $1}' \
  | grep -v "$prefix" \
  | grep -Ev 'std::|__gnu_cxx::|^operator new|^operator delete|_GLOBAL_|^_ZNSt|^_ZNKSt|^_ZSt|^_ZN9__gnu_cxx|^_ZNK9__gnu_cxx' \
  || true)

if [ -n "$leaked" ]; then
  echo "error:$libs define external symbols without the '$prefix' prefix:" >&2
  echo "$leaked" >&2
  exit 1
fi
