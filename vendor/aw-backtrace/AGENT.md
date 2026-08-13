# AGENT.md — orientation for coding agents

Where things live and why they are the way they are. Deliberately not a
substitute for reading the code: when this file describes a mechanism it says
*why it exists*, not how it is spelled. Grep for a named symbol rather than
trusting a description that looks stale.

`README.md` is the *user's* introduction — what the library does, why, and how
to call it. It is also where the design rationale now lives; this file is the
part that only matters once you are editing the code.

## 1. What this project is

A from-scratch, **async-signal-safe backtrace library for profilers** ("aw" =
the prefix on all public symbols). Unwinds from `.eh_frame` / `.eh_frame_hdr`
CFI parsed by hand, with no libunwind/libgcc dependency.

The contract:

* Never crash, never deadlock, never allocate on the sampling path. An
  occasional *wrong* backtrace is acceptable; crashing is not.
* Only PC, SP and FP are unwound. Full DWARF register-state unwinding is
  deliberately absent — the bet is that essentially all real frames are either
  fixed-offset-from-SP or RBP-framed.
* Anything outside that model is an explicit, reported error, never a silent
  guess.
* Modern Linux, glibc 2.35+ (`_dl_find_object`). x86-64 is where the testing
  is; aarch64 builds and passes the basic tests but has no comparer and no
  `GuessUnwindInfo`. riscv and 32-bit eventually.

## 2. Build and test

**Two build systems that build different things.** Neither is a superset.

### Bazel — the library and the test suite

bzlmod, one top-level package, module `aw-backtrace`.

```
bazel test ...:all               # the normal loop: 12 tests
bazel test -c opt ...:all        # NDEBUG: drops assert(), keeps CHECK()
./test-all-cfg.rb                # gcc/clang x dbg/opt sweep, all four green
```

Eleven tests: nine in the top package plus `//perf-convert`'s two. `:all` instead
of `...:all` skips perf-convert. `v/mini-x86-int` is in `.bazelignore`, so
`@mini-x86-int//:sim_stepper_test` runs only by explicit label.

Traps worth knowing before you fight the build:

* **`-std=c++20 -fno-exceptions -fno-rtti` live in `BUILD.bazel`'s `CXXOPTS`,
  per target, on purpose.** A project that consumes this module never reads our
  `.bazelrc`, so anything we need has to travel with the targets. `cxxopts`
  rather than `copts` so it stays off the C compiles (`aw-addrcheck.c`). The
  public header is deliberately standard-agnostic — it compiles as C99 and as
  C++11 and up — so a consumer on an older standard can still include it.
* **All internal headers are one target, `:internal`** (a `glob(["*.h"])`).
  Every target that compiles C++ deps `:internal` and `#include`s whatever it
  needs — there are no per-header libraries and no dependency-layering to keep
  in sync. `:internal` carries no compile options (headers only), so it leaks
  nothing into a consumer; the `.cc` implementations still have their own
  targets. Two things stay out of the glob: `aw-addrcheck.h` (its own `:aw-
  addrcheck` C target, `exclude`d) and the public header (`:aw-backtrace-hdr`,
  shipped under `aw-backtrace/` via `strip_include_prefix`). `:internal` also
  carries `linkopts = ["-latomic"]`, so every C++ target gets it.
* **`.bazelrc` is for *this* workspace only.** It forces `-std=c++20` across the
  whole graph so abseil matches what `//perf-convert` compiles against. It does
  nothing for anyone depending on us.
* **`.bazelversion` pins 9.2.0, load-bearing.** `dev_dependency` belongs on the
  `bazel_dep` line; 9.2.0 rejects it on `local_path_override` and every `bazel`
  invocation dies.
* **GNU ld intermittently segfaults linking this package.** Worked around with
  `features = ["-supports_start_end_lib"]` on the targets that hit it — a
  toolchain interaction, not a code bug. Copy the line onto any new binary
  target that starts crashing `ld`.
* **`-latomic` is required for gcc *and* clang.** `UnwindInfoCache` uses 16-byte
  atomics; without `-mavx -mcx16` both compilers call libatomic (~2.5ns/step
  under clang). `ATTR_DWORD_ATOMICS`, which used to inline them, is deliberately
  an empty macro — it emitted VEX unconditionally and `SIGILL`ed on pre-AVX.
* **`v/mini-x86-int` is "vendored" only mechanically** — co-developed, same
  author, no upstream. A fix belonging in the stepper goes in the stepper.
* `:aw-backtrace` is the only public target and the only public header
  (`include/aw-backtrace/aw-backtrace.h`, via `strip_include_prefix`).
