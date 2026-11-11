#pragma once
// Lock-free single-producer / single-consumer ring buffer.
//
// One thread only pushes, one thread only pops, so no compare-and-swap is
// needed: each index has exactly one writer. Correctness comes from
// acquire/release ordering:
//   producer: write slot, then tail.store(release)  -> publishes the slot
//   consumer: tail.load(acquire), then read slot     -> sees the published slot
//
// Performance details:
//   * Capacity is a power of two, so `index & mask` replaces `index % capacity`.
//   * head and tail grow forever (no wraparound bookkeeping); 64-bit counters
//     will not overflow in practice.
//   * Producer-owned and consumer-owned fields sit on separate 64-byte cache
//     lines so the two cores do not invalidate each other's lines (false sharing).
//   * Each side caches the other side's index and only re-reads the shared
//     atomic when the cached value says the ring looks full/empty.

#include <atomic>
#include <cstddef>
#include <memory>
#include <stdexcept>

namespace apex {

inline constexpr std::size_t kCacheLine = 64;

template <class T>
class SpscRing {
 public:
  explicit SpscRing(std::size_t capacity)
      : capacity_(capacity), mask_(capacity - 1), buf_(std::make_unique<T[]>(capacity)) {
    if (capacity < 2 || (capacity & (capacity - 1)) != 0)
      throw std::invalid_argument("SpscRing capacity must be a power of two >= 2");
  }

  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;

  // Producer thread only.
  bool try_push(const T& v) noexcept {
    const std::size_t t = tail_.load(std::memory_order_relaxed);
    if (t - cached_head_ == capacity_) {
      cached_head_ = head_.load(std::memory_order_acquire);
      if (t - cached_head_ == capacity_) return false;  // full
    }
    buf_[t & mask_] = v;
    tail_.store(t + 1, std::memory_order_release);
    return true;
  }

  // Consumer thread only.
  bool try_pop(T& out) noexcept {
    const std::size_t h = head_.load(std::memory_order_relaxed);
    if (h == cached_tail_) {
      cached_tail_ = tail_.load(std::memory_order_acquire);
      if (h == cached_tail_) return false;  // empty
    }
    out = buf_[h & mask_];
    head_.store(h + 1, std::memory_order_release);
    return true;
  }

  std::size_t capacity() const noexcept { return capacity_; }

 private:
  const std::size_t capacity_;
  const std::size_t mask_;
  std::unique_ptr<T[]> buf_;

  // Producer's cache line.
  alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
  std::size_t cached_head_ = 0;

  // Consumer's cache line.
  alignas(kCacheLine) std::atomic<std::size_t> head_{0};
  std::size_t cached_tail_ = 0;

  char pad_[kCacheLine - sizeof(std::atomic<std::size_t>) - sizeof(std::size_t)];
};

}  // namespace apex
