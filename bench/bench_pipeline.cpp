// Two-thread pipeline benchmark: gateway thread -> SPSC ring -> matching thread.
//   1) saturated: push messages as fast as possible, report end-to-end throughput
//   2) paced: push at a fixed rate below capacity and measure each message's
//      latency from gateway enqueue to "matching finished", including the
//      cross-core hop through the ring.
//
// Usage: bench_pipeline [messages=5000000] [paced_rate_per_sec=200000]

#include <cstdio>
#include <cstdlib>

#include "apex/engine.hpp"
#include "apex/latency_histogram.hpp"
#include "order_flow.hpp"

using namespace apex;

namespace {
struct LatencyListener : NullListener {
  LatencyHistogram hist;
  bool measure = false;
  std::uint64_t processed = 0;
  void on_processed(const Request& r) {
    ++processed;
    if (measure) hist.record(now_ns() - r.ts_ns);
  }
};
}  // namespace

int main(int argc, char** argv) {
  const std::size_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 5'000'000;
  const double rate = argc > 2 ? std::strtod(argv[2], nullptr) : 200'000;
  const BookConfig cfg{1 << 16, 1 << 21};
  auto flow = bench::make_flow(n, 42, static_cast<Price>(cfg.price_levels));

  // ---- saturated throughput ----
  {
    LatencyListener l;
    Engine<LatencyListener> eng(cfg, 1 << 16, l);
    eng.start(1);
    pin_to_cpu(0);
    const auto t0 = now_ns();
    for (const auto& r : flow) eng.submit(r);
    eng.stop();  // returns once every message has been matched
    const auto t1 = now_ns();
    std::printf("Pipeline, saturated: %.2f M msgs/sec end-to-end (%llu processed)\n",
                static_cast<double>(n) * 1e9 / static_cast<double>(t1 - t0) / 1e6,
                static_cast<unsigned long long>(l.processed));
  }

  // ---- paced latency ----
  {
    LatencyListener l;
    l.measure = true;
    Engine<LatencyListener> eng(cfg, 1 << 16, l);
    eng.start(1);
    pin_to_cpu(0);
    const double gap_ns = 1e9 / rate;
    const std::size_t m = std::min<std::size_t>(n, static_cast<std::size_t>(rate * 10));  // ~10 s
    const auto start = now_ns();
    for (std::size_t i = 0; i < m; ++i) {
      const auto due = start + static_cast<std::uint64_t>(static_cast<double>(i) * gap_ns);
      while (now_ns() < due) cpu_relax();
      Request r = flow[i];
      r.ts_ns = now_ns();
      eng.submit(r);
    }
    eng.stop();
    const auto& h = l.hist;
    std::printf("Pipeline, paced at %.0f msgs/sec (%llu samples): gateway -> matched latency\n",
                rate, static_cast<unsigned long long>(h.count()));
    std::printf("  p50 %llu ns | p90 %llu ns | p99 %llu ns | p99.9 %llu ns | max %llu ns\n",
                static_cast<unsigned long long>(h.percentile(50)),
                static_cast<unsigned long long>(h.percentile(90)),
                static_cast<unsigned long long>(h.percentile(99)),
                static_cast<unsigned long long>(h.percentile(99.9)),
                static_cast<unsigned long long>(h.max()));
    h.write_csv("pipeline_latency.csv");
  }
  return 0;
}
