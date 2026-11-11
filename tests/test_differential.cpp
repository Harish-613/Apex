// Differential test: run random order flow through Apex and through a slow,
// obviously-correct reference book built on std::map/std::deque, and require
// identical trades, cancels, rejects and final book state.

#include <algorithm>
#include <deque>
#include <map>
#include <random>
#include <vector>

#include "apex/order_book.hpp"
#include "recording_listener.hpp"
#include "test_framework.hpp"

using namespace apex;
using namespace apex_test;

namespace {

class ReferenceBook {
 public:
  explicit ReferenceBook(Price levels) : levels_(levels) {}
  RecordingListener ev;

  void submit(const Request& r) {
    if (r.action == Action::New) add(r);
    else if (r.action == Action::Cancel) cancel(r.id);
    else if (r.action == Action::Modify) modify(r.id, r.price, r.qty);
  }

  std::map<Price, std::deque<Order>> bids, asks;  // bids: best is rbegin

  Price best_bid() const { return bids.empty() ? kNoPrice : bids.rbegin()->first; }
  Price best_ask() const { return asks.empty() ? kNoPrice : asks.begin()->first; }

 private:
  bool valid(Price p) const { return p >= 0 && p < levels_; }

  bool live(OrderId id) const {
    auto has = [&](const std::map<Price, std::deque<Order>>& side) {
      for (auto& [p, q] : side)
        for (auto& o : q)
          if (o.id == id) return true;
      return false;
    };
    for (auto& o : stops_) if (o.id == id) return true;
    return has(bids) || has(asks);
  }

  void add(const Request& r) {
    if (r.qty == 0) return ev.on_reject(r.id, RejectReason::InvalidQty);
    const bool np = r.type == OrderType::Limit || r.type == OrderType::StopLimit;
    const bool ns = r.type == OrderType::Stop || r.type == OrderType::StopLimit;
    if (np && !valid(r.price)) return ev.on_reject(r.id, RejectReason::InvalidPrice);
    if (ns && !valid(r.stop_price)) return ev.on_reject(r.id, RejectReason::InvalidPrice);
    if (live(r.id)) return ev.on_reject(r.id, RejectReason::DuplicateId);
    Order o{};
    o.id = r.id;
    o.side = r.side;
    o.type = r.type;
    o.price = np ? r.price : kNoPrice;
    o.stop_price = ns ? r.stop_price : kNoPrice;
    o.qty = r.qty;
    o.seq = ++seq_;
    ev.on_accept(o);
    if (o.type == OrderType::Limit) exec_limit(o);
    else if (o.type == OrderType::Market) exec_market(o);
    else {
      stops_.push_back(o);
      check_stops();
    }
    drain();
  }

  bool match(Order& t, bool is_market) {
    bool traded = false;
    auto& opp = t.side == Side::Buy ? asks : bids;
    while (t.qty > 0 && !opp.empty()) {
      auto it = t.side == Side::Buy ? opp.begin() : std::prev(opp.end());
      const Price px = it->first;
      if (!is_market && (t.side == Side::Buy ? px > t.price : px < t.price)) break;
      auto& q = it->second;
      while (t.qty > 0 && !q.empty()) {
        Order& m = q.front();
        const Qty fill = std::min(t.qty, m.qty);
        t.qty -= fill;
        m.qty -= fill;
        last_ = px;
        traded = true;
        ev.on_trade(Trade{t.id, m.id, px, fill, t.side});
        if (m.qty == 0) q.pop_front();
      }
      if (q.empty()) opp.erase(it);
    }
    return traded;
  }

  void exec_limit(Order o) {
    const bool traded = match(o, false);
    if (o.qty > 0) (o.side == Side::Buy ? bids : asks)[o.price].push_back(o);
    if (traded) check_stops();
  }

  void exec_market(Order o) {
    const bool traded = match(o, true);
    if (o.qty > 0) ev.on_cancel(o.id, o.qty);
    if (traded) check_stops();
  }

  void check_stops() {
    if (last_ == kNoPrice) return;
    std::vector<Order> buys, sells, keep;
    for (auto& o : stops_) {
      if (o.side == Side::Buy && o.stop_price <= last_) buys.push_back(o);
      else if (o.side == Side::Sell && o.stop_price >= last_) sells.push_back(o);
      else keep.push_back(o);
    }
    std::stable_sort(buys.begin(), buys.end(), [](auto& a, auto& b) {
      return a.stop_price != b.stop_price ? a.stop_price < b.stop_price : a.seq < b.seq;
    });
    std::stable_sort(sells.begin(), sells.end(), [](auto& a, auto& b) {
      return a.stop_price != b.stop_price ? a.stop_price > b.stop_price : a.seq < b.seq;
    });
    stops_ = keep;
    for (auto& o : buys) { ev.on_trigger(o.id); pending_.push_back(o); }
    for (auto& o : sells) { ev.on_trigger(o.id); pending_.push_back(o); }
  }

  void drain() {
    while (!pending_.empty()) {
      Order o = pending_.front();
      pending_.pop_front();
      if (o.type == OrderType::Stop) { o.type = OrderType::Market; exec_market(o); }
      else { o.type = OrderType::Limit; exec_limit(o); }
    }
  }

