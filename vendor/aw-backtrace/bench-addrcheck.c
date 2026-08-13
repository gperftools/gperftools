// SPDX-License-Identifier: 0BSD
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "aw-addrcheck.h"

int main() {
  aw_addrcheck_initialize();
  aw_addrcheck_set_disable_ioctl_for_test(true);

  static char thebuf[128 << 14];
  snprintf(thebuf, sizeof(thebuf), "/proc/%d/maps", getpid());
  clock_t before = clock();
  long count = 10 << 13;
  for (long i = count; i > 0; i--) {
    // addrcheck_session_t *s = addrcheck_open_file(thebuf);
    aw_addrcheck_session_t* s = aw_addrcheck_open(thebuf, sizeof(thebuf));
    aw_addrcheck_free(s);
  }
  clock_t after = clock();
  double secs = (after - before) / (double)CLOCKS_PER_SEC;
  printf("took: %g sec, %g nsec/iteration\n", secs, secs * 1e9 / count);
}
