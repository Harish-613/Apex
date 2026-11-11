// Tests for the building blocks: memory pool, SPSC ring, price bitset, order map.

#include <random>
#include <set>
#include <thread>
#include <unordered_map>

#include "apex/memory_pool.hpp"
#include "apex/order_map.hpp"
#include "apex/price_bitset.hpp"
#include "apex/spsc_ring.hpp"
#include "test_framework.hpp"

using namespace apex;

TEST(pool_allocates_until_exhausted_then_reuses_lifo) {
  MemoryPool<Order> pool(3);
  Order* a = pool.allocate();
  Order* b = pool.allocate();
  Order* c = pool.allocate();
  CHECK(a && b && c);
  CHECK(pool.allocate() == nullptr);
  CHECK_EQ(pool.in_use(), 3u);
  pool.deallocate(b);
  CHECK(pool.allocate() == b);  // LIFO: most recently freed slot comes back first
  pool.deallocate(a);
  pool.deallocate(c);
  CHECK_EQ(pool.in_use(), 1u);
}

TEST(pool_returns_zeroed_objects) {
  MemoryPool<Order> pool(1);
  Order* o = pool.allocate();
  o->id = 42;
  o->qty = 7;
  pool.deallocate(o);
  o = pool.allocate();
  CHECK_EQ(o->id, 0u);
  CHECK_EQ(o->qty, 0u);
}

TEST(ring_rejects_non_power_of_two) {
  bool threw = false;
  try {
    SpscRing<int> r(6);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
}

TEST(ring_full_and_empty_single_thread) {
  SpscRing<int> r(4);
  int v = 0;
  CHECK(!r.try_pop(v));
  for (int i = 0; i < 4; ++i) CHECK(r.try_push(i));
  CHECK(!r.try_push(99));
  for (int i = 0; i < 4; ++i) {
    CHECK(r.try_pop(v));
    CHECK_EQ(v, i);
  }
  CHECK(!r.try_pop(v));
}

TEST(ring_two_threads_preserve_order) {
  constexpr int kN = 2'000'000;
  SpscRing<int> r(1024);
  std::thread producer([&] {
    for (int i = 0; i < kN; ++i)
      while (!r.try_push(i)) {
      }
  });
  int expected = 0;
  bool in_order = true;
  int v;
  while (expected < kN) {
    if (r.try_pop(v)) {
      if (v != expected) in_order = false;
      ++expected;
    }
  }
  producer.join();
  CHECK(in_order);
}

TEST(bitset_matches_brute_force) {
  std::mt19937_64 rng(7);
  for (std::size_t n : {1u, 63u, 64u, 65u, 5000u, 70000u}) {
    PriceBitset bits(n);
    std::set<std::size_t> ref;
    for (int step = 0; step < 4000; ++step) {
      const std::size_t i = rng() % n;
      if (rng() % 3 == 0) {
        bits.clear(i);
        ref.erase(i);
      } else {
        bits.set(i);
        ref.insert(i);
      }
      const std::size_t q = rng() % n;
      auto it = ref.lower_bound(q);
      const std::int64_t want_next = it == ref.end() ? PriceBitset::npos : static_cast<std::int64_t>(*it);
      auto jt = ref.upper_bound(q);
      const std::int64_t want_prev =
          jt == ref.begin() ? PriceBitset::npos : static_cast<std::int64_t>(*std::prev(jt));
      CHECK_EQ(bits.find_next(q), want_next);
      CHECK_EQ(bits.find_prev(q), want_prev);
      CHECK_EQ(bits.test(q), ref.count(q) == 1);
    }
  }
}

TEST(order_map_matches_unordered_map) {
  std::mt19937_64 rng(11);
  OrderMap map(1000);
  std::unordered_map<OrderId, Order*> ref;
  std::vector<Order> storage(5000);
  for (int step = 0; step < 200'000; ++step) {
    const OrderId k = rng() % 3000;
    const int op = static_cast<int>(rng() % 3);
    if (op == 0 && ref.size() < 1000) {
      Order* v = &storage[k];
      const bool inserted = map.insert(k, v);
      CHECK_EQ(inserted, ref.emplace(k, v).second);
    } else if (op == 1) {
      CHECK_EQ(map.erase(k), ref.erase(k) == 1);
    }
    auto it = ref.find(k);
    CHECK(map.find(k) == (it == ref.end() ? nullptr : it->second));
    CHECK_EQ(map.size(), ref.size());
  }
}
