# aw-backtrace

aw-backtrace is a library for capturing backtraces that is safe,
robust, and quick. It is also fully async-signal-safe. Our slogan:
Producing Excellent Backtraces At Last

aw-backtrace goals are the following (mostly in decreasing priority
order):

* Never crash. Real-world compilers will keep screwing up at producing
  unwind info, and so we might occasionally produce wrong backtraces,
  but we won't crash. aw-backtrace is also fully async-signal-safe. Go
  ahead and call it in fatal signal handlers or SIGPROF handlers and
  it will just work. Need your backtraces in the deepest guts, e.g.,
  malloc sampling, or lock contention profiler?  aw-backtrace will
  handle it without crashes or deadlocks.
* Produce perfect backtraces as often as possible. We will
  occasionally fail at this. For example, at some asm codes that don't
  have CFI bits, but we try hard to cover even those cases with
  reasonable heuristics. We're not perfect (yet), but what we have
  today should easily cover nearly all the practical cases, even some
  exotic ones. I've actually tested many of them.
* Be quick. People should not have reason to resort to imperfect
  methods, such as frame-pointer-based backtracers. In other words:
  long live -fomit-frame-pointer!

## What works today

It is worth noting that this is just the first public release (today
is Sep 1st 2026). Testing was somewhat significant, yet since it was
tested only by the author, an unknown number of bugs might still
lurk. Please test it on your workloads and report back. Any feedback
is highly welcome!

We currently support modern Linux with `_dl_find_object` (so
glibc-only right now; needs glibc 2.35 or later). x86-64 support has
been the main focus so far, particularly on the testing side. Aarch64
support exists, but is currently very basic.

I did all the testing on a recent Linux kernel with the
`PROCMAP_QUERY` ioctl (so 6.11 or later), and the best performance
requires it. There is an alternative method for discovering VMA
boundaries: reading proc-pid-maps, but it is not as fast and scales
poorly with the number of VMAs. We cache each thread's stack VMA
bounds, so in most cases we won't need to look this up again.

The eventual goal is to support other systems, including those without
`_dl_find_object`. The author plans to add full support for common
architectures: aarch64, i386, RISC-V, and (classic, 32-bit) ARM.

The bulk of the testing was validating that aw-backtrace, indeed,
meets its goals ***at every instruction boundary*** in practical
binaries (with very few exceptions).

Header is plain C (see `include/aw-backtrace/aw-backtrace.h`). It is
just 2 functions similar to `backtrace()` and
`_Unwind_Backtrace()`. Code is C++20 (but should be possible to
downgrade to C++17; drop me a line if you want this). Currently
building with Bazel (yes, I am sorry for that). There is no "make
install" story yet. And the API isn't stable yet. I am still not sure
about the finer details of a backtracing API that is rock-solid, yet
fast. You can "consume" aw-backtrace via the Bazel central repo
thing. Or via gperftools, which vendors the code and exposes its
normal google3/abseil-style stack trace APIs.

aw-backtrace's main approach is to capture backtraces via
`.eh_frame{,_hdr}` information. People commonly call it "DWARF",
although there is a subtle difference. Backtracing via DWARF has
gotten a somewhat bad reputation. Enough that people are actively
developing alternatives. While I am not religious about how to get
perfect backtraces, and aw-backtrace will adapt to whatever is
shipping, I became quite convinced that DWARF isn't the problem people
think it is. aw-backtrace implements a subset of DWARF which is
slightly richer than what is assumed by ORC or SFrame. Common
unsupported cases (e.g., GNU ld puts smart DWARF expressions
describing PLT entries, or occasional DRAP, or signal frames) are
handled by dedicated heuristics. I've swept my own machine for cases
that need more DWARF than is currently supported, and there are only a
few such FDEs, mostly in "leaf functions", so shouldn't compromise CPU
profiles too much and should not affect other cases at all (e.g.,
malloc heap sampling).

For crash-freeness, we validate and bounds-check all the
`.eh_frame{,_hdr}` data. We validate stack accesses when fetching
saved register values. And when we inspect executable code for
heuristics, we also validate those accesses.

