/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#include "backtrace-comparer.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <span>

#include "aw-backtrace/aw-backtrace.h"
//
#include "check.h"
#include "comparer-arch.h"
#include "symbolize-backtrace.h"
#include "utils.h"
//
#include "v/pstepper/pstepper.h"

#define TLS_ATTR __attribute__((tls_model("initial-exec")))

namespace {

using namespace aw_backtrace_comparer;

// How deep a single capture (and a single shadow-stack dump) can go.
constexpr size_t kMaxFrames = 1024;

// ---------------------------------------------------------------------------
// Diagnostics output.
//
// Nothing here may go through stdio, for two independent reasons:
//
//  * We print from inside the stepper callback, i.e. at an arbitrary
//    instruction boundary in the target. glibc's stdio locks are recursive
//    per thread, so re-entering stdio from the callback does not deadlock --
//    it quietly interleaves with whatever the target was in the middle of and
//    corrupts the FILE. Same for the malloc printf does on first use.
//  * The target owns descriptors 0/1/2 and is free to close or redirect
//    them. A diagnostic that lands in /dev/null is worse than no diagnostic
//    at all.
//
// So we take private duplicates of the target's STDERR at startup and write(2)
// to them directly. AW_BT_DIAG_FILE=<path> overrides the destination
// ---------------------------------------------------------------------------

int diag_out_fd = -1;

int MoveDiagFD(int fd, bool close_original) {
  // Well out of the way of anything a target is likely to care about, so that
  // our descriptors do not perturb which numbers the target's own opens get.
  static constexpr int kDiagFdBase = 900;

  int moved = fcntl(fd, F_DUPFD_CLOEXEC, kDiagFdBase);
  if (moved < 0) {
    return fd;  // perhaps RLIMIT_NOFILE
  }
  if (close_original) {
    close(fd);
  }
  return moved;
}

void EnsureDiagFD() {
  static bool done;
  if (done) {
    return;
  }
  done = true;

  if (const char* path = getenv("AW_BT_DIAG_FILE")) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0) {
      diag_out_fd = MoveDiagFD(fd, /* close_original = */ true);
      return;
    }
  }

  diag_out_fd = MoveDiagFD(STDERR_FILENO, /* close_original = */ false);
}

// Callers are the stepper callback and libc interposers, i.e. code running at
// an arbitrary instruction boundary in the target, so errno is saved and
// restored rather than left wherever write() put it.
void DiagWrite(const char* buf, size_t len) {
  int saved_errno = errno;
  while (len > 0) {
    ssize_t n = write(diag_out_fd, buf, len);
    if (n > 0) {
      buf += n;
      len -= n;
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    break;  // nothing useful left to do about it
  }
  errno = saved_errno;
}

// Same errno discipline as DiagWrite.
NEVER_INLINE __attribute__((format(printf, 1, 2))) void DiagPrintf(const char* fmt, ...) {
  int saved_errno = errno;
  char buf[1 << 10];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n > 0) {
    DiagWrite(buf, std::min<size_t>(n, sizeof(buf) - 1));
  }
  errno = saved_errno;
}

// Restores errno on the way out of anything that runs at an arbitrary
// instruction boundary in the target.
struct SavedErrno {
  int saved_errno = errno;
  ~SavedErrno() {
    errno = saved_errno;
  }
};

// ---------------------------------------------------------------------------
// Counters. Relaxed atomics rather than plain globals: every thread of the
// target steps through this code, and these are read from a SIGUSR1 handler
// and at exit.
// ---------------------------------------------------------------------------

std::atomic<uint64_t> steps;
std::atomic<uint64_t> return_buffer_pop_skips;

// Unsuppressed diagnostics -- shadow-stack mismatches and cross-check
// failures alike, i.e. every one that reached ActOnDiagnostic. Only
// AW_BT_REQUIRE_STEPPER (below) reads it; without that it is just a number
// in the exit report.
std::atomic<uint64_t> diagnostics;

// ---------------------------------------------------------------------------
// The shadow call stack.
//
// Before touching this, note three properties that are deliberate and easy to
// break:
//
//  * Entries carry the sp their frame will return to, so one return can retire
//    several at once -- which is what a longjmp leaves behind, having restored
//    sp with no returns at all. The reverse case (shadow stack shorter than
//    the capture) is accepted outright: the comparer can start mid-stack.
//  * Pop can drain the stack entirely, and an empty shadow stack passes
//    everything, so one mis-modelled return turns the comparer off until calls
//    refill it. That is what makes longjmp survivable, but it traded a noisy
//    failure mode for a quiet one -- return_buffer_pop_skips is the
//    instrument, and a big jump in it means go looking.
//  * What a control transfer contributes is the arch's business, not this
//    struct's: see comparer-arch.h.
// ---------------------------------------------------------------------------

