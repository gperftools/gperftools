/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#include "renamings.h"
//
#include "aw-backtrace/aw-backtrace.h"
//
#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <ucontext.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "aw-addrcheck.h"
#include "aw-arch.h"
#include "aw-backtrace-fastpath.h"
#include "aw-structs.h"
#include "backtrace-core.h"
#include "backtrace-drap.h"
#include "check.h"
#include "function_ref.h"
#include "simple-counter.h"
#include "static_storage.h"
#include "unwind-info-cache.h"
#include "utils.h"

using aw_backtrace_internal::Cursor;
using aw_backtrace_internal::FrameInfo;
using aw_backtrace_internal::PrepareCursor;

using aw_backtrace_ext::DebugExtensionV0;

namespace aw_backtrace_internal {

int dl_iterate_phdr_with_fn(aw_backtrace_internal::FunctionRef<int(struct dl_phdr_info*, size_t)> body) {
  return dl_iterate_phdr(body.fn, body.data);
}

// V0 cacheabilty approach. Since we don't have reliable means to
// invalidate caches on .so object unloads, we only cache for the most
// common case of "initially loaded objects". We assume aw-backtrace
// is loaded by either being linked with initially loaded .so-s or
// LD_PRELOAD. Its constructor runs dl_iterate_phdr and finds all
// initially loaded objects. Those are then segments of addresses that
// we're willing to put in our caches.

struct LoadedSetHolder {
  bool initialized;
  aw_backtrace_internal::StaticStorage<std::vector<std::pair<uintptr_t, uintptr_t>>> vec_storage;

  std::optional<std::span<const std::pair<uintptr_t, uintptr_t>>> loaded_set() const {
    if (!initialized) {
      return std::nullopt;
    }
    return {std::span<const std::pair<uintptr_t, uintptr_t>>(*vec_storage.get())};
  }

  bool IsCacheableAddr(uintptr_t addr) const {
    std::optional<std::span<const std::pair<uintptr_t, uintptr_t>>> maybe_loaded = loaded_set();
    if (!maybe_loaded) {
      return false;
    }
    auto it = std::upper_bound(maybe_loaded->begin(), maybe_loaded->end(), addr,
                               [](uintptr_t addr, const auto& pair) -> bool { return addr < pair.second; });
    return it != maybe_loaded->end() && it->first <= addr;
  }

  void Initialize() {
    CHECK(!initialized);

    std::vector<std::pair<uintptr_t, uintptr_t>> loaded_set;
    dl_iterate_phdr_with_fn([&](struct dl_phdr_info* info, size_t) -> int {
      // We inspect all PT_LOAD headers and compute union of their
      // load addresses. This interval is then appended to loaded_set.

      uintptr_t load_bias = info->dlpi_addr;
      uintptr_t map_start = ~uintptr_t{0};
      uintptr_t map_end = 0;
      std::span<const ElfW(Phdr)> all_phdrs{info->dlpi_phdr, (size_t)info->dlpi_phnum};
      for (const auto& phdr : all_phdrs) {
        if (phdr.p_type != PT_LOAD) {
          continue;
        }
        uintptr_t start = load_bias + phdr.p_vaddr;
        uintptr_t end = start + phdr.p_memsz;
        map_start = std::min(map_start, start);
        map_end = std::max(map_end, end);
      }

      if (map_start < map_end) {
        loaded_set.emplace_back(map_start, map_end);
      }
      return 0;
    });

    std::sort(loaded_set.begin(), loaded_set.end(),
              [](const auto& pair_a, const auto& pair_b) -> bool { return pair_a.second < pair_b.second; });

    vec_storage.Construct(std::move(loaded_set));
    initialized = true;
  }
};
// Ensure that cacheable_addrs is in .bss and has no initializers
LoadedSetHolder cacheable_addrs;
static_assert(std::is_trivially_default_constructible_v<LoadedSetHolder>);
static_assert(std::is_trivially_destructible_v<LoadedSetHolder>);

void __attribute__((constructor)) initialize() {
  aw_addrcheck_initialize();
  cacheable_addrs.Initialize();
  aw_backtrace_ext::DebugExtensionV0::TryGet();  // construct the instance
}

class AddrChecker {
 public:
  AddrChecker() {
    s_ = aw_addrcheck_open(small_buffer_, sizeof(small_buffer_));
  }
  std::optional<aw_addrcheck_entry> Lookup(uintptr_t addr) {
    std::optional<aw_addrcheck_entry> ret{std::in_place};
    int rv = aw_addrcheck_lookup(s_, addr, &*ret);
    if (!rv) {
      ret.reset();
    }
    return ret;
  }
  ~AddrChecker() {
    aw_addrcheck_free(s_);
  }