* `with-exit.h` is header-only: `WithExit::Run`/`Exit` are a thin
  `_setjmp`/`_longjmp` wrapper.

### genbuild.rb / ninja — the LD_PRELOAD comparer

`genbuild.rb` is a self-contained generator (`build!` at the top is the part you
edit). **Edit `genbuild.rb`, never `build.ninja`.**

```
./genbuild.rb ninja                          # regenerate if stale, then build
./genbuild.rb ninja CC=clang CXX=clang++     # ENV overrides; re-execs itself
```

Defaults are `-ggdb3 -O2 -DNDEBUG -Wall -Wextra -march=native`, i.e. an NDEBUG,
machine-specific build — don't copy the `.so` to another machine, it will
`SIGILL`. Five artifacts, none of which Bazel builds: `backtrace-comparer.so`,
`sym_helper_bin`, `test-signal-disable`, and `cjm`/`cjm0`.

### Two things about test configurations

**Only `bazel test -c dbg` traps on unwinder diagnostics.** Non-`NDEBUG` builds
capture with `trap_diagnostics`, so a diagnostic aborts where it happened; `#if
defined(NDEBUG) || defined(BUILD_SO)` opts out, so the preloadable `.so` never
traps — it walks startup and libc code nobody will fix.

**A comparer mismatch is a test failure.** `aw-backtrace-test` sets
`AW_BT_DIAG=0` and `AW_BT_DIAG_VIA_CORE=1` (with `overwrite=0`, so your values
win) so the first mismatch kills the process, in `-c opt` too. Knobs in §5.
Cache counters only exist when `AW_BUMP_STATS_IN_PRODUCTION` is defined —
`genbuild.rb` defines it, Bazel does not.

## 3. Code map

### Core library

| file | what |
| --- | --- |
| `include/aw-backtrace/aw-backtrace.h` | the whole public API: `aw_backtrace_full`, `aw_backtrace`, plus `aw_backtrace_ext::DebugExtensionV0` (test/inspection surface, `TryGet()` may return null, not stable ABI) |
| `aw-backtrace.cc` | entry points, `UnwindLoop`, `UnwindLoopFastPath`, `StackAccess`, the `NoDiag`/`RuntimeDiag` policies, `FrameInfoCache`, `DebugExtensionV0` impl |
| `aw-backtrace-fastpath.h` | `TryFastFrameInfo` / `FastPathFrame` (§4.8): a single inlined pass over `.eh_frame_hdr`+FDE+CIE that handles the common frame shapes and bails (`Failure()`) on anything else. `ToFrameInfo` turns its result into a `FrameInfo` |
| `backtrace-core.{h,cc}` | the *policy* half of CFI lookup: a visitor turning decoded CFI into a `FrameInfo`. No `.eh_frame` parsing, no arch cases, no `is_leaf` |
| `eh-frame-reader.h` | the *decoder* half: `.eh_frame_hdr` search, CIE/FDE parsing, the CFI opcode loop. Visitor-templated, knows nothing above it |
| `backtrace-drap.h` | x86-64 only. Second minimal visitor for gcc's DRAP prologues (§4.3) |
| `aw-structs.h` | the unwind model: `CfaRule`, `RegisterRule`, `FrameInfo`, `Cursor`, `CompressedFrameInfo` |
| `aw-arch.h`, `aw-arch-x86_64.h`, `aw-arch-aarch64.h` | per-arch `struct Arch` with a fixed static interface. Adding an arch means implementing exactly that set |
| `unwind-info-cache.h` | `UnwindInfoCache`: lock-free, fixed-size, bucketed, second-chance eviction (§4.4) |
| `aw-addrcheck.{h,c}` | async-signal-safe `/proc/self/maps` querying, `PROCMAP_QUERY` or snapshot+bsearch. Validates addresses before dereferencing |
| `with-exit.h` | `WithExit::Run`/`Exit` — header-only `_setjmp`/`_longjmp` wrapper hiding the returns-twice. **Nothing runs on the way out**, no destructors |
| `check.h` | `CHECK()`, unconditional (not `NDEBUG`-gated). For differential self-checks, never the capture path |
| `dwarf-constants.h`, `utils.h`, `simple-counter.h`, `static_storage.h`, `function_ref.h` | constants and small utilities lifted from gperftools/tcmalloc |

### Reference / comparison implementations

* `simple-fp-backtrace.{h,cc}` — pure frame-pointer walker, the passing baseline.

### Test / tooling

