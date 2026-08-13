#!/usr/bin/env ruby
# -*- mode: ruby -*-
#
# Differential harness for perf-convert against perf's own libdw unwinder.
#
#   perf-convert/compare.rb <input.perf.data> [options]
#
#   --our FILE       use this converted file instead of building one
#   --keep           don't delete the generated converted file
#   --diverge N      print the first N mid-chain divergences with context
#   --dump[=DIR]     write a .sample for every divergence (implies --diverge)
#   --limit N        only compare the first N samples (passes --max_samples)
#   --symbolize_dump input file is dump, we take our stack trace and symbolize instead of anything else
#
# "libdw" here == `perf script` unwinding the raw input on the fly. It is a
# second unwinder with its own bugs (it fabricates garbage past dyn_size on
# some samples); an implausible address on the libdw side is scored as
# "libdw wrong", not as a perf-convert failure.

require 'optparse'
require 'tmpdir'
require 'fileutils'

opts = { diverge: 0, dump: nil, limit: 0, symbolize_dump: false }
OptionParser.new do |o|
  o.on('--our FILE') { |v| opts[:our] = v }
  o.on('--keep') { opts[:keep] = true }
  o.on('--diverge N', Integer) { |v| opts[:diverge] = v }
  o.on('--dump [DIR]') { |v| opts[:dump] = v || 'perf-convert/testdata'; opts[:diverge] = 200 if opts[:diverge].zero? }
  o.on('--limit N', Integer) { |v| opts[:limit] = v }
  o.on('--symbolize_dump') { opts[:symbolize_dump] = true }
end.parse!

