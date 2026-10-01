#!/usr/bin/env ruby
#
# Executable build model for the pstepper plugin and its guest test
# programs. Same approach as aw-backtrace's genbuild.rb -- the NinjaBulder
# machinery below is lifted from there mostly verbatim; the default flags
# and the build! function are what's pstepper-specific.
#
#   ./genbuild.rb              (re)generate build.ninja
#   ./genbuild.rb ninja        regenerate, then run `ninja -v`
#   ./genbuild.rb ninja CC=clang  one-off env overrides (script re-execs)
#   ./genbuild.rb check        regenerate, then run `ninja -v check`
#
# Then, to build + run the guest tests:
#   ninja            build the plugin + all test binaries (the default)
#   ninja check      build, then run every test under qemu (always -- no
#                    result caching), print a summary
# See testrunner.rb for the check machinery.
#
# Overridable via the environment (see PRECIOUS_ENVS): CC, CFLAGS,
# LDFLAGS, LIBS, QEMU (`ninja check` runs tests under qemu. Only the
# one matching the target arch is consulted). Empty/unset QEMU is
# treated specially by testrunner.rb. It then takes qemu from PATH.

require 'digest/md5'
require 'shellwords'

# -Wall but not -Wextra: the qemu plugin callback signatures have many
# deliberately-unused parameters and -Wextra just makes noise on them.
BASE_CFLAGS = ENV['CFLAGS'] || "-O2 -ggdb3 -Wall"
LDFLAGS = ENV['LDFLAGS'] || ''
LIBS = ENV['LIBS'] || ''
CC = ENV['CC'] || 'cc'

# Target architecture, straight from the compiler ($CC -dumpmachine). Drives
# which guest-plumbing variant is compiled (pstepper_<arch>.{c,h},
# pstepper_trampoline_<arch>.S, pstepper_frame_<arch>.h) and which qemu-<arch>
# runs the tests. Go-style suffix (amd64 / arm64), matching the checked-in
# files. The plugin .so is host-native and target-neutral -- it is not
# affected.
TARGET_TUPLE = begin
                 `#{CC} -dumpmachine`.chomp
               rescue Errno::ENOENT
                 abort "genbuild.rb: compiler #{CC.inspect} not found (set CC=)"
               end
ARCH = case TARGET_TUPLE
       when /\A(x86_64|amd64)(-|\z)/  then 'amd64'
       when /\A(aarch64|arm64)(-|\z)/ then 'arm64'
       else
         abort "genbuild.rb: unsupported target #{TARGET_TUPLE.inspect} from " \
               "`#{CC} -dumpmachine` (known: x86_64, aarch64)"
       end

# -D_GNU_SOURCE: pstepper_<arch>.h (pulled in via pstepper.h) includes
# <ucontext.h> and _Static_asserts its pstepper_gpregs_t field order against
# the platform mcontext layout, which glibc only exposes under __USE_GNU. The
# guest plumbing needs it for that; the plugin includes only pstepper_abi.h
# but keeps the flag for a uniform command line.
PLUGIN_CFLAGS = ["-D_GNU_SOURCE",
                 "-Ithird_party/",
                 `pkg-config --cflags glib-2.0`.chomp].join(' ')

GUEST_CFLAGS = ["-D_GNU_SOURCE"]
GUEST_LDLIBS = []

# A simple "executable model" of what gets built -- see the long comment on
# the same function in aw-backtrace/genbuild.rb. `inputs:` / `src:` are
# auto-flattened, so a lone string or a (nested) array both work.
def build!(b)
  # Guest-side plumbing, linked into every test program. Portable core
  # (pstepper.c) plus the arch slice for ARCH -- pstepper_<arch>.c and its
  # hand-written trampoline pstepper_trampoline_<arch>.S.
  plumbing = b.o(src: ["pstepper.c", "pstepper_#{ARCH}.c", "pstepper_trampoline_#{ARCH}.S"],
                 cflags: GUEST_CFLAGS)

  maybe_fsgsbase = if ARCH == 'amd64' then PLUGIN_CFLAGS + " -mfsgsbase" else PLUGIN_CFLAGS end

  # The plugin. dot_so compiles -fPIC and links -shared. Only this TU sees
  # the qemu / glib headers.
  plugin = b.dot_so(name: "pstepper_plugin.so",
                    inputs: b.o(src: "pstepper_plugin.c", cflags: maybe_fsgsbase))

  # Self-checking guest programs (each computes a native reference in-process
  # and compares). `b.test` wraps the program in a Test artifact -- returned
  # like any other, the builder pulls the Test-s out and emits their
  # `.tests/<name>.trs` run edges + the `check` target (see emit_tests! /
  # testrunner.rb). The plugin .so is listed as a dep for ordering (build it
  # before running), though `ninja check` re-runs every test regardless.
  #
  # pstepper_xsave_test / pstepper_ucontext_test are x86-64-specific (xsave
  # area; the gregset_t REG_* mapping); pstepper_sve_test is arm64-specific
  # (and needs SVE codegen, hence the per-file -march). arm64 covers FP
  # round-tripping via the other tests and needs its own ucontext test later.
  test_names = %w[pstepper_test pstepper_signal_test pstepper_thread_test]
  test_names += %w[pstepper_xsave_test pstepper_ucontext_test] if ARCH == 'amd64'
  test_names += %w[pstepper_sve_test] if ARCH == 'arm64'
  tests = test_names.map do |t|
    cflags = GUEST_CFLAGS
    cflags += %w[-march=armv8-a+sve] if t == 'pstepper_sve_test'
    prog = b.program(name: t,
                     inputs: [b.o(src: "#{t}.c", cflags: cflags), plumbing],
                     libs: GUEST_LDLIBS)
    b.test(program: prog, deps: [plugin])
  end

  [plugin, *tests]
