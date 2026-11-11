#pragma once
// Fixed-capacity object pool.
//
// All storage is allocated once in the constructor. allocate()/deallocate()
// just pop/push a LIFO free list, so the matching hot path never calls
// operator new or free(). LIFO reuse also means the most recently freed slot
// (likely still in cache) is handed out next.

#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>

namespace apex {

template <class T>
class MemoryPool {
  static_assert(std::is_trivially_destructible_v<T>,
                "MemoryPool is meant for plain data types");

  union Slot {
    Slot* next;
    T value;
    Slot() : next(nullptr) {}
  };

 public:
  explicit MemoryPool(std::size_t capacity)
      : capacity_(capacity), slots_(std::make_unique<Slot[]>(capacity)) {
    // Thread every slot onto the free list.
    for (std::size_t i = 0; i + 1 < capacity_; ++i) slots_[i].next = &slots_[i + 1];
    if (capacity_ > 0) slots_[capacity_ - 1].next = nullptr;
    free_ = capacity_ > 0 ? &slots_[0] : nullptr;
  }

  MemoryPool(const MemoryPool&) = delete;
  MemoryPool& operator=(const MemoryPool&) = delete;

  // Returns nullptr when the pool is exhausted.
  [[nodiscard]] T* allocate() noexcept {
    Slot* s = free_;
    if (!s) return nullptr;
    free_ = s->next;
    ++in_use_;
    return ::new (static_cast<void*>(&s->value)) T{};
  }

  void deallocate(T* p) noexcept {
    Slot* s = reinterpret_cast<Slot*>(p);
    s->next = free_;
    free_ = s;
    --in_use_;
  }

  std::size_t capacity() const noexcept { return capacity_; }
  std::size_t in_use() const noexcept { return in_use_; }

 private:
  std::size_t capacity_;
  std::unique_ptr<Slot[]> slots_;
  Slot* free_ = nullptr;
  std::size_t in_use_ = 0;
};

}  // namespace apex
