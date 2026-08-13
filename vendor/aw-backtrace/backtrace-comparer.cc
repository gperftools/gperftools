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
#include <sys/syscall.h>
#include <sys/ucontext.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <span>

#include "aw-backtrace/aw-backtrace.h"
#include "check.h"
#include "sim_stepper.h"
#include "symbolize-backtrace.h"
#include "utils.h"

#define TLS_ATTR __attribute__((tls_model("initial-exec")))

namespace {

// ---------------------------------------------------------------------------
// Diagnostics output.
//
// Nothing here may go through stdio, for two independent reasons:
//
//  * We print from inside the SIGTRAP handler, i.e. at an arbitrary
//    instruction boundary in the target. glibc's stdio locks are recursive
//    per thread, so re-entering stdio from the handler does not deadlock --
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

// Callers are signal handlers and libc interposers, i.e. code running at an
// arbitrary instruction boundary in the target, so errno is saved and restored
// rather than left wherever write() put it.
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

std::atomic<uintptr_t> return_buffer_pop_skips;

struct BacktraceBuffer {
  // we use circular buffer to store backtraces. If we pop and the buffer is empty, we keep it empty. If we push and the
  // buffer is full, then "nestedmost" entry is eaten.
  static constexpr size_t kBTSize = 1024;
  static constexpr size_t kBTMask = kBTSize - 1;
  static_assert((kBTSize & (kBTSize - 1)) == 0);

  uint64_t addrs[kBTSize] = {};
  uint64_t stack_locations[kBTSize] = {};
  int suppressed_index = -1;
  size_t pos{};
  size_t size{};

  constexpr BacktraceBuffer() = default;

  void Push(uint64_t addr, uint64_t stack_pointer) {
    pos = (pos + 1) & kBTMask;
    addrs[pos] = addr;
    stack_locations[pos] = stack_pointer;
    if ((int)pos == suppressed_index) {
      suppressed_index = -1;
    }
    if (size < kBTSize) {
      size++;
    }
    // printf("push %zu, %zu\n", pos, size);
    // printf("push 0x%zx at 0x%zx\n", addr, stack_pointer);
  }

  uint64_t Pop(uint64_t stack_pointer) {
  again:
    if (size == 0) {
      return 0;
    }
    uint64_t addr = addrs[pos];
    uint64_t ret_stack = stack_locations[pos];
    if ((int)pos == suppressed_index) {
      suppressed_index = -1;
    }
    pos = (pos - 1) & kBTMask;
    size--;
    // printf("pop 0x%zx at 0x%zx\n", addr, ret_stack);
    if (ret_stack != stack_pointer) {
      // printf("going again because stack_pointer is 0x%zx!\n", stack_pointer);
      return_buffer_pop_skips.fetch_add(1, std::memory_order_relaxed);
      goto again;
    }
    // printf("pop %zu, %zu\n", pos, size);
    return addr;
  }

  uint64_t Peek(size_t offset) {
    return addrs[(pos - offset) & kBTMask];
  }

  bool IsSuppressionActive() {
    return (suppressed_index != -1);
  }

  void SetSuppression() {
    assert(!IsSuppressionActive());
    suppressed_index = (int)pos;
  }

  void ClearShadowStack() {
    size = 0;
  }

  bool IsEmpty() const {
    return size == 0;
  }
  bool CompareWith(void* current_rip, std::span<void*> backtrace, int* out_mismatch_at) {
    CHECK(!backtrace.empty());
    size_t size_to_check = std::min(size + 1, backtrace.size());
    if (backtrace[0] != current_rip) {
      *out_mismatch_at = 0;
      return false;
    }
    for (size_t i = 1; i < size_to_check; i++) {
      if (addrs[(pos - i + 1) & kBTMask] != (uint64_t)backtrace[i]) {
        *out_mismatch_at = i;
        return false;
      }
    }
    // Note: is okay for our "actual" call stack to be smaller than
    // captured. I.e. because we started with non-0 stack or if we
    // overflowed at some point.
    if (backtrace.size() < size + 1) {
      *out_mismatch_at = backtrace.size();
      return false;
    }
    return true;
  }