  std::pair<uintptr_t, uintptr_t> ExecutableBoundsFor(uintptr_t addr) {
    auto maybe_vma = Lookup(addr);
    if (!maybe_vma || !maybe_vma->perm_exec) {
      return {};
    }
    return {maybe_vma->start, maybe_vma->end};
  }

 private:
  aw_addrcheck_session_t* s_;
  char small_buffer_[128];
};

class LazyAddrChecker {
 public:
  std::optional<aw_addrcheck_entry> Lookup(uintptr_t addr) {
    return get()->Lookup(addr);
  }
  std::pair<uintptr_t, uintptr_t> ExecutableBoundsFor(uintptr_t addr) {
    return get()->ExecutableBoundsFor(addr);
  }

 private:
  AddrChecker* get() {
    if (!checker_.has_value()) {
      checker_.emplace();
    }
    return &checker_.value();
  }
  std::optional<AddrChecker> checker_;
};

class StackAccess {
 public:
  explicit StackAccess(LazyAddrChecker* checker, uintptr_t stack_pointer) : checker_{checker} {
#if AW_DISABLE_STACK_CHECKS
    low_ = high_ = 0;
#else
    std::tie(low_, high_) = ReadLowHighTLS();
    if (low_ != 0) {
      return;
    }

    std::tie(low_, high_) = DiscoverBounds(checker, stack_pointer);
#endif
  }

  ALWAYS_INLINE bool TryReadPtr(uintptr_t at, uintptr_t* out_ptr) {
#if !AW_DISABLE_STACK_CHECKS
    uintptr_t end = at + sizeof(uintptr_t);

    if (end < at) {  // overflow
      return false;
    }

    if ((at & (sizeof(uintptr_t) - 1)) != 0) {
      return false;
    }

    if (at < low_ || end > high_) {
      // maybe we "jumped stack" i.e. traversed sigaltstack-ed signal frame
      std::tie(low_, high_) = DiscoverBounds(checker_, at);
      if (at < low_ || end > high_) {
        return false;
      }
    }

#endif
    *out_ptr = *reinterpret_cast<uintptr_t*>(at);
    return true;
  }

  // Try to read stack at the given address. Return 0 and put true
  // into *error if address is outside of stack area.
  uintptr_t ReadPointer(uintptr_t at, bool* error) {
#if !AW_DISABLE_STACK_CHECKS
    uintptr_t end = at + sizeof(uintptr_t);

    if (end < at) {  // overflow
      goto error;
    }

    if ((at & (sizeof(uintptr_t) - 1)) != 0) {
      goto error;
    }

    if (at < low_ || end > high_) {
      // maybe we "jumped stack" i.e. traversed sigaltstack-ed signal frame
      std::tie(low_, high_) = DiscoverBounds(checker_, at);
      if (at < low_ || end > high_) {
      error:
        if (error) {
          *error = true;
        }
        return 0;
      }
    }

#endif
    return *reinterpret_cast<uintptr_t*>(at);
  }

  std::optional<uintptr_t> ReadPtr(uintptr_t a) {
    uintptr_t value;
    if (!TryReadPtr(a, &value)) {
      return {};
    }
    return std::optional<uintptr_t>{value};
  }

