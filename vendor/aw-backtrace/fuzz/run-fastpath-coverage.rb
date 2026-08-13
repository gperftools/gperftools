#!/usr/bin/ruby

require 'rake'

just_build = false

if ARGV.size != 1
  if ARGV.size == 2 && ARGV[0] == "--just-build"
    just_build = true
    ARGV.shift
  else
    raise "need directory for temporaries (or run with `mktemp -d`)"
  end
end

DIR = File.realpath(ARGV[0])

raise "not directory" unless File.directory? DIR

THIS_DIR = File.realpath(File.dirname(__FILE__))

CXX = ENV["CXX"] || "clang++"

ARGS = %w[-std=c++20 -DFUZZ_SKIP_IDENTITY=1 -DNDEBUG -O0 -ggdb3 -I. -Iinclude -fsanitize=fuzzer -fprofile-instr-generate -fcoverage-mapping]

sh "cd #{THIS_DIR}/.. && #{CXX} #{ARGS.join(' ')} -o #{DIR}/fcov fuzz/fastpath-fuzz.cc"

exit(0) if just_build

Dir.chdir DIR

ENV['LLVM_PROFILE_FILE'] = 'cov.profraw'
ENV['FASTPATH_FUZZ_ELF'] ||= File.realpath(File.join(THIS_DIR, "../bazel-bin/amd64-leaf-test"))

sh "./fcov #{THIS_DIR}/../fuzz/corpus/ -runs=0"
sh "llvm-profdata merge -sparse cov.profraw -o cov.profdata"
sh "llvm-cov show ./fcov -instr-profile=cov.profdata -format=html -output-dir=coverage_html --show-branches=count --show-line-counts-or-regions #{THIS_DIR}/../aw-backtrace-fastpath.h"

idx = File.realpath(File.join(DIR, "coverage_html/index.html"))
puts "index: #{idx}"
