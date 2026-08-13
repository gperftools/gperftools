/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
//
// Originally authored for gperftools and relicensed to 0BSD by the author.
#include "single_stepper.h"

#if __linux__ && __x86_64__ && !defined(TCMALLOC_OMIT_SINGLE_STEPPER)

#include <errno.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#include <atomic>
#include <type_traits>

#include "base/static_storage.h"

static inline void do_check_fail(const char* file, int line, const char* cond) {
  static char buf[4096];
  int written = snprintf(buf, sizeof(buf), "Check fail at %s:%d: %s\n", file, line, cond);
  written = write(2, buf, written);
  (void)written;
  asm volatile("int $3; nop");
}

#define CHECK(cond)                             \
  do {                                          \
    if (!(cond)) {                              \
      do_check_fail(__FILE__, __LINE__, #cond); \
    }                                           \
  } while (false)

namespace {

// This is stolen from gperftools's spinlock.h and reworked. Note:
// this strictly to be used as global/static variable, never on stack
// or heap.
class TrivialOnce {
 public:
  template <typename Body>
  bool RunOnce(Body body) {
    auto done_atomic = reinterpret_cast<std::atomic<int>*>(&done_flag_);
    int old = done_atomic->load(std::memory_order_acquire);

    do {
      if (old == 1) {
        return false;
      }
      if (old == 2) {
        sched_yield();
        old = 0;  // try swapping 0 -> 2 after sleep
      }
    } while (!done_atomic->compare_exchange_strong(old, 2));

    // barrier provided by lock
    body();

    done_atomic->store(1, std::memory_order_release);
    return true;
  }

 private:
  // We keep pre-c+20 behavior of gperftools of avoiding constexpr for
  // complicated reasons. So it is bare 'int' not std::atomic and it is explicitly uninitialized
  int done_flag_;
};
static_assert(std::is_trivially_default_constructible_v<TrivialOnce>);
static_assert(std::is_trivially_destructible_v<TrivialOnce>);

}  // namespace

#include "base/cleanup.h"

#define STEPPER_SUPPORTED 1

namespace tcmalloc {
namespace {

// FIXME: this is leaky. But we're effectively single threaded anyways.
alignas(16) static thread_local char* alt_stack __attribute__((tls_model("initial-exec")));
static constexpr size_t kAltStackSize = 256 << 10;

void set_altstack() {
  stack_t ss;
  CHECK(sigaltstack(nullptr, &ss) == 0);
  if (ss.ss_flags != SS_DISABLE) {
    return;  // already has sigaltstack
  }
  if (!alt_stack) {
    static constexpr size_t kPage = 8192;
    alt_stack = (char*)mmap(0, kAltStackSize + kPage * 2, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    CHECK(alt_stack != MAP_FAILED);
    void* ret = mmap(alt_stack, kPage, PROT_NONE, MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0);
    CHECK(ret != MAP_FAILED);
    alt_stack += kPage;
    CHECK(mmap(alt_stack + kAltStackSize, kPage, PROT_NONE, MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0) != MAP_FAILED);
  }
  ss.ss_sp = alt_stack;
  ss.ss_size = kAltStackSize;
  ss.ss_flags = 0;
  CHECK(sigaltstack(&ss, nullptr) == 0);
}

__attribute__((naked))
void syscall_trampoline() {
  asm volatile (R"(
	syscall
	nop
	nop
	nop
	nop
syscall_trampoline_end:
	ud2
)");
}

extern "C" {
  extern const char syscall_trampoline_end[];
}

static thread_local uintptr_t saved_syscall_location __attribute__((tls_model("initial-exec")));

struct SingleStepperImpl : public SingleStepper {
  ~SingleStepperImpl() = default;

  static inline std::atomic<bool> active = {};
  static inline SingleStepperImpl* active_impl = nullptr;
  static constexpr uintptr_t kTF = 0x100;  // Trace flag in x86 FLAGS register.

  SteppingCallbackFn callback;

  static void step_handler(int, siginfo_t*, void* _uc) {
    int saved_errno = errno;
    tcmalloc::Cleanup preserve_errno([&saved_errno]() { errno = saved_errno; });

    ucontext_t* uc = static_cast<ucontext_t*>(_uc);

    // If we trapped inside the trampoline, put rip back where the target
    // thinks it is. This has to happen before the !active check below:
    // returning with rip still in the trampoline runs it off the end into the
    // ud2. And stopping while a syscall is redirected is an ordinary thing to
    // do -- StopSingleStepper() only clears the callback, so Stop() is reached
    // from the next trap, which may well be one we redirected.
    {
      uintptr_t trampoline_start = reinterpret_cast<uintptr_t>(syscall_trampoline);
      uintptr_t trampoline_end = reinterpret_cast<uintptr_t>(syscall_trampoline_end);
      uintptr_t rip = uc->uc_mcontext.gregs[REG_RIP];
      if (trampoline_start < rip && rip < trampoline_end) {
        // Nothing gets into the trampoline except by our own doing, so this
        // must have been set. Notably it is thread-local, and a thread we
        // failed to exclude below would arrive here with a fresh, zero copy.
        CHECK(saved_syscall_location != 0);
        uc->uc_mcontext.gregs[REG_RIP] = saved_syscall_location;
      }
    }

    if (!active.load(std::memory_order_relaxed)) {
      uc->uc_mcontext.gregs[REG_EFL] &= ~kTF;
      return;
    }

    uc->uc_mcontext.gregs[REG_EFL] |= kTF;

    errno = saved_errno;
    active_impl->callback(uc, active_impl);
    saved_errno = errno;

    // NOTE: the callback runs the interpreter, which advances uc's RIP past
    // every instruction it was able to simulate. So we must re-read RIP here;
    // the instruction we're about to let the hardware execute is the one the
    // interpreter stopped at, not the one we were entered with.
    auto at_rip = reinterpret_cast<uint8_t*>(uc->uc_mcontext.gregs[REG_RIP]);
    // check if next instruction is a syscall
    if (at_rip[0] != 0x0f || at_rip[1] != 0x05) {
      return; // Not syscall. We're done.
    }

    // for all syscalls we "redirect" them to special
    // trampoline. Because syscalls "skip" SIGTRAP for the next
    // instruction after them. Except if we're blocking sigtrap
    if (try_handle_sigtrap_blocking(uc) || is_thread_creating_syscall(uc)) {
      return;
    }

    saved_syscall_location = reinterpret_cast<uintptr_t>(at_rip + 2);
    uc->uc_mcontext.gregs[REG_RIP] = reinterpret_cast<uintptr_t>(syscall_trampoline);
  }

  // clone/clone3 must not be redirected. Both the parent and the child return
  // to the instruction right after the syscall -- which would be inside the
  // trampoline -- but a newly created thread gets a fresh TLS block, so its
  // saved_syscall_location is 0 and we'd send it to rip 0. (fork and vfork are
  // fine: the child gets a copy of our TLS, or shares it outright.)
  //
  // Letting these keep the skipped-instruction behaviour costs us exactly the
  // test of the syscall's result -- "testq %rax,%rax" in glibc's clone.S --
  // which is never a call or a ret, so a shadow call stack does not miss it.
  static bool is_thread_creating_syscall(const ucontext_t* uc) {
    auto nr = uc->uc_mcontext.gregs[REG_RAX];
    if (nr == SYS_clone) {
      return true;
    }
#ifdef SYS_clone3
    if (nr == SYS_clone3) {
      return true;
    }
#endif
    return false;
  }

  static bool try_handle_sigtrap_blocking(ucontext_t* uc) {
    // We're at syscall instruction. Lets check if someone is about to block
    // SIGTRAP. If so we must turn off single-stepping, because
    // otherwise blocked SIGTRAP and pending single-stepping will kill
    // the process.

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

    // okay, once we detected this case, we drop single-stepping
    // flag, block SIGTRAP and raise it. So that when SIGTRAP is
    // eventually unblocked, we'll get back to signal hander and
    // re-set single-stepping back.
    regs[REG_EFL] &= ~kTF;
    raise(SIGTRAP);
    void* oldmask = reinterpret_cast<void*>(regs[REG_RDX]);
    if (oldmask) {
      // kernel signal set size is 64 bits (glibc reserves more).
      constexpr size_t kSigSetSize = 8;
      // handle "get old mask" part, so we can block our
      // signal. I.e. populate sigmask from our ucontext and so the
      // caller gets what they want. Notably, with SIGTRAP not
      // blocked.
      memcpy(oldmask, &uc->uc_sigmask, kSigSetSize);
      regs[REG_RDX] = 0;
    }
    // And then we silently add SIGTRAP to the blocked set even if
    // syscall instruction is about to disable it anyways. This is due
    // to raise(SIGTRAP) above.
    sigaddset(&uc->uc_sigmask, SIGTRAP);

    return true;
  }

  void Start(SteppingCallbackFn callback) override {
    CHECK(!active.load(std::memory_order_relaxed));

    this->callback = callback;
    active_impl = this;

    // Then we prepare SIGTRAP signal handler.
    {
      struct sigaction sa;
      memset(&sa, 0, sizeof(sa));
      sa.sa_sigaction = step_handler;
      sa.sa_flags = SA_RESTART | SA_SIGINFO | SA_ONSTACK;
      CHECK(sigaction(SIGTRAP, &sa, nullptr) == 0);
    }

    active.store(true, std::memory_order_relaxed);
    set_altstack();
    raise(SIGTRAP);
  }

  void Stop() override {
    active.store(false, std::memory_order_relaxed);
  }

  bool IsAtLockInstruction(void* uc) override {
    ucontext_t* real_uc = static_cast<ucontext_t*>(uc);
    auto at_rip = reinterpret_cast<uint8_t*>(real_uc->uc_mcontext.gregs[REG_RIP]);
    return (*at_rip == 0xf0);  // LOCK prefix
  }
};

SingleStepperImpl* GetStepper() {
  static TrivialOnce once;
  static StaticStorage<SingleStepperImpl> storage;
  once.RunOnce([]() { storage.Construct(); });
  return storage.get();
}

}  // anonymous namespace
}  // namespace tcmalloc

#endif

namespace tcmalloc {

SingleStepper::~SingleStepper() = default;

std::optional<SingleStepper*> SingleStepper::Get() {
#if STEPPER_SUPPORTED
  return GetStepper();
#else
  return std::nullopt;
#endif
}

}  // namespace tcmalloc
