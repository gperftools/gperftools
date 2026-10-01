# -*- Mode: ruby -*-
#
# Shared helpers for the ci/ scripts. Nothing here is CI-specific on
# purpose: every script under ci/ is meant to run the same way on a
# developer's box as it does on a GitHub runner or under `act`, so the
# workflow yaml stays a list of script invocations and there is nothing in
# it worth debugging.
#
# The one thing that does know about CI is `group`, which emits GitHub's
# log-folding markers. They are harmless noise in a terminal.

require 'etc'
require 'rbconfig'

module CI
  module_function

  # An optional leading Hash is an environment, as Kernel#system takes it;
  # it is stripped from the echoed line so the output stays readable.
  def sh!(*cmd, **opts)
    shown = cmd.first.is_a?(Hash) ? cmd[1..] : cmd
    puts "$ #{shown.join(' ')}"
    $stdout.flush
    system(*cmd, **opts) or abort "ci: command failed (#{$?.exitstatus}): #{shown.join(' ')}"
  end

  # Like sh!, but a nonzero exit is a result rather than the end of the run.
  def sh(*cmd, **opts)
    shown = cmd.first.is_a?(Hash) ? cmd[1..] : cmd
    puts "$ #{shown.join(' ')}"
    $stdout.flush
    system(*cmd, **opts)
  end

  # Foldable section in the GitHub log; plain lines anywhere else.
  def group(title)
    puts "::group::#{title}"
    $stdout.flush
    yield
  ensure
    puts "::endgroup::"
    $stdout.flush
  end

  def root?
    Process.uid == 0
  end

  # apt needs root on a GitHub runner and already has it inside act's
  # container images, where sudo may not even be installed.
  def sudo
    root? ? [] : ['sudo']
  end

  def nproc
    Etc.nprocessors
  end

  # Go-style arch name, matching v/pstepper's genbuild.rb (amd64 / arm64).
  def arch
    case RbConfig::CONFIG['host_cpu']
    when /\A(x86_64|amd64)\z/ then 'amd64'
    when /\A(aarch64|arm64)\z/ then 'arm64'
    else
      abort "ci: unsupported host cpu #{RbConfig::CONFIG['host_cpu'].inspect}"
    end
  end

  # Where build-qemu.rb installs to, and where everything else looks for it.
  # Deliberately outside the checkout: the workflow caches this directory,
  # and act bind-mounts the checkout from the host.
  def qemu_prefix
    File.expand_path(ENV['QEMU_PREFIX'] || File.join(Dir.home, 'qemu'))
  end

  def qemu_binary
    name = { 'amd64' => 'qemu-x86_64', 'arm64' => 'qemu-aarch64' }.fetch(arch)
    File.join(qemu_prefix, 'bin', name)
  end

  # Repository root, so a script works from any cwd.
  def repo_root
    File.expand_path('..', __dir__)
  end

  def which(prog)
    ENV.fetch('PATH', '').split(File::PATH_SEPARATOR).each do |dir|
      path = File.join(dir, prog)
      return path if File.executable?(path) && !File.directory?(path)
    end
    nil
  end

  # Where ci/install-bazelisk.rb puts `bazel`. On PATH already on most
  # machines; the workflow adds it explicitly.
  def local_bin
    File.expand_path(ENV['CI_BIN_DIR'] || File.join(Dir.home, '.local', 'bin'))
  end

  def bazel
    which('bazel') || File.join(local_bin, 'bazel')
  end

  def dpkg_installed?(pkg)
    status = `dpkg-query -W -f='${Status}' #{pkg} 2>/dev/null`
    $?.success? && status.include?('ok installed')
  end

  # Install the ones that are missing, or nothing at all. apt-only, which is
  # what the workflow runs on; on a developer box it is a no-op.
  def install_packages!(packages)
    # missing = packages.reject { |pkg| dpkg_installed?(pkg) }
    # if missing.empty?
    #   puts "ci: all #{packages.size} package(s) already installed"
    #   return
    # end
    missing = packages

    puts "ci: installing #{missing.join(' ')}"
    # apt-get is interactive-by-default in ways that hang a CI log.
    env = {'DEBIAN_FRONTEND' => 'noninteractive'}
    group('apt-get update') { sh!(*sudo, 'apt-get', 'update', '-qq') }
    group('apt-get install') do
      sh!(env, *sudo, 'apt-get', 'install', '-y', '--no-install-recommends', '--no-upgrade', *missing)
    end
  end
end