 private:
  // thread_low_/thread_high_ cache the last-discovered stack vma for
  // this thread, so that repeated unwinds (and repeated ReadPointer
  // calls within one unwind) don't all pay for a fresh /proc/self/maps
  // style lookup. The cache is per-thread TLS, which is what makes it
  // tricky: it isn't only read and written by "this thread" in the
  // usual sense, it's read and written by whatever is running on this
  // thread's stack right now, which includes signal handlers that
  // preempted whoever was here before. E.g. the backtrace-comparer
  // facility single-steps by sigaltstack-ing on (essentially) every
  // instruction, so a write here can be interrupted, mid-update, by
  // another write for a *different* address range (the sigaltstack's
  // own vma).
  //
  // That's a classic seqlock situation, except for one twist: this is
  // strictly single-threaded reentrancy (signal handlers nesting on
  // one thread's call stack), not multiple threads on separate
  // cores. So instead of spinning, both sides simply drop out when
  // they'd otherwise have to wait.
  static inline __thread volatile uintptr_t thread_low_ __attribute__((tls_model("initial-exec")));
  static inline __thread volatile uintptr_t thread_high_ __attribute__((tls_model("initial-exec")));
  static inline __thread volatile unsigned thread_seq_ __attribute__((tls_model("initial-exec")));

  static std::pair<uintptr_t, uintptr_t> ReadLowHighTLS() {
    unsigned seq1 = thread_seq_;
    if (seq1 & 1) {
      // A write is in progress up our own call stack (we're a signal
      // handler that interrupted it). It won't finish until we
      // return, so there is nothing to wait for -- just report a miss.
      return {0, 0};
    }

    uintptr_t low = thread_low_;
    uintptr_t high = thread_high_;

    unsigned seq2 = thread_seq_;
    if (seq1 != seq2) {
      // Either a write started and finished entirely inside our read
      // window, or one is still in progress -- either way, low/high
      // above may be a torn mix of old and new. Report a miss rather
      // than try again: the writer that caused this is on our own
      // stack, so looping here wouldn't converge any faster.
      return {0, 0};
    }

    return {low, high};
  }

  static std::pair<uintptr_t, uintptr_t> DiscoverBounds(LazyAddrChecker* checker, uintptr_t ptr) {
    std::optional<aw_addrcheck_entry> maybe_vma = checker->Lookup(ptr);
    if (!maybe_vma || !maybe_vma->perm_read) {
      return {0, 0};  // nothing found, or not readable
    }

    uintptr_t low = maybe_vma->start;
    uintptr_t high = maybe_vma->end;

    unsigned seq = thread_seq_;
    if (seq & 1) {
      // We interrupted another DiscoverBounds call on this same
      // thread (it's mid-write).
      return {low, high};
    }

    thread_seq_ = seq + 1;  // odd: lock, write in progress
    thread_low_ = low;
    thread_high_ = high;
    thread_seq_ = seq + 2;  // even: unlock, publish

    return {low, high};
  }

  LazyAddrChecker* const checker_;
  uintptr_t low_, high_;
};

// The FrameInfo-level view of UnwindInfoCache: the compression and the
// "may this address be cached at all" policy live here, the storage and
// the eviction live in the cache itself. Counters are the cache's own
// (g_unwind_info_cache_stats); UseStats is what keeps their atomic RMWs
// out of the production build.
class FrameInfoCache {
 public:
  constexpr FrameInfoCache() = default;

  template <bool UseStats>
  bool Lookup(uintptr_t lookup_ip, FrameInfo* info) {
    CompressedFrameInfo cfi;
    if (!cache_.Lookup<UseStats>(lookup_ip, &cfi)) {
      return false;
    }
    *info = DecompressFrameInfo(cfi);
    return true;
  }

  template <bool UseStats>
  void Put(uintptr_t lookup_ip, FrameInfo* info) {
    if (!cacheable_addrs.IsCacheableAddr(lookup_ip)) {
      return;
    }
    std::optional<CompressedFrameInfo> maybe_cfi = CompressFrameInfo(*info);
    if (maybe_cfi.has_value()) {
      cache_.Put<UseStats>(lookup_ip, maybe_cfi.value());
    }
  }