struct BacktraceBuffer {
  // we use circular buffer to store backtraces. If we pop and the buffer is empty, we keep it empty. If we push and the
  // buffer is full, then "nestedmost" entry is eaten.
  static constexpr size_t kBTSize = kMaxFrames;
  static constexpr size_t kBTMask = kBTSize - 1;
  static_assert((kBTSize & (kBTSize - 1)) == 0);

  uint64_t addrs[kBTSize] = {};
  uint64_t retire_sps[kBTSize] = {};
  // Non-null for a frame the kernel built rather than a call: the context
  // inside the signal frame it came from. Such a frame's pc is read back out
  // of that context every time it is looked at, never from addrs -- a handler
  // is free to edit the context it is going to resume through, and the
  // unwinder reports the edited value. aw-backtrace-test's SIGILL handler does
  // exactly that, stepping the saved pc past its ud2.
  const ucontext_t* sig_ucontexts[kBTSize] = {};
  int suppressed_index = -1;
  size_t pos{};
  size_t size{};

  constexpr BacktraceBuffer() = default;

  uint64_t AddrAt(size_t idx) const {
    const ucontext_t* uc = sig_ucontexts[idx];
    if (uc == nullptr) {
      return addrs[idx];
    }
    return ComparerArch::PC(uc);
  }

  void Push(const ShadowEntry& entry) {
    pos = (pos + 1) & kBTMask;
    addrs[pos] = entry.pc;
    retire_sps[pos] = entry.retire_sp;
    sig_ucontexts[pos] = nullptr;
    if ((int)pos == suppressed_index) {
      suppressed_index = -1;
    }
    if (size < kBTSize) {
      size++;
    }
  }

  // The interrupted frame of a signal delivery: same as Push, but its pc stays
  // tied to the live context inside the signal frame.
  void PushSignalFrame(const ShadowEntry& entry, const ucontext_t* interrupted_uc) {
    Push(entry);
    sig_ucontexts[pos] = interrupted_uc;
  }

  uint64_t Pop(uint64_t stack_pointer) {
  again:
    if (size == 0) {
      return 0;
    }
    uint64_t addr = addrs[pos];
    uint64_t retire_sp = retire_sps[pos];
    if ((int)pos == suppressed_index) {
      suppressed_index = -1;
    }
    pos = (pos - 1) & kBTMask;
    size--;
    if (retire_sp != stack_pointer) {
      return_buffer_pop_skips.fetch_add(1, std::memory_order_relaxed);
      goto again;
    }
    return addr;
  }

  uint64_t Peek(size_t offset) const {
    return AddrAt((pos - offset) & kBTMask);
  }

  bool IsSuppressionActive() const {
    return (suppressed_index != -1);
  }

  void SetSuppression() {
    assert(!IsSuppressionActive());
    suppressed_index = (int)pos;
  }

  bool CompareWith(uint64_t current_pc, std::span<void* const> capture, int* out_mismatch_at) const {
    CHECK(!capture.empty());
    size_t size_to_check = std::min(size + 1, capture.size());
    if ((uint64_t)capture[0] != current_pc) {
      *out_mismatch_at = 0;
      return false;
    }
    for (size_t i = 1; i < size_to_check; i++) {
      if (AddrAt((pos - i + 1) & kBTMask) != (uint64_t)capture[i]) {
        *out_mismatch_at = (int)i;
        return false;
      }
    }
    // Note: is okay for our "actual" call stack to be smaller than
    // captured. I.e. because we started with non-0 stack or if we
    // overflowed at some point.
    if (capture.size() < size + 1) {
      *out_mismatch_at = (int)capture.size();
      return false;
    }
    return true;
  }

  int DumpIntoArray(std::span<void*> out) const {
    int i = 0;
    size_t left = std::min(this->size, out.size());
    size_t at = this->pos;
    while (left > 0) {
      out[i++] = reinterpret_cast<void*>(AddrAt(at));
      at = (at - 1) & kBTMask;
      left--;
    }
    return i;
  }
};

// ---------------------------------------------------------------------------
// Per-thread state.
//
// All of it, in one named place. It used to be a pile of function-local
// statics inside StepperCallback, which made the one thing worth knowing
// about the callback -- exactly what carries over from the previous
// instruction, because that is the whole of the comparer's model -- the
// hardest thing to see.
// ---------------------------------------------------------------------------

struct ThreadState {
  // The call stack this thread's calls and returns have built up.
  BacktraceBuffer shadow_stack;

  // The previous step's pc and sp. A zero prev_sp means "no step seen on this
  // thread yet", which is what keeps signal-entry recognition from firing
  // before there is anything to tie a candidate frame back to.
  uint64_t prev_pc = 0;
  uint64_t prev_sp = 0;

  // What the previous step's instruction was about to do to the call stack.
  // A transfer can only be accounted for after the fact -- what a call put
  // where is not there until it has retired -- so it is classified on one
  // step and resolved on the next.
  ControlTransfer pending = ControlTransfer::kNone;