end

def better_shell_escape(str)
  return "''" if !str
  return str if Shellwords.split(str) == [str]
  simple = "'#{str}'"
  return simple if Shellwords.split(simple) == [str]
  return Shellwords.escape(str)
end

class NinjaBulder
  Artifact = Struct.new(:name, :build)
  OBJ = Struct.new(:src_name, :cflags)

  # A test. The program to run plus how to run it. `name`/`build`
  # delegate to the wrapped program Artifact so a Test flows through
  # dedup_and_emit!'s `default` line and build-statement collection
  # exactly like the bare program would; `args`/`deps` drive
  # emit_tests!.
  Test = Struct.new(:program, :args, :deps) do
    def name;  program.name;  end
    def build; program.build; end
  end

  PRECIOUS_ENVS = %w[CC CFLAGS LDFLAGS LIBS QEMU NO_GENERATOR]

  OVERRIDES = begin
                vs = PRECIOUS_ENVS.map { |n| ENV[n] }
                used_pairs = PRECIOUS_ENVS.zip(vs).select { |(_, v)| v }
                used_pairs.map { |k, v| "#{k}=#{better_shell_escape v}" }.join(' ')
              end.tap { |str| str[0, 0] = ' ' unless str.empty? }

  PREAMBLE = (<<HERE)
# generated by #{__FILE__}#{OVERRIDES}. Edit there

rule CC
  command = #{CC} -c $in #{BASE_CFLAGS} ${extra_cflags} -MMD -MF $out.d -o $out
  depfile = $out.d
  deps = gcc
  description = CC	$in : ${extra_cflags}

rule CC_PIC
  command = #{CC} -c $in #{BASE_CFLAGS} -fPIC ${extra_cflags} -MMD -MF $out.d -o $out
  depfile = $out.d
  deps = gcc
  description = CC_PIC	$in : ${extra_cflags}

rule LINK_PROG
  command = #{CC} -o $out #{LDFLAGS} $in ${link_libs} #{LIBS}

rule LINK_SO
  command = #{CC} -shared -o $out #{LDFLAGS} $in ${link_libs} #{LIBS}

rule RUN_TEST
  command = ./testrunner.rb run --qemu #{better_shell_escape ENV['QEMU']} --arch #{better_shell_escape ARCH} --no-stdout --name $name $in $args
  description = RUN $name

rule AGGREGATE
  command = ./testrunner.rb aggregate $in
  description = CHECK

rule regenerate_build_files
  command = #{__FILE__}#{OVERRIDES}
  generator = 1

#{if ENV['NO_GENERATOR'] then '#' else '' end}build build.ninja: regenerate_build_files #{__FILE__}

