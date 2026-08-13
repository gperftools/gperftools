#!/usr/bin/ruby

require 'rake'

COPTS = %w(-ggdb3 -Wall -Wextra -Wno-error -Wno-sign-compare -Wno-character-conversion).map {|f| "'--copt=#{f}'"}.join(' ')
sh "bazel test -c dbg #{COPTS} ...:all"
sh "bazel test -c opt #{COPTS} ...:all"
sh "CC=clang bazel test -c dbg #{COPTS} ...:all"
sh "CC=clang bazel test -c opt #{COPTS} ...:all"
