/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#include <time.h>

#include <iostream>
#include <string>
#include <string_view>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include "aw-backtrace/aw-backtrace.h"
#include "backtrace-comparer.h"

// Just some lua code "deep" enough to exercise some lua compiler
// code. Specifically the parts that seem to have largest FDEs.
std::string_view g_code = R"(
local x, y, z, t, g = 1, 2, 'a', {1}, _G
x = x + 1; x = x - 5; x = 5 - x; x = x * y; x = x / y
x = x << 2; x = 2 << x; x = x >> 3; x = x << -2
x = x & 0xFF; x = x | y; x = x ~ 0xAA
x = (x == 10); x = (x ~= y); x = (x < 20); x = (20 > x); x = (x >= y)
x = (x and y) or (y and x) or (x and 5)
z = z .. z .. 'b' .. z
)";

extern "C" {
// Make lua seeding be consistent (needs sysctl randomize_va_space to be 0)
time_t time(time_t* __timer) __THROW {
  time_t ret = 1785177006;
  if (__timer) {
    *__timer = ret;
  }
  return ret;
}
}

int main() {
  lua_State* L = luaL_newstate();
  if (!L) {
    std::cerr << "Failed to allocate Lua state.\n";
    return 1;
  }

#if __x86_64__
  StartBacktraceComparer();
#endif

  int status = luaL_loadbuffer(L, g_code.data(), g_code.size(), "=stress");
  if (status != LUA_OK) {
    std::cerr << "Compilation error: " << lua_tostring(L, -1) << "\n";
    return 1;
  }

  std::cout << "Success!\n";

  lua_close(L);
  return 0;
}
