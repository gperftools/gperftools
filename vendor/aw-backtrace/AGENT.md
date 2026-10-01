# AGENT.md — orientation for coding agents

Where things live and the traps worth knowing before editing. `README.md` is
the user-facing introduction and design rationale. When a description here
looks stale, grep for the named symbol and trust the code.

## 1. What this project is

A from-scratch, **async-signal-safe backtrace library for profilers**. The
public symbols start with `aw_`. It unwinds from hand-parsed `.eh_frame` /
`.eh_frame_hdr` CFI and does not depend on libunwind or libgcc.

The contract:

* Never crash, never deadlock, never allocate on the sampling path. An
  occasional *wrong* backtrace is acceptable; crashing is not.
* Only PC, SP and FP are unwound. There is no full DWARF register-state
  unwinding, on the bet that real frames are SP-offset or FP-framed.
* CFI outside that model fails the frame (reported when diagnostics are
  enabled; otherwise the backtrace is silently truncated). It is never
  reinterpreted. Heuristics (PLT and trampoline byte matching,
  `GuessUnwindInfo`) run only when CFI is missing or is an expression
  (§4.1), and they run silently.
* Targets modern Linux with glibc 2.35+ (`_dl_find_object`). x86-64
  and aarch64 are supported; x86-64 has much more test coverage. riscv
  and 32-bit may come later.

## 2. Build and test

**There are two build systems, and they build different things.**

### Bazel — the library and the test suite

bzlmod, module `aw-backtrace`, two packages: `//` and `//perf-convert`.

```
bazel test ...:all
bazel test -c opt ...:all        # NDEBUG: drops assert(), keeps CHECK()
./test-all-cfg.rb                # gcc/clang x opt/dbg, plain and stepped; what CI runs
```

For each of the four configurations, `test-all-cfg.rb` first runs a plain
`bazel test ...:all`. It then reruns the `stepped`-tagged targets
(`aw-backtrace-test`, `lua-test`) with
`--run_under="qemu-$(uname -m) -plugin v/pstepper/pstepper_plugin.so"`,
`AW_BT_REQUIRE_STEPPER=1` and `--test_output=all`. It needs:

* `qemu-<arch>` on `PATH`. Build it with `ci/build-qemu.rb`, which installs to
  `~/qemu/bin`.
* The plugin, built with `cd v/pstepper && ./genbuild.rb check`.

Extra arguments are passed through to every `bazel` invocation. On arm64 it
adds `-cpu cortex-a76` to qemu and sets `AW_BT_CLANG_KNOWN_BUGS=1` for clang
(§5). It leaves out `//perf-convert` when the compiler lacks a working
`std::source_location`, which is clang 14 on ubuntu-22.04 (abseil needs it).

Traps:

* **`-std=c++20 -fno-exceptions -fno-rtti` are in `BUILD.bazel`'s `CXXOPTS`, per
  target.** A consumer of the module never reads our `.bazelrc`, so the flags
  must travel with the targets. They are in `cxxopts` rather than `copts` to
  keep them off the C compiles. The public header compiles as C99 and as
  C++11 and later, so keep it that way.
* **Every internal header is in one target, `:headers`** (`glob(["*.h"])`). A
  target that compiles C++ deps on `:headers`; there are no per-header
  libraries. Two headers are outside the glob: `aw-addrcheck.h` (in
  `:aw-addrcheck`) and the public header (`:aw-backtrace`, which exposes it as
  `aw-backtrace/aw-backtrace.h` via `strip_include_prefix`). `:aw-backtrace` is
  the only public target in `//`.
* **`.bazelrc` applies to this workspace only.** It forces `-std=c++20` across
  the whole graph so that abseil matches what `//perf-convert` compiles
  against.
* **`.bazelversion` pins 9.2.0.** If you add a `local_path_override` or
  `git_override`, put `dev_dependency` on the `bazel_dep` line only. 9.2.0
  rejects it on the override.
* **GNU ld intermittently segfaults when linking this package.** The
  workaround is `features = ["-supports_start_end_lib"]`. Copy it onto any new
  binary target that crashes `ld`.
* `aw-backtrace-prefixed-symbols-test` rebuilds the library with
  `AW_RENAME_PREFIX=tcmalloc_` and fails if an external symbol escapes
  `renamings.h`. When you add an external-linkage symbol, add it there too.

### genbuild.rb / ninja — the LD_PRELOAD comparer

**Edit `genbuild.rb`, never `build.ninja`.** The part you edit is `build!` at
the top of the file.

```
./genbuild.rb ninja                          # regenerate if stale, then build
./genbuild.rb ninja CC=clang CXX=clang++     # ENV overrides; re-execs itself
```

