/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#ifndef CHECK_H_
#define CHECK_H_

#include <stdarg.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <span>

namespace aw_backtrace_internal {

struct CheckFail {
  static std::span<char> AllocCrashBuf() {
    static constexpr size_t kBufSize = 32 << 10;
    char* buf = (char*)mmap(0, kBufSize, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_NORESERVE | MAP_PRIVATE, -1, 0);
    return {buf, kBufSize};
  }
  [[noreturn]] static __attribute__((noinline)) void PrintfAndDie(const char* fmt, ...) {
    std::span<char> buf = AllocCrashBuf();
    va_list va;
    va_start(va, fmt);
    int written = vsnprintf(buf.data(), buf.size(), fmt, va);
    written = std::min<int>(std::max(written, 0), (int)buf.size());
    auto ignored_res = write(2, buf.data(), (size_t)written);
    (void)ignored_res;
    va_end(va);
    __builtin_trap();
  }
  static __attribute__((noinline)) void LogAndDie(const char* file, int line, const char* cond) {
    PrintfAndDie("Check fail at %s:%d: %s\n", file, line, cond);
  }
};

}  // namespace aw_backtrace_internal

#define CHECK(cond)                                                           \
  do {                                                                        \
    if (__builtin_expect(!(cond), 0)) {                                       \
      aw_backtrace_internal::CheckFail::LogAndDie(__FILE__, __LINE__, #cond); \
    }                                                                         \
  } while (false)

#endif  // CHECK_H_
