#!/usr/bin/env ruby
#
# Test driver behind `ninja check`. genbuild.rb wires this into build.ninja
# as the RUN_TEST / AGGREGATE rules -- it is not meant to be run by hand
# (use ./run-tests.sh for ad-hoc and stress runs).
#
# The model, automake-style -- `ninja check` re-runs every test, every time
# (no result caching):
#
#   * Each test is a ninja edge  .tests/<name>.trs : RUN_TEST <binary>
#     with this script and the always-dirty `always` phony as implicit deps
#     (the latter is what defeats caching). It runs the guest under qemu,
#     tees output to .tests/<name>.log, and records the verdict in
#     .tests/<name>.trs. A *test* failure still exits 0 (ninja moves on to
#     the other tests); only a *harness* error -- no qemu, no plugin --
#     exits nonzero, so ninja stops before pretending to test.
#
#   * `check` : AGGREGATE <every .trs> re-reads the .trs files, prints the
#     summary, (re)writes test-suite.log, and exits nonzero if any test did
#     not pass.
#
#   subcommands: (--qemu is guessed if --arch is given)
#     testrunner.rb run --qemu Q --arch amd64 --name N BINARY [ARGS...]
#     testrunner.rb aggregate TRS...

require 'fileutils'

# We mix puts with system()/exec(); keep our lines in order with the
# children's output (and don't lose a buffered line across exec).
$stdout.sync = true

TESTDIR = '.tests'
PLUGIN  = './pstepper_plugin.so'
SUITE_LOG = 'test-suite.log'

# A harness error (not a test failure): print to stderr and exit nonzero so
# ninja stops.
def harness_error(msg)
  STDERR.puts "testrunner: #{msg}"
  exit 2
end

def trs_path(name);  File.join(TESTDIR, "#{name}.trs"); end
def log_path(name);  File.join(TESTDIR, "#{name}.log"); end

# .trs is a trivial key=value file -- we own both ends.
def write_trs(name, result:, seconds:, exit_code:)
  File.write(trs_path(name), <<~TRS)
    result=#{result}
    name=#{name}
    seconds=#{'%.2f' % seconds}
    exit=#{exit_code}
  TRS
end

def read_trs(path)
  h = {}
  File.foreach(path) do |line|
    k, v = line.chomp.split('=', 2)
    h[k.intern] = v if k
  end
  h
end

def cmd_run(argv)
  qemu = nil
  name = nil
  arch = nil
  print_stdout = true
  while argv.first&.start_with?('--')
    case argv.shift
    when '--qemu' then qemu = argv.shift
    when '--name' then name = argv.shift
    when '--arch' then arch = argv.shift
    when '--no-stdout' then print_stdout = false
    else harness_error("unknown option")
    end
  end
  binary = argv.shift
  if (!qemu || qemu.empty?) && arch
    qemu = case arch
           when 'amd64' then "qemu-x86_64"
           when 'arm64' then "qemu-aarch64"
           else
             harness_error("don't know arch #{arch.inspect}. Note this is go-style: amd64 or arm64")
           end
  end
  harness_error("run: need --qemu or --arch and binary") unless qemu && binary
  if !name
    name = File.basename(binary)
  end
  extra = argv

  harness_error("no plugin at #{PLUGIN} (run `ninja` first)") unless File.file?(PLUGIN)

  FileUtils.mkdir_p(TESTDIR)
  log = log_path(name)

  cmd = [qemu, '-plugin', PLUGIN, "./#{binary}", *extra]

  if arch == "arm64"
    # qemu emuluates too advanced arm cpu (e.g. uses memcopy instructions)
    cmd[1,0] = %w[-cpu cortex-a53]
  end

  started = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  ok = File.open(log, 'w') do |out|
    out.puts "$ #{cmd.join(' ')}"
    out.flush
    system(*cmd, out: out, err: out)
  end
  seconds = Process.clock_gettime(Process::CLOCK_MONOTONIC) - started
  status = $?

  # system() returns nil if the command could not even be spawned.
  harness_error("could not execute #{qemu}") if ok.nil?

  if print_stdout
    puts(IO.read(log))
  end

  result = ok ? 'PASS' : 'FAIL'
  write_trs(name, result: result, seconds: seconds,
            exit_code: status.exitstatus || -1)

  tag = ok ? 'PASS' : 'FAIL'
  line = "#{tag}: #{name} (#{'%.1f' % seconds}s)"
  line += "  -- #{log}" unless ok
  puts line
  # Deliberately exit 0 even on a test failure: the verdict lives in the
  # .trs, and `ninja check` should run every test before failing.
  exit 0
end

def cmd_aggregate(trs_files)
  harness_error("aggregate: no .trs files") if trs_files.empty?

  rows = trs_files.sort.map do |p|
    harness_error("missing #{p}") unless File.file?(p)
    read_trs(p)
  end

  width = rows.map { |r| r[:name].to_s.length }.max
  failed = []

  summary = +""
  rows.each do |r|
    result, name, seconds = r.values_at(:result, :name, :seconds)
    pass = (result == 'PASS')
    result = 'FAIL' unless result
    l = "%-4s  %-#{width}s  %6.1fs" % [result, name, seconds.to_f]
    l += "  #{log_path(name)}" unless pass
    summary << l << "\n"
    failed << name unless pass
  end

  n = rows.length
  tally = "#{n} test#{'s' if n != 1}, #{n - failed.length} passed" \
          "#{", #{failed.length} FAILED" unless failed.empty?}"

  # Console output: the table + tally.
  puts summary
  puts tally

  # test-suite.log: the same, plus the full log of every failure (automake
  # does this so a CI transcript carries the failing output inline).
  File.open(SUITE_LOG, 'w') do |f|
    f.puts summary
    f.puts tally
    failed.each do |name|
      f.puts
      f.puts "=" * 70
      f.puts "#{name}  (#{log_path(name)})"
      f.puts "=" * 70
      f.puts(File.read(log_path(name))) if File.file?(log_path(name))
    end
  end

  exit(failed.empty? ? 0 : 1)
end

case ARGV.shift
when 'run'       then cmd_run(ARGV)
when 'aggregate' then cmd_aggregate(ARGV)
else
  harness_error("usage: testrunner.rb {run|aggregate} ...")
end