It builds four artifacts that Bazel does not: `backtrace-comparer.so`,
`sym_helper_bin`, `cjm` and `cjm0`. The default flags are `-ggdb3 -O2
-DNDEBUG`. No cross build support yet.

### `ci/` and the workflow

`.github/workflows/ci.yml` runs one job, `tests`, on ubuntu-22.04 and
ubuntu-24.04 (x86-64 only). Its steps are `ci/install-deps.rb`,
`ci/build-qemu.rb` (cached), `ci/check-pstepper.rb`, `ci/install-bazelisk.rb`
and then `test-all-cfg.rb`. Every step is a ruby script that also runs on a
developer box.

* `ci/build-qemu.rb` builds a pinned upstream qemu, linux-user only, with
  every patch in `ci/qemu-patches/` applied. **The patch is required.**
  Without it, upstream i386 TCG leaves EIP and the lazy EFLAGS stale at the
  arbitrary instruction boundaries where the plugin reads or redirects state,
  and `v/pstepper`'s tests segfault. The cache key hashes both the script and
  the patches.
* `ci/check-pstepper.rb` runs `v/pstepper`'s own suite under that qemu. It is
  fast, and when it fails, every stepped test would fail less legibly.
* `ci/install-deps.rb` includes **`libc6-dbg`, which the stepped tests
  need** (§4.6, suppressions).

### Test configurations

**Only the comparer's capture traps on unwinder diagnostics**, and only in
non-`NDEBUG`, non-`BUILD_SO` builds (`CaptureBacktrace` passes
`trap_diagnostics`). The comparer captures only while stepping, so a native
`bazel test -c dbg` never traps. The preloadable `.so` never traps, because it
walks startup and libc code.

**A comparer mismatch fails a test only under qemu.** A plain `bazel test`
reports `pstepper_enable FAILED: Function not implemented`, passes, and
compares nothing. `AW_BT_REQUIRE_STEPPER=1` exists to catch that silent pass
(§5).

The production (`NoDiag`) path bumps the cache and per-frame fast-path
counters only when `AW_BUMP_STATS_IN_PRODUCTION` is defined. `genbuild.rb`
defines it; Bazel does not. The `RuntimeDiag` path always bumps them, so
`aw-backtrace-test`, which routes every capture through
`OverrideGlobalBacktracer`, still gets numbers.

## 3. Code map

### Core library

| file | what |
| --- | --- |
| `include/aw-backtrace/aw-backtrace.h` | the public API: `aw_backtrace_full`, `aw_backtrace`, plus `aw_backtrace_ext::DebugExtensionV0`. The extension is a test/inspection surface, `TryGet()` may return null, and it is not a stable ABI |
| `aw-backtrace.cc` | entry points, `UnwindLoop`, `UnwindLoopFastPath`, `StackAccess`, the `NoDiag`/`RuntimeDiag` policies, `FrameInfoCache`, `LoadedSetHolder` |
| `aw-backtrace-fastpath.h` | `TryFastFrameInfo` / `FastPathFrame` (§4.7) |
| `backtrace-core.{h,cc}` | the *policy* half of CFI lookup: a visitor that turns decoded CFI into a `FrameInfo`. Also `DoUnwindLookupFromInputs`, which serves a module without a live process |
| `eh-frame-reader.h` | the *decoder* half: `.eh_frame_hdr` search, CIE/FDE parsing and the CFI opcode loop. It is visitor-templated |
| `backtrace-drap.h` | x86-64 only: a second visitor for gcc DRAP prologues (§4.3) |
| `aw-structs.h` | the unwind model: `CfaRule`, `RegisterRule`, `FrameInfo`, `Cursor`, `CompressedFrameInfo` |
| `aw-arch.h`, `aw-arch-{x86_64,aarch64}.h` | per-arch `struct Arch` with a fixed static interface: register numbers, PLT and signal-frame byte matching, `GuessUnwindInfo` |
| `unwind-info-cache.h` | `UnwindInfoCache` (§4.4) |
| `aw-addrcheck.{h,c}` | async-signal-safe `/proc/self/maps` queries, using `PROCMAP_QUERY` or a snapshot plus bsearch |
| `with-exit.h` | `WithExit::Run`/`Exit`, a `_setjmp`/`_longjmp` wrapper. **Nothing runs on the way out, not even destructors** |
| `check.h` | `CHECK()`, which is not gated on `NDEBUG`. Use it for self-checks. On the capture path it only guards can't-happen invariants (`Decoder::DoExit`) |
| `renamings.h` | the `AW_RENAME_PREFIX` symbol renaming used by embedders such as gperftools |
| `dwarf-constants.h`, `utils.h`, `simple-counter.h`, `static_storage.h`, `function_ref.h` | small utilities |

