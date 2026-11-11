#pragma once
// Fixed-bucket latency histogram (preallocated, so recording never allocates).
// 10 ns buckets up to 1 ms; anything slower lands in an overflow bucket.

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace apex {

class LatencyHistogram {
 public:
  static constexpr std::uint64_t kBucketNs = 10;
  static constexpr std::size_t kBuckets = 100'000;  // covers 0 .. 1 ms

  LatencyHistogram() : counts_(kBuckets + 1, 0) {}

  void record(std::uint64_t ns) noexcept {
    std::size_t b = static_cast<std::size_t>(ns / kBucketNs);
    if (b > kBuckets) b = kBuckets;
    ++counts_[b];
    ++total_;
    if (ns > max_) max_ = ns;
    sum_ += ns;
  }

  // Upper edge of the bucket containing the p-th percentile (p in [0, 100]).
  std::uint64_t percentile(double p) const noexcept {
    if (total_ == 0) return 0;
    const auto target = static_cast<std::uint64_t>(p / 100.0 * static_cast<double>(total_ - 1)) + 1;
    std::uint64_t seen = 0;
    for (std::size_t b = 0; b <= kBuckets; ++b) {
      seen += counts_[b];
      if (seen >= target) return b == kBuckets ? max_ : (b + 1) * kBucketNs;
    }
    return max_;
  }

  double mean() const noexcept { return total_ ? static_cast<double>(sum_) / static_cast<double>(total_) : 0; }
  std::uint64_t max() const noexcept { return max_; }
  std::uint64_t count() const noexcept { return total_; }

  // CSV of non-empty buckets: bucket_start_ns,count (read by scripts/latency_report.py).
  bool write_csv(const std::string& path) const {
    std::ofstream f(path);
    if (!f) return false;
    f << "bucket_ns,count\n";
    for (std::size_t b = 0; b <= kBuckets; ++b)
      if (counts_[b]) f << b * kBucketNs << ',' << counts_[b] << '\n';
    return true;
  }

 private:
  std::vector<std::uint64_t> counts_;
  std::uint64_t total_ = 0;
  std::uint64_t max_ = 0;
  std::uint64_t sum_ = 0;
};

}  // namespace apex
