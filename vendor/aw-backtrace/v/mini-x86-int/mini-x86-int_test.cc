/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#include "mini-x86-int.h"

#include <assert.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include <memory>

/*
 * Approach to testing. We setup small asm fragments that we expect to
 * be successfully interpreted by our mini interpreter. We'll run
 * those fragments with interpreter and "directly". Running directly
 * will end with ud2 instruction and we'll intercept SIGILL to capture
 * a set of general purpose registers. We'll then compare registers
 * and selected memory locations to ensure the outcome is the same.
 */

static constexpr uint32_t kMagicWord = 0xdefeb144;

typedef void (*test_fn_ptr)();

struct TCPointer {
  test_fn_ptr ptr;
  const char* name;
};

extern "C" {
// actual tests are in test-cases.S
extern TCPointer __start_test_case_pointers;
extern TCPointer __stop_test_case_pointers;
extern uint64_t test_scratch_space[];
extern uint64_t test_scratch_space_end[];
}

static void SetupSignal(int signo, void (*handler)(int, siginfo_t*, void*)) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = handler;
  sa.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
  if (sigaction(signo, &sa, nullptr) != 0) {
    abort();
  }
}

static void CopyRegs(greg_t* from, greg_t* to) {
  // Only copy actual registers (plus flags), not stuff like trapno
  // etc
  memcpy(to, from, sizeof(to[0]) * (REG_EFL + 1));
}

static bool IsRightUD2(uint8_t* at_rip) {
  if (at_rip[0] != 0x0f || at_rip[1] != 0x0b) {
    return false;  // ud2 instruction
  }
  uint32_t w;
  memcpy(&w, at_rip + 2, sizeof(w));
  return (w == kMagicWord);
}

gregset_t sigill_captured_regs;
ucontext_t sigill_return_context;

static void sigill_handler(int signo, siginfo_t* si, void* _uc) {
  ucontext_t* uc = static_cast<ucontext_t*>(_uc);
  uint8_t* at_rip = reinterpret_cast<uint8_t*>(uc->uc_mcontext.gregs[REG_RIP]);
  if (!IsRightUD2(at_rip)) {
    printf("Actual bad instruction!\n");
    abort();
  }

  CopyRegs(uc->uc_mcontext.gregs, sigill_captured_regs);
  CopyRegs(sigill_return_context.uc_mcontext.gregs, uc->uc_mcontext.gregs);
  sigill_captured_regs[REG_EFL] ^= (1 << 16);  // drop 'return flag'
}

ucontext_t jump_into_regs;
bool jumped;
static void sigusr1_handler(int signo, siginfo_t*, void* _uc) {
  ucontext_t* uc = static_cast<ucontext_t*>(_uc);
  jumped = true;
  CopyRegs(jump_into_regs.uc_mcontext.gregs, uc->uc_mcontext.gregs);
}

static void jump_real_cpu() {
  jumped = false;
  getcontext(&sigill_return_context);
  if (jumped) {
    return;
  }

  raise(SIGUSR1);
  // this is actually unreachable but the compiler cannot know it is,
  // otherwise it won't maintain stack frame as necessary
  __asm__ __volatile__("ud2" : : : "memory");
}

static gregset_t simulated_regs;
static void jump_simulated_cpu() {
  ucontext_t* uc = &jump_into_regs;
  while (true) {
    uint8_t* at_rip = reinterpret_cast<uint8_t*>(uc->uc_mcontext.gregs[REG_RIP]);
    if (IsRightUD2(at_rip)) {
      break;
    }
    if (!mini_x86_int_try_advance(uc)) {
      printf("failed to advance\n");
      asm volatile("int $3");
    }
  }
  CopyRegs(uc->uc_mcontext.gregs, simulated_regs);
}

static void jump_simulated_cpu_expect_failure() {
  ucontext_t* uc = &jump_into_regs;
  while (true) {
    uint8_t* at_rip = reinterpret_cast<uint8_t*>(uc->uc_mcontext.gregs[REG_RIP]);
    if (!mini_x86_int_try_advance(uc)) {
      // Expected failure
      break;
    }
    if (IsRightUD2(at_rip)) {
      printf("Unexpected success (or reached unsupported op without failure)\n");
      // This is a failure of the test expectation
      asm volatile("int $3");
      break;
    }
  }
  CopyRegs(uc->uc_mcontext.gregs, simulated_regs);
}

extern "C" uint64_t initialize_flags();

static constexpr size_t kTestStackSize = 32 << 10;
static char test_stack[kTestStackSize] __attribute__((aligned(16)));

__attribute__((noinline)) static void prepare_regs_and_jump(void (*jump_fn)(), void (*test_fn)()) {
  memset(test_stack, 0, sizeof(test_stack));

  uint64_t flags = initialize_flags();

  memset(&jump_into_regs, 0, sizeof(jump_into_regs));
  auto& grps = jump_into_regs.uc_mcontext.gregs;
  grps[REG_RAX] = 0;
  grps[REG_RCX] = 1;
  grps[REG_RDX] = 2;
  grps[REG_RBX] = 3;
  grps[REG_RSI] = 4;
  grps[REG_RDI] = 5;
  grps[REG_RBP] = 6;
  grps[REG_R8] = 7;
  grps[REG_R9] = 8;
  grps[REG_R10] = 9;
  grps[REG_R11] = 10;
  grps[REG_R12] = 11;
  grps[REG_R13] = 12;
  grps[REG_R14] = 13;
  grps[REG_R15] = 14;

  grps[REG_EFL] = flags;
  grps[REG_RIP] = reinterpret_cast<uintptr_t>(reinterpret_cast<void*>(test_fn));
  char* stack_top = test_stack + sizeof(test_stack) - sizeof(void*);
  memset(stack_top, 0, sizeof(void*));
  jump_into_regs.uc_mcontext.gregs[REG_RSP] = reinterpret_cast<uintptr_t>(stack_top);

  size_t scratch_size = sizeof(*test_scratch_space) * (test_scratch_space_end - test_scratch_space);
  assert(scratch_size == 1024);
  memset(test_scratch_space, 0x55, scratch_size);

  jump_fn();
  // just in case, access stack array to avoid tail-calling above
  (void)*(const_cast<char volatile*>(&test_stack[0]));
}