### Comparer

* **`backtrace-comparer.cc` is the best test asset in the repo.** It
  single-steps the whole process via `v/pstepper` and keeps a shadow call stack
  from calls and returns. At every instruction it compares a capture against
  that stack, so it needs no expected values. Known-bogus spots are suppressed
  by symbolizing the top frames (§4.6). The file is arch-neutral; all
  machine-dependent code sits behind `ComparerArch`.
* `comparer-arch.h`, `comparer-arch-{x86_64,aarch64}.h`, `comparer-types.h`
  hold the per-arch `struct ComparerArch` (§4.6).
* `nop-backtrace-comparer.c` stands in for the comparer on other arches.
* `backtrace-comparer.so` (`-DBUILD_SO`, ninja only) is the comparer as an
  LD_PRELOAD object. It interposes `sigaction`, `unsetenv`s `LD_PRELOAD`, and
  exports almost nothing (`backtrace-comparer.so.map`). Usage is in §5.
* `v/pstepper/` is a co-developed sibling that provides a per-instruction
  upcall facility. It is implemented by a QEMU TCG plugin, and the comparer uses only
  `pstepper.h`. `pstepper_enable` returns `ENOSYS` when no plugin is loaded.
  "Acceleration" runs the handler as *host* code when the guest and host
  arches match. The design is in its `README.md`.

### Tests

* `aw-backtrace-test.cc` is the main end-to-end test and hosts the comparer. It
  captures from a SIGILL handler and checks that walking *through* the signal
  frame without a `ucontext` lands in the same place. That is the direct test
  of `Arch::IsSignalFrame`. It also hosts the DRAP fixtures. A second build,
  `simple-backtrace-test` (`BT_USE_SIMPLE`), runs the same source against
  `simple-fp-backtrace`.
* `lua-test.cc` runs the comparer over the Lua compiler to get varied real
  codegen.
* `amd64-leaf-test.cc` holds hand-written asm with deliberate CFI shapes and
  compares it against glibc `backtrace()`. It is the minimal repro vehicle
  for leaf and epilogue issues, and it includes the jump-through-null case
  (§4.1).
* `arm64-leaf-test.cc` is the aarch64 companion and the only test of §4.8. Its
  `cfiless_*` fixtures have no `.cfi_startproc`, so every step out of them is
  a guess. The PLT0 fixtures are also CFI-less, but `DetectPLTEntry` handles
  them. glibc's `backtrace()` gives up on these frames, so the fixtures export
  their own expected addresses. The `paciasp` fixture has no effect without
  FEAT_PAuth.
* `amd64-drap-test.{c,s}` is gcc 16 `-mforce-drap` output (`minimal_drap`). The
  pre-gcc-16 shape is `minimal_drap_2` in `aw-backtrace-test.cc`.
* `fastpath-sweep-test.cc` is the fast path's oracle (§4.9).
* `eh-frame-reader-test.cc` is the only place where the decoder runs without a
  process image. `Optionalize()` turns a `Fail()` into `std::nullopt`.
* `symbolize-backtrace-test.cc` installs a `SIGCHLD` handler that `abort()`s
  (§4.5). It also covers dumping with 0/1/2 closed and the dead-daemon
  fallback.
* `aw-backtrace-skip-test.cc` tests `aw_backtrace`'s `skip` argument.
  `with-exit-test.cc` and `aw-addrcheck-test.c` are unit tests. The addrcheck
  test `#undef`s `NDEBUG` because it asserts for effect.
* These assert nothing: `comparer-longjmp.cc` (`cjm`/`cjm0`, written to make
  the comparer fail), `recursion-test.cc` (the benchmark behind README, built
  as aw/libgcc/fp variants) and `bench-addrcheck.c`.

### Symbolizer (diagnostics and tests only)

`symbolize-backtrace.{h,cc}`, `sym-helper.cc` and `sym-helper-protocol.h` make
up the symbolizer. `sym-helper` is embedded as a blob and runs as a daemon per
process image (§4.5).

### Elsewhere

* `fuzz/` holds two libFuzzer targets for `TryFastFrameInfo`
  (`fastpath-fuzz`, `fastpath-fuzz-naive`), built by `fuzz/build.sh` (clang,
  ASan+UBSan) rather than Bazel. Its `FuzzXlate` is
  why `Access` and `TryFastFrameInfo` are templated on an `Xlate`. See
  `fuzz/README.md`.
