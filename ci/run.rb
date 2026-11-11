#!/usr/bin/env ruby
# Builds and tests CI configs from ci/configs.json (generated from
# ci/configs.cue by ci/gen.rb).
#
# Usage: ci/run.rb <tag>...
#
# Runs, in order, every config that has all the given tags, and stops
# at the first failure. Configs carrying an opt-in tag (e.g. i386,
# msvc) are only selected when that tag is given. With no tags, lists
# the configs. CI passes a config's full tag set, which selects
# exactly that config.
#
# Each config builds out of tree in its own ci-build/<tags> directory
# under the source dir. Note that autotools refuses to build out of
# tree when the source dir itself is configured (make distclean first).

require 'json'
require 'etc'
require 'fileutils'
require 'shellwords'

SRCDIR = File.expand_path('..', __dir__)
DATA = JSON.parse(File.read(File.join(__dir__, 'configs.json')))
CONFIGS = DATA['configs']
OPT_IN = DATA['optInTags']

def sh!(*cmd, env: {}, chdir:)
  puts "+ #{cmd.shelljoin}"
  $stdout.flush
  system(env, *cmd, chdir: chdir) or abort("FAILED: #{cmd.shelljoin}")
end

# Like sh!, but on failure retries once with retry_cmd (to tolerate
# flaky tests).
def sh_retry!(cmd, retry_cmd, env: {}, chdir:)
  puts "+ #{cmd.shelljoin}"
  $stdout.flush
  return if system(env, *cmd, chdir: chdir)
  puts "!!! failed, retrying: #{retry_cmd.shelljoin}"
  sh!(*retry_cmd, env: env, chdir: chdir)
end

def run_config(c)
  env = c['env']
  builddir = File.join(SRCDIR, c['builddir'])
  FileUtils.mkdir_p builddir
  numcpus = Etc.nprocessors
  morecpus = numcpus * 3
  puts "=== #{c['tags'].join(' ')} (NUMCPUS=#{numcpus} MORECPUS=#{morecpus})"

  case c['sys']
  when 'autotools'
    sh! './autogen.sh', chdir: SRCDIR unless File.exist?(File.join(SRCDIR, 'configure'))
    sh! File.join(SRCDIR, 'configure'), *c['configure'], env: env, chdir: builddir
    sh! 'make', "-j#{numcpus}", env: env, chdir: builddir
    sh_retry! ['make', "-j#{morecpus}", 'check'], ['make', "-j#{morecpus}", 'recheck'],
              env: env, chdir: builddir
  when 'cmake'
    # On Windows, build commands run with the MSVC environment set up.
    wrap = c['msvc'] ? ['ruby', File.join(SRCDIR, '.github/workflows/scripts/msvc-env.rb')] : []
    sh!(*wrap, 'cmake', '-G', 'Ninja', *c['configure'], SRCDIR, env: env, chdir: builddir)
    sh!(*wrap, 'ninja', '-v', env: env, chdir: builddir)
    if c['msvc']
      sh!('ninja', '-v', 'test', env: env.merge('CTEST_PARALLEL_LEVEL' => '12'), chdir: builddir)
    else
      sh_retry!(%w[ctest -j12 --test-dir .], %w[ctest -j12 --test-dir . --rerun-failed],
                env: env, chdir: builddir)
    end
  end

  # Ensure that notable optional features are actually built and work.
  c['expect'].each { |t| sh! "./#{t}", env: env, chdir: builddir }
  puts "=== #{c['tags'].join(' ')}: OK"
end

if ARGV.empty?
  puts "usage: #{$0} <tag>...\nconfigs:"
  CONFIGS.each { |c| puts "  #{c['tags'].join(' ')}" }
  puts "opt-in tags (must be given to select): #{OPT_IN.join(' ')}"
  exit 2
end

selected = CONFIGS.select do |c|
  (ARGV - c['tags']).empty? && ((c['tags'] & OPT_IN) - ARGV).empty?
end
abort "no config has all of: #{ARGV.join(' ')}" if selected.empty?
selected.each { |c| run_config(c) }