  constexpr ThreadState() = default;
};

// initial-exec: the callback runs at an arbitrary instruction boundary in the
// target, the dynamic loader included, so a general-dynamic access (which can
// reach __tls_get_addr, and through it malloc) is not an option.
thread_local constinit ThreadState tls_state TLS_ATTR;

// ---------------------------------------------------------------------------
// Mismatch suppression.
//
// Locations whose mismatch has already been reported once, so each one is
// reported once rather than on every instruction that walks through it. Also
// where the known-bogus spots (_dl_fixup, call_init, __run_exit_handlers) end
// up after being recognized by symbol name.
//
// Process-wide rather than per-thread: a location that is bogus is bogus on
// every thread. Lock-free and best-effort by design -- a slot is claimed with
// one fetch_add and written exactly once, so a concurrent Register can never
// corrupt an entry, and the worst a race does is let two threads report the
// same location. A zero slot means "not claimed yet", which is why the scan
// cannot stop at the first one.
//
// **This cuts both ways**: Contains() is asked about the top of the shadow
// stack as well as the capture, so a once-reported pc that later shows up as a
// return address silently skips that comparison too.
// ---------------------------------------------------------------------------

struct SuppressionCache {
  static constexpr size_t kMaxEntries = 256;

  std::atomic<uintptr_t> entries[kMaxEntries] = {};
  std::atomic<size_t> claimed = 0;
  std::atomic<bool> complained = false;

  bool Contains(uintptr_t addr) const {
    if (addr == 0) {
      return false;
    }
    for (const auto& entry : entries) {
      if (entry.load(std::memory_order_relaxed) == addr) {
        return true;
      }
    }
    return false;
  }

  void Register(uintptr_t addr) {
    if (addr == 0) {
      return;
    }
    size_t idx = claimed.fetch_add(1, std::memory_order_relaxed);
    if (idx < kMaxEntries) {
      entries[idx].store(addr, std::memory_order_relaxed);
      DiagPrintf("added cached suppression at 0x%zx (size %zu)\n", addr, idx + 1);
      return;
    }
    // Otherwise we silently stop remembering locations and go back to
    // reporting every one of them, every time. Say so once.
    if (!complained.exchange(true, std::memory_order_relaxed)) {
      DiagPrintf("suppression cache full at %zu entries; every location gets reported from now on\n", kMaxEntries);
    }
  }
};

constinit SuppressionCache suppressions;

// ---------------------------------------------------------------------------
// Knobs, mostly read once in StartBacktraceComparer (AW_BT_DIAG_FILE is read
// in EnsureDiagFD, AW_BT_CLANG_KNOWN_BUGS in ReportAtExit and
// AW_BT_DONT_DROP_PRELOAD in the .so's constructor). See AGENT.md section 5.
// ---------------------------------------------------------------------------

pid_t orig_pid;

// AW_BT_BREAK_AT=0x<addr>[:<skips>]: stop at this pc so a gdb session can
// attach. Zero when unset, and a pc is never zero while it is executing, so
// the check below costs one compare and never fires.
uint64_t soft_break_pc;
std::atomic<intptr_t> soft_break_pc_skips;

// AW_BT_DIAG=N: act on the (N+1)-th unsuppressed diagnostic, where acting is
// ActOnDiagnostic's raise(SIGTRAP). The default is INTPTR_MAX, i.e. report
// every diagnostic and never act on one.
std::atomic<intptr_t> stop_at_diag = INTPTR_MAX;

// AW_BT_CROSS_CHECK=0 turns off the fast-path/cache cross-check below. It
// doubles the cost of every step, which is the only reason to want it off.
bool cross_check_reference = true;

// Exit codes for the two AW_BT_REQUIRE_STEPPER failures. Distinct from any
// test framework's own so a CI log says which one happened.
constexpr int kNoStepperExitCode = 91;
constexpr int kDiagnosticsExitCode = 92;

// AW_BT_REQUIRE_STEPPER=1: this run is *supposed* to be stepping, so anything
// that would otherwise leave it silently comparing nothing is fatal -- no
// pstepper underneath, or a process that exits having stepped zero
// instructions. It also turns an unsuppressed diagnostic into a nonzero exit
// status at the end of the run (see ReportAtExit).
//
// This exists because the failure it guards against is invisible: without a
// plugin, pstepper_enable returns ENOSYS, every test passes, and nothing has
// been compared at all. It is for automation -- a CI run sets it and lets
// every diagnostic report before failing the run; AW_BT_DIAG is the
// interactive tool, which stops at one. Setting this ignores AW_BT_DIAG
// rather than fighting it, so acceleration stays on and the run finishes.
bool require_stepper;

volatile bool print_next_backtrace;