* `perf-convert/` rewrites `perf record --call-graph dwarf` stack dumps as
  plain callchains. It uses `//:aw-fastpath` (header-only, visible only to
  this package) and links abseil freely, because it is neither signal-safe
  nor dependency-free. `compare.rb` diffs its output against `perf`'s own.
  `frame-info-dump` also links `//:aw-backtrace`. It prints what both decoders
  derive at a module vaddr without running the process; use it for offline
  CFI debugging on either arch.
* `doc/amd64-drap-problem.adoc` explains DRAP. `TODO` is the live task
  list.  The license is 0BSD. C/C++ sources carry an SPDX line;
  scripts and build files don't.

## 4. How it works

### 4.1 The main loop

`aw_backtrace_full` (callback) and `aw_backtrace` (array) build a `Cursor`
with `PrepareCursor()`, starting from the `ucontext` or from their own
`__builtin_frame_address(0)`. They, and `DebugExtensionV0::BacktraceExt`, are
`NEVER_INLINE` because that frame address must name their own frame. They then
call `UnwindLoopFastPath()`, which falls through to `UnwindLoop()`. When
`OverrideGlobalBacktracer` is set, they go through `DiagUnwindLoop`
(`RuntimeDiag`) instead.

**`is_leaf` means "the pc came from a register file, not from an unwind
step"**; that is why the public parameter is named `pc_before_insn`. It is
true for frame 0 of a `ucontext` capture and for a frame reached through a
signal trampoline. Non-leaf frames look up `pc - 1` and use the cache. Leaf
frames always do a fresh lookup. `UnwindLoop` threads `next_uc` /
`this_frame_uc` so that the flag goes with the frame being *reported*.

**A zero pc means different things depending on `is_leaf`.** After an unwind
step it marks the end of the chain. From a register file it is a jump through
a null pointer, and the caller can still be recovered: `call` pushed the
return address, or `blr` wrote x30. Both loops therefore stop only on
`pc == 0 && !is_leaf`, which also keeps `pc - 1` from underflowing. libgcc and
libunwind both give up here, so the leaf tests are the only reference.

**`kUndefinedRA`** (`DW_CFA_undefined` on the RA column, covering the pc)
stops the walk. It ends every backtrace at `_start`, and it saves the
`ExecutableBoundsFor` call that the fallback chain would otherwise make.

When a lookup produces nothing, the **fallback chain** runs in this order:

1. PLT detection, leaf frames only. x86-64 matches these shapes:
   * classic lazy `.plt` and PLT0;
   * IBT `.plt` / `.plt.sec`;
   * the MPX `bnd` variants;
   * GNU ld's 8-byte `.plt.sec` / `.plt.got` entries;
   * lld `-z retpolineplt`;
   * glibc's runtime `plt_rewrite`.

   aarch64 matches ordinary 16-byte entries and PLT0 (optionally `bti c`),
   which moves sp by 16.
2. Signal-trampoline byte match, at any instruction of the trampoline.
3. DRAP: x86-64 only, and only if the failure was a CFI expression.
4. Refusal, if the failure was a CFI expression.
5. `Arch::GuessUnwindInfo`.

Everything in the chain that reads code is bounds-checked against
`LazyAddrChecker::ExecutableBoundsFor()`. Byte matching of this kind breaks
easily across compiler and linker versions.

The unwind step's stack reads go through `StackAccess`, which checks
alignment and that the address lies in a readable VMA. It starts from the
sp's VMA. A read outside the cached bounds rediscovers them from whatever VMA
holds the address, so unwinding can cross a `sigaltstack`. The TLS bounds
cache is a seqlock because a nested signal can update it mid-write. The
guesses (`GuessUnwindInfo`) read the stack directly, after doing their own
read+write VMA lookup.

### 4.2 CFI lookup and failure reporting

**The decoder and the visitor are separate.** `eh-frame-reader.h` knows nothing
about this unwinder, and `backtrace-core.cc` gives decoded instructions their
meaning. **Callback arguments are the whole contract**: a new callback must
receive what it needs as arguments.

`DoUnwindLookup` returns `kOk`, `kFail`, `kFailExpression` or `kUndefinedRA`,
and takes `DiagFlags` by value. Reporting goes through one variadic
`ReportError`. The `kFailExpression` message costs a second decode, so it is
compiled out of production with `if constexpr`.

**The two kinds of failure behave differently, and that is a live trap.**

* The reader gives up through `Decoder::Fail()`, which `WithExit::Exit`s
  **without running destructors**. Nothing in `eh-frame-reader.h` may own
  anything by RAII.
* The visitor gives up by returning false. A helper that detects a problem can
  only *return* it, and a caller that forgets to propagate the failure turns
  an error into a wrong answer. That is why `NarrowOffset` and `NarrowReg` are
  `[[nodiscard]]`.

