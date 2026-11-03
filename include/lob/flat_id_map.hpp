#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "lob/types.hpp"

namespace lob {

// Open-addressing hash map from OrderId to a pointer, sized once and never
// resized. Linear probing keeps lookups to a few adjacent cache lines;
// deletion uses backward-shift instead of tombstones, so probe sequences
// stay short no matter how many cancels happen.
//
// Key 0 marks an empty slot, which is why OrderId 0 is invalid in the book.
// Capacity is at least 2x the maximum number of live keys, so the load
// factor never exceeds 0.5 and insert() cannot fail while the caller stays
// within max_keys.
template <class V>
class FlatIdMap {
 public:
  explicit FlatIdMap(std::size_t max_keys) {
    std::size_t cap = std::bit_ceil(max_keys * 2 < 16 ? std::size_t{16} : max_keys * 2);
    keys_.assign(cap, 0);
    vals_.assign(cap, V{});
    mask_ = cap - 1;
    max_keys_ = cap / 2;
  }

  V find(OrderId key) const noexcept {
    for (std::size_t i = slot(key);; i = (i + 1) & mask_) {
      if (keys_[i] == key) return vals_[i];
      if (keys_[i] == 0) return V{};
    }
  }

  bool contains(OrderId key) const noexcept { return find(key) != V{}; }

  // Precondition: key != 0 and key not present. Returns false if full.
  bool insert(OrderId key, V value) noexcept {
    if (size_ >= max_keys_) return false;
    std::size_t i = slot(key);
    while (keys_[i] != 0) i = (i + 1) & mask_;
    keys_[i] = key;
    vals_[i] = value;
    ++size_;
    return true;
  }

  bool erase(OrderId key) noexcept {
    std::size_t i = slot(key);
    while (keys_[i] != key) {
      if (keys_[i] == 0) return false;
      i = (i + 1) & mask_;
    }
    // Backward-shift deletion: walk the cluster after the hole and move back
    // any entry whose home slot is at or before the hole (cyclically), so
    // every remaining key is still reachable from its home slot.
    std::size_t j = i;
    for (;;) {
      j = (j + 1) & mask_;
      if (keys_[j] == 0) break;
      std::size_t home = slot(keys_[j]);
      if (((j - home) & mask_) >= ((j - i) & mask_)) {
        keys_[i] = keys_[j];
        vals_[i] = vals_[j];
        i = j;
      }
    }
    keys_[i] = 0;
    vals_[i] = V{};
    --size_;
    return true;
  }

  std::size_t size() const noexcept { return size_; }
  std::size_t capacity() const noexcept { return keys_.size(); }

 private:
  // splitmix64 finalizer: sequential ids spread evenly across slots.
  static std::uint64_t mix(std::uint64_t x) noexcept {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
  }
  std::size_t slot(OrderId key) const noexcept { return static_cast<std::size_t>(mix(key)) & mask_; }

  std::vector<OrderId> keys_;
  std::vector<V> vals_;
  std::size_t mask_ = 0;
  std::size_t size_ = 0;
  std::size_t max_keys_ = 0;
};

}  // namespace lob