if opts[:symbolize_dump]
  input = ARGV.shift
  raise "need file to symbolize" unless input
  lines = IO.readlines(input)
  stack_string = lines.map {|l| next unless l =~ /\Aours /; $'}.compact.first
  raise "couldn't parse our: line with stack trace" unless stack_string
  addrs = stack_string.split.map {|l| l.to_i(16)}
  unless ARGV.empty?
    addrs = ARGV.map {|e| e.to_i 16}
    puts "replaced addrs with cmdline: #{addrs.map {|i| i.to_s(16)}.join(' ')}"
  end
  mappings = lines.map do |l|
    next unless l =~ /\Amap\s/
    from, to, offset, path = *($'.split)
    raise unless path
    [from,to,offset].map {|s| s.to_i 16} << path
  end.compact
  mapping_biases = mappings.map do |(from, to, offset, path)|
    # LOAD 0x03d380 0x000000000003e380 0x000000000003e380 0x0f76b0 0x0f76b0 R E 0x1000
    relf_cmd = "readelf -l -W #{path}"
    # puts relf_cmd
    output = `#{relf_cmd}`
    loads = output.split("\n").map {|l| next unless l =~ /\A\s*LOAD\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s/; [$1.to_i(16), $2.to_i(16)]}.compact
    raise if loads.empty?
    loads = loads.map {|pair| pair.map {|a| a & ~0xfff}} # round down to page start
    this_load = loads.find {|l_offset, _| l_offset == offset}
    raise unless this_load
    from - this_load.last
  end
  mappings.zip(mapping_biases).each {|m, bias| m << bias}
  vaddrs_binaries = addrs.map do |a|
    m = mappings.select {|(from, to, *_)| (from <= a && a < to)}.first
    case m
    in [from, to, offset, path, module_vaddr]
      [a - module_vaddr, path]
    in nil
      [a, nil]
    end
  end
  vaddrs_binaries.zip(addrs).each do |((vaddr, path), addr)|
    cmd = "llvm-addr2line -f -p -C -e #{path} 0x#{vaddr.to_s 16}"
    # cmd = "llvm-addr2line -f -p -C -i -e #{path} 0x#{vaddr.to_s 16}"
    out = `#{cmd}`
    prefix = "0x%016x " % [addr]
    puts(out.split("\n").map {|l| prefix + l + " #{path} + 0x#{vaddr.to_s 16} (vaddr)"})
  end
  exit
end

input = ARGV.shift or abort "usage: compare.rb <input.perf.data> [options]"
root = File.expand_path('..', __dir__)
Dir.chdir(root)

bin = 'bazel-bin/perf-convert/perf-convert'
system('bazel', 'build', '-c', 'opt', '//perf-convert:perf-convert', exception: true)

our = opts[:our]
tmp = nil
unless our
  tmp = Dir.mktmpdir('perf-convert')
  our = File.join(tmp, 'converted.data')
  args = [bin, '--input', input, '--output', our]
  args += ['--max_samples', opts[:limit].to_s] if opts[:limit] > 0
  puts "== converting =="
  system(*args, exception: true)
end

# Parse `perf script` callchain output into { [pid,tid,time_ns] => [addr,...] }.
def load(path)
  out = {}
  key = nil
  cur = nil
  IO.popen(['perf', 'script', '-i', path, '--ns', '-F', 'pid,tid,time,ip', '--no-inline'],
           err: File::NULL) do |io|
    io.each_line do |ln|
      if (m = ln.match(/^\s+(\d+)\/(\d+)\s+(\d+)\.(\d+):/))
        out[key] = cur if key
        key = [m[1].to_i, m[2].to_i, m[3].to_i * 1_000_000_000 + m[4].to_i]
        cur = []
      elsif ln.start_with?("\t") && cur
        cur << Integer(ln.strip.split.first, 16) if ln =~ /\h/
      end
    end
  end
  out[key] = cur if key
  out
end

KERNEL = 0xffff_8000_0000_0000
def userpart(ch) = ch.drop_while { |a| a >= KERNEL }

def implausible?(a) = a < 0x10_000 || (a > 0x0000_8000_0000_0000 && a < 0xffff_0000_0000_0000)

# Best common-prefix length allowing mine to carry 0 or 1 extra leading
# frame and a +/-1 slop per frame (mine stores raw RA, libdw pre-decrements).
def prefix(mine, ref)
  best = [-1, 0]
  [0, 1].each do |s|
    mm = mine.drop(s)
    d = 0
    d += 1 while d < mm.size && d < ref.size && (mm[d] - ref[d]).abs <= 1
    best = [d, s] if d > best[0]
  end
  best
end

puts "== comparing =="
ref = load(input)
mine = load(our)
common = ref.keys & mine.keys
puts "ref #{ref.size} samples, mine #{mine.size}, common #{common.size}"

exact = deeper = truncated = diverge = libdw_empty = 0
hist = Hash.new(0)
diverges = []

common.each do |k|
  mu = userpart(mine[k])
  ru = userpart(ref[k])
  (libdw_empty += 1; next) if ru.empty?
  d, s = prefix(mu, ru)
  hist[[d, 30].min] += 1
  mm = mu.drop(s)
  if d == ru.size && d == mm.size
    exact += 1
  elsif d == ru.size && mm.size > d
    deeper += 1
  elsif d == mm.size && ru.size > d
    truncated += 1
    # diverges << [k, d, s, mu, ru]
  else
    diverge += 1
    diverges << [k, d, s, mu, ru]
  end
end

total = exact + deeper + truncated + diverge
d_garbage = diverges.count { |_k, d, _s, _mu, ru| implausible?(ru[d] || 0) }
puts
puts "  exact match ................ #{exact}  (#{'%.1f' % (100.0 * exact / total)}%)"
puts "  mine agrees, goes deeper ... #{deeper}   (libdw stopped early)"
puts "  mine truncated early ....... #{truncated}"
puts "  mid-chain divergence ....... #{diverge}  (#{d_garbage} = libdw garbage, #{diverge - d_garbage} genuine)"
puts "  libdw produced no frames ... #{libdw_empty}"
puts
puts "  prefix-match-depth histogram (capped 30):"
hist.sort.each { |dd, n| puts "    #{'%2d' % dd}: #{n}" }

if opts[:diverge] > 0 && !diverges.empty?
  puts
  puts "== first #{[opts[:diverge], diverges.size].min} divergences =="
  FileUtils.mkdir_p(opts[:dump]) if opts[:dump]
  want = []
  diverges.first(opts[:diverge]).each do |k, d, s, mu, ru|
    lo = [d - 1, 0].max
    tag = implausible?(ru[d] || 0) ? '  [libdw garbage]' : ''
    puts "  #{k.join(':')}  diverge@#{d}#{tag}"
    puts "    mine: #{mu.drop(s)[lo, 4].map { |x| '0x%x' % x }.join(' ')}"
    puts "    ref : #{ru[lo, 4].map { |x| '0x%x' % x }.join(' ')}"
    want << k.join(':')
  end
  if opts[:dump]
    puts
    puts "== dumping #{want.size} samples to #{opts[:dump]} =="
    system(bin, '--input', input, '--output', File::NULL,
           '--dump_samples', want.join(','), '--dump_dir', opts[:dump])
  end
end

FileUtils.remove_entry(tmp) if tmp && !opts[:keep]