Unsupported constructs fail the frame, with these deliberate exceptions:

* expressions on non-critical registers;
* unknown registers;
* `DW_CFA_undefined` on RA, which produces `kUndefinedRA`;
* on aarch64, `DW_CFA_same_value` on x30, which means "the RA is live in x30".

**`DW_CFA_undefined` on RA is decided by the row covering the pc, not by the
instruction.** A later `DW_CFA_offset` / `DW_CFA_restore` can define RA again,
and bailing out early would silently truncate the walk.

The reader rejects inputs that would otherwise produce plausible nonsense:

* `code_align` or `data_align` equal to 0 or outside ±16;
* multiplications in `AdvanceLoc` / `OffsetWithDataAlign` that overflow.

`LocateEHFrame` fails the frame when `_dl_find_object` cannot place the
`.eh_frame_hdr`. The library has no "assume readable" fallback.

`DW_CFA_restore` restores the architectural default rather than the CIE's
initial rule, and refuses the frame when the CIE set up something else.

### 4.3 DRAP (x86-64 only)

gcc's stack-realigning prologues produce CFI that the three-register model
cannot follow, and in places the CFI is missing. Read
`doc/amd64-drap-problem.adoc` first. The handling is a **second decode of the
same FDE** with a visitor that tracks a four-state machine, and each state
maps onto a `Cursor`. Two of the states read `%r10`, so unwinding in them is
leaf-only.

The test depends on `drap_test_trampoline`, an RSP-framed naked caller. A
naive guess gets RIP and RBP right and only **RSP** wrong, so the bug is
visible only under an RSP-framed caller. The test covers both gcc 16's shape
(with `.cfi_restore 6`) and the older one.

### 4.4 Caching

**There is one cache, and it holds non-leaf frames only.** It caches only
addresses in modules that were present at constructor time, because the cache
has no invalidation and those modules are assumed never to be unloaded. There
are two layers:

* `FrameInfoCache` is the policy: compression and cacheability.
* `UnwindInfoCache` is the storage and eviction, and knows nothing about the
  unwinder.

**`LoadedSetHolder`** records the executable `PT_LOAD` segments of every module
at constructor time, one sorted entry per segment. The same table answers
`ExecutableBoundsFor` without touching `/proc/self/maps`. A single segment is
contiguous, with no `PROT_NONE` holes, and its extent is the segment's own
rather than page-rounded. Addresses outside the table go to `aw-addrcheck`:
JIT code, later `dlopen`s and anonymous executable mappings.

**Each bucket has a seqlock, and no atomic is wider than 64 bits.** An entry
is two `atomic<uint64_t>`s: key+flags, and the compressed `FrameInfo`
`bit_cast` to a word. The `uint32_t` seqs are a separate table, so each bucket
is exactly 128 bytes and 128-aligned. Wraparound is harmless.

**Nobody ever waits on the lock**, because a signal can interrupt a writer on
the same thread:

* `Put` try-locks and drops the insert if it fails.
* A `Lookup` that finds the bucket locked counts as a miss. It retries only if
  a writer came and went during the read.
* The one unlocked write is a hit clearing `pending_eviction`, which readers
  ignore.
* Eviction is second-chance. `PrintStats` shows `dropped` and `retries`.

`CompressedFrameInfo` has an explicit `unused` member instead of padding,
enforced by `has_unique_object_representations`. `bit_cast` of a padding
byte would be indeterminate.

### 4.5 The symbolizer daemon

This code is diagnostics-only, but the comparer preloads into arbitrary
processes, so process-level side effects matter.

**Each process image has one persistent `sym-helper`.** It is spawned lazily
and reached through an `AF_UNIX` `SOCK_SEQPACKET` mailbox. Each request is one
datagram carrying two descriptors via `SCM_RIGHTS`:

* A memfd holding the addresses and the caller's `/proc/self/maps`. The daemon
  rewrites it in place with a binary response that the caller `mmap`s. The
  maps are the caller's so that qemu-user's synthesized guest view is what
  gets resolved.
* A fresh pipe write end whose close signals "committed". A daemon that dies
  also closes it, which is why the caller checks `kSymResponseMagic`.

Concurrent callers, including forked children sharing the mailbox, need no
locking. Only the spawn is serialized, by a CAS spinlock with all signals
blocked. The spawn is a `vfork` + `execve` of the blob from a memfd:

* It passes on **only `PATH`**, so `LD_PRELOAD` does not follow.
* The mailbox is `CLOEXEC`, so an exec'd child spawns its own daemon.

