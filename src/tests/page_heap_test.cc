// Copyright 2009 Google Inc. All Rights Reserved.
// Author: fikes@google.com (Andrew Fikes)

#include "config_for_unittests.h"
#include "page_heap.h"
#include "system-alloc.h"
#include <stdio.h>
#include "base/logging.h"
#include "common.h"

DECLARE_int64(tcmalloc_heap_limit_mb);

namespace {

static bool HaveSystemRelease =
    TCMalloc_SystemRelease(TCMalloc_SystemAlloc(kPageSize, NULL, 0), kPageSize);

static void CheckStats(const tcmalloc::PageHeap* ph,
                       uint64_t system_pages,
                       uint64_t free_pages,
                       uint64_t unmapped_pages) {
  tcmalloc::PageHeap::Stats stats = ph->stats();

  if (!HaveSystemRelease) {
    free_pages += unmapped_pages;
    unmapped_pages = 0;
  }

  EXPECT_EQ(system_pages, stats.system_bytes >> kPageShift);
  EXPECT_EQ(free_pages, stats.free_bytes >> kPageShift);
  EXPECT_EQ(unmapped_pages, stats.unmapped_bytes >> kPageShift);
}

static void TestPageHeap_Stats() {
  tcmalloc::PageHeap* ph = new tcmalloc::PageHeap();

  // Empty page heap
  CheckStats(ph, 0, 0, 0);

  // Allocate a span 's1'
  tcmalloc::Span* s1 = ph->New(256);
  CheckStats(ph, 256, 0, 0);

  // Split span 's1' into 's1', 's2'.  Delete 's2'
  tcmalloc::Span* s2 = ph->Split(s1, 128);
  Length s2_len = s2->length;
  ph->Delete(s2);
  CheckStats(ph, 256, 128, 0);

  // Unmap deleted span 's2'
  ph->ReleaseAtLeastNPages(1);
  CheckStats(ph, 256, 0, 128);

  // Delete span 's1'
  ph->Delete(s1);
  CheckStats(ph, 256, 128, 128);

  delete ph;
}

static void TestPageHeap_Limit() {
  tcmalloc::PageHeap* ph = new tcmalloc::PageHeap();

  CHECK_EQ(kMaxPages, 1 << (20 - kPageShift));

  // We do not know much is taken from the system for other purposes,
  // so we detect the proper limit:
  {
    FLAGS_tcmalloc_heap_limit_mb = 1;
    tcmalloc::Span* s = NULL;
    while((s = ph->New(kMaxPages)) == NULL) {
      FLAGS_tcmalloc_heap_limit_mb++;
    }
    FLAGS_tcmalloc_heap_limit_mb += 9;
    ph->Delete(s);
    // We are [10, 11) mb from the limit now.
  }

  // Test AllocLarge and GrowHeap first:
  {
    tcmalloc::Span * spans[10];
    for (int i=0; i<10; ++i) {
      spans[i] = ph->New(kMaxPages);
      EXPECT_NE(spans[i], NULL);
    }
    EXPECT_EQ(ph->New(kMaxPages), NULL);

    for (int i=0; i<10; i += 2) {
      ph->Delete(spans[i]);
    }

    tcmalloc::Span *defragmented = ph->New(5 * kMaxPages);

    if (HaveSystemRelease) {
      // EnsureLimit should release deleted normal spans
      EXPECT_NE(defragmented, NULL);
      EXPECT_TRUE(ph->CheckExpensive());
      ph->Delete(defragmented);
    }
    else
    {
      EXPECT_EQ(defragmented, NULL);
      EXPECT_TRUE(ph->CheckExpensive());
    }

    for (int i=1; i<10; i += 2) {
      ph->Delete(spans[i]);
    }
  }

  // Once again, testing small lists this time (twice smaller spans):
  {
    tcmalloc::Span * spans[20];
    for (int i=0; i<20; ++i) {
      spans[i] = ph->New(kMaxPages >> 1);
      EXPECT_NE(spans[i], NULL);
    }
    // one more half size allocation may be possible:
    tcmalloc::Span * lastHalf = ph->New(kMaxPages >> 1);
    EXPECT_EQ(ph->New(kMaxPages >> 1), NULL);

    for (int i=0; i<20; i += 2) {
      ph->Delete(spans[i]);
    }

    for(Length len = kMaxPages >> 2; len < 5 * kMaxPages; len = len << 1)
    {
      if(len <= kMaxPages >> 1 || HaveSystemRelease) {
        tcmalloc::Span *s = ph->New(len);
        EXPECT_NE(s, NULL);
        ph->Delete(s);
      }
    }

    EXPECT_TRUE(ph->CheckExpensive());

    for (int i=1; i<20; i += 2) {
      ph->Delete(spans[i]);
    }

    if (lastHalf != NULL) {
      ph->Delete(lastHalf);
    }
  }

  delete ph;
}

}  // namespace

int main(int argc, char **argv) {
  TestPageHeap_Stats();
  TestPageHeap_Limit();
  printf("PASS\n");
  return 0;
}