* **`backtrace-comparer.cc` — the best test asset in the repo.**
  Single-steps the whole process via `mini-x86-int`'s sim-stepper,
  maintains a shadow call stack from `call`/`ret`, and compares the
  capture against it *at every instruction*. Needs no expected
  values. Known-bogus spots (`_dl_fixup`, `call_init`,
  `__run_exit_handlers`) are suppressed by symbolizing the top frames;
  matches are cached, and an unsuppressed mismatch registers its own
  top frame so each location reports once. §4.6 before trusting a
  quiet run. Uses x86 TF flag and SIGTRAP so "hostile" to GDB-ing.
* **`backtrace-comparer.so`** (`-DBUILD_SO`, ninja only) — the same
  thing as an LD_PRELOAD object, which is what points the differential
  machine at arbitrary already-built binaries. Interposes
  `sigaltstack`/`sigaction` so the target can't disable the stepper,
  `unsetenv`s `LD_PRELOAD` so it doesn't follow into children, exports
  almost nothing (version script). Usage in §5.
* `aw-backtrace-test.cc` — main end-to-end test. Captures from a
  SIGILL handler (so the top frame is a real signal frame with a
  `ucontext`) and checks that walking *through* the signal frame
  without one lands in the same place — the direct test of
  `Arch::IsSignalFrame`. Also hosts the DRAP fixtures. Compiled twice,
  the second time as `simple-backtrace-test` (`BT_USE_SIMPLE`).
* `amd64-leaf-test.cc` — hand-written asm with deliberate CFI shapes,
  compared against glibc `backtrace()`. The minimized repro vehicle
  for leaf/epilogue issues, and the gdb-friendly binary (no comparer).
  Also holds the jump-through-null test (§4.1): `call *%rax` with `%rax == 0`,
  caught by a SIGSEGV handler that captures and `siglongjmp`s out.
* `aw-backtrace-skip-test.cc` — the `skip` argument of `aw_backtrace`: skipping
  N frames must equal a full capture with N dropped, and the returned count is
  what was actually filled.
* `amd64-drap-test.{c,s}` — gcc 16 `-mforce-drap` output supplying
  `minimal_drap`; the pre-gcc-16 shape lives on as `minimal_drap_2` in
  `aw-backtrace-test.cc`.
* `eh-frame-reader-test.cc` — the only place the decoder runs without a real
  process image. `Optionalize()` turns a reader `Fail()` into `std::nullopt`, so
  "must be rejected" is an `EXPECT_EQ`. Stops at the byte-level helpers.
* `symbolize-backtrace-test.cc` — installs a **`SIGCHLD` handler that
  `abort()`s** first thing in `main`; that is the assertion for §4.5's whole
  design. Also the repeated-PC regression test and
  `DumpStackTraceWithClosedStdio` (symbolizes with 0/1/2 closed).
* `comparer-longjmp.cc` → `cjm`/`cjm0` — the first in-tree program written to
  make the comparer *fail*: `_longjmp` out of deep recursion, then a frame
  pointer deliberately off by one bit. **Asserts nothing** — an eyeball vehicle.
  So does `recursion-test.cc`, a jump-table-heavy benchmark built three ways
  (aw/libgcc/fp) to compare speed — the numbers in README's Benchmark section
  come from it.
* `lua-test.cc` — the comparer over the Lua compiler, for varied real codegen.
* `aw-addrcheck-test.c`, `bench-addrcheck.c` — unit test / microbenchmark. The
  test `#undef NDEBUG`s at the top because it asserts for effect.
* `symbolize-backtrace.{h,cc}` + `sym-helper.cc` — async-signal-safe
  symbolization; `sym-helper` is embedded as a blob, written out, spawned, and
  shells to addr2line. **Diagnostics and tests only, never the capture path.**
  The spawning half is the subtlest code in the tree after the cache — §4.5.
* `v/mini-x86-int/` — co-developed sibling: partial x86-64 interpreter +
  `sim_stepper` providing the per-instruction callback the comparer rides on.
  **No `ptrace` anywhere in it** — stepping is `EFLAGS.TF` plus an `SA_ONSTACK`
  `SIGTRAP` handler, i.e. the process steps itself, and the interpreter is a
  fast path that simulates runs of instructions to avoid a trap each. §4.7.

### Fuzzing and offline tooling

* `fuzz/` — libFuzzer target for the fast path (`TryFastFrameInfo`), fed real
  `.eh_frame_hdr`+`.eh_frame` blobs from `fuzz/data/` plus byte mutations. The
  `FuzzXlate` accessor is the reason `Access` / `TryFastFrameInfo` are templated
  on an `Xlate` at all (production is `IdentityXlate` and compiles away); it
  emits `__sanitizer_cov_trace_cmp4` so mutations aim at bytes the decoder
  actually reads. `fuzz/README.md` has the input layout and how to run it.
  Built by `fuzz/build.sh` (clang, ASan+UBSan), not by Bazel.
