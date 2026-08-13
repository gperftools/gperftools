/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#ifndef UNWIND_INFO_CACHE_H_
#define UNWIND_INFO_CACHE_H_

#include <stdint.h>

#include <atomic>

#include "aw-structs.h"
#include "simple-counter.h"

// TODO: decide here. Sadly Intel did ship CPUs without AVX relatively
// recently. And AVX is kinda important-ish for efficient 128-bit
// atomic loads. On the other hand if someone is building with -mavx
// -mcx16 they'll get the right code at least with clang. So someone
// who cares don't need the stuff below. And everyone else will get
// the "best" fallback via libatomic (at the cost of calling there via
// PLT indirection). Maybe we should do our own runtime dispatch?
//
// Build with -march=x86-64-v3 (requires Haswell) or -mavx -mcx16
// (Sandy Bridge; except some recent-ish celeron models) to get the
// equivalence of the switches below.
//
// #if __x86_64__
// #define ATTR_DWORD_ATOMICS __attribute__((target("avx,cx16")))
// #endif

#ifndef ATTR_DWORD_ATOMICS
#define ATTR_DWORD_ATOMICS
#endif

namespace aw_backtrace_internal {

// Counters for the one cache we have. Bumping them costs an atomic RMW,
// so every bump is under the kUseStats template argument of the call
// that does it; see UnwindInfoCache::Bump. Read them via
// DebugExtensionV0::PrintStats().
struct UnwindInfoCacheStats {
  SimpleCounter lookups;
  SimpleCounter gets;
  SimpleCounter puts;
  SimpleCounter saves;
  SimpleCounter undos;
};

inline constinit UnwindInfoCacheStats g_unwind_info_cache_stats;

// Lock-free, fixed-size, bucketed cache of unwind info, with
// second-chance eviction. Everything about its shape is fixed: it caches
// CompressedFrameInfo keyed by exact instruction pointer, and there is
// exactly one of it (see g_frame_info_cache in aw-backtrace.cc, which
// wraps it with the compression and cacheability policy).
//
// There is no invalidation, so only addresses that can never be
// unloaded may be Put here. That is the wrapper's business, not ours.
//
// Key and value together are 128 bits, so an entry is one atomic
// object: it is read, replaced and CAS-ed as a unit and a value can
// never be seen next to a tag that doesn't belong to it. That is what
// pays for the shape of the code below -- there is no seqlock around
// the value, no locked bit, and no retry loop in Put.
//
// This does require a *lock-free* 16-byte atomic on the capture path,
// which is what ATTR_DWORD_ATOMICS above is for: given avx+cx16 clang
// emits vmovdqa for the loads and lock cmpxchg16b for the CAS/store,
// and needs no libatomic. gcc ignores it and routes every 16-byte
// atomic through libatomic (so gcc builds must link -latomic), whose
// x86-64 implementation resolves to cmpxchg16b at load time when the
// CPU has it. std::atomic<Entry>::is_always_lock_free is deliberately
// not static_asserted here, because it is false under gcc even when
// the generated code is lock-free.
class UnwindInfoCache {
 public:
  using Value = CompressedFrameInfo;

  constexpr UnwindInfoCache() = default;

  // Exact lookup: the entry's key must be equal to addr.
  template <bool kUseStats>
  bool Lookup(uintptr_t addr, Value* value_out);

  template <bool kUseStats>
  void Put(uintptr_t addr, const Value& value_in);

 private:
  // Total number of entries, how many of them share a bucket, and the
  // resulting table shape. The sizes must stay powers of two, and the
  // *Bits/*Width constants their base-2 logs; the static_asserts below
  // are the whole enforcement, since a non-template class can't call a
  // consteval helper of its own while it is still incomplete.
  static constexpr unsigned kCapacity = 1 << 12;
  static constexpr unsigned kBucketWidthBits = 3;
  static constexpr unsigned kBucketWidth = 1 << kBucketWidthBits;
  static constexpr unsigned kTableSize = kCapacity / kBucketWidth;
  static constexpr unsigned kTableWidth = 9;

