#!/usr/bin/ruby

require 'rake'
require 'shellwords'

COPTS = %w[
  -ggdb3
  -Wall
  -Wextra
  -Wno-error
  -Wno-sign-compare
  -Wno-character-conversion
  -Wno-unknown-warning-option
].map { |f| "--copt=#{f}" }

extra_argv = ARGV.map {|a| Shellwords.escape a}

q = "qemu-#{`uname -m`.chomp}"

def which(prog)
  ENV['PATH'].split(':').each do |dir|
    exe = File.join(dir, prog)
    return exe if File.executable?(exe) && !File.directory?(exe)
  end
  raise "no #{prog} in PATH"
end

q = which(q)
is_arm = (q =~ /aarch64/)
if is_arm
  # qemu defaults to some very capable cpu, too capable for
  # accelerated stepped (see v/pstepper/README.me for acceleration
  # quirks).
  q << " -cpu cortex-a76"
end

unless File.executable?("v/pstepper/pstepper_plugin.so")
  puts "Warning: pstepper_plugin.so not built. Do: $ cd v/pstepper && ./genbuild check"
end

# abseil (pulled in by //perf-convert only) takes std::source_location
# whenever the language level is C++20 and <source_location> exists. On
# ubuntu-22.04, clang 14 sits on libstdc++ 12, whose <source_location> is
# empty without __builtin_source_location (clang >= 15) -- so abseil fails
# to compile there. Probe for the real thing and leave //perf-convert out
# of the sweep where it is missing, rather than touching the toolchain.
def std_source_location?(compiler)
  IO.popen([compiler, '-x', 'c++', '-std=c++20', '-fsyntax-only', '-'],
           'w', err: File::NULL) do |io|
    io.puts "#include <source_location>\nstd::source_location l = std::source_location::current();"
  end
  $?.success?
end

(%w[gcc clang].product(%w[opt dbg])).each do |cc, c|
  base_command = "CC=#{cc} bazel test -c #{c} #{COPTS.join(' ')} --nocache_test_results #{extra_argv.join(' ')}"
  targets = "...:all"
  targets += " -- -//perf-convert/..." unless std_source_location?(cc)

  maybe_known_bad = if is_arm && cc == "clang"
                      # Clang has several known cfi bugs and all platforms we tested so far. But only arm64 is failing tests.
                      "--test_env=AW_BT_CLANG_KNOWN_BUGS=1"
                    end

  sh "#{base_command} #{targets}"
  run_under_arg = Shellwords.escape("#{q} -plugin #{File.realpath('v/pstepper/pstepper_plugin.so')}")
  sh "#{base_command} --run_under=#{run_under_arg} --test_env=AW_BT_REQUIRE_STEPPER=1 #{maybe_known_bad} --test_tag_filters=stepped --test_output=all #{targets}"
end