* `perf-convert/` — offline `perf.data` converter: reads a `--call-graph dwarf`
  recording and rewrites the raw stack dumps as plain callchains. Uses
  `//:aw-fastpath` (the header-only fast-path decoder) and none of the
  in-process unwinder, and links abseil freely — it is neither signal-safe nor
  dependency-free, deliberately. `//:aw-fastpath` is visible only to this
  package, since header-only targets can't carry `CXXOPTS` into a consumer's
  compile. `perf-convert/compare.rb` diffs its output against `perf`'s own.

### Docs

* `doc/amd64-drap-problem.adoc` — why the DRAP handling exists. The only
  long-form doc that survived the release cleanup.
* `TODO` — live task list.
* `LICENSE` is 0BSD; every source file carries an SPDX line.

## 4. How it works

### 4.1 The main loop

`aw_backtrace_full` (callback) / `aw_backtrace` (array) → `PrepareCursor()`
(from the `ucontext` for a signal capture, or from `__builtin_frame_address(0)`
for a direct call; both `NEVER_INLINE`, which matters for correctness) →
`UnwindLoopFastPath()`, which falls through to `UnwindLoop()`.

`is_leaf` really means **"pc came from a register file, not from an unwind
step"** — hence the public callback parameter being named `pc_before_insn`. It
is true for frame 0 of a `ucontext` capture and for a frame reached by stepping
through a signal trampoline. Non-leaf frames look up `pc - 1` and consult the
cache; leaf frames go straight to a fresh lookup. Getting this flag onto the
right frame is fiddlier than it looks: `UnwindLoop` threads `next_uc` /
`this_frame_uc` precisely so the flag belongs to the frame being *reported*
rather than the one being unwound *from*.

**A zero pc means two different things, and `is_leaf` is what tells them
apart.** From an unwind step it is the end of the chain — the outermost frame's
return-address slot is zeroed — and the walk stops. From a register file it is a
live jump through a null function pointer: the pc really is 0, the `call` pushed
its return address before faulting on the fetch at 0, so `sp` points straight at
it and `Arch::GuessUnwindInfo` recovers the caller. Both loops therefore break
only on `pc == 0 && !is_leaf`, which is also what keeps `lookup_pc`'s `- 1` from
underflowing. Regression test in `amd64-leaf-test.cc`; libgcc and libunwind both
give up here, so there is no reference implementation to diff against.

When the lookup produces nothing, a **fallback chain** runs in this order: PLT
detection (leaf only) → signal-trampoline byte match → DRAP (x86-64, and only
if the failure was a CFI expression) → refuse outright if it was a CFI
expression → `Arch::GuessUnwindInfo`. Everything in it that reads code bytes is
bounds-checked against `AddrChecker::ExecutableBoundsFor()`, the DRAP `lea`
sniff included — that one takes the bounds as a parameter and falls back to
trusting the unwind info when it can't read safely.

All stack reads go through `StackAccess` (alignment + containment in a stack
VMA). Two properties, both driven by comparer findings: a read outside the
cached bounds re-discovers them rather than failing, so an unwind can cross onto
or off a `sigaltstack`; and the TLS bounds cache is a seqlock, because a signal
handler can nest on the same thread and update it for a different VMA mid-write.

### 4.2 The CFI lookup, and how failure is reported

**Decoder/visitor split.** `eh-frame-reader.h` reads bytes and knows nothing
about this unwinder; `backtrace-core.cc` is the visitor that gives decoded
instructions meaning. **Callback arguments are the whole contract** — the
visitor is handed no reader state, so a new callback must be passed what it
needs rather than handed the state back.

**Diagnostics are flags, not an interface.** `DoUnwindLookup` returns
`kOk`/`kFail`/`kFailExpression` and takes a `DiagFlags` by value; reporting is a
single variadic `ReportError` that formats into a stack buffer and optionally
`write(2)`s or traps. `kFailExpression` is broken out because it is the one
failure the fallback chain can plausibly explain away; its message costs a
second full decode, so it is `if constexpr`-gated out of production entirely.

**Two flavours of failure, not symmetric.** *The reader gives up* (truncation,
unsupported encoding, unknown opcode, out-of-range address) via
`Decoder::Fail()`, which `WithExit::Exit`s out, **discarding every intervening
frame with no destructors** — so nothing in `eh-frame-reader.h` may own anything
by RAII. *The visitor gives up* by returning false, which stops the decode
normally. Either way the caller only sees an outcome code.