 private:
  UnwindInfoCache cache_;
};

constinit FrameInfoCache g_frame_info_cache;

// The unwinding machinery is templated on a diagnostics policy. NoDiag is
// what aw_backtrace_full()/aw_backtrace() use: every knob folds to the
// compile-time constant it was before, so the generated code is what it
// always was. RuntimeDiag is what DebugExtensionV0::BacktraceExt uses, and
// consults the caller's DiagOptions instead. Both just hand back the
// DiagFlags DoUnwindLookup wants; neither does any reporting itself.
struct NoDiag {
#ifdef AW_BUMP_STATS_IN_PRODUCTION
  static inline constexpr bool kUseCacheStats = true;
#else
  static inline constexpr bool kUseCacheStats = false;
#endif

  static inline constexpr bool kNoDiag = true;

  DiagFlags flags() const {
    return {};
  }

  // The production path always takes the fast decoder and always
  // caches; both fold to compile-time constants here so the generated
  // code is exactly what it was. TESTING_NO_FASTPATH / TESTING_NO_CACHE
  // force the slow / uncached path for benchmarking (see recursion-test).
  bool use_fastpath() const {
#if __x86_64__ && !defined(TESTING_NO_FASTPATH)
    return true;
#else
    return false;
#endif
  }
  bool use_cache() const {
#ifndef TESTING_NO_CACHE
    return true;
#else
    return false;
#endif
  }
};

const DebugExtensionV0::DiagOptions* global_options_override;

struct RuntimeDiag {
  const DebugExtensionV0::DiagOptions* opts;

  static inline constexpr bool kUseCacheStats = true;
  static inline constexpr bool kNoDiag = false;

  DiagFlags flags() const {
    return {.report = opts->print_diagnostics, .trap = opts->trap_diagnostics};
  }

  DiagFlags flags_with_expression_diag() const {
    return {.report = opts->print_diagnostics, .trap = opts->trap_diagnostics, .report_expression_diag = true};
  }

