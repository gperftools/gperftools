/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#ifndef UNWIND_INFO_CACHE_H_
#define UNWIND_INFO_CACHE_H_

#include <stdint.h>

#include <atomic>
#include <bit>

#include "aw-structs.h"
#include "simple-counter.h"

namespace aw_backtrace_internal {

// Counters for the one cache we have. Bumping them costs an atomic RMW,
// so every bump is under the kUseStats template argument of the call
// that does it; see UnwindInfoCache::IncStat. Read them via
// DebugExtensionV0::PrintStats().
struct UnwindInfoCacheStats {
  SimpleCounter lookups;
  SimpleCounter gets;
  SimpleCounter puts;
  SimpleCounter saves;
  // Puts that found their bucket locked by another writer and gave up.
  SimpleCounter dropped;
  // Lookups that hit, then saw a writer had been in the bucket, and
  // went round again.
  SimpleCounter retries;
};

inline constinit UnwindInfoCacheStats g_unwind_info_cache_stats;

// Fixed-size, bucketed cache of unwind info, with second-chance
// eviction. Everything about its shape is fixed: it caches
// CompressedFrameInfo keyed by exact instruction pointer, and there is
// exactly one of it (see g_frame_info_cache in aw-backtrace.cc, which
// wraps it with the compression and cacheability policy).
//
// There is no invalidation, so only addresses that can never be
// unloaded may be Put here. That is the wrapper's business, not ours.
//
// Concurrency is a seqlock per bucket, and no atomic wider than 64 bits.
// (It used to be one 16-byte atomic per entry, which needs no lock at
// all -- but libatomic on aarch64, not even an old one, implements
// those with a lock table, which is neither fast nor async-signal
// safe.)
//
// The lock is never waited on, by anyone, because a signal handler can
// interrupt a writer holding it and then land in here on the same
// thread:
//
//  * A writer try-locks the bucket and, if somebody else has it, just
//    drops the write. Losing a Put is always allowed for a cache.
//  * A reader that sees the bucket locked reports a miss. It only
//    retries when a writer came *and went* while it was reading, which
//    means that writer is making progress.
//
// The one write that doesn't take the lock is a Lookup clearing its
// entry's pending_eviction bit, an atomic RMW on that one bit. Readers
// never look at the bit, so it needs no seqlock; and a lost race over
// it costs at most one entry's second chance.
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

  // An entry's metadata word: two flag bits, then the tag. in_use says
  // the entry holds anything at all. pending_eviction is set on every
  // other entry of the bucket by a Put and cleared by a Lookup that
  // hits, so it means "not used since the last insert into this
  // bucket", and it is what Put evicts first. See Put.
  static constexpr unsigned kFlagsWidth = 2;
  static constexpr uint64_t kInUseBit = 1;
  static constexpr uint64_t kPendingEvictionBit = 2;

  // The hash's top kTableWidth bits pick the bucket and need not be
  // stored -- the entry's position says what they were. Everything the
  // metadata word has left over after the flags goes to the tag, which
  // is more bits than the split needs; the ones at the seam are simply
  // stored twice. Index and tag between them still cover all 64 hash
  // bits, which is what makes a tag match an exact hit rather than a
  // probabilistic one. See Hash().
  static constexpr unsigned kIndexShift = 64 - kTableWidth;
  static constexpr unsigned kTagWidth = 64 - kFlagsWidth;
  static constexpr uint64_t kTagMask = (~uint64_t{0}) >> kFlagsWidth;
  static_assert(kTagWidth + kTableWidth >= 64);

  // What an in-use entry for this tag has in its metadata word, give or
  // take kPendingEvictionBit.
  static uint64_t LiveMeta(uint64_t tag) {
    return (tag << kFlagsWidth) | kInUseBit;
  }

  static bool MatchesLive(uint64_t meta, uint64_t live_meta) {
    return (meta & ~kPendingEvictionBit) == live_meta;
  }

  static_assert(sizeof(Value) == sizeof(uint64_t));
  static_assert(std::has_unique_object_representations_v<Value>);

