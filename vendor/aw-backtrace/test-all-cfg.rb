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

def flag_supported?(compiler, flag, link: false)
  IO.popen([compiler, '-x', 'c', '-Werror', flag, *(link ? [] : ['-c']), '-o', File::NULL, '-'],
           'w', err: File::NULL) do |io|
    io.puts(link ? "int main(void) { return 0; }" : "int f(void) { return 0; }")
  end
  $?.success?
end

# gcc has GNU ld apply its Cortex-A53 erratum 843419 workaround by default,
# which moves the odd instruction into a CFI-less stub and branches there and
# back. A sample inside such a stub can't be unwound right, so the stepped
# runs would keep flagging them. Our runners have no Cortex-A53, so link
# without it. (clang doesn't ask for the workaround, and rejects the flag.)
def link_opts(compiler, is_arm)
  return [] unless is_arm && flag_supported?(compiler, '-mno-fix-cortex-a53-843419', link: true)
  %w[--linkopt=-mno-fix-cortex-a53-843419]
end

# Flag sets that change frame layout or CFI. Each is run with every compiler
# and compilation mode, plain and stepped. The flags go in as --copt, which
# bazel places after its own toolchain flags (-fstack-protector,
# -fno-omit-frame-pointer, ...), so ours win. A flag the compiler rejects is
# dropped, and a set left empty by that is skipped.
FLAG_SETS = {
  "default" => [],
  # What distros ship.
  "hardened" => %w[-fstack-protector-strong -fstack-clash-protection] +
                (is_arm ? %w[-mbranch-protection=standard] : %w[-fcf-protection=full]),
  # A canary in every frame (so gcc's record-right-before-the-CFA layout on
  # aarch64), and no frame pointers.
  "stress" => %w[-fstack-protector-all -fomit-frame-pointer],
  # Typical performance builds.
  "bare" => %w[-fno-stack-protector -fomit-frame-pointer],
}
# No -mbranch-protection=pac-ret+pc set: libgcc's unwinder (and so glibc's
# backtrace(), which several tests use as a reference) aborts on its
# DW_CFA_AARCH64_negate_ra_state_with_pc. eh-frame-reader-test covers our
# decoding of it.
if !is_arm && File.read("/proc/cpuinfo") =~ /\bavx2\b/
  # Stack realignment, i.e. DRAP frames. Needs AVX2 to run.
  FLAG_SETS["x86-64-v3"] = %w[-march=x86-64-v3]
end

# Every config runs even after a failure; the failures are listed at the end.
failures = []
run = lambda do |what, cmd|
  sh(cmd) { |ok, _| failures << what unless ok }
end

# The flag sets other than default only run with -c opt: that is what ships,
# and at -O0 (dbg) frames are simple enough that these flags change little.
(%w[gcc clang].product(%w[opt dbg], FLAG_SETS.keys)).each do |cc, c, set|
  next if c == "dbg" && set != "default"
  flags = FLAG_SETS[set].select { |f| flag_supported?(cc, f) }
  dropped = FLAG_SETS[set] - flags
  puts "Note: #{cc} rejects #{dropped.join(' ')}, dropped from flag set #{set}" unless dropped.empty?
  if flags.empty? && !FLAG_SETS[set].empty?
    puts "Skipping flag set #{set} for #{cc}"
    next
  end
  puts "=== #{cc} -c #{c}, flag set #{set}: #{flags.join(' ')}"

  copts = COPTS + flags.map { |f| "--copt=#{f}" } + link_opts(cc, is_arm)
  base_command = "CC=#{cc} bazel test -c #{c} #{copts.join(' ')} --nocache_test_results #{extra_argv.join(' ')}"
  targets = "...:all"
  # perf-convert (an offline tool) pulls in abseil, the bulk of every rebuild,
  # and nothing in it depends on frame layout, so only the default set runs it.
  targets += " -- -//perf-convert/..." unless set == "default" && std_source_location?(cc)

  maybe_known_bad = if is_arm && cc == "clang"
                      # Clang has several known cfi bugs and all platforms we tested so far. But only arm64 is failing tests.
                      "--test_env=AW_BT_CLANG_KNOWN_BUGS=1"
                    end

  run.("#{cc} -c #{c} #{set}", "#{base_command} #{targets}")
  # qemu's default x86-64 cpu lacks AVX2. Haswell is exactly x86-64-v3, so
  # (unlike -cpu max) it advertises nothing an AVX2-capable host lacks, which
  # matters because accelerated stepping runs the handler natively.
  qq = (set == "x86-64-v3") ? "#{q} -cpu Haswell-noTSX" : q
  run_under_arg = Shellwords.escape("#{qq} -plugin #{File.realpath('v/pstepper/pstepper_plugin.so')}")
  run.("#{cc} -c #{c} #{set} (stepped)",
      "#{base_command} --run_under=#{run_under_arg} --test_env=AW_BT_REQUIRE_STEPPER=1 #{maybe_known_bad} --test_tag_filters=stepped --test_output=all #{targets}")
end

unless failures.empty?
  puts "FAILED configs:", failures.map { |f| "  #{f}" }
  exit 1
end
