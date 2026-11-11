#pragma once
// OrderId -> Order* hash map with open addressing and linear probing.
//
// std::unordered_map allocates a node per insert. This map preallocates a flat
// array (sized to keep the load factor <= 0.5), so inserts and erases never
// allocate, and probes walk adjacent memory instead of chasing pointers.
// Erase uses backward-shift deletion, so there are no tombstones to clean up.

#include <bit>
#include <cstdint>
#include <vector>

#include "types.hpp"

namespace apex {

class OrderMap {
  struct Entry {
    OrderId key = 0;
    Order* value = nullptr;  // nullptr marks an empty slot
  };

 public:
  explicit OrderMap(std::size_t max_entries)
      : slots_(std::bit_ceil(max_entries * 2 < 16 ? 16 : max_entries * 2)),
        mask_(slots_.size() - 1) {}

  Order* find(OrderId key) const noexcept {
    for (std::size_t i = home(key);; i = (i + 1) & mask_) {
      const Entry& e = slots_[i];
      if (!e.value) return nullptr;
      if (e.key == key) return e.value;
    }
  }

  // Returns false if the key is already present.
  bool insert(OrderId key, Order* value) noexcept {
    std::size_t i = home(key);
    for (;; i = (i + 1) & mask_) {
      if (!slots_[i].value) break;
      if (slots_[i].key == key) return false;
    }
    slots_[i] = Entry{key, value};
    ++size_;
    return true;
  }

  bool erase(OrderId key) noexcept {
    std::size_t i = home(key);
    for (;; i = (i + 1) & mask_) {
      if (!slots_[i].value) return false;
      if (slots_[i].key == key) break;
    }
    // Backward-shift: pull later entries of the same probe run into the hole
    // unless doing so would move them before their home slot.
    std::size_t j = i;
    for (;;) {
      j = (j + 1) & mask_;
      if (!slots_[j].value) break;
      const std::size_t k = home(slots_[j].key);
      const bool k_in_hole_to_j = (i <= j) ? (i < k && k <= j) : (i < k || k <= j);
      if (k_in_hole_to_j) continue;
      slots_[i] = slots_[j];
      i = j;
    }
    slots_[i] = Entry{};
    --size_;
    return true;
  }

  std::size_t size() const noexcept { return size_; }

 private:
  // splitmix64 finalizer: spreads sequential ids evenly across slots.
  std::size_t home(OrderId k) const noexcept {
    k ^= k >> 30;
    k *= 0xbf58476d1ce4e5b9ULL;
    k ^= k >> 27;
    k *= 0x94d049bb133111ebULL;
    k ^= k >> 31;
    return static_cast<std::size_t>(k) & mask_;
  }

  std::vector<Entry> slots_;
  std::size_t mask_;
  std::size_t size_ = 0;
};

}  // namespace apex
