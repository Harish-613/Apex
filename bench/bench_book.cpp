// Single-threaded order book benchmark.
//   1) throughput: run N messages back to back, report messages/sec
//   2) per-message latency: time each submit() call, report percentiles
//
// Usage: bench_book [messages=5000000]

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "apex/engine.hpp"
#include "apex/latency_histogram.hpp"
#include "apex/order_book.hpp"
#include "order_flow.hpp"

using namespace apex;

namespace {
struct CountingListener : NullListener {
  std::uint64_t trades = 0, rejects = 0;
  void on_trade(const Trade&) { ++trades; }
  void on_reject(OrderId, RejectReason) { ++rejects; }
};
}  // namespace

int main(int argc, char** argv) {
  const std::size_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 5'000'000;
  const BookConfig cfg{1 << 16, 1 << 21};
  pin_to_cpu(0);

  std::printf("Generating %zu messages...\n", n);
  const auto flow = bench::make_flow(n, 42, static_cast<Price>(cfg.price_levels));

  // ---- throughput ----
  double best_rate = 0;
  CountingListener stats;
  for (int run = 0; run < 3; ++run) {
    CountingListener l;
    OrderBook<CountingListener> book(cfg, l);
    const auto t0 = now_ns();
    for (const auto& r : flow) book.submit(r);
    const auto t1 = now_ns();
    const double rate = static_cast<double>(n) * 1e9 / static_cast<double>(t1 - t0);
    best_rate = std::max(best_rate, rate);
    stats = l;
    std::printf("  run %d: %.2f M msgs/sec  (%llu trades, %llu live orders at end)\n", run + 1,
                rate / 1e6, static_cast<unsigned long long>(l.trades),
                static_cast<unsigned long long>(book.live_orders()));
  }

  // ---- per-message latency ----
  CountingListener l;
  OrderBook<CountingListener> book(cfg, l);
  LatencyHistogram h;
  for (const auto& r : flow) {
    const auto t0 = now_ns();
    book.submit(r);
    h.record(now_ns() - t0);
  }
  // Clock overhead: two back-to-back now_ns() calls.
  LatencyHistogram clock_h;
  for (int i = 0; i < 1'000'000; ++i) {
    const auto a = now_ns();
    clock_h.record(now_ns() - a);
  }

  std::printf("\nSingle-thread book\n");
  std::printf("  throughput (best of 3): %.2f M msgs/sec\n", best_rate / 1e6);
  std::printf("  submit() latency: p50 %llu ns | p99 %llu ns | p99.9 %llu ns | mean %.0f ns\n",
              static_cast<unsigned long long>(h.percentile(50)),
              static_cast<unsigned long long>(h.percentile(99)),
              static_cast<unsigned long long>(h.percentile(99.9)), h.mean());
  std::printf("  (timer overhead included above: p50 %llu ns)\n",
              static_cast<unsigned long long>(clock_h.percentile(50)));
  std::printf("  trades per run: %llu, rejects (cancel of already-filled orders etc.): %llu\n",
              static_cast<unsigned long long>(stats.trades),
              static_cast<unsigned long long>(stats.rejects));
  h.write_csv("book_latency.csv");
  return 0;
}