  // Caller-controlled: disable_fastpath / disable_cache in DiagOptions
  // let the comparer (and anyone debugging) force the slow, uncached
  // walk and check it against the normal one.
  bool use_fastpath() const {
    return NoDiag{}.use_fastpath() && !opts->disable_fastpath;
  }
  bool use_cache() const {
    return !opts->disable_cache;
  }
};

static uintptr_t AddOffset(uintptr_t base, int32_t offset) {
  return base + static_cast<uintptr_t>(offset);  // sign-extend
}

template <typename Diag>
void UnwindLoop(Cursor cursor, const ucontext_t* in_uc, aw_backtrace_callback callback, void* user_data, Diag diag,
                bool skip_first_callback) {
  LazyAddrChecker checker;
  StackAccess acc{&checker, cursor.sp};

  const ucontext_t* next_uc = in_uc;
  while (true) {
    const ucontext_t* this_frame_uc = next_uc;
    const bool is_leaf = (this_frame_uc != nullptr);
    next_uc = nullptr;

    // A zero pc means two entirely different things depending on where it came
    // from. From an unwind step it is the end of the chain: the return address
    // slot held 0, which is how the outermost frame is set up. From a register
    // file -- the initial ucontext, or a sigcontext we stepped through -- it is
    // a live jump through a null function pointer. There the pc is genuinely 0
    // and worth reporting, and the stack is intact: `call *%rax` pushed the
    // return address before faulting on the fetch at 0, so sp points right at
    // it and Arch::GuessUnwindInfo below recovers the caller. Bailing out here
    // would throw away the whole backtrace in the case that needs it most.
    //
    // This is also what keeps lookup_pc's `- 1` below from underflowing.
    if (cursor.pc == 0 && !is_leaf) {
      break;
    }

    if (PREDICT_FALSE(skip_first_callback)) {
      skip_first_callback = false;
    } else {
      if (!callback(reinterpret_cast<void*>(cursor.pc), reinterpret_cast<void*>(cursor.sp), is_leaf, user_data)) {
        break;
      }
    }

    // Why -1? For non-leaf frames we're right "past" the call
    // site. In most cases finding unwind info at that location is
    // perfectly reasonable. But sometimes there is call site for the
    // call that never returns and compiler adds no instructions after
    // the call. This "address just pass the call" could even end up
    // in the another function entirely. So, to avoid such "bogus
    // lookups" we lookup ip - 1. Doing this is pretty common among
    // all backtracers.
    const uintptr_t lookup_pc = is_leaf ? cursor.pc : cursor.pc - 1;

    FrameInfo info;
    LookupOutcome outcome = LookupOutcome::kFail;

    if (is_leaf) {
      outcome = DoUnwindLookup(lookup_pc, &info, diag.flags());

      if (outcome == LookupOutcome::kOk) {
        goto have_info;
      }

    } else {  // !is_leaf
      const bool use_cache = diag.use_cache();
      if (use_cache && g_frame_info_cache.Lookup<Diag::kUseCacheStats>(lookup_pc, &info)) {
        goto have_info;
      }

      outcome = DoUnwindLookup(lookup_pc, &info, diag.flags());

      if (outcome == LookupOutcome::kOk) {
        if (use_cache) {
          g_frame_info_cache.Put<Diag::kUseCacheStats>(lookup_pc, &info);
        }
        goto have_info;
      }
    }

    {
      std::pair<uintptr_t, uintptr_t> pc_bounds = checker.ExecutableBoundsFor(lookup_pc);

      if (is_leaf && Arch::DetectPLTEntry(lookup_pc, &info, pc_bounds)) {
        // success! info is already filled-in
        goto have_info;
      }

      if (Arch::IsSignalFrame(cursor.pc, cursor.sp, pc_bounds, &next_uc)) {
        // Step through signal frame
        cursor = Arch::CursorFromContext(next_uc);
        continue;
      }

      if (outcome == LookupOutcome::kFailExpression) {
        // DRAP stuff is x86_64
#if __x86_64__
        auto acc_reader = FunctionRef<std::optional<uintptr_t>(uintptr_t)>{
            [](uintptr_t addr, void* data) { return static_cast<StackAccess*>(data)->ReadPtr(addr); }, &acc};
        if (auto maybe_cursor = TryLookupWithDRAP(lookup_pc, cursor, this_frame_uc, acc_reader, pc_bounds)) {
          cursor = maybe_cursor.value();
          continue;
        }
#endif  // __x86_64__

        // Recovery failed. Redo the lookup, this time letting the
        // expression diagnostic actually fire -- a second full decode, but
        // this is already the exceptional, off-the-fast-path case.
        if constexpr (!Diag::kNoDiag) {
          DoUnwindLookup(lookup_pc, &info, diag.flags_with_expression_diag());
        }
        break;
      }

      if (Arch::GuessUnwindInfo(cursor, this_frame_uc, &checker, &info)) {
        // info is good already
        goto have_info;
      }

      break;  // no way to step from current cursor
    }

  have_info:
    // Unwind step
    uintptr_t cfa = 0;
    uintptr_t new_ip = 0, new_fp = cursor.fp, new_sp = 0;
    switch (info.cfa.kind) {
      case CfaRule::Kind::SpRel:
        cfa = AddOffset(cursor.sp, info.cfa.offset);
        break;
      case CfaRule::Kind::FpRel:
        cfa = AddOffset(cursor.fp, info.cfa.offset);
        break;
      case CfaRule::Kind::RegRel:
        if (!is_leaf) {
          return;
        }
        cfa = AddOffset(Arch::GetDWARFReg(this_frame_uc, info.cfa.reg), info.cfa.offset);
        break;
      case CfaRule::Kind::DerefFpRel: {
        if (PREDICT_FALSE(!acc.TryReadPtr(AddOffset(cursor.fp, info.cfa.offset), &cfa))) {
          return;
        }
        break;
      }
      case CfaRule::Kind::Unsupported:
        return;
    }

    new_sp = cfa;

    // RA (PC)
    switch (info.ra.kind) {
      case RegisterRule::Kind::InReg:
        if (!is_leaf) {
          return;
        }
        new_ip = Arch::GetDWARFReg(this_frame_uc, info.ra.reg);
        break;
      case RegisterRule::Kind::MemCfaRel:
        if (!acc.TryReadPtr(AddOffset(cfa, info.ra.offset), &new_ip)) {
          new_ip = 0;
        }
        break;
      default:
        return;
    }

    // FP
    switch (info.fp.kind) {
      case RegisterRule::Kind::InReg:
        if (!is_leaf) {
          return;
        }
        new_fp = Arch::GetDWARFReg(this_frame_uc, info.fp.reg);
        break;
      case RegisterRule::Kind::MemCfaRel:
        if (!acc.TryReadPtr(AddOffset(cfa, info.fp.offset), &new_fp)) {
          return;
        }
        break;
      case RegisterRule::Kind::MemFpRel:
        if (!acc.TryReadPtr(AddOffset(cursor.fp, info.fp.offset), &new_fp)) {
          return;
        }
        break;
      case RegisterRule::Kind::SameValue:
        new_fp = cursor.fp;
        break;
      case RegisterRule::Kind::Unsupported:
      case RegisterRule::Kind::Undefined:
        return;
    }

    cursor.pc = Arch::CleanReturnAddress(new_ip);
    cursor.fp = new_fp;
    cursor.sp = new_sp;
  }  // while(true)
}

// Fast-path counters. Like the cache's own, the per-frame bumps are an
// atomic RMW each, so they are compiled out unless the Diag policy asks
// for stats (kUseCacheStats -- same AW_BUMP_STATS_IN_PRODUCTION switch).
// fast_path_fallbacks is off the hot path, so it stays unconditional.
constinit SimpleCounter fast_path_fallbacks;
constinit SimpleCounter fast_path_frames;
constinit SimpleCounter fast_path_cache_hits;

template <bool kUseStats>
static inline void BumpFastPathStat(SimpleCounter& counter) {
  if constexpr (kUseStats) {
    counter.Add();
  }
}

template <typename Diag>
void UnwindLoopFastPath(Cursor cursor, const ucontext_t* uc, aw_backtrace_callback callback, void* user_data,
                        Diag diag) {
  if (!diag.use_fastpath()) {
    return UnwindLoop<Diag>(cursor, uc, callback, user_data, diag, /*skip_first_callback=*/false);
  }

  LazyAddrChecker checker;
  StackAccess acc{&checker, cursor.sp};

  // Folds to a compile-time constant for the production (NoDiag) path.
  const bool use_cache = diag.use_cache();

  EHReaderInputs eh_frame_storage;
  EHReaderInputs* eh_frame = nullptr;
  FrameInfo info;

  while (true) {
    // Same policy as UnwindLoop: a zero pc ends the walk only when it came from
    // an unwind step. A leaf zero pc is a jump through a null function pointer,
    // which we report and then hand to UnwindLoop -- there is no .eh_frame for
    // pc 0, so LocateEHFrame below fails and we take the fallback anyway.
    if (PREDICT_FALSE(!cursor.pc) && uc == nullptr) {
      break;
    }

    if (!callback(reinterpret_cast<void*>(cursor.pc), reinterpret_cast<void*>(cursor.sp), uc != nullptr, user_data)) {
      return;
    }

    const uintptr_t lookup_pc = PREDICT_TRUE(uc == nullptr) ? cursor.pc - 1 : cursor.pc;

    // Same policy as UnwindLoop: cache non-leaf frames only (uc == nullptr
    // here means "pc is a return address", i.e. not leaf).
    bool cached =
        use_cache && PREDICT_TRUE(uc == nullptr) && g_frame_info_cache.Lookup<Diag::kUseCacheStats>(lookup_pc, &info);
    if (PREDICT_FALSE(!cached)) {
      if (PREDICT_FALSE(!(eh_frame && eh_frame->map_start <= lookup_pc && lookup_pc < eh_frame->map_end))) {
        eh_frame = LocateEHFrame(lookup_pc, &eh_frame_storage);
      }

      if (PREDICT_FALSE(!eh_frame))
        goto fallback;
      fastpath::FastPathFrame ff =
          TryFastFrameInfo(eh_frame->eh_frame_start, eh_frame->eh_frame_end, eh_frame->eh_frame_hdr, lookup_pc);
      if (PREDICT_FALSE(!ff.ToFrameInfo(&info)))
        goto fallback;

      BumpFastPathStat<Diag::kUseCacheStats>(fast_path_frames);

      if (use_cache && PREDICT_TRUE(uc == nullptr)) {
        g_frame_info_cache.Put<Diag::kUseCacheStats>(lookup_pc, &info);
      }
    } else {
      BumpFastPathStat<Diag::kUseCacheStats>(fast_path_cache_hits);
    }  // !cached

    {
      uintptr_t cfa;
      if (PREDICT_TRUE(info.cfa.kind == CfaRule::Kind::SpRel)) {
        cfa = AddOffset(cursor.sp, info.cfa.offset);
      } else {
        // fast-path only ever returns SpRel or FpRel, but cache may return something else
        if (info.cfa.kind != CfaRule::Kind::FpRel) {
          goto fallback;
        }
        cfa = AddOffset(cursor.fp, info.cfa.offset);
      }

      uintptr_t next_fp = cursor.fp;
      uintptr_t next_pc = 0;

      if (info.fp.kind == RegisterRule::Kind::MemCfaRel) {
        if (PREDICT_FALSE(!acc.TryReadPtr(AddOffset(cfa, info.fp.offset), &next_fp))) {
          break;
        }
      } else {
        if (info.fp.kind != RegisterRule::Kind::SameValue) {  // same as above
          goto fallback;
        }
        // no-op
      }
      if (info.ra.kind != RegisterRule::Kind::MemCfaRel) {
        if (info.ra.kind == RegisterRule::Kind::Undefined) {
          break;  // this case is specially considered as "end of chain"
        }
        goto fallback;
      }
      if (PREDICT_FALSE(!acc.TryReadPtr(AddOffset(cfa, info.ra.offset), &next_pc))) {
        break;
      }

      cursor.sp = cfa;
      cursor.fp = next_fp;
      cursor.pc = next_pc;
    }

    uc = nullptr;
  }  // while(true)

  return;

fallback:
  fast_path_fallbacks.Add();
  return UnwindLoop<Diag>(cursor, uc, callback, user_data, diag, /*skip_first_callback=*/true);
}

NEVER_INLINE
void DiagUnwindLoop(Cursor cursor, const ucontext_t* uc, aw_backtrace_callback callback, void* user_data,
                    const DebugExtensionV0::DiagOptions& options) {
  RuntimeDiag diag{&options};
  return UnwindLoopFastPath<RuntimeDiag>(cursor, uc, callback, user_data, diag);
}

}  // namespace aw_backtrace_internal