Unsupported things report and fail the frame rather than guessing, with a few
deliberate exceptions the code calls out: expressions on non-critical registers,
`DW_CFA_undefined` on RA (a normal stop — crt startup has nothing to return to),
and unknown registers, so a future greg expansion won't break unwinding.

**The asymmetry above is a live trap when editing `backtrace-core.cc`.** The
reader exits non-locally; the visitor does not. So a visitor helper that detects
a problem can only *return* the failure, and a caller that forgets to propagate
it turns an explicit error into a silent wrong answer — which is exactly how an
invalid `DW_CFA_register` operand once became `%rax`. `NarrowOffset` and
`NarrowReg` are therefore `[[nodiscard]]`: forgetting is a compile error.

The reader also rejects a few things up front that would otherwise produce
plausible-looking nonsense rather than a failure: a `code_align` of 0 (every
`advance_loc` becomes a no-op, so the row search never advances and the frame
gets built from every row in the FDE), a `data_align` of 0 (every offset becomes
0, putting RA at CFA+0), and either of them beyond ±16. `AdvanceLoc` and
`OffsetWithDataAlign` check the multiplications for overflow.

`LocateEHFrame` returns null — failing the frame — when `_dl_find_object` can't
place the `.eh_frame_hdr` it just handed us. There is no "assume everything is
readable" fallback: that would be the one unbounded read in the library.

### 4.3 DRAP (x86-64 only)

gcc's stack-realigning prologues produce unwind info the three-register model
cannot follow, and in places it is outright missing — read
`doc/amd64-drap-problem.adoc` first. The handling is a **second decode of the
same FDE** with a visitor tracking a four-state machine that maps directly onto
a new `Cursor`. Two of the four states read `%r10`, for them unwind is leaf-only.

The test bites because of `drap_test_trampoline`, an RSP-framed naked caller: a
DRAP prologue looks like an ordinary frame, so the naive guess gets RIP and RBP
right and only **RSP** wrong — invisible unless the caller is RSP-framed. Both
shapes are covered (gcc 16's, with the `.cfi_restore 6`, and every gcc before
it). Verified, not assumed: making the DRAP call unreachable fails the test.

### 4.4 Caching

**One cache, non-leaf only** — leaf frames always take a fresh
lookup. We also only cache for addresses in modules present at
constructor time, because there is no invalidation. Those modules are
assumed to be permanently loaded (not unloadable). Cache
implementation is two layers: `FrameInfoCache` is policy (compression,
cacheability) and `UnwindInfoCache`, which is storage and eviction
and knows nothing about the unwinder.

**The whole entry is one 128-bit atomic** — 8 bytes key+flags, 8 bytes
compressed `FrameInfo` — and the rest follows: no seqlock, no lock bit, no retry
loop, because a reader can never catch a half-written value. The only cleanup is
a duplicate-insert undo. Eviction is second-chance: an insert marks the other
entries in its bucket, a hit clears its own mark, `Put` prefers free, then
marked, then random.

**Three constraints on edits here.** The entry must stay exactly 16 bytes with
no padding — `compare_exchange` compares bitwise, so an indeterminate padding
bit makes a CAS fail forever — hence anything added to the metadata comes out of
the tag; the `alignas(16)` is load-bearing; and every function touching the
table carries `ATTR_DWORD_ATOMICS` (a no-op today, kept for when it returns).

`sizeof(...) == 8` does *not* enforce the first one: dropping a field leaves the
byte behind as padding and the size assert still passes. That happened once.
`static_assert(std::has_unique_object_representations_v<...>)` on
`CompressedFrameInfo` is what actually catches it, which is why the struct
carries an explicit `unused` member rather than a hole.

### 4.5 Spawning the symbolizer helper

Diagnostics-only code, documented here because it is the one place where
*process*-level details carry the correctness argument: the comparer preloads
into arbitrary host processes, and a symbolizer that spawns a visible child
perturbs whatever process management the host is doing.

`WithSpawnedChild` builds a two-level sandwich: the caller blocks all signals
and clones an **intermediate** (`CLONE_VM | CLONE_VFORK`), which clones the
**helper**, runs the caller's body itself, reaps the helper and relays its
status. Every layer of that is load-bearing and the reasoning is commented in
the file — the short version is that `execve` unconditionally resets
`exit_signal` to `SIGCHLD`, so an intermediate that never execs is the only way
to keep a `SIGCHLD` from reaching the host or showing up in its `wait(-1)`; that
`CLONE_VFORK` suspends the caller, so the body has to run in the intermediate or
the pipe deadlocks; and that `CLONE_VM` is what lets the output land in the
caller's address space anyway. Don't simplify a layer away without reading why
it is there.