Healthy operation produces **no `SIGCHLD` for the host**. The `addr2line`s are
the daemon's children, and the daemon exits only on mailbox EOF. The daemon
also closes every descriptor it inherited, so it cannot hold a host pipe open.

**Descriptors 0/1/2 are the recurring hazard.** Targets close them (gnulib's
`close_stdout`, used by coreutils, does), so caller-side descriptors can land there. The daemon
first moves its mailbox to fd 16 or higher, then reopens 0/1/2. A dump with
**no body** means this area broke. Treat any new descriptor handed across the
same way.

**A failed request still produces frames.** A `sym-helper request failed`
line goes to stderr. Then `ReportUnsymbolized` hands each pc to the callback
with its module and vaddr, but no function or line. The daemon is never
respawned.

### 4.6 The comparer's shadow stack

**`ComparerArch` holds everything machine-dependent**, and it has two jobs:

* classify the instruction at the trapped pc as a call or a return, and say
  what a retired call put where;
* recognize a signal delivery from the kernel-built frame.

**It deliberately shares no code with the unwinder's `struct Arch`.** Otherwise
a wrong `Arch::IsSignalFrame` would agree with itself and the tests would pass.

**The model is the same on both arches, for different reasons.**

* On x86-64 an entry retires at `sp + 8`.
* On aarch64 `bl` touches no stack, so an entry retires at exactly the sp the
  call was made with.
* x86-64's "`*sp` is the return address" must not be ported. At a callee's
  entry `*sp` belongs to the caller, and often holds the caller's spilled x30.

`BacktraceBuffer`:

* **Entries carry the SP their frame will return to**, so one `ret` can retire
  several of them. That is how `longjmp` is survived. A shadow stack shorter
  than the capture is accepted, because the comparer can start mid-stack.
* **An empty shadow stack passes everything except frame 0**, which must
  equal the current pc. One mis-modelled return can silently disable
  comparison until calls refill the stack. Watch the "pop resync skips" line
  in the comparer's exit report (`return_buffer_pop_skips`).
* **Suppressions are a cache**, checked against the top two frames of both the
  capture and the shadow stack. A suppressed pc that later shows up as a
  return address also skips comparison. The cache saturates at 256 entries.
  An active suppression also disables the fast-path cross-check (§4.7) until
  its frame is popped.
* **Suppressions match symbol names and need glibc debug info**
  (`libc6-dbg`). The names are `_dl_fixup`, `_dl_runtime_resolve`,
  `call_init`, `__run_exit_handlers` and `__libc_arm_za_disable`. Without
  debug info, addr2line returns the nearest exported symbol, every suppression
  misses, and the frames read `(:0)`.
* **Signal delivery is modelled explicitly.** `TryRecognizeSignalEntry` spots
  the `rt_sigframe` at the new sp and pushes two entries: the interrupted pc
  and the trampoline return. `rt_sigreturn` is then just a return. The
  interrupted entry re-reads its pc from the frame, because handlers may edit
  the context. A call or return pending from the previous step is resolved
  against the *interrupted* register file (`sig.interrupted_uc`). It is
  skipped when the signal is that instruction's own fault, because then the
  instruction never retired.

### 4.7 The fast path

`aw-backtrace-fastpath.h` has TryFastFrameInfo which is one
`NEVER_INLINE` function. It reads `.eh_frame_hdr`, binary-searches the
FDE, and decodes just enough CFI to fill a `FastPathFrame`. It
hard-codes the encodings that sane toolchains emit:

* `eh_frame_hdr` encodings `0x1b`, `0x03` and `0x3b`;
* augmentation `z...`;
* one CIE alignment pair per arch, compared with `memcmp`: `1/-8` on x86-64
  and `4/-8` on aarch64.

For anything else it returns `Failure()`, never a wrong answer. It never
reports diagnostics and owns nothing. `Access`, `kSmallBump` and `kSlop` keep
a malformed section from walking off the end. Every raw read goes through
`Access::internal_ptr_as`. For struct headers, only the first touch goes
through it, so a translating `Xlate` sees one call per struct.

`UnwindLoopFastPath` checks the cache (non-leaf only), then calls
`TryFastFrameInfo` and `ToFrameInfo`, then takes the same cursor step as
`UnwindLoop`. On *any* miss it tail-calls `UnwindLoop` with
`skip_first_callback=true`, so the fast path must never change behaviour.
**Every `goto fallback` sits before the cursor commit.** `UnwindLoop` must see
the cursor still describing the frame that was already reported.

Per-arch details:

* **A zero `ra_offset` means "architectural default"**, as supplied by
  `Arch::ResetFrameInfo`: CFA-8 on x86-64, live in x30 on aarch64.
