#!/usr/bin/env ruby
# -*- Mode: ruby -*-
#
# Build v/pstepper's TCG plugin and run its own test suite under the qemu
# ci/build-qemu.rb installed.
#
#   ci/check-pstepper.rb
#
# This is the functional proof that the cached qemu is usable: the plugin
# loads into it, the magic-syscall ABI matches, and the guest-side plumbing
# steps a real program. Everything downstream (the comparer, the stepped
# bazel tests) rides on exactly this, so when it breaks there is no point
# looking anywhere else first.
#
# QEMU can be overridden to point at another emulator; by default it is the
# one in $QEMU_PREFIX (~/qemu). v/pstepper's genbuild.rb treats QEMU as a
# "precious" variable and bakes it into build.ninja, so changing it
# regenerates the build rather than silently testing the wrong thing.

require_relative 'lib'

qemu = ENV['QEMU'] = File.expand_path(ENV['QEMU'] || CI.qemu_binary)
abort "ci: no qemu at #{qemu} (run ci/build-qemu.rb first)" unless File.executable?(qemu)

pstepper = File.join(CI.repo_root, 'v', 'pstepper')
puts "ci: pstepper check with QEMU=#{qemu}"

ok = CI.group('pstepper ninja check') do
  # `./genbuild.rb check` regenerates build.ninja if stale, builds the
  # plugin and the guest tests, then runs every test under qemu. It exits
  # nonzero if any test failed -- but keep going here so the logs below get
  # printed either way.
  system('./genbuild.rb', 'check', chdir: pstepper)
end

unless ok
  # testrunner.rb's aggregate step writes the full output of every failing
  # test into test-suite.log; the console summary only names the log files,
  # which is no use in a CI transcript.
  log = File.join(pstepper, 'test-suite.log')
  if File.file?(log)
    puts
    puts "=== #{log} ==="
    puts File.read(log)
  end
  abort 'ci: pstepper tests failed'
end

puts 'ci: pstepper tests passed'