**Descriptors 0/1/2 are the recurring hazard here, twice over.** A target may
close its standard descriptors — glibc's `close_stdout` atexit handler does —
and then `memfd_create` and `pipe()` hand back numbers in that range, where the
helper's `dup2` onto stdout collides with them. Both sites are guarded now, with
`DumpStackTraceWithClosedStdio` as the regression test. Neither symptom was
loud: the memfd one crashed on a `CHECK`, the pipe one made every symbolized
dump come back silently **empty**. Assume a third descriptor acquired here has
the same problem.

### 4.6 The comparer's shadow stack

Before touching `BacktraceBuffer`:

* **Entries carry the SP their frame will return to**, so one `ret` can retire
  several at once — which is what a `longjmp` leaves behind, having restored
  `%rsp` with no `ret`s at all. The reverse case (shadow stack shorter than the
  capture) is accepted outright: the comparer can start mid-stack.
* **`Pop` can drain the stack entirely, and an empty shadow stack passes
  everything**, so one mis-modelled return turns the comparer off until calls
  refill it. Deliberate — it is what makes `longjmp` survivable — but it traded
  a noisy failure mode for a quiet one. `return_buffer_pop_skips` in
  `PrintStats` is the instrument; a big jump means go looking.
* **Suppression is also a cache, and it cuts both ways.** The table is checked
  against the top of the *shadow stack* as well as the capture, so a
  once-reported pc that later appears as a return address silently skips that
  comparison too; and it saturates at 256 entries (saying so once).
* **Windows where `SIGTRAP` is blocked are forgiven.** The stepper sees nothing
  while the target has it masked, so the comparer recognizes the *disable*
  syscall and resynchronizes from empty on the next callback — by definition
  already past the re-enable. That check must sit **first** in the
  sigtrap-disable/call/ret chain.

### 4.7 Why the stepper redirects syscalls through a trampoline