  // Both words are only ever written under the bucket's lock, except
  // for the pending_eviction bit (see the class comment). Value is
  // stored bit_cast to uint64_t.
  struct Entry {
    std::atomic<uint64_t> meta;
    std::atomic<uint64_t> value;
  };

  static_assert(sizeof(Entry) == 16);

  struct alignas(sizeof(Entry) * kBucketWidth) Bucket {
    Entry entries[kBucketWidth];
  };
  static_assert(sizeof(Bucket) == sizeof(Entry) * kBucketWidth);

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
  static void ClearPendingEviction(Entry* entry, uint64_t meta);

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

  // The seqs live in a table of their own rather than in Bucket, so
  // that a bucket stays a power of two in size and aligned to it, i.e.
  // exactly two cache lines. The price is a third line per lookup, and
  // sixteen buckets sharing each line of seqs -- so one bucket's writer
  // also makes its fifteen neighbors' readers miss that line. Writes
  // are rare (a Put follows a miss; hits never touch the seq), so that
  // is cheap.
  //
  // A seq is odd while a writer holds the bucket. Every unlock leaves it
  // 2 higher than it was at the lock, so a reader that sees the same
  // even value before and after has seen no writer at all.
  //
  // 32 bits, because that is the one width every target does natively,
  // 32-bit ones included. Wrapping is harmless: 2^32 is even, so parity
  // survives it, and a reader would have to sleep through 2^31 writes
  // to one bucket between its two loads to be fooled.
  alignas(64) std::atomic<uint32_t> seqs_[kTableSize];
  static_assert(std::atomic<uint32_t>::is_always_lock_free);
  Bucket buckets_[kTableSize];
};

// The reader half of the seqlock, in the usual shape: acquire-load seq,
// relaxed-load the data, acquire fence, relaxed-load seq again. Only a
// hit is validated -- a miss is always an acceptable answer, however
// torn the bucket was when we looked.
template <bool kUseStats>
bool UnwindInfoCache::Lookup(uintptr_t addr, Value* value_out) {
  IncStat<kUseStats>(g_unwind_info_cache_stats.lookups);

  uint64_t hash = Hash(addr);
  uint64_t live_meta = LiveMeta(hash & kTagMask);

  uint32_t index = BucketIndex(hash);
  std::atomic<uint32_t>& bucket_seq = seqs_[index];
  Bucket& bucket = buckets_[index];

  for (;;) {
    uint32_t seq = bucket_seq.load(std::memory_order_acquire);
    if (seq & 1) {
      // A writer is in there -- quite possibly the code this signal
      // handler interrupted -- so waiting for it could be waiting
      // forever.
      return false;
    }

    bool torn = false;
    for (uint32_t i = ProbeStart(hash), count = kBucketWidth; count > 0; count--, i = (i + 1) % kBucketWidth) {
      Entry& entry = bucket.entries[i];
      uint64_t meta = entry.meta.load(std::memory_order_relaxed);
      if (!MatchesLive(meta, live_meta)) {
        continue;  // miss
      }

      uint64_t value = entry.value.load(std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_acquire);
      if (bucket_seq.load(std::memory_order_relaxed) != seq) {
        torn = true;
        break;
      }

      *value_out = std::bit_cast<Value>(value);

      ClearPendingEviction<kUseStats>(&entry, meta);

      IncStat<kUseStats>(g_unwind_info_cache_stats.gets);

      return true;
    }

    if (!torn) {
      return false;
    }
    IncStat<kUseStats>(g_unwind_info_cache_stats.retries);
  }
}