  static_assert((kCapacity & (kCapacity - 1)) == 0);  // power of 2
  static_assert(kTableSize == (1u << kTableWidth));

  // Everything in the entry that isn't the tag: one bit saying the
  // entry holds anything at all, one saying it is next in line to be
  // evicted.
  static constexpr unsigned kFlagsWidth = 2;

  // The hash's top kTableWidth bits pick the bucket and need not be
  // stored -- the entry's position says what they were. Everything
  // MetaFields has left over after the flags goes to the tag, which is
  // more bits than the split needs; the ones at the seam are simply
  // stored twice. Index and tag between them still cover all 64 hash
  // bits, which is what makes a tag match an exact hit rather than a
  // probabilistic one. See Hash().
  static constexpr unsigned kIndexShift = 64 - kTableWidth;
  static constexpr unsigned kTagWidth = 64 - kFlagsWidth;
  static constexpr uint64_t kTagMask = (~uint64_t{0}) >> kFlagsWidth;

  // MetaFields must be exactly 64 bits with every bit spoken for: it
  // travels inside Entry through compare_exchange, which compares
  // bitwise, and an indeterminate padding bit would break that.
  static_assert(kTagWidth + kFlagsWidth == 64);
  static_assert(kTagWidth + kTableWidth >= 64);

  struct MetaFields {
    uint64_t in_use : 1;
    // Set on every other entry of the bucket by a Put, cleared by a
    // Lookup that hits. So it means "not used since the last insert
    // into this bucket", and it is what Put evicts first. See Put.
    uint64_t pending_eviction : 1;
    uint64_t tag : kTagWidth;  // the hash bits the bucket index doesn't carry

    bool IsInUse() const {
      return in_use != 0;
    }
  };
  static_assert(sizeof(MetaFields) == sizeof(uint64_t));

  // The unit of atomicity: key and value in one 128-bit word. Alignment
  // is spelled out because a 16-byte atomic must be 16-byte aligned to
  // be lock-free; the members alone would only ask for 8.
  struct alignas(16) Entry {
    MetaFields meta;
    Value value;
  };
  static_assert(sizeof(Entry) == sizeof(uint64_t) * 2);

  // Counter bumps compile away entirely unless the caller asked for stats.
  template <bool kUseStats>
  static void IncStat(SimpleCounter& counter) {
    if constexpr (kUseStats) {
      counter.Add();
    }
  }

  // This is "stolen" from gperftools' sampler.h
  // Returns the next prng value.
  // pRNG is: aX+b mod c with a = 0x5DEECE66D, b =  0xB, c = 1<<48
  // This is the lrand64 generator.
  static uint64_t NextRandom(uint64_t rnd) {
    const uint64_t prng_mult = 0x5DEECE66DULL;
    const uint64_t prng_add = 0xB;
    const uint64_t prng_mod_power = 48;
    const uint64_t prng_mod_mask = ~((~static_cast<uint64_t>(0)) << prng_mod_power);
    return (prng_mult * rnd + prng_add) & prng_mod_mask;
  }

  static uint32_t RollDice() {
    static constexpr uint64_t kSeed = 0x294dd1512e9ba234ULL;
    static __thread __attribute__((tls_model("initial-exec"))) uint64_t rng = kSeed;

    rng = NextRandom(rng);
    return static_cast<uint32_t>(rng >> 16);  // top 32 bits out of 48 bits of total rng state
  }

  template <bool kUseStats>
  static void ClearPendingEviction(std::atomic<Entry>* const bucket, uint32_t i, Entry entry);

  // Fibonacci hashing: one multiply by an odd constant (2^64 / phi).
  //
  // Multiplying by an odd number is a bijection on 64 bits, so the
  // bucket index and the stored tag together still identify the address
  // exactly -- a tag match is a real hit, never a probabilistic one. The
  // index has to come from the *top* bits: in a multiplicative hash the
  // low bits barely mix (bit 0 of the product is just bit 0 of the
  // address), while the top bits depend on the whole input.
  static constexpr uint64_t kHashMult = 0x9e3779b97f4a7c15ULL;