  int DumpIntoArray(std::span<void*> backtrace) {
    int i = 0;
    size_t size = std::min(this->size, backtrace.size());
    size_t pos = this->pos;
    while (size > 0) {
      backtrace[i++] = reinterpret_cast<void*>(addrs[pos]);
      pos = (pos - 1) & kBTMask;
      size--;
    }
    return i;
  }
};

// Eat common x86-prefixes. To help us recognize call and returns
uint8_t* EatPrefixes(uint8_t* at_rip) {
  if (*at_rip == 0x66) {  // operand-size prefix
    return EatPrefixes(at_rip + 1);
  }
  if (*at_rip == 0x67) {  // address-size prefix
    return EatPrefixes(at_rip + 1);
  }
  if ((*at_rip & 0xf0) == 0x40) {  // REX prefix
    return EatPrefixes(at_rip + 1);
  }
  if (at_rip[0] == 0xf3) {  // rep prefix
    return EatPrefixes(at_rip + 1);
  }
  return at_rip;
}

bool IsAtCallInstruction(uint8_t* at_rip) {
  at_rip = EatPrefixes(at_rip);
  if (at_rip[0] == 0xe8)  // regular "constant" call
    return true;
  if (at_rip[0] == 0xff && (at_rip[1] & 0x38) == 0x10)  // indirect call
    return true;
  return false;
}

bool IsAtReturnInstruction(uint8_t* at_rip) {
  at_rip = EatPrefixes(at_rip);
  return at_rip[0] == 0xc3;
}

bool IsAtSigtrapDisable(uint8_t* at_rip, const ucontext_t* uc) {
  if (!(at_rip[0] == 0x0f && at_rip[1] == 0x05)) {
    return false;
  }
  // syscall instruction. Lets check if someone is about to block
  // SIGTRAP.

  auto& regs = uc->uc_mcontext.gregs;
  if (regs[REG_RAX] != SYS_rt_sigprocmask) {
    return false;
  }
  if (regs[REG_RDI] != SIG_SETMASK && regs[REG_RDI] != SIG_BLOCK) {
    return false;
  }
  sigset_t* newmask = reinterpret_cast<sigset_t*>(regs[REG_RSI]);
  if (!newmask || !sigismember(newmask, SIGTRAP)) {
    return false;
  }
  return true;
}

struct ExecLog {
  static constexpr size_t kLogSize = 32;
  size_t idx = 0;
  uint8_t* rips[kLogSize];

  constexpr ExecLog() = default;

