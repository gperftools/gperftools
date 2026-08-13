/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#ifndef PERF_CONVERT_INTERVAL_MAP_H_
#define PERF_CONVERT_INTERVAL_MAP_H_

#include <stdint.h>

#include <map>
#include <optional>
#include <utility>

namespace perf_convert {

// A map from disjoint half-open address ranges [start, end) to values, with
// mmap semantics: Insert() makes the new range authoritative, clipping or
// splitting whatever it overlaps. This is what a MMAP record does to a
// process address space -- a fresh mapping wins, and it can land in the
// middle of an existing one (which then survives as two pieces).
//
// Value V is copied. Ranges with start >= end are ignored.
template <typename V>
class IntervalMap {
 public:
  struct Entry {
    uint64_t end;
    V value;
  };
  using Map = std::map<uint64_t, Entry>;

  void Insert(uint64_t start, uint64_t end, V value) {
    if (start >= end) {
      return;
    }

    // The one entry that might start before `start` and reach into or across
    // the new range.
    auto it = map_.upper_bound(start);
    if (it != map_.begin()) {
      auto prev = std::prev(it);
      const uint64_t ps = prev->first;
      const uint64_t pe = prev->second.end;
      if (pe > start) {
        if (pe > end) {
          // prev spans the whole new range: keep its tail as a fresh entry.
          map_.emplace(end, Entry{pe, prev->second.value});
        }
        // Clip prev's tail back to `start` (or drop it if that empties it).
        if (ps < start) {
          prev->second.end = start;
        } else {
          map_.erase(prev);
        }
      }
    }

    // Entries starting within [start, end): erase if fully covered, otherwise
    // (they reach past `end`) re-key their surviving tail at `end`.
    for (auto e = map_.lower_bound(start); e != map_.end() && e->first < end;) {
      if (e->second.end <= end) {
        e = map_.erase(e);
      } else {
        Entry tail = e->second;
        e = map_.erase(e);
        map_.emplace(end, std::move(tail));
        break;  // that tail starts at `end`, nothing after it is in range
      }
    }

    map_.emplace(start, Entry{end, std::move(value)});
  }

  struct Hit {
    uint64_t start;
    uint64_t end;
    const V* value;
  };

  // The range containing `addr`, or nullopt.
  std::optional<Hit> Lookup(uint64_t addr) const {
    auto it = map_.upper_bound(addr);
    if (it == map_.begin()) {
      return std::nullopt;
    }
    --it;
    if (addr < it->first || addr >= it->second.end) {
      return std::nullopt;
    }
    return Hit{it->first, it->second.end, &it->second.value};
  }

  // The value whose range contains `addr`, or nullptr.
  const V* Find(uint64_t addr) const {
    auto h = Lookup(addr);
    return h ? h->value : nullptr;
  }

  void Clear() {
    map_.clear();
  }
  bool empty() const {
    return map_.empty();
  }
  size_t size() const {
    return map_.size();
  }

  // Iteration yields std::pair<const uint64_t start, Entry>, ordered by start.
  typename Map::const_iterator begin() const {
    return map_.begin();
  }
  typename Map::const_iterator end() const {
    return map_.end();
  }

 private:
  Map map_;
};

}  // namespace perf_convert

#endif  // PERF_CONVERT_INTERVAL_MAP_H_
