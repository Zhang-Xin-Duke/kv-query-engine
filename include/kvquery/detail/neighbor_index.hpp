// Adapted from supplied immutable_neighbor_hash_table.h; see docs/PROVENANCE.md.
#pragma once
#include <algorithm>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace kvquery::detail {
inline void Prefetch(const void* p) noexcept {
#if defined(__GNUC__) || defined(__clang__)
  __builtin_prefetch(p, 0, 1);
#else
  (void)p;
#endif
}

class NeighborIndex {
 public:
  static constexpr uint64_t kOffsetBits = 36;
  static constexpr uint64_t kValueLenBits = 18;
  static constexpr uint64_t kOffsetMask = (1ULL << kOffsetBits) - 1;
  static constexpr uint32_t kValueLenSentinel = (1U << kValueLenBits) - 1;
  static constexpr uint64_t kPackedDataMask = (1ULL << 54) - 1;
  static constexpr int32_t kMaxChainOffset = 511;
  static constexpr int32_t kMinChainOffset = -512;
  struct Entry {
    uint64_t key, packed;
  };
  struct FlatNode {
    uint64_t key = 0;
    uint64_t packed_value = 0;
    bool IsEmpty() const noexcept { return packed_value == 0; }
    uint64_t ValueOffset() const noexcept { return packed_value & kOffsetMask; }
    uint32_t ValueLength() const noexcept {
      return static_cast<uint32_t>((packed_value >> 36) & kValueLenSentinel);
    }
    int ChainLink() const noexcept {
      const int raw = static_cast<int>(packed_value >> 54);
      return raw >= 512 ? raw - 1024 : raw;
    }
    void SetData(uint64_t k, uint64_t p) noexcept {
      key = k;
      packed_value = p & kPackedDataMask;
    }
    void SetChainLink(int32_t off) noexcept {
      // Encode in unsigned arithmetic: left-shifting a negative signed value is UB.
      packed_value = (packed_value & kPackedDataMask) | ((static_cast<uint64_t>(off) & 1023) << 54);
    }
  };
  static_assert(sizeof(FlatNode) == 16);
  struct Stats {
    uint64_t direct = 0, collisions = 0, relocations = 0;
  };

  NeighborIndex(std::span<const Entry> entries, uint32_t capacity, uint64_t seed = 0)
      : flat_nodes_(std::max(1U, capacity)), bucket_size_(flat_nodes_.size()), seed_(seed) {
    for (const auto& entry : entries) Insert_(entry);
  }
  uint32_t bucket(uint64_t key) const noexcept { return HashBucket_(key); }
  const FlatNode& node(uint32_t index) const noexcept { return flat_nodes_[index]; }
  uint32_t next(uint32_t index, int link) const noexcept {
    return WrapIndex_(static_cast<int64_t>(index) + link);
  }
  void prefetch(uint32_t index) const noexcept { Prefetch(&flat_nodes_[index]); }
  const FlatNode* find(uint64_t key) const noexcept {
    uint32_t pos = bucket(key);
    while (true) {
      const auto& n = node(pos);
      if (n.IsEmpty()) return nullptr;
      if (n.key == key) return &n;
      const int link = n.ChainLink();
      if (!link) return nullptr;
      pos = next(pos, link);
    }
  }
  uint64_t capacity() const noexcept { return bucket_size_; }
  Stats stats() const noexcept { return stats_; }

 private:
  uint32_t HashBucket_(uint64_t key) const noexcept {
    key ^= seed_;
    // Mix all 64 bits to avoid clustering when upper bits have low entropy
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdULL;
    key ^= key >> 33;
    key *= 0xc4ceb9fe1a85ec53ULL;
    key ^= key >> 33;
    return static_cast<uint32_t>(((__uint128_t)key * bucket_size_) >> 64);
  }

  uint32_t WrapIndex_(int64_t idx) const noexcept {
    const int64_t n = static_cast<int64_t>(bucket_size_);
    // A single add/subtract handles ordinary links without integer division.
    // Build-time neighborhood probing may wrap repeatedly for tiny snapshots.
    if (idx < 0)
      idx += n;
    else if (idx >= n)
      idx -= n;
    if (idx < 0 || idx >= n) idx = (idx % n + n) % n;
    return static_cast<uint32_t>(idx);
  }

