/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#ifndef AW_BACKTRACE_H_
#define AW_BACKTRACE_H_
#include <stdbool.h>
#include <stdint.h>

// NOTE: this is pre-release API. So neither API, nor ABI is
// guaranteed to be stable yet.

#if defined(__cplusplus)
extern "C" {
#endif

// Note, we get a little more things down to backtrace callback than
// other backtracing APIs. And we give actual values rather than some
// generic "context" object. In addition to the usual PC (address for
// the backtrace entry), we also give SP and PC_BEFORE_INSN.
//
// SP is stack pointer for the frame we're reporting. Most people
// don't need it, but some Google internal crash handlers print stack
// frame sizes. It is a handy thing to have in your crash log
// message. Because of that both Abseil and gperftools have backtrace
// API variants that supply those. So we support this too. Note, in
// some cases we won't provide actual SP and will give 0. E.g. when
// backtracing via shadow stack. Even when SP is non-0 it could be a
// guess. Don't count on it for anything beyond simple diagnostics.
//
// PC_BEFORE_INSN is the flag saying that PC points to the instruction
// about to be executed rather than return address from call site. For
// symbolizing backtraces you typically want to symbolize PC - 1 for
// call sites, so that function:file:lineno is for the call site, not
// the instruction just after the call. When PC_BEFORE_INSN is set PC
// is the exact address you want to symbolize. For "classic"
// backtraces (with NULL ucontext) PC_BEFORE_INSN is rare (only
// happens if your backtrace through signal frame).
//
// Callback should return true if we should continue backtracing. And
// false to stop.
typedef bool (*aw_backtrace_callback)(void* pc, void* sp, bool pc_before_insn, void* user_data);

// aw_backtrace collects backtrace (aka stacktrace) and invokes given
// CALLBACK with USER_DATA on each stack frame it discovers (starting
// from the most recent). ucontext can be passed to backtrace from the
// signal frame. If NULL ucontext pointer is passed, then we backtrace
// from the call-site.
//
// aw_backtrace is fully async-signal-safe (if your callback is).
void aw_backtrace_full(const void* uc, aw_backtrace_callback callback, void* user_data);

// aw_backtrace is a simplified backtrace API (vs aw_backtrace_full) that collects PC
// addresses into an array. Pass NULL as ucontext to get the same
// behavior as classic backtrace() API. Fully async-signal-safe as
// well.
//
// SKIP is the number of innermost frames to drop before filling
// result[]; pass 0 to keep all of them.
//
// Returns the number of entries of result[] that were filled (i.e. the
// count *after* skipping), never more than max_frames.
int aw_backtrace(const void* uc, void** result, int max_frames, int skip);

#if defined(__cplusplus)
}  // extern "C"
#endif

#if defined(__cplusplus)

#include <stddef.h>

// Declared, not defined, on purpose: LookupFrameInfo hands out a pointer to
// one, but a caller only needs the complete type if it wants to look inside,
// in which case it includes aw-structs.h itself.
namespace aw_backtrace_internal {
struct FrameInfo;
}

namespace aw_backtrace_ext {

class DebugExtensionV0 {
 public:
  // NOTE: DebugExtensionV0 is not part of stable ABI. A future
  // version is likely to retire V0 in favor of a V1, V2 etc, and
  // start returning nullptr here while remaining fully compatible, so
  // callers must handle nullptr. V0 is also not guaranteed to be
  // stable API (V1 and later will). Use only for various
  // optional/testing/diagnostic means.
  //
  // TryGet is not strictly speaking async-signal safe. First call
  // will construct the instance. Good enough for tests. In other
  // environments make sure to have first invocation from non-signal
  // context.
  static DebugExtensionV0* TryGet();

  struct DiagOptions {
    // Lets the library tell which fields a caller was compiled against.
    size_t struct_size = sizeof(DiagOptions);

    bool print_diagnostics = true;
    bool trap_diagnostics = false;
    bool disable_fastpath = false;
    bool disable_cache = false;
  };

  virtual void BacktraceExt(const void* uc, aw_backtrace_callback callback, void* user_data, const DiagOptions& options) = 0;

  // Looks up unwind info for lookup_ip and hands the decoded frame to
  // callback. Deliberately bypasses the cache. Returns false, without
  // invoking callback, if nothing could be found.
  virtual bool LookupFrameInfo(uintptr_t lookup_ip,
                               void (*callback)(const aw_backtrace_internal::FrameInfo* frame_info, void* user_data),
                               void* user_data) = 0;

  virtual void OverrideGlobalBacktracer(const DiagOptions* options) = 0;

  virtual void PrintStats() = 0;

 protected:
  // Instances are never owned by anyone except the library.
  virtual ~DebugExtensionV0();
};

}  // namespace aw_backtrace_ext

#endif  // __cplusplus

#endif  // AW_BACKTRACE_H_