For correctness of backtraces, we special-case PLT, signal frames, and
amd64 DRAP frames. We even work around gcc unwind info bug in the DRAP
functions. We also have, hopefully, a better-than-usual heuristic to
guess backtracing through CFI-less code, such as JIT or assembly.

Performance is a major highlight as well. While there is still a ton
of low-hanging fruit in the performance area in this early release, a
key design aspect already makes performance quite competitive. The
observation it rests on is that backtraces have great spatial
locality, i.e., the same call sites are seen over and over. Which
makes it super attractive to apply caching. This is not a new idea, of
course. "Classic" libunwind has had a caching facility for some time
now. But it is not async-signal-safe, and it is per-thread (in default
configuration) and unbounded, so not good for general use. We have a
simple and fully async-signal-safe cache that makes most backtraces
super quick. We can afford "DWARF bits" not to be particularly quick,
simply because cache hits cover the speed in practice. Although I did
pay some attention to proving that "DWARF bits" are not too bad
perf-wise.

Current cache implementation requires double-word atomics. But it is
load-only in the common case (so no cache lines dirtying). I had a
fancier design but decided to ship a simpler, more obviously correct
variant. Don't want people to get the impression that a fast,
async-signal-safe backtracer requires an overly complex cache
design--just some careful engineering. See unwind-info-cache.h

Another aspect of caching is invalidation policy. `_dl_find_object`
makes it difficult for us. Right now the implementation assumes that
aw-backtrace is loaded together with the initial set of modules (e.g.,
with the main binary or `LD_PRELOAD`). It sweeps the initially loaded
set via `dl_iterate_phdr`, and then the cache is only populated for
frames from those modules. Those modules are assumed to be never
unloadable. So caching for them is safe. Good enough for most
use-cases, given current limitations. I plan to address this
limitation and enable caching that fully respects `dl{open,close}`.

## Testing