// Set while pstepper is armed. Without it, StopBacktraceComparer would issue
// a PSTEPPER_CMD_DISABLE that no plugin is listening for, and pstepper_disable
// dies on a failed command -- which is every run that is not under qemu, i.e.
// every plain `bazel test`.
bool stepping_armed;

// ---------------------------------------------------------------------------
// Capturing
// ---------------------------------------------------------------------------

int CaptureWithOptions(const ucontext_t* uc, std::span<void*> out,
                       const aw_backtrace_ext::DebugExtensionV0::DiagOptions& options) {
  int count = 0;
  std::span<void*> frames = out;
  auto cb = [&](void* pc, void*, bool) -> bool {
    if (frames.empty()) {
      return false;
    }
    count++;
    frames[0] = pc;
    frames = frames.subspan(1);
    return true;
  };
  aw_backtrace_internal::FunctionRef<bool(void*, void*, bool)> fnref{cb};
  aw_backtrace_ext::DebugExtensionV0::TryGet()->BacktraceExt(uc, fnref.fn, fnref.data, options);
  return count;
}

// The capture under test.
int CaptureBacktrace(const ucontext_t* uc, std::span<void*> out) {
#if defined(NDEBUG) || defined(BUILD_SO)
  // The preloadable .so walks startup and libc code nobody will fix, so a
  // diagnostic must report rather than trap. Same for NDEBUG builds, which
  // have no assert() to trap with anyway.
  return aw_backtrace(uc, out.data(), (int)out.size(), 0);
#else
  return CaptureWithOptions(uc, out, {.trap_diagnostics = true});
#endif
}

// Reference capture with the fast path and the cache both turned off, so it
// exercises only the plain .eh_frame walk.
int CaptureReferenceBacktrace(const ucontext_t* uc, std::span<void*> out) {
  return CaptureWithOptions(uc, out, {.print_diagnostics = false, .disable_fastpath = true, .disable_cache = true});
}

// ---------------------------------------------------------------------------
// Acting on a diagnostic
// ---------------------------------------------------------------------------

// Return true if we want the code to repeat backtrace capturing for the debugging.
bool ActOnDiagnostic() {
  diagnostics.fetch_add(1, std::memory_order_relaxed);
  if (--stop_at_diag >= 0) {
    return false;
  }

  // give us chance to catch it via gdb and diagnose it further
  raise(SIGTRAP);
  return true;
}

void PrintStats() {
#ifdef BUILD_SO
  // Only the preloaded .so prints these: it is the one case where the target
  // has no idea the unwinder is there and cannot ask for its counters itself.
  // A linked-in comparer's host does (aw-backtrace-test registers its own
  // atexit for exactly this), and printing them here too just doubles it.
  auto* ext = aw_backtrace_ext::DebugExtensionV0::TryGet();
  if (ext) {
    ext->PrintStats();
  }
#endif
  DiagPrintf("Instructions stepped: %llu\n", (unsigned long long)steps.load(std::memory_order_relaxed));
  DiagPrintf("Unsuppressed diagnostics: %llu\n", (unsigned long long)diagnostics.load(std::memory_order_relaxed));
  DiagPrintf("Return buffer return pop resync skips: %llu\n",
             (unsigned long long)return_buffer_pop_skips.load(std::memory_order_relaxed));
}

// ---------------------------------------------------------------------------
// The per-instruction work
// ---------------------------------------------------------------------------

// A transfer classified on the previous step has now retired, so this is where
// it lands on the shadow stack.
//
// `uc` and `sp` must describe the register file the transfer retired into,
// and the same one as each other. Normally that is the current one. The
// exception is a signal delivered at the instruction boundary right after a
// call or a return: by then sp points at the frame the kernel built, so a
// return address resolved against it would be garbage. StepperCallback
// handles that case by passing the interrupted register file from
// SignalDelivery instead, which is why EntryForRetiredCall takes the pair.
void ResolvePendingTransfer(ThreadState& ts, const ucontext_t* uc, uint64_t sp) {
  switch (ts.pending) {
    case ControlTransfer::kNone:
      break;
    case ControlTransfer::kCall:
      ts.shadow_stack.Push(ComparerArch::EntryForRetiredCall(uc, sp));
      break;
    case ControlTransfer::kReturn:
      ts.shadow_stack.Pop(sp);
      break;
  }
  ts.pending = ControlTransfer::kNone;
}

// The two frames the kernel inserted without a call, innermost last.
void PushSignalDelivery(ThreadState& ts, const SignalDelivery& sig) {
  ts.shadow_stack.PushSignalFrame(sig.interrupted, sig.interrupted_uc);
  ts.shadow_stack.Push(sig.trampoline);
}

