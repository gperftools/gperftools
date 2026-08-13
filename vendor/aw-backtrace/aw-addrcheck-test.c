// SPDX-License-Identifier: 0BSD
#undef NDEBUG

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "aw-addrcheck.h"

static void test_self_maps(void) {
  printf("Testing aw_addrcheck_open(NULL, 0)...\n");
  aw_addrcheck_session_t* s = aw_addrcheck_open(NULL, 0);
  assert(s != NULL);

  struct aw_addrcheck_entry entry;
  /* Test lookup of a known stack address */
  int x = 42;
  uintptr_t addr = (uintptr_t)&x;

  printf("Looking up stack address %p...\n", (void*)addr);
  if (aw_addrcheck_lookup(s, addr, &entry)) {
    printf("  Found: %lx-%lx %d%d%d%d\n", entry.start, entry.end, entry.perm_read, entry.perm_write, entry.perm_exec,
           entry.perm_priv);
    assert(addr >= entry.start && addr < entry.end);
  } else {
    printf("  Error: stack address not found!\n");
    abort();
  }

  /* Test lookup of a known code address (this function) */
  addr = (uintptr_t)&test_self_maps;
  printf("Looking up code address %p...\n", (void*)addr);
  if (aw_addrcheck_lookup(s, addr, &entry)) {
    printf("  Found: %lx-%lx %d%d%d%d\n", entry.start, entry.end, entry.perm_read, entry.perm_write, entry.perm_exec,
           entry.perm_priv);
    assert(addr >= entry.start && addr < entry.end);
    assert(entry.perm_exec);
  } else {
    printf("  Error: code address not found!\n");
    abort();
  }

  /* Test invalid lookup */
  addr = 0x123;
  printf("Looking up invalid address 0x123...\n");
  if (aw_addrcheck_lookup(s, addr, &entry)) {
    printf("  Error: found mapping for 0x123?!\n");
    abort();
  } else {
    printf("  Correct: not found.\n");
  }

  aw_addrcheck_free(s);
  printf("Self maps test passed.\n\n");
}

static void test_user_buffer(void) {
  printf("Testing aw_addrcheck_open with user buffer...\n");
  size_t buf_size = 1080 + 32;
  void* buf = malloc(buf_size);
  assert(buf != NULL);

  aw_addrcheck_session_t* s = aw_addrcheck_open(buf, buf_size);
  assert(s != NULL);

  struct aw_addrcheck_entry entry;
  uintptr_t addr = (uintptr_t)&test_user_buffer;
  if (aw_addrcheck_lookup(s, addr, &entry)) {
    printf("  Found self in user buffer session.\n");
  } else {
    printf("  Error: self not found in user buffer session!\n");
    abort();
  }

  aw_addrcheck_free(s);
  free(buf);
  printf("User buffer test passed.\n\n");
}

static void test_synthetic_large_map(void) {
  printf("Testing synthetic large map (exercises mremap)...\n");

  /* Generate 10,000 mappings with some very long paths */
  FILE* f = tmpfile();
  assert(f != NULL);

  for (int i = 0; i < 10000; i++) {
    uintptr_t start = 0x100000000ULL + i * 0x1000;
    uintptr_t end = start + 0x1000;
    fprintf(f, "%08lx-%08lx r-xp %08x 00:00 0 ", start, end, i * 0x1000);
    if (i % 100 == 0) {
      /* Add a very long path (> 4K) occasionally */
      for (int j = 0; j < 500; j++) {
        fprintf(f, "/very/long/path/element/%d", j);
      }
    } else {
      fprintf(f, "/usr/lib/libtest_%d.so", i);
    }
    fprintf(f, "\n");
  }
  fflush(f);

  char tmp_path[128];
  snprintf(tmp_path, sizeof(tmp_path), "/proc/self/fd/%d", fileno(f));
  aw_addrcheck_session_t* s = aw_addrcheck_open_file(tmp_path);
  assert(s != NULL);

  struct aw_addrcheck_entry entry;
  /* Lookup the first, the middle, and the last entry */
  uintptr_t lookups[] = {0x100000000ULL, 0x100000000ULL + 5000 * 0x1000, 0x100000000ULL + 9999 * 0x1000};

  for (int i = 0; i < 3; i++) {
    if (aw_addrcheck_lookup(s, lookups[i], &entry)) {
      assert(entry.start == lookups[i]);
      assert(entry.perm_exec);
    } else {
      printf("  Error: synthetic lookup %d failed!\n", i);
      abort();
    }
  }

  aw_addrcheck_free(s);
  fclose(f);
  printf("Synthetic large map test passed.\n");
}

int main(void) {
  aw_addrcheck_set_disable_ioctl_for_test(true);

  test_self_maps();
  test_user_buffer();
  test_synthetic_large_map();
  printf("\nAll addrcheck tests passed!\n");
  return 0;
}
