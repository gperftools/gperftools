#!/usr/bin/env ruby
# -*- Mode: ruby -*-
#
# Put a `bazel` on PATH, via bazelisk, from its prebuilt release binary.
#
#   ci/install-bazelisk.rb            install if missing
#
# bazelisk rather than a distro bazel because .bazelversion pins 9.2.0 and
# bazelisk is what honours it. The release binary is the *latest* one, on
# purpose: bazelisk is a thin launcher (.bazelversion still pins bazel
# itself), and building it with go pulls a newer go toolchain, which is slow.
# The bazel release bazelisk then downloads lands in ~/.cache/bazelisk, which
# the workflow caches separately -- this script only produces the launcher.
#
# It installs as `bazel`, not `bazelisk`: every script and every set of
# instructions in this tree says `bazel`, and bazelisk is a drop-in.

require 'fileutils'
require_relative 'lib'

ARCHES = { 'x86_64' => 'amd64', 'aarch64' => 'arm64', 'arm64' => 'arm64' }

abort 'usage: ci/install-bazelisk.rb' unless ARGV.empty?

bindir = CI.local_bin
bazel = File.join(bindir, 'bazel')

CI.group('install bazelisk') do
  machine = `uname -m`.strip
  arch = ARCHES[machine] or abort "ci: no bazelisk release for #{machine}"
  os = `uname -s`.strip.downcase
  url = "https://github.com/bazelbuild/bazelisk/releases/latest/download/bazelisk-#{os}-#{arch}"

  FileUtils.mkdir_p(File.expand_path("~/.cache/bazelisk"))
  etag_file = File.expand_path("~/.cache/bazelisk/bazel.etag")
  cached_bazelisk = File.expand_path("~/.cache/bazelisk/bazel")

  FileUtils.mkdir_p(bindir)
  CI.sh!(*%W[curl -fsSL --retry 3 --etag-save #{etag_file} --etag-compare #{etag_file} -o #{cached_bazelisk} #{url}])
  FileUtils.chmod(0o755, cached_bazelisk, verbose: true)
  FileUtils.cp(cached_bazelisk, bazel, verbose: true)
end

puts "ci: installed #{bazel} (latest bazelisk release)"
puts "ci: add #{bindir} to PATH"
