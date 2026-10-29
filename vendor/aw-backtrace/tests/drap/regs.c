// SPDX-License-Identifier: 0BSD

// NOTE: build with -mforce-drap. Keeping fn and the first result
// live across the calls makes gcc save two callee-saved registers (%rbx and
// %r12) in the DRAP frame. %r12 gets pushed before %r10, so the CFA expression
// is breg6 -16 rather than -8, and both registers get DW_CFA_expression rules.
int drap_saved_regs(void (*fn)(int*)) {
  int aligned_array[16] __attribute__((aligned(4096)));
  fn(aligned_array);
  int first = aligned_array[0];
  fn(aligned_array);
  return first + aligned_array[0];
}