// Cross-check the normal capture (fast path + cache) against a plain
// .eh_frame walk. Neither the fast path nor the cache is allowed to change
// the answer, so any difference here is a bug -- and this catches it
// independently of the shadow-stack machinery.
bool CrossCheckAgainstReference(const ucontext_t* uc, std::span<void* const> capture) {
  void* ref_storage[kMaxFrames];
  int ref_frames = CaptureReferenceBacktrace(uc, ref_storage);

  bool same = ((size_t)ref_frames == capture.size());
  for (size_t i = 0; same && i < capture.size(); i++) {
    same = (ref_storage[i] == capture[i]);
  }
  if (same) {
    return false;
  }

  DiagPrintf("fast-path/cache backtrace disagrees with plain unwinder (fast=%zu ref=%d frames)\n", capture.size(),
             ref_frames);
  DumpStackTraceToFD(diag_out_fd, capture.data(), (int)capture.size(), true, "--fast: ");
  DiagPrintf("---\n");
  DumpStackTraceToFD(diag_out_fd, ref_storage, ref_frames, true, "--ref:  ");
  return ActOnDiagnostic();
}

// Is this mismatch one of the known-bogus spots? Checked by symbol name, and
// cached in `suppressions` so the symbolization happens once per location
// rather than once per instruction that reaches it.
bool ShouldSuppressMismatch(const ThreadState& ts, std::span<void* const> capture) {
  bool want_to_suppress = false;

  auto maybe_add = [&](Symbolizer* s, uintptr_t addr) {
    if (want_to_suppress) {
      return;
    }
    if (suppressions.Contains(addr)) {
      want_to_suppress = true;
      return;
    }
    s->Add(addr);
  };

  WithSymbolizerFnRef(
      [&](Symbolizer* s) -> void {
        // we check top 2 frames for known suppressions
        for (size_t i = 0; i < std::min<size_t>(capture.size(), 2); i++) {
          maybe_add(s, reinterpret_cast<uintptr_t>(capture[i]));
        }
        for (size_t i = 0; i < std::min<size_t>(ts.shadow_stack.size, 2); i++) {
          maybe_add(s, ts.shadow_stack.Peek(i));
        }
      },
      [&](const SymbolizeOutcome& outcome) -> void {
        auto to_suppress = [&]() -> bool {
          if (outcome.function == "_dl_fixup") {
            return true;
          }
          if (outcome.function == "_dl_runtime_resolve") {
            return true;
          }
          if (outcome.function == "__libc_arm_za_disable") {
            return true;
          }
          if (outcome.function == "call_init" && outcome.filename.find("libc-start.c") != std::string_view::npos) {
            return true;
          }
          if (outcome.function == "call_init" && outcome.filename.find("elf/dl-init.c") != std::string_view::npos) {
            return true;
          }
          if (outcome.function == "__run_exit_handlers") {
            return true;
          }
          return false;
        };
        bool s = to_suppress();
        if (!want_to_suppress && s) {
          suppressions.Register(outcome.pc);
        }
        want_to_suppress = want_to_suppress || s;
      });

  return want_to_suppress;
}

// Report a shadow-stack mismatch, unless the location is suppressed.
//
// `storage` is the caller's whole capture array, not just the filled prefix:
// the shadow-stack dump is built in place in it, over the capture that has
// already been printed.
bool ReportMismatch(const ThreadState& ts, uint64_t pc, std::span<void*> storage, int frames, int mismatch_at) {
  std::span<void* const> capture = storage.first((size_t)frames);

  if (ShouldSuppressMismatch(ts, capture)) {
    return false;
  }

  // Register the top frame so this location reports once rather than on every
  // instruction that walks through it.
  if (frames > 0) {
    suppressions.Register(reinterpret_cast<uintptr_t>(capture[0]));
  }

  DiagPrintf("Mismatch at %d\n", mismatch_at);
  DumpStackTraceToFD(diag_out_fd, capture.data(), frames, true, "--bad: ");
  DiagPrintf("---\n");

  storage[0] = reinterpret_cast<void*>(pc);
  int buffer_frames = ts.shadow_stack.DumpIntoArray(storage.subspan(1));
  // +1: DumpIntoArray counts what it wrote at storage + 1, and the pc we
  // seeded at storage[0] is a frame too. Without it the deepest shadow entry
  // never printed.
  DumpStackTraceToFD(diag_out_fd, storage.data(), buffer_frames + 1, true, "--good: ");

  return ActOnDiagnostic();
}

// Cheap gate in front of ComparerArch::TryRecognizeSignalEntry, so we only
// read through a candidate frame when what we just saw cannot have been an
// ordinary instruction: the pc did not advance sequentially *and* sp moved by
// more than a push's worth. No single instruction does both -- on x86-64 a
// call is -8, a ret +8, a branch 0, and the instructions that do move sp a lot
// (sub, leave) do not branch; on aarch64 nothing that branches touches sp at
// all.
bool LooksLikeNonLocalTransfer(uint64_t pc, uint64_t sp, uint64_t prev_pc, uint64_t prev_sp) {
  bool sequential = (pc > prev_pc && pc - prev_pc <= ComparerArch::kMaxInsnLength);
  int64_t sp_delta = static_cast<int64_t>(sp) - static_cast<int64_t>(prev_sp);
  return !sequential && (sp_delta > 8 || sp_delta < -8);
}