template <bool kUseStats>
void UnwindInfoCache::Put(uintptr_t addr, const Value& value_in) {
  IncStat<kUseStats>(g_unwind_info_cache_stats.puts);

  uint64_t hash = Hash(addr);
  uint64_t live_meta = LiveMeta(hash & kTagMask);
  uint32_t index = BucketIndex(hash);
  std::atomic<uint32_t>& bucket_seq = seqs_[index];
  Bucket& bucket = buckets_[index];
  uint32_t start_idx = ProbeStart(hash);

  // Try-lock, never wait (see the class comment). The acquire pairs
  // with the previous writer's unlock, so we see its entries; the
  // release fence after it is the writer half of the seqlock: any
  // reader that sees one of our entry stores below also sees seq odd
  // on its re-check.
  uint32_t seq = bucket_seq.load(std::memory_order_relaxed);
  if ((seq & 1) ||
      !bucket_seq.compare_exchange_strong(seq, seq + 1, std::memory_order_acquire, std::memory_order_relaxed)) {
    IncStat<kUseStats>(g_unwind_info_cache_stats.dropped);
    return;
  }
  std::atomic_thread_fence(std::memory_order_release);

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
  bool present = false;

  for (uint32_t k = 0; k < kBucketWidth; k++) {
    uint32_t i = (start_idx + k) % kBucketWidth;
    uint64_t meta = bucket.entries[i].meta.load(std::memory_order_relaxed);

    if (MatchesLive(meta, live_meta)) {
      // There is already entry for the address and we assume it is
      // equivalent. Treat it as a use, like Lookup would.
      //
      // TODO: NOTE that the equivalence assumption might need to be
      // adjusted after we've implemented the cache invalidation.
      ClearPendingEviction<kUseStats>(&bucket.entries[i], meta);
      present = true;
      break;
    }

    uint32_t rank = !(meta & kInUseBit) ? kRankFree : (meta & kPendingEvictionBit) ? kRankPending : kRankOther;
    if (rank < victim_rank) {
      victim = i;
      victim_rank = rank;
    }
  }

  if (!present) {
    if (victim == kBucketWidth) {
      // Every entry is live and every one of them earned its keep since
      // the last insert. Nothing here deserves to go more than anything
      // else, so pick at random rather than always picking on the same
      // slot.
      victim = RollDice() % kBucketWidth;
    }

    Entry& entry = bucket.entries[victim];
    entry.value.store(std::bit_cast<uint64_t>(value_in), std::memory_order_relaxed);
    entry.meta.store(live_meta, std::memory_order_relaxed);

    // Mark everything else for eviction. That is the clock hand: an
    // entry survives the next insert into this bucket only if some
    // Lookup clears the mark in the meantime. So "recently used" is
    // measured in inserts into this bucket, not in time, and the cost
    // of maintaining it falls on Put, which is already the slow path.
    //
    // An RMW rather than a store, because a Lookup may be clearing the
    // same bit concurrently without the lock.
    for (uint32_t i = 0; i < kBucketWidth; i++) {
      if (i == victim) {
        continue;
      }
      std::atomic<uint64_t>& meta = bucket.entries[i].meta;
      uint64_t m = meta.load(std::memory_order_relaxed);
      if ((m & kInUseBit) && !(m & kPendingEvictionBit)) {
        meta.fetch_or(kPendingEvictionBit, std::memory_order_relaxed);
      }
    }
  }

  bucket_seq.store(seq + 2, std::memory_order_release);
}

// The lookup path's only write, and it happens only when this entry has
// been marked since the last time it was used -- i.e. at most once per
// insert into its bucket, rather than on some fraction of all hits.
//
// Deliberately not under the bucket lock, and not checked against the
// entry still being the one we matched: if it was replaced meanwhile,
// all we have done is hand a fresh entry a second chance it didn't
// need.
template <bool kUseStats>
void UnwindInfoCache::ClearPendingEviction(Entry* entry, uint64_t meta) {
  if (!(meta & kPendingEvictionBit)) {
    return;
  }

  uint64_t old = entry->meta.fetch_and(~kPendingEvictionBit, std::memory_order_relaxed);
  if (old & kPendingEvictionBit) {
    IncStat<kUseStats>(g_unwind_info_cache_stats.saves);
  }
}

}  // namespace aw_backtrace_internal

#endif  // UNWIND_INFO_CACHE_H_