There are some basic tests to ensure simple, straightforward cases are
covered. Those pass even on aarch64 currently. The main testing
approach was the `backtrace-comparer` thing. The backtrace comparer
uses a single-stepping interrupt of x86. It tracks the ~actual call
stack by monitoring the execution of call and return instructions. And
it invokes aw_backtrace and compares the two. This testing currently
finds cases of missing or incorrect unwind info, as well as some
unwind cases that are currently not supported (e.g., longjmp with
unusual CFI bits (caller's SP != CFA)).

Another form of testing that I did was via the perf-convert tool. It
currently uses a fastpath subset of aw-backtrace to read perf record
dumps with the --call-graph dwarf option. It reads the stack and mapped
files and produces perf recordings that perf thinks were generated
with frame pointers. I was comparing those backtraces against what
distro-shipped perf itself extracts with libdw, and so far,
aw-backtrace looks superior, even with fast-path limitations. It is
also incredibly fast at converting multi-gigabyte perf recordings in
seconds (and without any caching!).

Another form of testing is in the sibling project unwind-check,
which performs "offline" analysis of code's stack/call-frame-slot
effects against CFI unwind info. Mismatches that I discovered and
verified manually are not due to bad unwind info
decoding/interpretation, but genuinely wrong unwind info.

BTW, the good news from a sweep of binaries on my machine is that
unwind info bugs are rare in relative terms: on the order of half a
percent of all FDEs across a few thousand binaries. Hopefully now they
are being reported, and compilers/runtimes will get them fixed. Of
those mismatches I inspected by far most come from just a small
handful of specific unwind info generation bugs. Of those, I have so
far isolated 2 seemingly distinct LLVM bugs (so, affecting clang and
rust); in both the mismatch is just a couple of instructions and
leaf-only (i.e., not crossing call sites). Reporting those very
shortly.

I've done some initial fuzzing too, and it caught a couple of missing
error-handling paths.

## Benchmark

Currently, on a microbenchmark, we get roughly 10x slower than the
trivial frame pointer backtracer for cache hits. IMHO good enough even
for some very extremely backtrace-heavy uses (e.g. gperftools'
classic/wasteful allocation profiler). Yes, microbenchmarks have
serious limitations. This microbenchmark is mostly cache hits, whereas
real backtrace requests are likely to see a decent number of cache
misses. Still, it is some signal and better than nothing. On the same
microbenchmark, libgcc is about 5x slower. LLVM's libunwind is another
5x slower than libgcc. All aw-backtrace speed comes with
safety. Unlike, e.g., libgcc, we won't crash the process on wrong
unwind info.

See recursion-test.cc for more details.

Here is what I get:

```
$ CC=clang bazel build -c opt --copt=-ggdb3 --copt=-fno-omit-frame-pointer --copt=-march=native :recursion-test{,-libgcc,-fp}

$ ./bazel-bin/recursion-test-fp
nanos per iter (for depth of 1024): 1179.49
... per unwind step: 1.15185
nanos per iter (for depth of 512): 621.68
... per unwind step: 1.21422
nanos per iter (for depth of 256): 238.281
... per unwind step: 0.930786
nanos per iter (for depth of 128): 113.086
... per unwind step: 0.883484
nanos per iter (for depth of 64): 51.3672
... per unwind step: 0.802612
nanos per iter (for depth of 32): 20.4102
... per unwind step: 0.637817

$ ./bazel-bin/recursion-test
nanos per iter (for depth of 1024): 9283.2
... per unwind step: 9.06563
nanos per iter (for depth of 512): 4614.16
... per unwind step: 9.01203
nanos per iter (for depth of 256): 2305.86
... per unwind step: 9.00726
nanos per iter (for depth of 128): 1151.76
... per unwind step: 8.99811
nanos per iter (for depth of 64): 583.496
... per unwind step: 9.11713
nanos per iter (for depth of 32): 308.984
... per unwind step: 9.65576

$ ./bazel-bin/recursion-test-libgcc
nanos per iter (for depth of 1024): 60040.4
... per unwind step: 58.6332
nanos per iter (for depth of 512): 27753.2
... per unwind step: 54.2055
nanos per iter (for depth of 256): 13398.6
... per unwind step: 52.3384
nanos per iter (for depth of 128): 6780.57
... per unwind step: 52.9732
nanos per iter (for depth of 64): 3415.14
... per unwind step: 53.3615
nanos per iter (for depth of 32): 1767.29
... per unwind step: 55.2277
Result: 0, Ops: 16385

# This is LLVM's libunwind
$ LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libunwind.so.1 ./bazel-bin/recursion-test-libgcc
nanos per iter (for depth of 1024): 293898
... per unwind step: 287.01
nanos per iter (for depth of 512): 145691
... per unwind step: 284.553
nanos per iter (for depth of 256): 70974
... per unwind step: 277.242
nanos per iter (for depth of 128): 35317.1
... per unwind step: 275.915
nanos per iter (for depth of 64): 18502.1
... per unwind step: 289.096
nanos per iter (for depth of 32): 9358.2
... per unwind step: 292.444
Result: 0, Ops: 16385

# turning off aw-backtrace cache
$ CC=clang bazel build -c opt --copt=-ggdb3 --copt=-fno-omit-frame-pointer --copt=-DTESTING_NO_CACHE --copt=-march=native :recursion-test
$ ./bazel-bin/recursion-test
nanos per iter (for depth of 1024): 29263
... per unwind step: 28.5771
nanos per iter (for depth of 512): 14647.6
... per unwind step: 28.6085
nanos per iter (for depth of 256): 7414.26
... per unwind step: 28.9619
nanos per iter (for depth of 128): 3668.65
... per unwind step: 28.6613
nanos per iter (for depth of 64): 1880.27
... per unwind step: 29.3793
nanos per iter (for depth of 32): 917.383
... per unwind step: 28.6682

# switching recursion-test codegen to longer CFI due to lack of frame pointers
$ CC=clang bazel build -c opt --copt=-ggdb3 --copt=-fomit-frame-pointer --copt=-DTESTING_NO_CACHE --copt=-march=native :recursion-test{,-libgcc,-fp}

$ ./bazel-bin/recursion-test
nanos per iter (for depth of 1024): 77895.4
... per unwind step: 76.0697
nanos per iter (for depth of 512): 35762.3
... per unwind step: 69.8483
nanos per iter (for depth of 256): 17789.6
... per unwind step: 69.4908
nanos per iter (for depth of 128): 8975.49
... per unwind step: 70.121
nanos per iter (for depth of 64): 4466.41
... per unwind step: 69.7876
nanos per iter (for depth of 32): 2273.34
... per unwind step: 71.0419

$ ./bazel-bin/recursion-test-libgcc
nanos per iter (for depth of 1024): 136785
... per unwind step: 133.579
nanos per iter (for depth of 512): 64173
... per unwind step: 125.338
nanos per iter (for depth of 256): 31194.6
... per unwind step: 121.854
nanos per iter (for depth of 128): 15681.6
... per unwind step: 122.513
nanos per iter (for depth of 64): 7838.67
... per unwind step: 122.479
nanos per iter (for depth of 32): 3976.76
... per unwind step: 124.274

$ LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libunwind.so.1 ./bazel-bin/recursion-test-libgcc
nanos per iter (for depth of 1024): 537799
... per unwind step: 525.194
nanos per iter (for depth of 512): 267301
... per unwind step: 522.072
nanos per iter (for depth of 256): 133751
... per unwind step: 522.466
nanos per iter (for depth of 128): 66814.6
... per unwind step: 521.989
nanos per iter (for depth of 64): 31475.1
... per unwind step: 491.798
nanos per iter (for depth of 32): 15956.7
... per unwind step: 498.648


```

## Code pointers

### Core library

Core library: the "production" and critical parts that run in
async-signal-safe context are:

* `include/aw-backtrace/aw-backtrace.h` "public" API header (as
  stated, the caveat is that API isn't stable yet).
* `aw-arch.h` and `aw-arch-{x86_64,aarch64}.h` --
  architecture-specific parts (arch defaults, plt/signal-frame
  detection, guessing unwind info etc).
* `aw-structs.h` and `dwarf-constants.h` -- basic arch-neutral
  definitions
* `eh-frame-reader.h` -- general code for reading `.eh_frame{,_hdr}`
  stuff. Independent of interpretation of the information.
* `backtrace-core.{h,cc}` -- the code that locates `eh_frame_hdr`
  via `_dl_find_object` and the "visitor" for eh-frame-reader that
  actually constructs FrameInfo-s describing how to unwind one frame.
* `aw-backtrace-fastpath.h` -- parser _and_ interpreter of a common
  subset of DWARF CFI that targets performance. Arguably, not super
  necessary, but I was curious to see how fast we can do "raw DWARF"
  while respecting safety guarantees.
* `unwind-info-cache.h` and `simple-counter.h` -- cache
  implementation.
* `aw-addrcheck.{c.h}` -- locating VMA bounds.
* `aw-backtrace.cc` the main thing that wires up everything and
  provides "public" API and some diagnostics.
* `backtrace-drap.h` -- specialized "visitor" for eh-frame-reader that
  knows how to recognize FrameInfo for x86 DRAP functions. See
  `doc/amd64-drap-problem.adoc`. Chances are, you've never heard of
  DRAP. In that case, I suggest you ignore this part.
* `with-exit.h` -- header-only non-local exits: a thin `_setjmp`/`_longjmp`
  wrapper (`WithExit::Run`/`Exit`) that hides the returns-twice from callers.

As you can see, the code is reasonably well split up and should be
possible to follow/inspect.

### Backtrace comparer

Note: backtrace comparer is used both by aw-backtrace-test (built and
exercised by bazel) and by ./genbuild.rb, which produces
LD_PRELOAD-able .so. As noted above, single-stepping callback tracks
real call stack (modulo non-local exits) and compares it with what
aw-backtrace captures. On every instruction (modulo fragments that
block SIGTRAP signal). If a mismatch is detected, it either dies or
prints the mismatch. Few known suppressions are considered. They're in
gcc startup/exit bits that, for complicated-ish reasons, don't have
unwind info and are not handled by unwind heuristics.

Code is a little messy in places, because the problem space is, and
because we can afford some mess for testing bits. Also notable is that
since it relies on TF and SIGTRAP, it is incompatible with gdb. Which
makes debugging mismatches a bit of pain and is the source of some
mess in backtrace-comparer.

* `v/mini-x86-int/**` -- "mini x86 interpreter" -- the single stepper
  bits I pulled from gperftools and some "interpreter" for
  small/simple subset of x86 so that single-stepping is not as slow.
* `backtrace-comparer.{cc,h,so.map}` -- the thing.
* `symbolize-backtrace.{h,cc}` and `sym-helper.cc` the bits that
  symbolize backtraces (convert addresses in backtrace to inline-ful
  function names + source locations)
* `genbuild.rb` -- experimental ninja-based producer of
  `backtrace-comparer.so`. Run with e.g. `$ ./genbuild.rb ninja` or
  `$ CXXFLAGS='-O0 -ggdb3' ./genbuild.rb ninja` or `./genbuild.rb && ninja`.

If you want to exercise backtrace-comparer yourself, here is how:

```
$ ./genbuild.rb ninja
$ LD_PRELOAD=./backtrace-comparer.so /usr/bin/true
$ LD_PRELOAD=./backtrace-comparer.so gcc --version
$ LD_PRELOAD=./backtrace-comparer.so ruby -e 'pp ENV'
```

The slowdown is approximately 1000x (what you wanted? We're capturing
full backtrace on every instruction boundary; twice).

### Testing/benchmarking

Mentioning only particularly notable parts:

* aw-backtrace-test.cc -- main test which does basics and exercises
  backtrace-comparer when on x86-64. Including the DRAP thing in
  `amd64-drap-test.s`
* lua-test.cc -- runs lua parser and compiler through
  backtrace-comparer.
* `simple-fp-backtrace.{h,cc}` -- the straightforward frame-pointer-based
  backtracer. Used to compare performance with our "production" bits.
* `recursion-test.cc` -- fake AST interpreter that produces deep stack
  traces and "interesting" CFI. This is our microbenchmark. Gets built
  into bazel-bin/recursion-test{,-fp,-libgcc}. I.e., 3 variants to
  compare. Do note that bazel currently builds with frame pointers by
  default. So it "simplifies" CFI by making it relative to %rbp. And
  non-cache-ful design run significantly quicker with this. So for
  full picture also build without frame pointers (as demonstrated in
  the Benchmark section).
* `fuzz/*` -- fuzzing bits. See `README.md` and sources inside for
  more details.

### perf-convert

`perf-convert` subdirectory contains the tool that reads a perf-record
file with saved user-space stack (e.g. with --call-graph dwarf) and
converts those raw stack bytes into actual stack traces, as if
recording was done with LBR or frame pointers. Currently, it uses
aw-backtrace-fastpath.h, so it only handles a subset of what we can
do, but works surprisingly well (speed-wise and
correctness-wise). Especially compared to what the perf tool uses.

So, it basically makes perf-record bearable for --call-graph
dwarf. Just note that perf's default sampling frequency is a little
high (e.g., 4 kHz for `task-clock`), so consider reducing it to
something sensible, like 100 Hz (same as gperftools and Go).

Building and running perf-convert is like this:

```
$ bazel build -c opt --copt=-g perf-convert:perf-convert
$ perf record -m 4M -e task-clock/freq=200/ --call-graph dwarf,32768 -p 591517
$ ./bazel-bin/perf-convert/perf-convert --input=perf.data --output=c.data
$ perf report -i c.data
```

`-c opt` is reasonably important. Sadly, Bazel is among those several
C++ build tools that produce mostly useless binaries by default.

## LLM disclosure

I've used LLMs (Gemini and Claude) to help me research, plan, develop,
and review this code.

I read the code. I fully own all the code, slop or not, produced by me
or an LLM under my supervision. In general, I keep LLM-produced code
in line with my style. Of course, especially in comments, LLMs produce
quite different output than what I write. But when the style differs
too much, I generally either edit/rewrite it myself or instruct the
LLM to do so.

Extensive parts of this code were written by me 'by hand' for various
reasons. Perhaps in the future I will lean on LLMs more to save time.