void StepperCallback(int, void*, ucontext_t* uc) {
  SavedErrno saved_errno;
  steps.fetch_add(1, std::memory_order_relaxed);

  if (getpid() != orig_pid) {
    return;
  }

  const uint64_t pc = ComparerArch::PC(uc);
  const uint64_t sp = ComparerArch::SP(uc);

  const uint64_t prev_pc = tls_state.prev_pc;
  const uint64_t prev_sp = tls_state.prev_sp;
  tls_state.prev_pc = pc;
  tls_state.prev_sp = sp;

  // A signal delivery is the one control transfer a shadow stack built from
  // calls and returns cannot see: the kernel writes a frame and jumps straight
  // at the handler.
  SignalDelivery sig{};
  bool at_signal_entry = prev_sp != 0 && LooksLikeNonLocalTransfer(pc, sp, prev_pc, prev_sp) &&
                         ComparerArch::TryRecognizeSignalEntry(uc, sp, prev_sp, &sig);

  if (at_signal_entry) {
    // The register file the pending instruction actually left behind. If the
    // signal *is* that instruction's fault it never retired, so there is
    // nothing to resolve.
    if (ComparerArch::PC(sig.interrupted_uc) != prev_pc) {
      ResolvePendingTransfer(tls_state, sig.interrupted_uc, ComparerArch::SP(sig.interrupted_uc));
    }
    tls_state.pending = ControlTransfer::kNone;
    PushSignalDelivery(tls_state, sig);
  } else {
    ResolvePendingTransfer(tls_state, uc, sp);
  }
  tls_state.pending = ComparerArch::ClassifyInsn(uc);

  if (pc == soft_break_pc) {
    if (soft_break_pc_skips.fetch_sub(1, std::memory_order_relaxed) <= 0) {
      // put gdb breakpoint here for stepping through the backtracing at the soft_break_pc
      asm volatile("nop" : : : "memory");
    }
  }

gdb_again:
  void* storage[kMaxFrames];
  int frames = CaptureBacktrace(uc, storage);

  if (print_next_backtrace) {
    print_next_backtrace = false;
    DiagPrintf("handling print_next_backtrace request:\n");
    DumpStackTraceToFD(diag_out_fd, storage, frames, true, "");
  }

  if (tls_state.shadow_stack.IsSuppressionActive()) {
    return;  // don't compare if suppressed
  }

  if (cross_check_reference) {
    if (CrossCheckAgainstReference(uc, {storage, (size_t)frames})) {
      goto gdb_again;
    }
  }

  int mismatch_at = -1;
  if (tls_state.shadow_stack.CompareWith(pc, {storage, (size_t)frames}, &mismatch_at)) {
    // everything matches.
    return;
  }

  if (ReportMismatch(tls_state, pc, storage, frames, mismatch_at)) {
    goto gdb_again;
  }
  // don't bother checking and reporting the same mismatch
  tls_state.shadow_stack.SetSuppression();
}

void ParseSoftBreakEnv(const char* soft_break) {
  char ex;
  size_t addr;
  size_t skips = 0;
  // We allow forms like 0x<addr> or <decimal-addr> or with
  // skip_count 0x<addr>:4 (skip 4 times before stopping)
  int scan = sscanf(soft_break, "%zi:%zi%c", &addr, &skips, &ex);
  if (scan == 3 || scan == EOF) {
  garbage:
    DiagPrintf("garbage after AW_BT_BREAK_AT=0x%zx:%zu\n", addr, skips);
    abort();
  }
  if (scan == 1) {
    scan = sscanf(soft_break, "%zi%c", &addr, &ex);
    if (scan == 2) {
      goto garbage;
    }
  }
  if (scan == 0) {
    DiagPrintf("failed to parse AW_BT_BREAK_AT=%s\n", soft_break);
    abort();
  }
  DiagPrintf("set AW_BT_BREAK_AT for addr = 0x%zx skips = %zu\n", addr, skips);
  soft_break_pc = addr;
  soft_break_pc_skips = static_cast<intptr_t>(skips);
}

}  // namespace