  // Find and erase a resting order; returns it.
  bool take_resting(OrderId id, Order& out) {
    for (auto* side : {&bids, &asks})
      for (auto it = side->begin(); it != side->end(); ++it)
        for (auto qi = it->second.begin(); qi != it->second.end(); ++qi)
          if (qi->id == id) {
            out = *qi;
            it->second.erase(qi);
            if (it->second.empty()) side->erase(it);
            return true;
          }
    return false;
  }

  Order* find_resting(OrderId id) {
    for (auto* side : {&bids, &asks})
      for (auto& [p, q] : *side)
        for (auto& o : q)
          if (o.id == id) return &o;
    return nullptr;
  }

  void cancel(OrderId id) {
    Order o{};
    if (take_resting(id, o)) return ev.on_cancel(id, o.qty);
    for (auto it = stops_.begin(); it != stops_.end(); ++it)
      if (it->id == id) {
        ev.on_cancel(id, it->qty);
        stops_.erase(it);
        return;
      }
    ev.on_reject(id, RejectReason::UnknownId);
  }

  void modify(OrderId id, Price px, Qty q) {
    Order* r = find_resting(id);
    if (!r) {
      for (auto& o : stops_)
        if (o.id == id) return ev.on_reject(id, RejectReason::NotModifiable);
      return ev.on_reject(id, RejectReason::UnknownId);
    }
    if (q == 0) return cancel(id);
    if (!valid(px)) return ev.on_reject(id, RejectReason::InvalidPrice);
    if (px == r->price && q <= r->qty) {
      r->qty = q;
      return ev.on_modify(id);
    }
    Order o{};
    take_resting(id, o);
    o.price = px;
    o.qty = q;
    o.seq = ++seq_;
    ev.on_modify(id);
    exec_limit(o);
    drain();
  }

  Price levels_;
  std::vector<Order> stops_;
  std::deque<Order> pending_;
  Price last_ = kNoPrice;
  std::uint64_t seq_ = 0;
};

void run_differential(std::uint64_t seed, int steps) {
  constexpr Price kLevels = 128;
  std::mt19937_64 rng(seed);
  RecordingListener ev;
  OrderBook<RecordingListener> book{BookConfig{kLevels, 1 << 14}, ev};
  ReferenceBook ref(kLevels);
  OrderId next_id = 1;

  for (int i = 0; i < steps; ++i) {
    Request r{};
    const int dice = static_cast<int>(rng() % 100);
    const Side side = rng() % 2 ? Side::Buy : Side::Sell;
    const Price px = 54 + static_cast<Price>(rng() % 20);  // narrow band => lots of crossing
    const Qty q = 1 + static_cast<Qty>(rng() % 20);
    const OrderId old = next_id > 1 ? 1 + rng() % (next_id - 1) : 1;
    if (dice < 50) r = limit(next_id++, side, px, q);
    else if (dice < 60) r = market(next_id++, side, q);
    else if (dice < 66) r = stop(next_id++, side, px, q);
    else if (dice < 70) r = stop_limit(next_id++, side, px, 54 + static_cast<Price>(rng() % 20), q);
    else if (dice < 88) r = cancel(old);
    else if (dice < 97) r = modify(old, px, q);
    else if (dice < 98) r = limit(old, side, px, q);           // duplicate id
    else if (dice < 99) r = limit(next_id++, side, 500, q);    // out of range
    else r = limit(next_id++, side, px, 0);                    // zero qty
    book.submit(r);
    ref.submit(r);
  }

  CHECK_EQ(ev.trades.size(), ref.ev.trades.size());
  for (std::size_t k = 0; k < ev.trades.size(); ++k) CHECK_EQ(ev.trades[k], ref.ev.trades[k]);
  CHECK(ev.cancels == ref.ev.cancels);
  CHECK(ev.rejects == ref.ev.rejects);
  CHECK(ev.triggers == ref.ev.triggers);
  CHECK(ev.modifies == ref.ev.modifies);
  CHECK_EQ(book.best_bid(), ref.best_bid());
  CHECK_EQ(book.best_ask(), ref.best_ask());
  for (Price p = 0; p < kLevels; ++p) {
    std::uint64_t rb = 0, ra = 0;
    if (auto it = ref.bids.find(p); it != ref.bids.end()) for (auto& o : it->second) rb += o.qty;
    if (auto it = ref.asks.find(p); it != ref.asks.end()) for (auto& o : it->second) ra += o.qty;
    CHECK_EQ(book.qty_at(Side::Buy, p), rb);
    CHECK_EQ(book.qty_at(Side::Sell, p), ra);
  }
  // Every event should be accounted for: trades happened and stops fired.
  CHECK(!ev.trades.empty());
  CHECK(!ev.triggers.empty());
}

}  // namespace

TEST(differential_vs_reference_book) {
  for (std::uint64_t seed = 1; seed <= 20; ++seed) {
    run_differential(seed, 20'000);
    if (current_failed()) {
      std::fprintf(stderr, "    (seed %llu)\n", static_cast<unsigned long long>(seed));
      return;
    }
  }
}
