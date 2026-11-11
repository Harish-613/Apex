#pragma once
// Threaded front end: a gateway thread pushes Requests into a lock-free SPSC
// ring; a dedicated matching thread pops them and runs the order book.
//
//   gateway thread --(SpscRing<Request>)--> matching thread --> OrderBook
//
// The matching thread owns the book exclusively, so the book needs no locks.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

#include "order_book.hpp"
#include "spsc_ring.hpp"
#include "types.hpp"

namespace apex {

inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__)
  asm volatile("yield");
#endif
}

inline std::uint64_t now_ns() noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

// Pin the calling thread to one core (Linux only; a no-op elsewhere).
inline void pin_to_cpu(int cpu) noexcept {
#if defined(__linux__)
  if (cpu < 0) return;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#else
  (void)cpu;
#endif
}

template <class Listener = NullListener>
class Engine {
 public:
  Engine(const BookConfig& cfg, std::size_t ring_capacity, Listener& listener)
      : ring_(ring_capacity), book_(cfg, listener) {}

  ~Engine() { stop(); }

  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  void start(int matcher_cpu = -1) {
    thread_ = std::thread([this, matcher_cpu] {
      pin_to_cpu(matcher_cpu);
      run();
    });
  }

  // Gateway side. Spins if the ring is full (backpressure).
  void submit(const Request& r) noexcept {
    while (!ring_.try_push(r)) cpu_relax();
  }

  // Sends a shutdown message and waits for the matcher to drain everything before it.
  void stop() {
    if (!thread_.joinable()) return;
    Request s{};
    s.action = Action::Shutdown;
    submit(s);
    thread_.join();
  }

  // Only safe to read once stop() has returned.
  OrderBook<Listener>& book() noexcept { return book_; }

 private:
  void run() {
    Request r;
    for (;;) {
      if (ring_.try_pop(r)) {
        if (r.action == Action::Shutdown) return;
        book_.submit(r);
      } else {
        cpu_relax();
      }
    }
  }

  SpscRing<Request> ring_;
  OrderBook<Listener> book_;
  std::thread thread_;
};

}  // namespace apex