void StartBacktraceComparer() {
  EnsureDiagFD();
  orig_pid = getpid();

  bool enable_acceleration = true;

  require_stepper = (getenv("AW_BT_REQUIRE_STEPPER") != nullptr);

  if (const char* soft_break = getenv("AW_BT_BREAK_AT")) {
    ParseSoftBreakEnv(soft_break);
    // any diagnostics settings turn off default of enable_acceleration
    enable_acceleration = false;
  }
  // Deliberately not honoured under AW_BT_REQUIRE_STEPPER: there the run
  // wants every diagnostic reported and the verdict at the end, not a
  // SIGTRAP at the first one from a handler that -- with acceleration on --
  // is running as host code inside qemu.
  if (const char* stop_at_diag_s = getenv("AW_BT_DIAG"); stop_at_diag_s && !require_stepper) {
    enable_acceleration = false;
    stop_at_diag = atoi(stop_at_diag_s);
  }

  if (const char* cross_check = getenv("AW_BT_CROSS_CHECK")) {
    cross_check_reference = (atoi(cross_check) != 0);
  }

  if (const char* acc = getenv("AW_BT_ACCEL")) {
    enable_acceleration = (atoi(acc) != 0);
  }

  int err = pstepper_enable(StepperCallback, 128 << 10, (int)enable_acceleration);
  if (err != 0) {
    DiagPrintf("pstepper_enable FAILED: %s\n", strerror(err));
    if (require_stepper) {
      DiagPrintf("AW_BT_REQUIRE_STEPPER is set and there is no stepper -- nothing would be compared\n");
      _exit(kNoStepperExitCode);
    }
    return;
  }
  stepping_armed = true;
  // a bunch of cleanup stuff in gcc's crtbegin.o etc does not have
  // unwind info. So lets just stop comparing before we have to deal
  // with it.
  atexit(StopBacktraceComparer);
}

void StopBacktraceComparer() {
  if (!stepping_armed) {
    return;
  }
  stepping_armed = false;
  pstepper_disable();
}

namespace {

// Runs after the atexit(StopBacktraceComparer) above, so by here stepping is
// already down; this is only the report. Nothing to say if we never stepped
// anything -- which is every run without a pstepper implementation under it.
//
// Under AW_BT_REQUIRE_STEPPER it is also the verdict, and _exit is how it
// delivers one: a destructor cannot make the process fail any other way, and
// by here the test's own main() has long since returned its 0. That does
// skip whatever destructors would have run after this one, which is fine --
// the run has already failed.
__attribute__((destructor)) void ReportAtExit() {
  StopBacktraceComparer();
  uint64_t stepped = steps.load(std::memory_order_relaxed);
  if (stepped == 0) {
    if (require_stepper) {
      DiagPrintf("AW_BT_REQUIRE_STEPPER is set and not one instruction was stepped\n");
      _exit(kNoStepperExitCode);
    }
    return;
  }
  DiagPrintf("BacktraceComparer done!\n");
  PrintStats();

  uint64_t bad = diagnostics.load(std::memory_order_relaxed);
  if (require_stepper && bad != 0) {
    if (getenv("AW_BT_CLANG_KNOWN_BUGS") != nullptr) {
      DiagPrintf(
          "So this would be failing test due to %llu unsuppressed diagnostic(s), but we're in CI and this not yet "
          "fixed compiler\n",
          (unsigned long long)bad);
      return;
    }
    DiagPrintf("FAILED: %llu unsuppressed diagnostic(s)\n", (unsigned long long)bad);
    _exit(kDiagnosticsExitCode);
  }
}

}  // namespace

#ifdef BUILD_SO

#include <dlfcn.h>
#include <link.h>

#include "backtrace-core.h"

// To be invoked from GDB.
struct DiagLookup {
  aw_backtrace_internal::LookupOutcome outcome;
  aw_backtrace_internal::FrameInfo info;
};

DiagLookup DiagUnwindLookup(uintptr_t lookup_ip) {
  DiagLookup res;
  res.outcome = aw_backtrace_internal::DoUnwindLookup(lookup_ip, &res.info, {});
  return res;
}

aw_backtrace_internal::EHReaderInputs DiagLocateEHFrame(uintptr_t lookup_ip) {
  aw_backtrace_internal::EHReaderInputs res{};
  aw_backtrace_internal::LocateEHFrame(lookup_ip, &res);
  return res;
}

// Diagnostics for accelerated runs. Data for add-symbol-file command of gdb.
struct TextSection {
  const char* path;
  uintptr_t text_location;
};

TextSection* diag_all_text_sections;
int diag_all_text_sections_count;