using aw_backtrace_internal::global_options_override;

NEVER_INLINE
void aw_backtrace_full(const void* _uc, aw_backtrace_callback callback, void* user_data) {
  const ucontext_t* uc = static_cast<const ucontext_t*>(_uc);
  Cursor cursor = PrepareCursor(uc, __builtin_frame_address(0));

  if (PREDICT_FALSE(global_options_override != nullptr)) {
    DiagUnwindLoop(cursor, uc, callback, user_data, *global_options_override);
    return;
  }

  aw_backtrace_internal::UnwindLoopFastPath(cursor, uc, callback, user_data, aw_backtrace_internal::NoDiag{});
}

NEVER_INLINE
int aw_backtrace(const void* _uc, void** result, int max_frames, int skip) {
  struct State {
    void** result;
    int max_frames;
    int skip;   // frames still to be dropped before we start filling result[]
    int count;  // entries of result[] filled so far

    static bool callback(void* ip, void*, bool, void* user_data) {
      struct State* state = static_cast<struct State*>(user_data);
      if (state->skip > 0) {
        state->skip--;
        return true;
      }
      if (state->count < state->max_frames) {
        state->result[state->count++] = ip;
        return true;
      }
      return false;
    }
  };

  if (PREDICT_FALSE(max_frames <= 0)) {
    return 0;
  }

  struct State state = {result, max_frames, skip > 0 ? skip : 0, 0};
  const ucontext_t* uc = static_cast<const ucontext_t*>(_uc);
  Cursor cursor = PrepareCursor(uc, __builtin_frame_address(0));

  if (PREDICT_FALSE(global_options_override != nullptr)) {
    DiagUnwindLoop(cursor, uc, State::callback, &state, *global_options_override);
    return state.count;
  }

  aw_backtrace_internal::UnwindLoopFastPath(cursor, uc, State::callback, &state, aw_backtrace_internal::NoDiag{});
  return state.count;
}