struct RegName {
  int idx;
  const char* name;
};

// clang-format off
#define D(R) {R, #R}
static constexpr RegName kRelevantRegs[] = {
  D(REG_RAX),
  D(REG_RBX),
  D(REG_RCX),
  D(REG_RDX),
  D(REG_R8),
  D(REG_R9),
  D(REG_R10),
  D(REG_R11),
  D(REG_R12),
  D(REG_R13),
  D(REG_R14),
  D(REG_R15),
  D(REG_RIP),
  D(REG_RSP),
  D(REG_RBP),
  D(REG_RSI),
  D(REG_RDI),
  D(REG_EFL)
};
#undef D
// clang-format on

static __attribute__((used)) void print_regs(greg_t* regs) {
  for (const RegName& rn : kRelevantRegs) {
    printf("regs[%s] = 0x%016zx\n", rn.name, size_t(regs[rn.idx]));
  }
}

static void print_arith_eflags(uint64_t eflags) {
  bool cf = eflags & (1 << 0);
  bool pf = eflags & (1 << 2);
  bool af = eflags & (1 << 4);
  bool zf = eflags & (1 << 6);
  bool sf = eflags & (1 << 7);
  bool of = eflags & (1 << 11);

  printf("eflags 0x%016zx: cf=%d, pf=%d, af=%d, zf=%d, sf=%d, of=%d\n", eflags, cf, pf, af, zf, sf, of);
}

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  {
    static constexpr int kSigStackSize = 32 << 10;
    static char signal_stack_buf[kSigStackSize];
    stack_t ss;
    ss.ss_sp = signal_stack_buf;
    ss.ss_size = sizeof(signal_stack_buf);
    ss.ss_flags = 0;
    if (sigaltstack(&ss, nullptr) != 0) {
      perror("sigaltstack");
      abort();
    }
  }

  SetupSignal(SIGUSR1, sigusr1_handler);
  SetupSignal(SIGILL, sigill_handler);

  const char* filter = nullptr;

  if (argc > 1) {
    if (argc > 2) {
      printf("I only accept single arg: test substring to filter on\n");
      return 1;
    }
    filter = argv[1];
  }

  bool had_mismatch = false;
  size_t scratch_size = sizeof(*test_scratch_space) * (test_scratch_space_end - test_scratch_space);

  // Allocate buffers to hold snapshots.
  std::unique_ptr<char[]> actual_stack = std::make_unique<char[]>(kTestStackSize);
  std::unique_ptr<char[]> good_stack = std::make_unique<char[]>(kTestStackSize);
  std::unique_ptr<char[]> actual_scratch = std::make_unique<char[]>(scratch_size);
  std::unique_ptr<char[]> good_scratch = std::make_unique<char[]>(scratch_size);

  for (TCPointer* place = &__start_test_case_pointers; place != &__stop_test_case_pointers; place++) {
    printf("%p: test %s with fn %p\n", place, place->name, place->ptr);
    if (filter && strstr(place->name, filter) == nullptr) {
      printf("skipped.\n");
      continue;
    }

    TCPointer test = *place;

    if (strstr(test.name, "unsupported") != nullptr) {
      // Some tests exercise unsupported instructions. So just we just run them with simulator (not real cpu) and verify
      // that the interpreter stops.
      prepare_regs_and_jump(jump_simulated_cpu_expect_failure, test.ptr);
      continue;
    }

    prepare_regs_and_jump(jump_simulated_cpu, test.ptr);
    memcpy(actual_stack.get(), test_stack, kTestStackSize);
    memcpy(actual_scratch.get(), test_scratch_space, scratch_size);

    prepare_regs_and_jump(jump_real_cpu, test.ptr);
    memcpy(good_stack.get(), test_stack, kTestStackSize);
    memcpy(good_scratch.get(), test_scratch_space, scratch_size);

    gregset_t good_regs;
    CopyRegs(sigill_captured_regs, good_regs);

    for (const RegName& rn : kRelevantRegs) {
      auto good = good_regs[rn.idx];
      auto actual = simulated_regs[rn.idx];
      if (good != actual) {
        printf("register %s mismatch: 0x%016zx (good) vs 0x%016zx (bad)\n", rn.name, size_t(good), size_t(actual));
        if (rn.idx == REG_EFL) {
          print_arith_eflags(good);
          print_arith_eflags(actual);
        }
        had_mismatch = true;
      }
    }

    if (memcmp(good_stack.get(), actual_stack.get(), kTestStackSize) != 0) {
      printf("stack mismatch\n");
      had_mismatch = true;
    }

    if (memcmp(good_scratch.get(), actual_scratch.get(), scratch_size) != 0) {
      printf("scratch mismatch\n");
      had_mismatch = true;
    }

    printf("all regs (from interpreter):\n");
    print_regs(simulated_regs);
    printf("\n\n");
  }

  printf("DONE!\n");

  return had_mismatch ? 1 : 0;
}