void DiagPopulateTextSections() {
  dl_iterate_phdr(
      [](struct dl_phdr_info* info, size_t, void*) -> int {
        auto do_pread = [](int fd, void* buf, size_t size, off_t off) -> bool {
          return (pread(fd, buf, size, off) == (ssize_t)size);
        };

        const char* path = info->dlpi_name;
        if (!path || !path[0])
          path = "/proc/self/exe";

        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
          goto out;

        ElfW(Ehdr) ehdr;
        if (!do_pread(fd, &ehdr, sizeof(ehdr), 0))
          goto out;

        size_t shdrs_sz;
        ElfW(Shdr) * shdrs;

        shdrs_sz = (size_t)ehdr.e_shnum * ehdr.e_shentsize;
        shdrs = (ElfW(Shdr)*)alloca(shdrs_sz);

        if (!do_pread(fd, shdrs, shdrs_sz, ehdr.e_shoff))
          goto out;

        if (ehdr.e_shstrndx >= ehdr.e_shnum)
          goto out;

        ElfW(Shdr) * strtab_hdr;
        char* shstrtab;

        strtab_hdr = &shdrs[ehdr.e_shstrndx];
        shstrtab = (char*)alloca(strtab_hdr->sh_size);

        if (!do_pread(fd, shstrtab, strtab_hdr->sh_size, strtab_hdr->sh_offset))
          goto out;

        for (int i = 0; i < ehdr.e_shnum; i++) {
          if (shdrs[i].sh_name < strtab_hdr->sh_size && std::string_view{shstrtab + shdrs[i].sh_name} == ".text") {
            uintptr_t text_addr = (uintptr_t)info->dlpi_addr + shdrs[i].sh_addr;
            int next_count = diag_all_text_sections_count + 1;

            diag_all_text_sections = (TextSection*)realloc(diag_all_text_sections, next_count * sizeof(TextSection));
            TextSection* added = diag_all_text_sections + diag_all_text_sections_count;

            added->path = path;
            added->text_location = text_addr;

            diag_all_text_sections_count = next_count;
            goto out;
          }
        }

      out:
        close(fd);
        return 0;
      },
      nullptr);

  DiagPrintf("diag_all_text_sections = %p\n", diag_all_text_sections);
}

extern "C" {

int sigaction(int signum, const struct sigaction* new_act, struct sigaction* old_act) {
  EnsureDiagFD();  // another library's constructor can reach us before ours has run

  static int (*orig_sigaction)(int, const struct sigaction*, struct sigaction*);
  if (!orig_sigaction) {
    void* res = dlsym(RTLD_NEXT, "sigaction");
    if (res == nullptr) {
      const char* err = dlerror();
      DiagPrintf("dlsym(sigaction): %s\n", err ? err : "(no error reported)");
      abort();
    }
    orig_sigaction = reinterpret_cast<decltype(orig_sigaction)>(res);
  }

  if (new_act == nullptr || new_act->sa_handler == SIG_DFL || new_act->sa_handler == SIG_IGN) {
    return orig_sigaction(signum, new_act, old_act);
  }

  struct SigPair {
    int signo;
    const char* name;
  };

  SigPair pairs[] = {{SIGUSR1, "SIGUSR1"}, {SIGUSR2, "SIGUSR2"}};

  for (int i = sizeof(pairs) / sizeof(pairs[0]) - 1; i >= 0; i--) {
    if (signum == pairs[i].signo) {
      DiagPrintf("not letting %s interception (caller: %p)\n", pairs[i].name, __builtin_return_address(0));
      if (old_act == nullptr) {
        return 0;
      }
      return orig_sigaction(signum, nullptr, old_act);
    }
  }

  return orig_sigaction(signum, new_act, old_act);
}
}

namespace {

void WithCmdline(aw_backtrace_internal::FunctionRef<void(std::string_view cmdline)> body) {
  FILE* f = fopen("/proc/self/cmdline", "r");
  char* line{};
  size_t size{};
  ssize_t nread = getline(&line, &size, f);
  if (nread < 0) {
    perror("getline");
    abort();
  }
  for (int i = 0; i < nread; i++) {
    if (!line[i])
      line[i] = ' ';
  }
  body(std::string_view{line, (size_t)nread});
  free(line);
  fclose(f);
}

__attribute__((constructor)) void initialize() {
  if (!getenv("AW_BT_DONT_DROP_PRELOAD")) {
    unsetenv("LD_PRELOAD");
  }

  // Before the first thing we print, and before the target has had any chance
  // to touch its own stdio.
  EnsureDiagFD();

  // The cmdline goes out through DiagWrite rather than as a DiagPrintf
  // argument: it can be longer than DiagPrintf's buffer, and this is the one
  // line where seeing all of it is the point.
  WithCmdline([](std::string_view cmdline) {
    DiagPrintf("Starting comparer pid %d: ", (int)getpid());
    DiagWrite(cmdline.data(), cmdline.size());
    DiagWrite("\n", 1);
  });
  DiagPrintf("comparer diagnostics on fd %d\n", diag_out_fd);

  DiagPopulateTextSections();

  signal(SIGUSR1, [](int) -> void {
    SavedErrno saved_errno;
    DiagPrintf("SIGUSR1 in comparer in pid %d\n", (int)getpid());
    PrintStats();
  });
  signal(SIGUSR2, [](int) -> void {
    SavedErrno saved_errno;
    print_next_backtrace = true;
  });

  StartBacktraceComparer();
}

}  // namespace

#endif  // BUILD_SO
