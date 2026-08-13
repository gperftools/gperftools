// SPDX-License-Identifier: 0BSD

// NOTE: build with -mforce-drap or older gcc
int minimal_drap(void (*fn)(int*)) {
  int aligned_array[16] __attribute__((aligned(4096)));
  fn(aligned_array);
  return aligned_array[0];
}