  void Record(uint8_t* addr) {
    idx = (idx + 1) % kLogSize;
    rips[idx] = addr;
  }
};

static pid_t orig_pid;

uintptr_t soft_break_pc;
std::atomic<intptr_t> soft_break_pc_skips;

std::atomic<intptr_t> stop_at_diag = (~uintptr_t{} >> 1);

__attribute__((unused)) volatile bool print_next_backtrace;

__attribute__((unused)) int backtrace_simple_with_diagnostics(const void* _uc, void** result, int max_frames) {
  int count = 0;
  std::span<void*> frames{result, static_cast<size_t>(max_frames)};
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
  aw_backtrace_ext::DebugExtensionV0::TryGet()->BacktraceExt(_uc, fnref.fn, fnref.data, {.trap_diagnostics = true});
  return count;
}

// Reference capture with the fast path and the cache both turned off,
// so it exercises only the plain .eh_frame walk. StepperCallback checks
// every normal capture against this and treats any difference as a
// failure -- that is the whole test for the fast path and the cache.
int CaptureReferenceBacktrace(const void* _uc, void** result, int max_frames) {
  int count = 0;
  std::span<void*> frames{result, static_cast<size_t>(max_frames)};
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
  aw_backtrace_ext::DebugExtensionV0::TryGet()->BacktraceExt(
      _uc, fnref.fn, fnref.data, {.print_diagnostics = false, .disable_fastpath = true, .disable_cache = true});
  return count;
}

void DoSoftBP(uintptr_t rip) {
  int pid = (int)getpid();

  DiagPrintf("got to the soft breakpoint at 0x%zx. Stopped; attach gdb to pid %d, or kill -CONT %d to resume.\n", rip,
             pid, pid);
  raise(SIGSTOP);
}

void StepperCallback(void* _uc, void*) {
  if (getpid() != orig_pid) {
    return;
  }

  ucontext_t* uc = (ucontext_t*)_uc;
  uint64_t rip = uc->uc_mcontext.gregs[REG_RIP];
  uint8_t* rip_ptr = (uint8_t*)rip;

  static thread_local bool was_at_call TLS_ATTR;
  static thread_local bool was_at_return TLS_ATTR;
  static thread_local bool was_at_sigtrap_disable TLS_ATTR;
  static thread_local constinit BacktraceBuffer backtrace_buffer TLS_ATTR;
  static thread_local ExecLog exec_log TLS_ATTR;

  bool after_sigtrap_reenable = false;

  uintptr_t stack_ptr = uc->uc_mcontext.gregs[REG_RSP];

  assert(!(was_at_call && was_at_return));
  if (was_at_call) {
    void** rsp = reinterpret_cast<void**>(stack_ptr);
    uint64_t last_rip = (uint64_t)rsp[0];
    backtrace_buffer.Push(last_rip, stack_ptr + 8);
  } else if (was_at_return) {
    backtrace_buffer.Pop(stack_ptr);
  } else if (was_at_sigtrap_disable) {
    after_sigtrap_reenable = true;
  }

  was_at_call = false;
  was_at_return = false;
  was_at_sigtrap_disable = false;

  if (IsAtSigtrapDisable(rip_ptr, uc)) {
    was_at_sigtrap_disable = true;
  } else if (IsAtCallInstruction(rip_ptr)) {
    was_at_call = true;
  } else if (IsAtReturnInstruction(rip_ptr)) {
    was_at_return = true;
  }

  // Note: exec_log recordings are not used directly. But occasionally
  // a handy debugging tool (e.g. to inspect via gdb).
  exec_log.Record(rip_ptr);

  if (rip == soft_break_pc) {
    if (soft_break_pc_skips.fetch_sub(1, std::memory_order_relaxed) <= 0) {
      DoSoftBP(rip);
    }
  }

  void* backtrace[1024];
  int frames;
again:
#if defined(NDEBUG) || defined(BUILD_SO)
  frames = aw_backtrace(uc, backtrace, 1024, 0);
#else
  frames = backtrace_simple_with_diagnostics(uc, backtrace, 1024);
#endif

  if (print_next_backtrace) {
    print_next_backtrace = false;
    DiagPrintf("handling print_next_backtrace request:\n");
    DumpStackTraceToFD(diag_out_fd, backtrace, frames, true, "");
  }

  if (backtrace_buffer.IsSuppressionActive()) {
    return;  // don't compare if suppressed
  }

  // Cross-check the normal capture (fast path + cache) against a plain
  // .eh_frame walk. Neither the fast path nor the cache is allowed to
  // change the answer, so any difference here is a bug.
  {
    void* ref_backtrace[1024];
    int ref_frames = CaptureReferenceBacktrace(uc, ref_backtrace, 1024);
    bool same = (ref_frames == frames);
    for (int i = 0; same && i < frames; i++) {
      same = (ref_backtrace[i] == backtrace[i]);
    }
    if (!same) {
      DiagPrintf("fast-path/cache backtrace disagrees with plain unwinder (fast=%d ref=%d frames)\n", frames,
                 ref_frames);
      DumpStackTraceToFD(diag_out_fd, (void* const*)backtrace, frames, true, "--fast: ");
      DiagPrintf("---\n");
      DumpStackTraceToFD(diag_out_fd, (void* const*)ref_backtrace, ref_frames, true, "--ref:  ");
      if (--stop_at_diag < 0) {
        if (!getenv("AW_BT_DIAG_VIA_CORE")) {
          DoSoftBP(rip);
        } else {
          StopSingleStepper();
          signal(SIGILL, SIG_DFL);
          asm volatile("ud2");
        }
      }
    }
  }

  int mismatch_at = -1;
  if (!backtrace_buffer.CompareWith(rip_ptr, {backtrace, static_cast<size_t>(frames)}, &mismatch_at)) {
    if (after_sigtrap_reenable) {
      backtrace_buffer.ClearShadowStack();
      return;
    }

    bool want_to_suppress = false;
    static std::array<uintptr_t, 256> suppressed_storage;
    static constinit std::span<uintptr_t> cached_suppressions{suppressed_storage.data(), 0};

    auto maybe_add = [&](Symbolizer* s, uintptr_t addr) {
      if (want_to_suppress) {
        return;
      }
      for (auto a : cached_suppressions) {
        if (a == addr) {
          want_to_suppress = true;
          return;
        }
      }
      s->Add(addr);
    };

    auto register_suppression = [&](uintptr_t addr) {
      if (cached_suppressions.size() < suppressed_storage.size()) {
        cached_suppressions = {suppressed_storage.data(), cached_suppressions.size() + 1};
        cached_suppressions[cached_suppressions.size() - 1] = addr;
        DiagPrintf("added cached suppression at 0x%zx (size %zu)\n", addr, cached_suppressions.size());
        return;
      }
      // Otherwise we silently stop remembering locations and go back to
      // reporting every one of them, every time. Say so once.
      static bool complained;
      if (!complained) {
        complained = true;
        DiagPrintf("suppression cache full at %zu entries; every location gets reported from now on\n",
                   suppressed_storage.size());
      }
    };

    WithSymbolizerFnRef(
        [&](Symbolizer* s) -> void {
          // we check top 2 frames for known suppressions
          for (int i = 0; i < std::min(frames, 2); i++) {
            maybe_add(s, reinterpret_cast<uintptr_t>(backtrace[i]));
          }
          for (size_t i = 0; i < std::min<size_t>(backtrace_buffer.size, 2); i++) {
            maybe_add(s, backtrace_buffer.Peek(i));
          }
        },
        [&](const SymbolizeOutcome& outcome) -> void {
          auto to_suppress = [&]() -> bool {
            if (outcome.function == "_dl_fixup") {
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
            register_suppression(outcome.pc);
          }
          want_to_suppress = want_to_suppress || s;
        });

    if (!want_to_suppress) {
      if (frames > 0) {
        register_suppression(reinterpret_cast<uintptr_t>(backtrace[0]));
      }
      DiagPrintf("Mismatch at %d\n", mismatch_at);
      DumpStackTraceToFD(diag_out_fd, (void* const*)backtrace, frames, true, "--bad: ");
      DiagPrintf("---\n");
      backtrace[0] = rip_ptr;
      int buffer_frames = backtrace_buffer.DumpIntoArray({backtrace + 1, sizeof(backtrace) / sizeof(backtrace[0]) - 1});
      // +1: DumpIntoArray counts what it wrote at backtrace + 1, and the rip we
      // seeded at backtrace[0] is a frame too. Without it the deepest shadow
      // entry never printed.
      DumpStackTraceToFD(diag_out_fd, (void* const*)backtrace, buffer_frames + 1, true, "--good: ");

      if (--stop_at_diag < 0) {
        if (!getenv("AW_BT_DIAG_VIA_CORE")) {
          DoSoftBP(rip);
          goto again;
        } else {
          StopSingleStepper();
          signal(SIGILL, SIG_DFL);
          asm volatile("ud2");
        }
      }
    }

    backtrace_buffer.SetSuppression();
  }
}

__attribute__((unused)) void HelperSignalHandler(int, siginfo_t*, void* _uc) {
  void* backtrace[1024];
  int frames = aw_backtrace(_uc, backtrace, 1024, 0);
  DumpStackTraceToFD(diag_out_fd, (void* const*)backtrace, frames, true, "");
}

}  // namespace

void StartBacktraceComparer() {
  EnsureDiagFD();
  orig_pid = getpid();
  const char* soft_break = getenv("AW_BT_BREAK_AT");
  if (soft_break) {
    char ex;
    size_t addr;
    size_t skips = 0;
    // We allow forms like 0x<addr> or <decimal-addr> or with
    // skip_count 0x<addr>:4 (skip 4 times before stopping)
    int scan = sscanf(soft_break, "%zi:%zi%c", &addr, &skips, &ex);
    if (scan == 3) {
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

  const char* stop_at_diag_s = getenv("AW_BT_DIAG");
  if (stop_at_diag_s) {
    stop_at_diag = atoi(stop_at_diag_s);
  }
#if 1
  StartSingleStepper(StepperCallback, nullptr);
  // a bunch of cleanup stuff in gcc's crtbegin.o etc does not have
  // unwind info. So lets just stop comparing before we have to deal
  // with it.
  atexit(StopSingleStepper);
#else
  struct sigaction sa = {};
  sa.sa_sigaction = HelperSignalHandler;
  sa.sa_flags = SA_SIGINFO;
  int rv = sigaction(SIGUSR1, &sa, nullptr);
  if (rv != 0) {
    DiagPrintf("sigaction: %s\n", strerror(errno));
    abort();
  }
#endif
}

void StopBacktraceComparer() {
  StopSingleStepper();
}

#ifdef BUILD_SO

#include <dlfcn.h>
#include <signal.h>

static bool starting_comparer;

extern "C" {
// NOTE: this is a hack, but let keep it simple
int sigaltstack(const stack_t* __restrict ss, stack_t* __restrict old_ss) __THROW {
  // Another library's constructor can reach us before ours has run, and every
  // refusal below is only useful if it is actually printed somewhere.
  EnsureDiagFD();

  static int (*orig_sigaltstack)(const stack_t* ss, stack_t* old_ss);
  if (!orig_sigaltstack) {
    void* res = dlsym(RTLD_NEXT, "sigaltstack");
    if (res == nullptr) {
      const char* err = dlerror();
      DiagPrintf("dlsym(sigaltstack): %s\n", err ? err : "(no error reported)");
      abort();
    }
    orig_sigaltstack = reinterpret_cast<decltype(orig_sigaltstack)>(res);
  }
  if (!ss) {
    return orig_sigaltstack(ss, old_ss);
  }
  if ((ss->ss_flags & SS_DISABLE) != 0) {
    DiagPrintf("prevented disabling sigaltstack\n");
    return orig_sigaltstack(nullptr, old_ss);
  }
  stack_t current_ss;
  CHECK(orig_sigaltstack(nullptr, &current_ss) == 0);
  if ((current_ss.ss_flags & SS_DISABLE) != 0 || current_ss.ss_size < ss->ss_size) {
    DiagPrintf("allowing sigaltstack for %zu at %p. Caller is %p (my stack at %p)\n", ss->ss_size, ss->ss_sp,
               __builtin_return_address(0), __builtin_frame_address(0));
    DiagPrintf("current: flags: %x, size: %zu, address: %p\n", current_ss.ss_flags, current_ss.ss_size,
               current_ss.ss_sp);
    return orig_sigaltstack(ss, old_ss);
  }
  DiagPrintf("prevented reducing sigaltstack from %zu to %zu\n", current_ss.ss_size, ss->ss_size);
  return orig_sigaltstack(nullptr, old_ss);
}

int sigaction(int signum, const struct sigaction* new_act, struct sigaction* old_act) {
  EnsureDiagFD();  // as in sigaltstack above

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

  if (starting_comparer || new_act == nullptr || new_act->sa_handler == SIG_DFL || new_act->sa_handler == SIG_IGN) {
    return orig_sigaction(signum, new_act, old_act);
  }

  struct SigPair {
    int signo;
    const char* name;
  };

  SigPair pairs[] = {{SIGTRAP, "SIGTRAP"}, {SIGUSR1, "SIGUSR1"}, {SIGUSR2, "SIGUSR2"}};

  for (int i = sizeof(pairs) / sizeof(pairs[0]) - 1; i >= 0; i--) {
    if (signum == pairs[i].signo) {
      DiagPrintf("not letting %s interception (caller: %p)\n", pairs[i].name, __builtin_return_address(0));
      if (old_act == nullptr) {
        return 0;
      }
      return orig_sigaction(signum, nullptr, old_act);
    }
  }

  // printf("sigaction for %d\n", signum);
  return orig_sigaction(signum, new_act, old_act);
}
}

static void WithCmdline(aw_backtrace_internal::FunctionRef<void(std::string_view cmdline)> body) {
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

static void PrintStats() {
  auto* ext = aw_backtrace_ext::DebugExtensionV0::TryGet();
  if (ext)
    ext->PrintStats();
  DiagPrintf("Instructions stepped: %zu (simulated %zu, altstack skips %zu)\n", sim_stepper_total_count.load(),
             sim_stepper_simulated_count.load(), sim_stepper_altstack_skips_count.load());
  DiagPrintf("Return buffer return pop resync skips: %zu\n", return_buffer_pop_skips.load());
}

static __attribute__((constructor)) void initialize() {
  starting_comparer = true;
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

  signal(SIGUSR1, [](int) -> void {
    int save_errno = errno;
    DiagPrintf("SIGUSR1 in comparer in pid %d\n", (int)getpid());
    PrintStats();
    errno = save_errno;
  });
  signal(SIGUSR2, [](int) -> void {
    int save_errno = errno;
    print_next_backtrace = true;
    errno = save_errno;
  });

  StartBacktraceComparer();
  starting_comparer = false;
}

static __attribute__((destructor)) void deinitialize() {
  StopBacktraceComparer();
  DiagPrintf("BacktraceComparer done!\n");
  PrintStats();
}
#endif