namespace aw_backtrace_ext {

// Out of line on purpose: being the first non-pure, non-inline virtual
// member makes this the key function, so the vtable and typeinfo are emitted
// here rather than in every translation unit that happens to need one. (They
// stay weak/COMDAT either way; what the key function decides is *where*.)
DebugExtensionV0::~DebugExtensionV0() = default;

namespace {

class DebugExtensionImpl final : public DebugExtensionV0 {
 public:
  constexpr DebugExtensionImpl() = default;

  // noinline for the same reason aw_backtrace() is: __builtin_frame_address(0)
  // has to name this frame, and within this TU the call is devirtualizable.
  NEVER_INLINE
  void BacktraceExt(const void* _uc, aw_backtrace_callback callback, void* user_data,
                    const DiagOptions& options) override {
    const ucontext_t* uc = static_cast<const ucontext_t*>(_uc);
    Cursor cursor = PrepareCursor(uc, __builtin_frame_address(0));
    DiagUnwindLoop(cursor, uc, callback, user_data, options);
  }

  bool LookupFrameInfo(uintptr_t lookup_ip,
                       void (*callback)(const aw_backtrace_internal::FrameInfo* frame_info, void* user_data),
                       void* user_data) override {
    FrameInfo info;
    // A fresh CFI walk for an exact pc, consulting no cache, reporting nothing.
    if (DoUnwindLookup(lookup_ip, &info, {}) != aw_backtrace_internal::LookupOutcome::kOk) {
      return false;
    }
    callback(&info, user_data);
    return true;
  }

  void OverrideGlobalBacktracer(const DiagOptions* options) override {
    global_options_override = options;
  }

  void PrintStats() override {
    using aw_backtrace_internal::SimpleCounter;
    const auto& stats = aw_backtrace_internal::g_unwind_info_cache_stats;
    auto p = [&](const SimpleCounter& a, const char* name) { printf("Counter %s: %zu\n", name, (size_t)a.Load()); };
#define P(n) p(stats.n, #n)
    P(lookups);
    P(gets);
    P(puts);
    P(saves);
    P(undos);
#undef P
    p(aw_backtrace_internal::fast_path_fallbacks, "fast_path_fallbacks");
    p(aw_backtrace_internal::fast_path_frames, "fast_path_frames");
    p(aw_backtrace_internal::fast_path_cache_hits, "fast_path_cache_hits");
  }
};

}  // namespace

DebugExtensionV0* DebugExtensionV0::TryGet() {
  static DebugExtensionV0* instance = ([]() {
    static aw_backtrace_internal::StaticStorage<DebugExtensionImpl> storage;
    storage.Construct();
    return storage.get();
  })();

  return instance;
}

}  // namespace aw_backtrace_ext
