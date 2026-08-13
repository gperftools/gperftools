#!/usr/bin/ruby

require 'rake'

sh "bazel test -c dbg --copt=-ggdb3 --copt=-Werror :all"
sh "bazel test -c opt --copt=-ggdb3 --copt=-Werror :all"
sh "CC=clang bazel test -c opt --copt=-ggdb3 --copt=-Werror :all"
sh "CC=clang bazel test -c opt --copt=-ggdb3 --copt=-Werror :all"
