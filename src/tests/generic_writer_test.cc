// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
#include "config_for_unittests.h"

#include "base/generic_writer.h"

#include <stdio.h>
#define _USE_MATH_DEFINES
#include <math.h>

#include <memory>

#include "gtest/gtest.h"

using tcmalloc::GenericWriter;

constexpr int kLargeAmount = 128 << 10;

std::string expected_output = ([] () {
  std::string s = "Answer is 42\nPI is 3.141593\n";
  s = s + s;
  s.resize(kLargeAmount, 'X');
  return s;
})();

void PrintLargeAmount(GenericWriter* writer) {
  char initial[256];
  int rv = snprintf(initial, sizeof(initial), "Answer is %d\nPI is %.6f\n", 42, M_PI);
  EXPECT_LT(rv, sizeof(initial));

  writer->AppendF("Answer is %d\nPI is %.6f\n", 42, M_PI);
  writer->AppendStr(initial);

  int rest_amount = kLargeAmount - strlen(initial) * 2;

  std::unique_ptr<char[]> large_data{new char[rest_amount]};
  memset(large_data.get(), 'X', rest_amount);

  writer->AppendMem(large_data.get(), rest_amount);
}

TEST(GenericWriterTest, File) {
#ifndef _WIN32
  FILE* f = tmpfile();
  if (!f) {
    perror("tmpfile");
    abort();
  }

  {
    tcmalloc::RawFDGenericWriter<128> writer(static_cast<RawFD>(fileno(f)));
    PrintLargeAmount(&writer);
  }

  rewind(f);
  fseek(f, 0, SEEK_END);
  EXPECT_EQ(ftell(f), kLargeAmount);

  rewind(f);

  std::string s;
  s.resize(kLargeAmount);
  fread(&(s[0]), 1, kLargeAmount, f);

  EXPECT_EQ(s, expected_output);
#endif
}

TEST(GenericWriterTest, ChunkedWriting) {
  char* str = tcmalloc::WithWriterToStrDup(
    tcmalloc::ChunkedWriterConfig{malloc, free, 128},
    [] (GenericWriter* writer) {
      PrintLargeAmount(writer);
    });
  EXPECT_EQ(std::string(str), expected_output);
  free(str);
}

TEST(GenericWriterTest, String) {
  std::string s;
  {
    tcmalloc::StringGenericWriter writer(&s);
    PrintLargeAmount(&writer);
  }
  EXPECT_EQ(s, expected_output);
}