HERE

  def shorten_digests(digests)
    return [] if digests.empty?

    prev, *digests = digests
    prev_l = 0
    prefixes = []
    digests.each do |d|
      l = [d.size, prev.size].min
      sz = l.times.detect do |i|
        d[0..i] != prev[0..i]
      end
      prefixes << prev[0..([prev_l, sz].max)]
      prev = d
      prev_l = sz
    end
    prefixes << prev[0..prev_l]

    prefixes
  end

  def map_build(artifact)
    if artifact.respond_to? :build
      map_build(artifact.build)
    elsif artifact.kind_of?(Enumerable)
      artifact.map { |a| map_build(a) }
    elsif artifact.kind_of? String
      artifact
    else
      raise "bad type? #{artifact.inspect}"
    end
  end

  def dedup_and_emit!(artifacts)
    tests = [artifacts].flatten.grep(Test)
    artifact_names = [artifacts].flatten.map(&:name).flatten

    statements = map_build(artifacts).flatten.uniq

    File.open("build.ninja", "w") do |f|
      f << PREAMBLE.split("\n").map(&:rstrip).join("\n") + "\n\n"

      chomped = statements.map(&:chomp)

      digests = chomped.map do |build|
        next unless build =~ /_([0-9a-f]{32})-/
        $1
      end.compact.sort.uniq

      replacements = digests.zip(shorten_digests(digests))

      chomped = chomped.map do |build|
        replacements.inject(build) do |b, (digest, replacement)|
          b.gsub(digest, replacement)
        end
      end
      f << chomped.join("\n\n")
      f.puts
      f.puts

      f.puts("default #{artifact_names.join(' ')}")

      emit_tests!(f, tests)
    end

    path = File.expand_path("./build.ninja")
    STDERR.puts "Target: #{ARCH} (#{TARGET_TUPLE})"
    if OVERRIDES.empty?
      STDERR.puts "Wrote #{path}"
    else
      STDERR.puts "Wrote #{path} with overrides:#{OVERRIDES}"
    end
  end

  # `b.test(program:, deps:)` -> a Test artifact (no side effects); return it
  # from build! alongside everything else. deps are extra implicit deps of
  # the `.tests/<name>.trs` run edge (Artifact or plain name); args are
  # appended to the guest command line.
  def test(program:, args: [], deps: [])
    Test.new(program,
             args,
             [deps].flatten.map {|d| d.respond_to?(:name) ? d.name : d})
  end

  # Emit `ninja check`: every test is re-run, every time (automake `make
  # check` semantics -- no result caching). Mechanism: an inputless `phony`
  # is perpetually dirty, so making it an implicit dep of each run edge
  # forces the edge to re-run on every `ninja check`; `check` itself has an
  # output nothing creates, so it too always runs -- it re-reads every .trs
  # and refreshes test-suite.log (a real implicit output). See testrunner.rb.
  # (Plain `ninja` is unaffected: none of this is in `default`.)
  def emit_tests!(f, tests)
    return if tests.empty?

    trs = tests.map { |t| ".tests/#{t.name}.trs" }

    f.puts
    f.puts "build always : phony"
    f.puts
    tests.each do |t|
      f.print gen_build(name: ".tests/#{t.name}.trs",
                        rule: "RUN_TEST",
                        inputs: t.name,
                        extra_out: [".tests/#{t.name}.log"],
                        extra_dep: (t.deps + %w[testrunner.rb always]).uniq,
                        vars: { name: t.name, args: t.args.join(' ') })
      f.puts
    end

    f.print gen_build(name: "check",
                      rule: "AGGREGATE",
                      inputs: trs,
                      extra_out: %w[test-suite.log],
                      extra_dep: %w[testrunner.rb])
    f.puts
  end

  def gen_build(name:, rule:, inputs:, extra_dep: [], extra_out: [], vars: {})
    extra_out = extra_out.empty? ? "" : " | #{extra_out.join(' ')}"
    extra_dep = extra_dep.empty? ? "" : " | #{extra_dep.join(' ')}"
    build = "build #{name}#{extra_out} : #{rule} #{[inputs].flatten.join(' ')}#{extra_dep}\n"
    vars.each do |k, v|
      next if v.empty?
      build << "  #{k} = #{v}\n"
    end
    build
  end

  def assert_strings!(strings)
    strings.each { |s| raise unless s.kind_of? String }
  end

  def o(src:, cflags: "")
    src = [src].flatten
    assert_strings! src
    src.map { |s| OBJ.new(s, cflags) }
  end

  def o_prefix(rule, cflags)
    @prefixes ||= {}
    @prefixes[[rule, cflags]] ||= if [cflags].flatten.join(' ').empty?
                                    "#{rule.downcase}-"
                                  else
                                    "#{rule.downcase}_#{Digest::MD5.hexdigest [cflags].flatten.join(' ')}-"
                                  end
  end

  # Handles .c, .cc, .S and .s, same as gcc/clang driver and make.
  def compile_o(src, rule, cflags)
    raise unless src.kind_of? String

    o_basename = File.basename(src).gsub(/\.(c|S)\z/, '.o')
    raise "not a .c/.S source: #{src}" if o_basename == File.basename(src)

    objname = ".obj/#{o_prefix(rule, cflags)}#{o_basename}"
    [objname,
     gen_build(name: objname,
               rule: rule,
               inputs: src,
               vars: { extra_cflags: [cflags].flatten.compact.join(" ") })]
  end

  def override_cflags(objs, &block)
    [objs].flatten.map do |o|
      if o.kind_of? OBJ
        o = o.dup
        o.cflags = yield(o.src_name, [o.cflags].flatten)
        o
      else
        o
      end
    end
  end

  def build_inputs(inputs, cc_rule)
    inputs.flatten.compact.map do |obj|
      raise "not an OBJ: #{obj.inspect}" unless obj.kind_of? OBJ
      compile_o(obj.src_name, cc_rule, obj.cflags)
    end
  end

  def dot_so(name:, inputs:, libs: [])
    raise "need .so suffix" unless name.end_with?(".so")
    obj_pairs = build_inputs(inputs, "CC_PIC")
    link = gen_build(name: name,
                     rule: "LINK_SO",
                     inputs: obj_pairs.map(&:first),
                     vars: { link_libs: libs.join(' ').strip })
    Artifact.new(name, [obj_pairs.map(&:last), link])
  end

  def program(name:, inputs:, libs: [])
    obj_pairs = build_inputs(inputs, "CC")
    artifact_libs, extra_libs = libs.partition { |l| l.kind_of?(Artifact) }
    extra_deps = artifact_libs.map(&:name)

    link = gen_build(name: name,
                     rule: "LINK_PROG",
                     inputs: obj_pairs.map(&:first),
                     extra_dep: extra_deps,
                     vars: { link_libs: (extra_deps + extra_libs).join(' ') })
    Artifact.new(name, [obj_pairs.map(&:last), link])
  end

  class B
    def initialize(parent)
      @parent = parent
    end
    def o(**args); @parent.o(**args); end
    def program(**args); @parent.program(**args); end
    def dot_so(**args); @parent.dot_so(**args); end
    def test(**args); @parent.test(**args); end
    def override_cflags(objs, &block); @parent.override_cflags(objs, &block); end
  end

  def build!
    b = NinjaBulder::B.new(self)
    artifacts = yield self
    dedup_and_emit! artifacts
  end