* **aarch64 clang emits `code_align 1 / data_align -4`**, so clang-built objects
  miss the fast path and take the slow one. The trade-off is accepted on
  purpose; do not add a second alignment pair to the hot loop.
* `DW_CFA_AARCH64_negate_ra_state` is a no-op here, as in the reader. gcc emits
  it in nearly every FDE under `-mbranch-protection`.
* **The leaf frame must support RA-in-register**, read from the `ucontext`.
  Without that, the fast path would bail on frame 0 of every signal capture.
  The recovered pc goes through `Arch::CleanReturnAddress`, which strips PAC on
  aarch64.

Both loops are templated on `NoDiag`/`RuntimeDiag`.
`RuntimeDiag::use_fastpath()` and `use_cache()` read
`DiagOptions::disable_fastpath` / `disable_cache`. `PrintStats` shows
`fast_path_*` counters.

**The comparer cross-checks the fast path at every step.**
`CaptureReferenceBacktrace` re-captures with the fast path and cache disabled
and requires an exact match (`--fast:` / `--ref:` dumps). This is on by default
and doubles the cost of each step; `AW_BT_CROSS_CHECK=0` turns it off.

### 4.8 The aarch64 guess

`Arch::GuessUnwindInfo` is per-arch because **`bl` puts the RA in x30, not on
the stack**. x86-64's "`*sp` looks like a pc" guess would report the
grandparent on aarch64. Two shapes remain:

* **x30 is live.** This covers leaf functions, prologue and epilogue windows,
  PLT stubs and `blr` through null. CFA == sp, which is exactly
  `ResetFrameInfo`. It needs a register file, so it applies to leaf frames
  only.
* **The AAPCS64 frame record at fp**, `{caller's x29, RA}`.

x86-64's mid-prologue guess has no aarch64 equivalent: a leaf frame has x30,
and a non-leaf pc can't land inside a prologue.

**x30 is tried first. The reason is damage, not confidence.** A stale x30
looks exactly as valid as a live one. Preferring x30 wrongly costs one
spurious frame, because the next step is non-leaf and reaches the real caller
through fp. Preferring the record wrongly deletes a caller.
`arm64-leaf-test.cc` covers both cases.

**The frame-record walk takes the CFA from `*(fp)`**, the caller's x29, because
`fp + 16` is only a lower bound. gcc keeps `x29 == sp` through the body
whenever the CFA is sp-based. The rule is `cfa = DerefFpRel(0)`,
`fp = MemFpRel(0)`, `ra = MemFpRel(8)`. `RegisterRule::MemFpRel` for RA exists
only for this rule, and `CompressedFrameInfo` cannot represent it, which is
fine because guesses are never cached.

**The guess never reads code**, so it cannot fault on execute-only text. A
candidate pc must be nonzero, 4-aligned and in an executable VMA. There is one
known wrong case, documented in `arm64-leaf-test.cc`: a CFI-less leaf that
carves stack space keeps x30 right but gets the CFA wrong.

### 4.9 The differential sweep

The fast path's contract is to *agree with `DoUnwindLookup` or fail*.
`fastpath-sweep-test.cc` checks this on both arches without stepping. For every
FDE of every loaded module, it probes four pcs through both decoders and
requires an exact `FrameInfo` match wherever the fast path answers. The
coverage percentage is only a loose floor, because the corpus depends on the
toolchain. On aarch64 clang only that floor is skipped (§4.7); mismatches
must still be zero.

One agreement is not exact: an RA `DW_CFA_undefined` is `EndOfChain` on the
fast path and `kUndefinedRA` on the slow one. The sweep counts these probes
separately and requires *both* sides to agree.

## 5. Conventions and gotchas

* The code is C++20 with 2-space indent and 120 columns, Google-ish
  (`.clang-format`). Put an Emacs mode line and an SPDX line on new files.
  Files have drifted from the format, so run clang-format on the block you
  touched, not on the whole file.
* Internal code lives in `namespace aw_backtrace_internal`. The public API is
  `extern "C"` and prefixed `aw_`. New file names use dashes.
* **The capture path stays allocation-free, lock-free and signal-safe.**
  Statics on it are `constinit` or trivially constructible, enforced by
  `static_assert`s.
* **The `sym_helper` blob carries an empty `.note.GNU-stack`**, added by
  `objcopy` in both `BUILD.bazel` and `genbuild.rb`. Without it, binutils older
  than 2.43 gives everything that links the blob an executable stack.
* Infer intent from `TODO`, `README.md` and this file. There is no
  pre-release git history.
