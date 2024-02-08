// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
#include "config_for_unittests.h"

#include "base/generic_writer.h"

#include <stdio.h>
#define _USE_MATH_DEFINES 
#include <math.h>

#include <memory>

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
  CHECK_LT(rv, sizeof(initial));

  writer->AppendF("Answer is %d\nPI is %.6f\n", 42, M_PI);
  writer->AppendStr(initial);

  int rest_amount = kLargeAmount - strlen(initial) * 2;

  std::unique_ptr<char[]> large_data{new char[rest_amount]};
  memset(large_data.get(), 'X', rest_amount);

  writer->AppendMem(large_data.get(), rest_amount);
}

void TestFile() {
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
  CHECK_EQ(ftell(f), kLargeAmount);

  rewind(f);

  std::string s;
  s.resize(kLargeAmount);
  fread(&(s[0]), 1, kLargeAmount, f);

  CHECK_EQ(s, expected_output);

  printf("TestFile: PASS\n");
#endif
}

void TestChunkedWriting() {
  char* str = tcmalloc::WithWriterToStrDup(
    tcmalloc::ChunkedWriterConfig{malloc, free, 128},
    [] (GenericWriter* writer) {
      PrintLargeAmount(writer);
    });
  CHECK_EQ(std::string(str), expected_output);
  free(str);
  printf("TestChunkedWriting: PASS\n");
}

void TestString() {
  std::string s;
  {
    tcmalloc::StringGenericWriter writer(&s);
    PrintLargeAmount(&writer);
  }
  CHECK_EQ(s, expected_output);
  printf("TestString: PASS\n");
}

int main() {
  TestFile();
  TestChunkedWriting();
  TestString();
}
