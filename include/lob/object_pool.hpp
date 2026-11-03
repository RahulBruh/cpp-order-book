#pragma once

#include <cstddef>
#include <memory>
#include <vector>

namespace lob {

// Fixed-capacity pool. All storage is allocated once in the constructor;
// acquire() and release() are O(1) and never touch the heap.
template <class T>
class ObjectPool {
 public:
  explicit ObjectPool(std::size_t capacity)
      : storage_(std::make_unique<T[]>(capacity)), capacity_(capacity) {
    free_.reserve(capacity);
    // Push in reverse so the first acquire() returns storage_[0]
    // (keeps early allocations contiguous, which is friendlier to the cache).
    for (std::size_t i = capacity; i-- > 0;) free_.push_back(&storage_[i]);
  }

  ObjectPool(const ObjectPool&) = delete;
  ObjectPool& operator=(const ObjectPool&) = delete;

  // Returns nullptr when exhausted.
  T* acquire() noexcept {
    if (free_.empty()) return nullptr;
    T* p = free_.back();
    free_.pop_back();
    return p;
  }

  void release(T* p) noexcept {
    *p = T{};
    free_.push_back(p);  // capacity was reserved up front, so no reallocation
  }

  std::size_t available() const noexcept { return free_.size(); }
  std::size_t in_use() const noexcept { return capacity_ - free_.size(); }
  std::size_t capacity() const noexcept { return capacity_; }

 private:
  std::unique_ptr<T[]> storage_;
  std::vector<T*> free_;
  std::size_t capacity_;
};

}  // namespace lob