  int32_t WrapOffset_(uint32_t from, uint32_t to) const noexcept {
    int64_t diff = static_cast<int64_t>(to) - from;
    const int64_t n = static_cast<int64_t>(bucket_size_);
    if (diff > n / 2) diff -= n;
    if (diff < -n / 2) diff += n;
    return static_cast<int32_t>(diff);
  }

  static constexpr int32_t kPrefetchDist = 4;

  // Find an empty slot reachable from BOTH `a` and `b` within chain offset range.
  // Used for splice insertions where the empty slot must link to two endpoints.
  // Wraps around the table edges.
  bool FindEmptyInRange_(uint32_t a, uint32_t b, uint32_t& out) const noexcept {
    for (int32_t dist = 1; dist <= kMaxChainOffset; ++dist) {
      if (dist + kPrefetchDist <= kMaxChainOffset) {
        Prefetch(&flat_nodes_[WrapIndex_(static_cast<int64_t>(a) + dist + kPrefetchDist)]);
      }
      uint32_t right = WrapIndex_(static_cast<int64_t>(a) + dist);
      int32_t b_to_right = WrapOffset_(b, right);
      if (b_to_right >= kMinChainOffset && b_to_right <= kMaxChainOffset &&
          flat_nodes_[right].IsEmpty()) {
        out = right;
        return true;
      }
      if (dist <= -kMinChainOffset) {
        if (dist + kPrefetchDist <= -kMinChainOffset) {
          Prefetch(&flat_nodes_[WrapIndex_(static_cast<int64_t>(a) - dist - kPrefetchDist)]);
        }
        uint32_t left = WrapIndex_(static_cast<int64_t>(a) - dist);
        int32_t b_to_left = WrapOffset_(b, left);
        if (b_to_left >= kMinChainOffset && b_to_left <= kMaxChainOffset &&
            flat_nodes_[left].IsEmpty()) {
          out = left;
          return true;
        }
      }
    }
    return false;
  }

  // The three-case insertion below follows the supplied implementation:
  // empty home; chain splice; relocate a lodger before claiming its slot.
  void Insert_(Entry e) {
    const uint32_t home = HashBucket_(e.key);
    if (flat_nodes_[home].IsEmpty()) {
      flat_nodes_[home].SetData(e.key, e.packed);
      ++stats_.direct;
    } else if (HashBucket_(flat_nodes_[home].key) == home) {
      uint32_t cur = home;
      for (;;) {
        if (flat_nodes_[cur].key == e.key) throw std::invalid_argument("duplicate key");
        const int link = flat_nodes_[cur].ChainLink();
        if (!link) break;
        cur = next(cur, link);
      }
      const int old_link = flat_nodes_[home].ChainLink();
      const uint32_t old_next = next(home, old_link);
      uint32_t empty;
      if (!FindEmptyInRange_(home, old_link ? old_next : home, empty))
        throw std::length_error("relative-link neighborhood exhausted");
      flat_nodes_[empty].SetData(e.key, e.packed);
      if (old_link) flat_nodes_[empty].SetChainLink(WrapOffset_(empty, old_next));
      flat_nodes_[home].SetChainLink(WrapOffset_(home, empty));
      ++stats_.collisions;
    } else {
      const uint32_t occupant_home = HashBucket_(flat_nodes_[home].key);
      uint32_t pre = occupant_home;
      while (next(pre, flat_nodes_[pre].ChainLink()) != home) {
        if (!flat_nodes_[pre].ChainLink()) throw std::logic_error("broken chain");
        pre = next(pre, flat_nodes_[pre].ChainLink());
      }
      const int lodger_link = flat_nodes_[home].ChainLink();
      const uint32_t lodger_next = lodger_link ? next(home, lodger_link) : pre;
      uint32_t empty;
      if (!FindEmptyInRange_(pre, lodger_next, empty))
        throw std::length_error("relative-link neighborhood exhausted");
      flat_nodes_[empty].SetData(flat_nodes_[home].key, flat_nodes_[home].packed_value);
      if (lodger_link) flat_nodes_[empty].SetChainLink(WrapOffset_(empty, lodger_next));
      flat_nodes_[pre].SetChainLink(WrapOffset_(pre, empty));
      flat_nodes_[home].SetData(e.key, e.packed);
      ++stats_.relocations;
    }
  }
  std::vector<FlatNode> flat_nodes_;
  uint64_t bucket_size_;
  uint64_t seed_;
  Stats stats_;
};
}  // namespace kvquery::detail