`EFLAGS.TF` stepping **cannot observe the instruction after a `syscall`**:
`SYSCALL` clears `TF` in hardware, the kernel leaves via `IRET`, and `IRET`
restoring `TF` re-arms stepping only for the instruction *after* it, so two
instructions retire between two callbacks. The kernel closes this hole only for
ptrace, which we don't use. Rather than model it in the comparer, the handler
points `rip` at a private `syscall; nop; …` trampoline and restores the real
resume address on the next trap; the load-bearing details (restore before the
`!active` early return, strict lower bound, `clone`/`clone3` excluded because a
new thread's fresh TLS would send it to `rip` 0) are commented at the site.

**The bigger hole this does not close: signal handlers are invisible**, because
the kernel clears `TF` on handler entry. A coverage gap rather than a
correctness one — a handler that returns normally is invisible but balanced.
Note the sigcontext handed to the target has `TF=1` in it, so a handler that
normalizes eflags silently kills stepping.

### 4.8 The fast path

`aw-backtrace-fastpath.h` is a single `ALWAYS_INLINE` function that reads
`.eh_frame_hdr`, binary-searches the FDE, and decodes just enough of the FDE+CIE
CFI to produce a `FastPathFrame` (SP/FP-relative CFA, optional `%rbp` spill
slot, "end of chain"). It hard-codes the encodings glibc/gcc/clang actually emit
(`eh_frame_hdr` enc `0x1b`/`0x03`/`0x3b`, CIE `code_align 1` / `data_align -8` /
aug `z...`, RA at CFA-8) and returns `FastPathFrame::Failure()` — not a wrong
answer — for anything outside that. It never reports a diagnostic and owns
nothing; the bounds math (`Access`, `kSmallBump`, `kSlop`) is what keeps a
malformed `.eh_frame` from walking off the section.

`Access` and `TryFastFrameInfo` are templated on an `Xlate` policy that maps a
decoder pointer to the bytes behind it. Production is `IdentityXlate` (a plain
dereference, compiles away); `fuzz/` swaps in one that redirects reads and
reports accessed offsets. Every raw read the decoder issues goes through
`Access::internal_ptr_as` for this. The struct-header reads (`hdr`, `cie_hdr`,
…) route their *first* touch through it and then the caller re-reads fields
off the returned pointer, so a translating accessor sees one call per struct,
not per field.

`UnwindLoopFastPath` (in `aw-backtrace.cc`) is the loop around it: cache lookup
(non-leaf only) → `TryFastFrameInfo` → `FastPathFrame::ToFrameInfo` → the same
cursor step as `UnwindLoop`. On *any* miss it tail-calls `UnwindLoop` with
`skip_first_callback=true` — so the fast path is a pure accelerator, never a
behaviour change. Neither loop counts frames; both return void and the caller
counts if it cares (`aw_backtrace` returns what it actually stored). Getting
that arithmetic wrong across the handoff was easier than getting it right.

**Every `goto fallback` sits upstream of the cursor commit.** The unwind step
reads into `next_fp`/`next_pc` and only writes `cursor` once every read has
succeeded, and `uc` is cleared after that — so when the fast path hands off,
`UnwindLoop` recomputes the same `is_leaf` and `lookup_pc` and continues from a
cursor that still describes the frame the fast path already reported. Committing
`cursor.fp` early used to leave the two halves describing different frames.

**It is on the diagnostics path too, not just production.** Both loops are
templated on the `NoDiag`/`RuntimeDiag` policy; `RuntimeDiag::use_fastpath()` /
`use_cache()` read `DiagOptions::disable_fastpath` / `disable_cache`. The
dispatch lives at the top of `UnwindLoopFastPath` itself, so it covers both
entry points and `DiagUnwindLoop` alike; for `NoDiag` on x86-64 it folds to a
compile-time `true` and the branch disappears, and on other arches the whole
fast-path body dead-codes out. Counters `fast_path_frames` /
`fast_path_cache_hits` / `fast_path_fallbacks` show in `PrintStats` (the
per-frame two only under `AW_BUMP_STATS_IN_PRODUCTION`, like the cache's own).

**The comparer cross-checks it every step.** `CaptureReferenceBacktrace` in
`backtrace-comparer.cc` re-captures with `disable_fastpath` + `disable_cache`
and `StepperCallback` requires an exact match against the normal capture before
it even consults the shadow stack — so a fast-path or cache bug is a test
failure with `--fast:` / `--ref:` dumps, independent of the shadow-stack
machinery.

## 5. Conventions and gotchas

* C++20, 2-space indent, 120 cols, Google-ish (`.clang-format`). Emacs mode
  lines on nearly every file, SPDX line under them — keep both on new ones.
  clang-format has not been run across the tree recently and several files have
  drifted, so format the block you touched, not the file.
* Everything internal is in `namespace aw_backtrace_internal`; the public API is
  `extern "C"` and `aw_`-prefixed.
* **The capture path stays allocation-free, lock-free and signal-safe.**
  Anything that allocates happens at init or in test-only code. Statics on that
  path are `constinit`/trivially-constructible so they land in `.bss` with no
  initializer-ordering hazard — there are `static_assert`s; don't add a
  constructor to those types.
* Nothing reachable from `eh-frame-reader.h`'s failure path may own anything by
  RAII (§4.2).
* Byte-pattern matching (PLT shapes, sigreturn trampolines, the DRAP `lea`) is
  compiler/linker-version fragile by nature.
* History was squashed at the first public release, so there is no pre-release
  archaeology to do. Infer intent from `TODO`, `README.md` and this file.
* **Debugging.** `aw-backtrace-test` and `lua-test` start the single-stepping
  comparer, which fights gdb. Use `aw-backtrace-test --nocompare`,
  `amd64-leaf-test` (never starts it), or `cjm0`. `AW_BT_BREAK_AT=0x<addr>` (or
  `0x<addr>:<skips>`) stops the stepper there, prints the pid and `SIGSTOP`s so
  you can attach; `kill -CONT` resumes.
* **Comparer mismatch knobs**, both read once at start: `AW_BT_DIAG=N` means
  "act on the (N+1)-th unsuppressed mismatch" (default: print and keep going),
  and `AW_BT_DIAG_VIA_CORE` picks *how* to act — `ud2` for a core dump instead
  of the soft breakpoint.
* **Comparer diagnostics never go through stdio** — they are `write(2)`s to a
  private dup of stderr taken at startup and moved to fd 900+, because the
  comparer prints from inside the SIGTRAP handler and because the target may
  have closed or redirected 0/1/2. `AW_BT_DIAG_FILE=<path>` redirects them. Use
  `DiagPrintf`/`DiagWrite`, never `printf`/`fprintf`, anywhere in that file.
* **Running the comparer against an arbitrary binary:**

  ```
  LD_PRELOAD=./backtrace-comparer.so some-program args...
  kill -USR1 <pid>   # cache stats + instructions stepped
  kill -USR2 <pid>   # dump the next captured backtrace
  ```

  Everything is single-stepped, so expect it to be *very* slow.
  `AW_BT_DONT_DROP_PRELOAD=1` keeps the preload for children (usually you want
  it off). The opening `allowing sigaltstack for 262144 …` line is the stepper
  installing its own stack, not a warning.

  **Expect zero mismatches** on a small dynamically linked program — `/bin/true
  hello` and friends print three `added cached suppression` lines and nothing
  else. When a run isn't clean: **check the exit code**, not just the mismatch
  count, and **check that a report has a body** — a `Mismatch at 1` with no
  `--bad:`/`--good:` lines means symbolization returned nothing, which is a bug
  in the spawn path (§4.5), not in the unwinder.

## 6. Known gaps

From `TODO`: a flags word instead of the callback's `pc_before_insn` bool
(including a signal-frame indicator); whether a signal frame should report
`pc_before_insn = true`; exposing whether a backtrace was
complete/reliable/guessed; finishing and making permanent the `-Wconversion`
cleanup; short-circuiting the walk into `_start`.

On `-Wconversion` specifically: `bazel build --copt=-Wconversion :aw-backtrace`
is down to three warnings — `unwind-info-cache.h`'s `uint64_t` → 62-bit `tag`
bitfield (benign; `tag` is already masked, gcc can't see it) and two
`-Wsign-conversion` in `aw-addrcheck.c`. Everything else in the core is clean.

True of the tree but not in `TODO`:

* **aarch64 builds and passes the basic tests, but is not release-ready** — no
  `GuessUnwindInfo`, no comparer (the sim-stepper is x86-only), no CI. The
  biggest coverage hole. It is also easy to break without noticing, since
  nothing in the default build compiles it; `aarch64-linux-gnu-g++ -c` on the
  core sources is a cheap smoke test.
* **The comparer's own machinery is the fastest-moving code in the tree and
  almost none of it is under test.** `BacktraceBuffer`, the truncation rule, the
  SIGTRAP resynchronization and the two libc interposers are validated only by
  "the tests still pass" and by eyeballing a preload run. Empirically not
  enough: three out-of-bounds reads plus both §4.5 descriptor bugs were
  unreachable from anything the tests assert on, and the descriptor ones each
  took a hand-run preload to find. `BacktraceBuffer` is the easy win — it needs
  nothing but `<span>`.
* **No dedicated test of the cache**, and no coverage of its *concurrency* at
  all. Single-threaded correctness is now checked at least: the comparer's
  §4.8 cross-check re-captures with `disable_cache` and diffs, so a stale or
  wrong entry fails a test.
* No CI, no sanitizer configs. The fuzzer (§3) is a crash-only oracle so far;
  the full-decoder wrong-answer cross-check and a hand-built CIE/FDE blob target
  (`eh-frame-reader-test.cc` is the unit-level beachhead) are still open, and it
  is not wired into any automated run.
* Open in-code on the DRAP path: only apply the tail workaround when the main
  DRAP shape was seen.
* Making `DW_CFA_restore` spec-accurate — it restores the CIE's initial rule,
  we restore the architectural default and refuse the frame when the CIE set up
  something else. Nothing in the current corpus exercises the difference.
* `//perf-convert`'s `SelfTest.SweepOwnFDEs` is a threshold over the *test
  binary's own* FDEs, so its bound moves with the toolchain — it has already had
  to be widened once. A fixed input would be better.

### Loose ends the first release shipped with

* **Symbol hygiene is nearly done, and the remaining bits are test-only.** The
  release set (`:aw-backtrace` and its deps) exports twelve unmangled symbols,
  all `aw_`-prefixed, which is what makes the planned `tcmalloc_` renaming for
  gperftools a small bounded job. What is *not* prefixed —
  `simple-fp-backtrace`'s `simple_backtrace`, `symbolize-backtrace.h`'s types in
  the global namespace, `_binary_sym_helper_bin_*` — belongs to targets that
  `:aw-backtrace` does not depend on. `aw-addrcheck.h` declares a test-only
  ioctl-disable setter, but that header is internal (only
  `include/aw-backtrace/aw-backtrace.h` is exported), so it is not a public API
  wart.
* **`linkopts = ["-latomic"]` on `:internal` propagates to every consumer.** Fine
  for a normal Bazel build, an integration wart for static links and non-glibc.
* Naming mixes dashes and underscores (`function_ref.h`, `static_storage.h`,
  target `sym_helper_obj`…); new files go the dashed way.
* No `make install`, no pkg-config, no CMake. Bazel or vendoring, for now.
