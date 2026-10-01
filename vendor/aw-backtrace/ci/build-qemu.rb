#!/usr/bin/env ruby
# -*- Mode: ruby -*-
#
# Build the qemu that v/pstepper's TCG plugin needs, and install it into a
# self-contained prefix the workflow can cache.
#
#   ci/build-qemu.rb              build into ~/qemu (or $QEMU_PREFIX)
#   ci/build-qemu.rb /some/where  ... or wherever you say
#   ci/build-qemu.rb --force      rebuild even if the prefix is up to date
#
# Why a build at all: the plugin is written against plugin API version 7
# (qemu_plugin_set_pc, the syscall filter callback, per-callback userdata),
# which no distro package carries yet. QEMU_COMMIT below is the upstream
# master commit this was developed against.
#
# The workflow keys its cache on a hash of *this file*, so bumping
# QEMU_COMMIT, CONFIGURE_FLAGS or the target list invalidates the cached
# prefix automatically. Keep anything that changes the resulting binary in
# here rather than in the yaml.

require 'digest'
require 'fileutils'
require_relative 'lib'

# Upstream master. This is what /opt/qemu was built from on the development
# box (v11.1.0-1168-gff1d2d19d7), so CI and that box run the same emulator.
QEMU_COMMIT = 'ff1d2d19d7e24893e2012d879f8e73077e17b9bd'
QEMU_REMOTE = 'https://github.com/qemu/qemu.git'

# Only the linux-user target for the host architecture: that is all the
# comparer ever runs under, and it keeps the build down to a few minutes.
QEMU_TARGET = { 'amd64' => 'x86_64-linux-user',
                'arm64' => 'aarch64-linux-user' }.fetch(CI.arch)

# --disable-werror because we build from git, where qemu turns -Werror on by
# default and a newer runner compiler would otherwise fail the build on
# warnings in code that is not ours.
CONFIGURE_FLAGS = %w[
  --enable-plugins
  --disable-system
  --disable-tools
  --disable-docs
  --disable-guest-agent
  --disable-werror
]

# Applied on top of QEMU_COMMIT, in name order. As of now we carry one
# patch required for x86 correctness.
PATCHES = Dir[File.join(__dir__, 'qemu-patches', '*.patch')].sort

force = ARGV.delete('--force')
prefix = File.expand_path(ARGV.shift || CI.qemu_prefix)
abort "usage: ci/build-qemu.rb [--force] [PREFIX]" unless ARGV.empty?

# So CI.qemu_binary below (and anything this script shells out to) agrees
# with the prefix we were actually given.
ENV['QEMU_PREFIX'] = prefix

# Everything that decides what lands in the prefix. Recorded in the prefix
# itself so a rerun (or a restored cache that predates a change here) can
# tell whether it still matches.
BUILD_ID = [QEMU_COMMIT, QEMU_TARGET, *CONFIGURE_FLAGS,
            *PATCHES.map { |p| "#{File.basename(p)}:#{Digest::SHA256.file(p).hexdigest[0, 12]}" }].join(' ')
stamp = File.join(prefix, '.aw-ci-qemu-build-id')

if !force && File.file?(stamp) && File.read(stamp).strip == BUILD_ID &&
   File.executable?(CI.qemu_binary)
  puts "ci: qemu already built at #{prefix} (#{QEMU_COMMIT[0, 12]})"
  exit 0
end

# The source tree is scratch space, not an artifact: only the prefix is
# cached, so this is kept out of the way and out of the checkout.
src = File.expand_path(ENV['QEMU_SRC'] || File.join(Dir.home, '.cache', 'aw-backtrace-ci', 'qemu-src'))
build = File.join(src, 'build')

CI.group("fetch qemu #{QEMU_COMMIT[0, 12]}") do
  FileUtils.mkdir_p(src)
  # A shallow fetch of one commit -- ~40MB, versus a full clone of qemu's
  # history. Fetching a bare sha (rather than a branch) is what keeps this
  # pinned; the gitlab mirror serves it.
  CI.sh!('git', 'init', '-q', src)
  unless system('git', '-C', src, 'remote', 'get-url', 'origin',
                out: File::NULL, err: File::NULL)
    CI.sh!('git', '-C', src, 'remote', 'add', 'origin', QEMU_REMOTE)
  end
  CI.sh!('git', '-C', src, 'remote', 'set-url', 'origin', QEMU_REMOTE)
  CI.sh!('git', '-C', src, 'fetch', '--depth', '1', 'origin', QEMU_COMMIT)
  # reset --hard rather than checkout: a rerun finds the tree carrying the
  # patches below, and they have to come off before they go back on. Not
  # `clean`, which would take the build directory with it.
  CI.sh!('git', '-C', src, 'checkout', '-q', '--detach', 'FETCH_HEAD')
  CI.sh!('git', '-C', src, 'reset', '--hard', '-q', 'FETCH_HEAD')
end

CI.group('patch qemu') do
  PATCHES.each { |p| CI.sh!('git', '-C', src, 'apply', p) }
  puts "ci: applied #{PATCHES.size} patch(es)"
end

CI.group('configure qemu') do
  FileUtils.mkdir_p(build)
  CI.sh!(File.join(src, 'configure'), "--prefix=#{prefix}",
         "--target-list=#{QEMU_TARGET}", *CONFIGURE_FLAGS, chdir: build)
end

CI.group('build qemu') do
  CI.sh!('make', "-j#{CI.nproc}", chdir: build)
end

CI.group('install qemu') do
  FileUtils.rm_f(stamp)
  CI.sh!('make', 'install', chdir: build)
end

# Prove the thing runs and has the plugin interface compiled in before
# anything downstream depends on it. `-plugin` with a path that does not
# exist fails either way, but the *message* differs: a build without plugin
# support rejects the option itself.
CI.group('verify qemu') do
  CI.sh!(CI.qemu_binary, '--version')
  out = `#{CI.qemu_binary} -plugin /nonexistent-plugin.so /bin/true 2>&1`
  if out.include?('plugin') && out =~ /not enabled|invalid option|unsupported/i
    abort "ci: qemu built without plugin support:\n#{out}"
  end
  puts "ci: plugin interface present"
end

File.write(stamp, "#{BUILD_ID}\n")
puts "ci: qemu #{QEMU_COMMIT[0, 12]} installed into #{prefix}"
