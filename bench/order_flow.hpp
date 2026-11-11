#pragma once
// Synthetic order flow for benchmarks. Generated up front so the benchmark
// times only the engine, not the random number generator.
//
// Mix (by message): 55% limit, 25% cancel, 5% modify, 10% market, 5% stop.
// Limit prices cluster around a slowly drifting mid, so some cross and trade
// and most rest near the top of the book, like a real market.

#include <cstdint>
#include <random>
#include <vector>

#include "apex/types.hpp"

namespace apex::bench {

inline std::vector<Request> make_flow(std::size_t n, std::uint64_t seed, Price levels) {
  std::mt19937_64 rng(seed);
  std::vector<Request> out;
  out.reserve(n);
  std::vector<OrderId> live;  // ids we may later cancel/modify (some will have filled)
  live.reserve(n);
  OrderId next_id = 1;
  Price mid = levels / 2;
  std::geometric_distribution<int> offset(0.25);  // mostly within a few ticks of mid

  auto pick_live = [&]() -> OrderId {
    const std::size_t i = rng() % live.size();
    const OrderId id = live[i];
    live[i] = live.back();
    live.pop_back();
    return id;
  };

  for (std::size_t i = 0; i < n; ++i) {
    if (rng() % 64 == 0) mid += (rng() % 2) ? 1 : -1;  // random-walk the mid
    if (mid < 100) mid = 100;
    if (mid > levels - 100) mid = levels - 100;

    const Side side = (rng() % 2) ? Side::Buy : Side::Sell;
    const Qty qty = 1 + static_cast<Qty>(rng() % 100);
    const int dice = static_cast<int>(rng() % 100);
    Request r{};
    r.side = side;
    r.qty = qty;
    r.stop_price = kNoPrice;
    r.price = kNoPrice;

    if (dice < 55 || live.empty()) {
      // Passive side of the mid, occasionally aggressive by a tick or two.
      const int off = offset(rng) - (rng() % 8 == 0 ? 2 : 0);
      r.action = Action::New;
      r.type = OrderType::Limit;
      r.price = side == Side::Buy ? mid - 1 - off : mid + 1 + off;
      r.id = next_id++;
      live.push_back(r.id);
    } else if (dice < 80) {
      r.action = Action::Cancel;
      r.id = pick_live();
    } else if (dice < 85) {
      r.action = Action::Modify;
      r.id = live[rng() % live.size()];
      r.price = side == Side::Buy ? mid - 1 - offset(rng) : mid + 1 + offset(rng);
    } else if (dice < 95) {
      r.action = Action::New;
      r.type = OrderType::Market;
      r.qty = 1 + static_cast<Qty>(rng() % 30);
      r.id = next_id++;
    } else {
      r.action = Action::New;
      r.type = OrderType::Stop;
      r.stop_price = side == Side::Buy ? mid + 3 + offset(rng) : mid - 3 - offset(rng);
      r.qty = 1 + static_cast<Qty>(rng() % 10);
      r.id = next_id++;
      live.push_back(r.id);
    }
    out.push_back(r);
  }
  return out;
}

}  // namespace apex::bench
