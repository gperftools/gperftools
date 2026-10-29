// SPDX-License-Identifier: 0BSD

// NOTE: build with -mforce-drap. The 4096-byte alignment makes gcc emit
// .cfi_restore for %rbp in the epilogue; with 64 bytes or less it doesn't
// (see minimal_drap_2 in tests/amd64-drap-test.cc).
int minimal_drap(void (*fn)(int*)) {
  int aligned_array[16] __attribute__((aligned(4096)));
  fn(aligned_array);
  return aligned_array[0];
}