  static uint64_t Hash(uintptr_t addr) {
    return uint64_t{addr} * kHashMult;
  }

  static uint32_t BucketIndex(uint64_t hash) {
    return static_cast<uint32_t>(hash >> kIndexShift);
  }

  // Where in the bucket to start probing (and, for Put, which entry to
  // consider evicting first). Taken from just below the index bits,
  // because those are the well-mixed ones.
  static uint32_t ProbeStart(uint64_t hash) {
    return static_cast<uint32_t>(hash >> (kIndexShift - kBucketWidthBits)) % kBucketWidth;
  }

  std::atomic<Entry> entries_[kTableSize][kBucketWidth];
};

// All the loads and stores below are relaxed. Nothing is published
// alongside an entry -- the value *is* inside the atomic -- so there is
// no second location whose visibility an acquire/release pair would
// have to order. The two exceptions in Put are marked where they are.
template <bool kUseStats>
ATTR_DWORD_ATOMICS bool UnwindInfoCache::Lookup(uintptr_t addr, Value* value_out) {
  IncStat<kUseStats>(g_unwind_info_cache_stats.lookups);

  uint64_t hash = Hash(addr);
  uint64_t tag = hash & kTagMask;

  std::atomic<Entry>* const bucket = entries_[BucketIndex(hash)];

  for (uint32_t i = ProbeStart(hash), count = kBucketWidth; count > 0; count--, i = (i + 1) % kBucketWidth) {
    Entry entry = bucket[i].load(std::memory_order_relaxed);
    if (!entry.meta.IsInUse() || entry.meta.tag != tag) {
      continue;  // miss
    }

    // Cache hit, and there is nothing to re-validate: the value came
    // out of the same atomic load as the tag that matched, so it is
    // this address's value no matter what anyone else is doing to the
    // bucket. Whatever happens to the entry from here on can only cost
    // us the second chance we hand it below.
    *value_out = entry.value;

    ClearPendingEviction<kUseStats>(bucket, i, entry);

    IncStat<kUseStats>(g_unwind_info_cache_stats.gets);

    return true;
  }

  return false;
}

template <bool kUseStats>
ATTR_DWORD_ATOMICS void UnwindInfoCache::Put(uintptr_t addr, const Value& value_in) {
  IncStat<kUseStats>(g_unwind_info_cache_stats.puts);

  uint64_t hash = Hash(addr);
  uint64_t tag = hash & kTagMask;
  std::atomic<Entry>* const bucket = entries_[BucketIndex(hash)];
  uint32_t start_idx = ProbeStart(hash);

  // One pass, doing both jobs: if the address is already here we're
  // done, and otherwise we come out knowing which entry we'd rather
  // lose. Preference order is free, then marked for eviction (i.e. not
  // looked up since the last insert into this bucket), then -- if every
  // entry is in use and every one of them has been looked up since --
  // whichever the dice pick below.

  // How much Put wants to be rid of an entry, most wanted first.
  static constexpr uint32_t kRankFree = 0;
  static constexpr uint32_t kRankPending = 1;
  static constexpr uint32_t kRankOther = 2;

  uint32_t victim = kBucketWidth;  // kBucketWidth means "nothing preferable"
  uint32_t victim_rank = kRankOther;

  for (uint32_t k = 0; k < kBucketWidth; k++) {
    uint32_t i = (start_idx + k) % kBucketWidth;
    Entry entry = bucket[i].load(std::memory_order_relaxed);

    if (entry.meta.IsInUse() && entry.meta.tag == tag) {
      // There is already entry for the address and we assume it is
      // equivalent. Treat it as a use, like Lookup would.
      //
      // TODO: NOTE that the equivalence assumption might need to be
      // adjusted after we've implemented the cache invalidation.
      ClearPendingEviction<kUseStats>(bucket, i, entry);
      return;
    }

    uint32_t rank = !entry.meta.IsInUse() ? kRankFree : entry.meta.pending_eviction ? kRankPending : kRankOther;
    if (rank < victim_rank) {
      victim = i;
      victim_rank = rank;
    }
  }

  if (victim == kBucketWidth) {
    // Every entry is live and every one of them earned its keep since
    // the last insert. Nothing here deserves to go more than anything
    // else, so pick at random rather than always picking on the same
    // slot.
    victim = RollDice() % kBucketWidth;
  }

  Entry new_entry = {.meta =
                         {
                             .in_use = 1,
                             .pending_eviction = 0,
                             .tag = tag,
                         },
                     .value = value_in};

  // A plain store, with no claim on the entry first and nothing to
  // retry: an entry is never half-written, so there is no state a
  // racing reader could catch us in. The race we do lose this way is
  // with another Put -- we may drop its insert, or its bump -- and
  // dropping either is always allowed for a cache.
  bucket[victim].store(new_entry, std::memory_order_seq_cst);

  // One more pass over the rest of the bucket, doing two things.
  //
  // First, mark everything else for eviction. That is the clock hand:
  // an entry survives the next insert into this bucket only if some
  // Lookup clears the mark in the meantime. So "recently used" is
  // measured in inserts into this bucket, not in time, and the cost of
  // maintaining it falls on Put, which is already the slow path.
  //
  // Second, check nobody inserted the same address elsewhere in the
  // bucket while we were picking a victim. Duplicates would be harmless
  // (same address, same value) but they waste an entry, so undo ours.
  //
  // The duplicate check is the one thing here that needs more than
  // relaxed: seq_cst on the store above and on the loads here is what
  // guarantees that of two racing inserters at least one sees the
  // other. Both may see each other and both undo, leaving the address
  // uncached until the next Put -- rarer, and cheaper, than a permanent
  // duplicate.
  for (uint32_t i = 0; i < kBucketWidth; i++) {
    if (i == victim) {
      continue;
    }
    Entry other = bucket[i].load(std::memory_order_seq_cst);
    if (!other.meta.IsInUse()) {
      continue;
    }

    if (other.meta.tag == tag) {
      // Only undo an insert that is still ours: a third thread may have
      // evicted it already, and clobbering its entry would be wrong.
      Entry empty = {};
      if (bucket[victim].compare_exchange_strong(new_entry, empty, std::memory_order_relaxed,
                                                 std::memory_order_relaxed)) {
        IncStat<kUseStats>(g_unwind_info_cache_stats.undos);
      }
      return;
    }

    if (other.meta.pending_eviction) {
      continue;  // already marked
    }

    Entry marked = other;
    marked.meta.pending_eviction = 1;
    // One attempt, no loop. A lost CAS means the entry was just
    // evicted or just used, and in either case marking it is no longer
    // the right answer.
    bucket[i].compare_exchange_strong(other, marked, std::memory_order_relaxed, std::memory_order_relaxed);
  }
}

// The lookup path's only write, and it happens only when this entry has
// been marked since the last time it was used -- i.e. at most once per
// insert into its bucket, rather than on some fraction of all hits.
template <bool kUseStats>
ATTR_DWORD_ATOMICS void UnwindInfoCache::ClearPendingEviction(std::atomic<Entry>* const bucket, uint32_t i,
                                                              Entry entry) {
  if (!entry.meta.pending_eviction) {
    return;
  }

  Entry saved = entry;
  saved.meta.pending_eviction = 0;

  // A lost CAS means somebody else got to this entry first -- saved it
  // themselves, or evicted it. Neither is worth retrying, and the
  // caller doesn't care either way: it has its value already.
  if (bucket[i].compare_exchange_strong(entry, saved, std::memory_order_relaxed, std::memory_order_relaxed)) {
    IncStat<kUseStats>(g_unwind_info_cache_stats.saves);
  }
}

}  // namespace aw_backtrace_internal

#endif  // UNWIND_INFO_CACHE_H_