* **Debugging.** Natively nothing steps, so every test works under gdb.
  `aw-backtrace-test --nocompare` skips the comparer entirely. Under qemu with
  acceleration, the handler runs as host code, so attach to qemu. With the
  preloaded `.so` (`BUILD_SO` only), `diag_all_text_sections` holds the
  `add-symbol-file` arguments.
  `AW_BT_BREAK_AT=0x<addr>[:<skips>]` runs a lone `nop` in `StepperCallback`
  at that pc so you can set a breakpoint on it. It also turns acceleration
  off.
* **Comparer knobs**, mostly read in `StartBacktraceComparer`:
  * `AW_BT_REQUIRE_STEPPER` (any value, even `0`) fails the run in three
    cases: no stepper, zero instructions stepped (exit 91), or any unsuppressed
    diagnostic at exit (exit 92). It ignores `AW_BT_DIAG`. Among in-tree
    programs, only `cjm` under a stepper trips it.
  * `AW_BT_CLANG_KNOWN_BUGS` (any value, read at exit) downgrades the exit-92
    failure to a message. `test-all-cfg.rb` sets it for clang on arm64.
  * `AW_BT_DIAG=N` calls `raise(SIGTRAP)` on the (N+1)-th unsuppressed
    diagnostic. `aw-backtrace-test` defaults it to 0 unless run with
    `--nocompare`.
  * `AW_BT_CROSS_CHECK=0` turns off the per-step cross-check (§4.7).
  * `AW_BT_ACCEL=0`/`1` forces acceleration off or on. `AW_BT_DIAG` and
    `AW_BT_BREAK_AT` turn it off by default, because a `raise` from host code
    lands in qemu.
  * `AW_BT_DIAG_FILE=<path>` redirects diagnostics (read in `EnsureDiagFD`).
  * `AW_BT_DONT_DROP_PRELOAD` (any value) keeps `LD_PRELOAD` for children.
    Only the `.so`'s constructor reads it.
* **Comparer diagnostics never use stdio.** They are `write(2)`s to a private
  dup of stderr at fd 900 or higher. Use `DiagPrintf`/`DiagWrite` in that file.
* **Running the comparer against an arbitrary binary:**

  ```
  qemu-x86_64 -plugin v/pstepper/pstepper_plugin.so \
      -E LD_PRELOAD=./backtrace-comparer.so some-program args...
  kill -USR1 <pid>   # cache stats + instructions stepped
  kill -USR2 <pid>   # dump the next captured backtrace
  ```

  **`-E` is required.** A bare `LD_PRELOAD=… qemu-x86_64 …` preloads into qemu
  itself: it prints a banner, then `pstepper_enable FAILED`. Expect the run to
  be very slow. A small program like `/bin/true` should print only a few
  `added cached suppression` lines. When a run isn't clean:
  * check the exit code;
  * check that each report has `--bad:`/`--good:` lines, because an empty
    report means a symbolizer bug (§4.5).

## 6. Known gaps

`TODO` lists these items:

* a flags word instead of `pc_before_insn`;
* signal-frame `pc_before_insn` semantics;
* reporting whether a backtrace was complete or guessed;
* making `-Wconversion` permanent. `bazel build --copt=-Wconversion
  :aw-backtrace` is down to two `-Wsign-conversion` warnings, both in
  `aw-addrcheck.c`.

Also open:

* **No guess-vs-CFI differ.** This is the cheap oracle for `GuessUnwindInfo`
  on either arch: wherever `DoUnwindLookup` succeeds, also run the guess and
  diff the step.
* **The comparer's own machinery is largely untested.** This covers
  `BacktraceBuffer`, the resync rule and the `sigaction` interposer. Past
  out-of-bounds reads and descriptor bugs were found only by hand-run
  preloads. `BacktraceBuffer` needs only `<span>` to test.
* **The cache has no dedicated test** and no concurrency coverage. The
  per-step cross-check does catch single-threaded staleness.
* **CI does not cover:**
  * the `genbuild.rb` artifacts or preload runs;
  * the fuzzer, which is a crash-only oracle so far;
  * sanitizers;
  * aarch64.
* No in-tree test covers the x86-64 `plt_rewrite` shape (tested manually once)
  or the lld retpoline shapes.
* On the DRAP path, the tail workaround should apply only when the main DRAP
  shape was seen.
* `//perf-convert` is still x86-64-only (`PLATFORM`). Its `perf.data` parsing
  needs an audit for arch assumptions. `SelfTest.SweepOwnFDEs` has a
  toolchain-dependent threshold, and `fastpath-sweep-test` mostly subsumes it.
* There is no `make install`, pkg-config or CMake; use Bazel or vendor the
  sources.
