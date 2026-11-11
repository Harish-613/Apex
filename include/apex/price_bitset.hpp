#pragma once
// Two-level bitset over price ticks: bit i is set when price level i is non-empty.
//
// When the best level empties, the book must find the next non-empty level.
// A plain array scan is O(range). Here a summary word marks which 64-bit words
// are non-zero, so a search touches at most a few words plus one
// count-leading/trailing-zeros instruction per level: one summary bit covers
// 4,096 ticks.

#include <bit>
#include <cstdint>
#include <vector>

namespace apex {

class PriceBitset {
 public:
  static constexpr std::int64_t npos = -1;

  explicit PriceBitset(std::size_t n)
      : n_(n), words_((n + 63) / 64, 0), summary_((words_.size() + 63) / 64, 0) {}

  void set(std::size_t i) noexcept {
    const std::size_t w = i >> 6;
    words_[w] |= bit(i);
    summary_[w >> 6] |= bit(w);
  }

  void clear(std::size_t i) noexcept {
    const std::size_t w = i >> 6;
    words_[w] &= ~bit(i);
    if (words_[w] == 0) summary_[w >> 6] &= ~bit(w);
  }

  bool test(std::size_t i) const noexcept { return (words_[i >> 6] & bit(i)) != 0; }

  // Lowest set index >= i, or npos.
  std::int64_t find_next(std::size_t i) const noexcept {
    if (i >= n_) return npos;
    std::size_t w = i >> 6;
    const std::uint64_t m = words_[w] & (~0ULL << (i & 63));
    if (m) return static_cast<std::int64_t>((w << 6) + std::countr_zero(m));
    const std::int64_t nw = next_word(w + 1);
    if (nw == npos) return npos;
    return (nw << 6) + std::countr_zero(words_[static_cast<std::size_t>(nw)]);
  }

  // Highest set index <= i, or npos.
  std::int64_t find_prev(std::size_t i) const noexcept {
    if (n_ == 0) return npos;
    if (i >= n_) i = n_ - 1;
    const std::size_t w = i >> 6;
    const std::uint64_t m = words_[w] & low_mask(i & 63);
    if (m) return static_cast<std::int64_t>((w << 6) + 63 - std::countl_zero(m));
    if (w == 0) return npos;
    const std::int64_t pw = prev_word(w - 1);
    if (pw == npos) return npos;
    return (pw << 6) + 63 - std::countl_zero(words_[static_cast<std::size_t>(pw)]);
  }

  std::size_t size() const noexcept { return n_; }

 private:
  static constexpr std::uint64_t bit(std::size_t i) noexcept { return 1ULL << (i & 63); }
  // Bits 0..b inclusive.
  static constexpr std::uint64_t low_mask(std::size_t b) noexcept {
    return b == 63 ? ~0ULL : ((1ULL << (b + 1)) - 1);
  }

  // First non-zero word index >= w.
  std::int64_t next_word(std::size_t w) const noexcept {
    if (w >= words_.size()) return npos;
    std::size_t s = w >> 6;
    std::uint64_t m = summary_[s] & (~0ULL << (w & 63));
    while (!m) {
      if (++s >= summary_.size()) return npos;
      m = summary_[s];
    }
    return static_cast<std::int64_t>((s << 6) + std::countr_zero(m));
  }

  // Last non-zero word index <= w.
  std::int64_t prev_word(std::size_t w) const noexcept {
    std::size_t s = w >> 6;
    std::uint64_t m = summary_[s] & low_mask(w & 63);
    while (!m) {
      if (s == 0) return npos;
      m = summary_[--s];
    }
    return static_cast<std::int64_t>((s << 6) + 63 - std::countl_zero(m));
  }

  std::size_t n_;
  std::vector<std::uint64_t> words_;
  std::vector<std::uint64_t> summary_;
};

}  // namespace apex