end

run_ninja = false
run_check = false
pseudo_target = if %w[ninja check].include? ARGV[0]
                  ARGV.shift
                end
if pseudo_target == 'ninja'
  run_ninja = true
elsif pseudo_target == 'check'
  run_check = true
else
  raise "bug: pseudo_target = #{pseudo_target.inspect}" unless pseudo_target.nil?
end

unless ARGV.empty?
  overrides = {}
  re = /\A(#{NinjaBulder::PRECIOUS_ENVS.map { |e| Regexp.quote e }.join('|')})=(.*)/ #/
  ARGV.each do |arg|
    unless arg =~ re
      puts "I only accept ENV overrides like this #{re.inspect}. Got: #{arg.inspect}"
      exit 1
    end
    overrides[$1] = $2
  end
  want_rexec = false
  overrides.each_pair do |k, v|
    next unless ENV[k] != v
    ENV[k] = v
    want_rexec = true
  end
  # if at least one override doesn't match the environment, re-exec with the
  # updated environment
  if want_rexec
    require 'rbconfig'
    Kernel.exec(RbConfig.ruby, $PROGRAM_NAME, *(Array(pseudo_target) + ARGV))
    raise
  end
end

skip_regen = false

if pseudo_target
  bf = "build.ninja"
  if File.file?(bf)
    first_line = File.open(bf, "r") { |f| f.readline }.chomp
    skip_regen = true if first_line == NinjaBulder::PREAMBLE.split("\n", 2).first
  end
  # NOTE: ninja itself will regenerate build.ninja if genbuild.rb is newer
end
unless skip_regen
  NinjaBulder.new.build! do |b|
    build! b
  end
  # Now write compile_commands.json
  file = "compile_commands.json"
  cmd = "ninja -t compdb >#{file}"
  puts "$ #{cmd}"
  system cmd
  unless $?.success?
    File.unlink file rescue nil
    raise "comdb generation failed: #{$?.inspect}"
  end
end

# NOTE: invoking ninja here may and sometimes will re-invoke
# ./genbuild.rb as a generator (without ninja|check 'command', but
# with PRECIOUS_ENVS). So we'd write the build file again. This is
# harmless, if a little suprising sometimes. Why this happens? Because
# ninja uses build.ninja mtime from it's database. So even if we just
# produced 'fresh' copy, it will still see it stale.
exec "ninja -v" if run_ninja
exec "ninja -v check" if run_check
