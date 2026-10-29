// SPDX-License-Identifier: 0BSD

// More gcc DRAP shapes. See doc/amd64-drap-problem.adoc. Build with
// -mforce-drap; without it gcc only uses DRAP when a realigned function also
// has alloca/VLAs, outgoing stack arguments, nonlocal gotos and so on.

// Six values live across calls fill all five callee-saved registers. r12-r15
// are pushed before %r10, so the CFA expression is breg6 -40 rather than -8.
int drap_many_regs(void (*fn)(int*)) {
  int aligned_array[16] __attribute__((aligned(4096)));
  fn(aligned_array);
  int r1 = aligned_array[0];
  fn(aligned_array);
  int r2 = aligned_array[0];
  fn(aligned_array);
  int r3 = aligned_array[0];
  fn(aligned_array);
  int r4 = aligned_array[0];
  fn(aligned_array);
  int r5 = aligned_array[0];
  fn(aligned_array);
  return r1 + r2 + r3 + r4 + r5 + aligned_array[0];
}

// A nested function gets the static chain in %r10, so gcc uses %r13 as the
// DRAP register: %r13 is pushed first and the CFA becomes r13+0.
int drap_r13_nested(void (*fn)(int*)) {
  int result = 0;
  __attribute__((noinline)) void inner(void) {
    int aligned_array[16] __attribute__((aligned(4096)));
    fn(aligned_array);
    result = aligned_array[0];
  }
  inner();
  return result;
}

// no_caller_saved_registers also makes gcc use %r13 for DRAP, and it saves
// %r10 and the other caller-saved registers with DW_CFA_expression rules.
__attribute__((no_caller_saved_registers, target("general-regs-only"))) int drap_r13_ncsr(void (*fn)(int*)) {
  int aligned_array[16] __attribute__((aligned(4096)));
  fn(aligned_array);
  return aligned_array[0];
}
