#!/usr/bin/ruby

# Use this script to produce compile_commands.json useful to feed
# clangd/ccls. Helpful for modern IDEs.

require 'json'

DIRNAME = File.dirname(__FILE__)
Dir.chdir(DIRNAME)

all_files = Dir['**/*.h'] + Dir['**/*.c'] + Dir['**/*.cc']
all_files.sort!

def classify(path)
  return :skip if path =~ /\/googletest\/.*\/test\//
  return :skip if path == "src/config.h"

  return :skip if path =~ /gmock/
  return :skip if path =~ /\/googletest\/samples\//
  return :skip if path.end_with?("/gtest-all.cc")

  return :skip if path.start_with?("src/windows/")
  return :skip if path.start_with?("vsprojects/")

  if path =~ /libbacktrace(\/|-)/
    return :default if path.end_with?(".cc")
    return :libbacktrace
  end

  :default
end

NORMAL_INCLUDES = %w[
  benchmark/ generic-config/ src/gperftools/
  src/base/ src/
  vendor/googletest/googletest/include/
  vendor/googletest/googletest/
].map {|d| "-I#{d}"}

CXX = ENV["CXX"] || "clang++"
CC = ENV["CC"] || CXX.gsub(/\+\+$/,"")

entries = all_files.map do |filename|
  args = case classify(filename)
         when :default
           # various libc_override_xyz headers
           # (e.g. libc_override_glibc.h) are not standalone, but we
           # can make clangd "compile" them by including
           # src/libc_override.h but setting up this "inclusion" to
           # skip actual override includes. Yes, ugly. But it works,
           # mostly.
           maybe_libc_override = if filename =~ /libc_override_/
                                   %w[-include src/libc_override.h -DTCMALLOC_SKIP_OVERRIDE]
                                 else
                                   []
                                 end
           # Note, getpc.h cannot include "config.h" because it is
           # being used by ./configure script. So we need to "help" it
           # for the clangd case.
           maybe_generic_config = if File.basename(filename) == "getpc.h"
                                    %w[-include generic-config/config.h]
                                  else
                                    []
                                  end
           [CXX, "-x", "c++", "-std=c++17",
            "-DENABLE_EMERGENCY_MALLOC",
            *NORMAL_INCLUDES,
            *maybe_libc_override,
            *maybe_generic_config,
            "--", filename]
         when :libbacktrace
           [CC,
            "-Ivendor/libbacktrace-integration",
            "-Ivendor/libbacktrace",
            "--", filename]
         when :skip
           next
         end
  {file: filename,
   directory: Dir.pwd,
   arguments: args}
end.compact

File.open("compile_commands.json", "w") do |f|
  f.puts JSON.pretty_generate(entries)
end
puts "produced #{File.join(DIRNAME, "compile_commands.json")}"
