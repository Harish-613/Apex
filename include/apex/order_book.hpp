#pragma once
// Limit order book and matching engine for a single instrument.
//
// Data layout
//   bids_/asks_   one PriceLevel per tick, indexed directly by price (O(1) lookup).
//                 Each level is a FIFO doubly-linked list of resting orders,
//                 which gives price-time priority: better price first, then
//                 earlier arrival first.
//   *_bits_       PriceBitset marking non-empty levels, used to find the next
//                 best price when a level empties.
//   best_bid_/best_ask_  cached top of book.
//   map_          OrderId -> Order* for O(1) cancel/modify.
//   pool_         preallocated Order storage (no allocation while matching).
//   buy_stops_/sell_stops_  dormant stop orders, bucketed by stop price.
//
// Matching rules
//   * An incoming order trades against the opposite side while prices cross.
//   * Trades execute at the resting (maker) order's price.
//   * Limit remainder rests; Market remainder is cancelled.
//   * Stops are checked once the incoming order (and any orders it triggers)
//     finishes matching: buy stops fire when last trade >= stop price, sell
//     stops when last trade <= stop price. Triggered orders run FIFO in
//     trigger order, and may trigger further stops (a stop cascade).
//   * Modify: lowering quantity at the same price keeps queue priority;
//     changing price or raising quantity is cancel/replace and loses priority.

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

#include "memory_pool.hpp"
#include "order_map.hpp"
#include "price_bitset.hpp"
#include "spsc_ring.hpp"
#include "types.hpp"

namespace apex {

struct BookConfig {
  std::size_t price_levels = 1 << 16;  // valid prices are [0, price_levels)
  std::size_t max_orders = 1 << 20;    // max live (resting + stop) orders
};

// One price level. alignas(64) gives each level its own cache line, so
// touching one level never pulls a neighbour's data in or invalidates it.
struct alignas(kCacheLine) PriceLevel {
  Order* head = nullptr;  // oldest order (first to fill)
  Order* tail = nullptr;  // newest order
  std::uint64_t total_qty = 0;
  std::uint32_t count = 0;
};

template <class Listener = NullListener>
class OrderBook {
 public:
  OrderBook(const BookConfig& cfg, Listener& listener)
      : levels_(cfg.price_levels),
        listener_(listener),
        pool_(cfg.max_orders),
        map_(cfg.max_orders),
        bids_(levels_),
        asks_(levels_),
        buy_stops_(levels_),
        sell_stops_(levels_),
        bid_bits_(levels_),
        ask_bits_(levels_),
        buy_stop_bits_(levels_),
        sell_stop_bits_(levels_) {
    pending_.reserve(cfg.max_orders);
  }

  OrderBook(const OrderBook&) = delete;
  OrderBook& operator=(const OrderBook&) = delete;

  // Single entry point used by the engine thread.
  void submit(const Request& r) {
    switch (r.action) {
      case Action::New: add(r); break;
      case Action::Cancel: cancel(r.id); break;
      case Action::Modify: modify(r.id, r.price, r.qty); break;
      case Action::Shutdown: break;
    }
    listener_.on_processed(r);
  }

  void add(const Request& r) {
    if (r.qty == 0) return reject(r.id, RejectReason::InvalidQty);
    const bool needs_price = r.type == OrderType::Limit || r.type == OrderType::StopLimit;
    const bool needs_stop = r.type == OrderType::Stop || r.type == OrderType::StopLimit;
    if (needs_price && !valid_price(r.price)) return reject(r.id, RejectReason::InvalidPrice);
    if (needs_stop && !valid_price(r.stop_price)) return reject(r.id, RejectReason::InvalidPrice);
    if (map_.find(r.id)) return reject(r.id, RejectReason::DuplicateId);

    Order* o = pool_.allocate();
    if (!o) return reject(r.id, RejectReason::BookFull);
    o->id = r.id;
    o->side = r.side;
    o->type = r.type;
    o->price = needs_price ? r.price : kNoPrice;
    o->stop_price = needs_stop ? r.stop_price : kNoPrice;
    o->qty = r.qty;
    o->seq = ++seq_;
    o->state = OrderState::Working;
    map_.insert(o->id, o);
    listener_.on_accept(*o);

    switch (o->type) {
      case OrderType::Limit: execute_limit(o); break;
      case OrderType::Market: execute_market(o); break;
      case OrderType::Stop:
      case OrderType::StopLimit: place_stop(o); break;
    }
    drain_triggers();
  }

  void cancel(OrderId id) {
    Order* o = map_.find(id);
    if (!o) return reject(id, RejectReason::UnknownId);
    detach(o);
    listener_.on_cancel(id, o->qty);
    release(o);
  }

  void modify(OrderId id, Price new_price, Qty new_qty) {
    Order* o = map_.find(id);
    if (!o) return reject(id, RejectReason::UnknownId);
    if (o->state != OrderState::Resting) return reject(id, RejectReason::NotModifiable);
    if (new_qty == 0) return cancel(id);
    if (!valid_price(new_price)) return reject(id, RejectReason::InvalidPrice);

    if (new_price == o->price && new_qty <= o->qty) {
      // Pure size reduction: keep place in the queue.
      level_for(o->side, o->price).total_qty -= (o->qty - new_qty);
      o->qty = new_qty;
      listener_.on_modify(id);
      return;
    }
    // Cancel/replace: new time priority, and the new price may cross the spread.
    detach(o);
    o->price = new_price;
    o->qty = new_qty;
    o->seq = ++seq_;
    o->state = OrderState::Working;
    listener_.on_modify(id);
    execute_limit(o);
    drain_triggers();
  }

  // ---- Read-only views (single-threaded use, or after the engine stops) ----
  Price best_bid() const noexcept { return best_bid_; }
  Price best_ask() const noexcept { return best_ask_; }
  Price last_trade() const noexcept { return last_trade_; }
  std::size_t live_orders() const noexcept { return map_.size(); }
  std::size_t price_levels() const noexcept { return levels_; }

  std::uint64_t qty_at(Side side, Price p) const noexcept {
    if (!valid_price(p)) return 0;
    return (side == Side::Buy ? bids_ : asks_)[static_cast<std::size_t>(p)].total_qty;
  }
  std::uint32_t orders_at(Side side, Price p) const noexcept {
    if (!valid_price(p)) return 0;
    return (side == Side::Buy ? bids_ : asks_)[static_cast<std::size_t>(p)].count;
  }
  const Order* find(OrderId id) const noexcept { return map_.find(id); }

  // Visit resting orders at one level in queue order.
  template <class F>
  void for_each_at(Side side, Price p, F&& f) const {
    if (!valid_price(p)) return;
    for (const Order* o = (side == Side::Buy ? bids_ : asks_)[static_cast<std::size_t>(p)].head; o;
         o = o->next)
      f(*o);
  }

  // Next non-empty level strictly worse than p (lower for bids, higher for asks).
  Price next_level(Side side, Price p) const noexcept {
    if (side == Side::Buy) {
      if (p <= 0) return kNoPrice;
      return static_cast<Price>(bid_bits_.find_prev(static_cast<std::size_t>(p - 1)));
    }
    return static_cast<Price>(ask_bits_.find_next(static_cast<std::size_t>(p + 1)));
  }

 private:
  static constexpr Price kMaxPrice = std::numeric_limits<Price>::max();
  static constexpr Price kMinPrice = std::numeric_limits<Price>::min();

  bool valid_price(Price p) const noexcept {
    return p >= 0 && static_cast<std::size_t>(p) < levels_;
  }

  PriceLevel& level_for(Side side, Price p) noexcept {
    return (side == Side::Buy ? bids_ : asks_)[static_cast<std::size_t>(p)];
  }

  void reject(OrderId id, RejectReason why) { listener_.on_reject(id, why); }

  // ---- intrusive list helpers ----
  static void push_back(PriceLevel& lvl, Order* o) noexcept {
    o->next = nullptr;
    o->prev = lvl.tail;
    if (lvl.tail) lvl.tail->next = o;
    else lvl.head = o;
    lvl.tail = o;
    lvl.total_qty += o->qty;
    ++lvl.count;
  }

  static void unlink(PriceLevel& lvl, Order* o) noexcept {
    if (o->prev) o->prev->next = o->next;
    else lvl.head = o->next;
    if (o->next) o->next->prev = o->prev;
    else lvl.tail = o->prev;
    o->prev = o->next = nullptr;
    lvl.total_qty -= o->qty;
    --lvl.count;
  }

  // ---- matching ----
  void execute_limit(Order* o) {
    const bool traded = match(o, o->price);
    if (o->qty > 0) rest(o);
    else release(o);
    if (traded) check_stops();
  }

  void execute_market(Order* o) {
    const bool traded = match(o, o->side == Side::Buy ? kMaxPrice : kMinPrice);
    if (o->qty > 0) listener_.on_cancel(o->id, o->qty);  // no more liquidity
    release(o);
    if (traded) check_stops();
  }

  // Trade `taker` against the opposite side while prices cross `limit`.
  bool match(Order* taker, Price limit) {
    bool traded = false;
    if (taker->side == Side::Buy) {
      while (taker->qty > 0 && best_ask_ != kNoPrice && best_ask_ <= limit) {
        traded = true;
        PriceLevel& lvl = asks_[static_cast<std::size_t>(best_ask_)];
        fill_level(taker, lvl, best_ask_);
        if (!lvl.head) {
          ask_bits_.clear(static_cast<std::size_t>(best_ask_));
          best_ask_ = static_cast<Price>(ask_bits_.find_next(static_cast<std::size_t>(best_ask_)));
        }
      }
    } else {
      while (taker->qty > 0 && best_bid_ != kNoPrice && best_bid_ >= limit) {
        traded = true;
        PriceLevel& lvl = bids_[static_cast<std::size_t>(best_bid_)];
        fill_level(taker, lvl, best_bid_);
        if (!lvl.head) {
          bid_bits_.clear(static_cast<std::size_t>(best_bid_));
          best_bid_ = static_cast<Price>(bid_bits_.find_prev(static_cast<std::size_t>(best_bid_)));
        }
      }
    }
    return traded;
  }

  void fill_level(Order* taker, PriceLevel& lvl, Price price) {
    while (taker->qty > 0 && lvl.head) {
      Order* maker = lvl.head;
      const Qty q = std::min(taker->qty, maker->qty);
      taker->qty -= q;
      maker->qty -= q;
      lvl.total_qty -= q;
      last_trade_ = price;
      listener_.on_trade(Trade{taker->id, maker->id, price, q, taker->side});
      if (maker->qty == 0) {
        unlink(lvl, maker);  // qty is 0, so total_qty is unaffected
        release(maker);
      }
    }
  }

  void rest(Order* o) {
    const auto p = static_cast<std::size_t>(o->price);
    o->state = OrderState::Resting;
    if (o->side == Side::Buy) {
      push_back(bids_[p], o);
      bid_bits_.set(p);
      if (best_bid_ == kNoPrice || o->price > best_bid_) best_bid_ = o->price;
    } else {
      push_back(asks_[p], o);
      ask_bits_.set(p);
      if (best_ask_ == kNoPrice || o->price < best_ask_) best_ask_ = o->price;
    }
  }

  // Remove an order from whichever list currently holds it.
  void detach(Order* o) {
    if (o->state == OrderState::Resting) {
      const auto p = static_cast<std::size_t>(o->price);
      if (o->side == Side::Buy) {
        unlink(bids_[p], o);
        if (!bids_[p].head) {
          bid_bits_.clear(p);
          if (o->price == best_bid_) best_bid_ = static_cast<Price>(bid_bits_.find_prev(p));
        }
      } else {
        unlink(asks_[p], o);
        if (!asks_[p].head) {
          ask_bits_.clear(p);
          if (o->price == best_ask_) best_ask_ = static_cast<Price>(ask_bits_.find_next(p));
        }
      }
    } else if (o->state == OrderState::PendingStop) {
      const auto p = static_cast<std::size_t>(o->stop_price);
      auto& levels = o->side == Side::Buy ? buy_stops_ : sell_stops_;
      auto& bits = o->side == Side::Buy ? buy_stop_bits_ : sell_stop_bits_;
      unlink(levels[p], o);
      if (!levels[p].head) bits.clear(p);
    }
  }

  void release(Order* o) {
    map_.erase(o->id);
    pool_.deallocate(o);
  }

  // ---- stops ----
  void place_stop(Order* o) {
    const auto p = static_cast<std::size_t>(o->stop_price);
    o->state = OrderState::PendingStop;
    if (o->side == Side::Buy) {
      push_back(buy_stops_[p], o);
      buy_stop_bits_.set(p);
    } else {
      push_back(sell_stops_[p], o);
      sell_stop_bits_.set(p);
    }
    check_stops();  // fires immediately if the market is already through it
  }

  void check_stops() {
    if (last_trade_ == kNoPrice) return;
    for (;;) {  // buy stops with stop_price <= last trade, lowest first
      const std::int64_t p = buy_stop_bits_.find_next(0);
      if (p == PriceBitset::npos || p > last_trade_) break;
      trigger_level(buy_stops_[static_cast<std::size_t>(p)]);
      buy_stop_bits_.clear(static_cast<std::size_t>(p));
    }
    for (;;) {  // sell stops with stop_price >= last trade, highest first
      const std::int64_t p = sell_stop_bits_.find_prev(levels_ - 1);
      if (p == PriceBitset::npos || p < last_trade_) break;
      trigger_level(sell_stops_[static_cast<std::size_t>(p)]);
      sell_stop_bits_.clear(static_cast<std::size_t>(p));
    }
  }

  void trigger_level(PriceLevel& lvl) {
    for (Order* o = lvl.head; o;) {
      Order* next = o->next;
      o->prev = o->next = nullptr;
      o->state = OrderState::Triggered;
      pending_.push_back(o);  // capacity reserved up front: never reallocates
      listener_.on_trigger(o->id);
      o = next;
    }
    lvl = PriceLevel{};
  }

  // Run triggered stops in FIFO order. Each may trade and trigger more.
  void drain_triggers() {
    while (pending_head_ < pending_.size()) {
      Order* o = pending_[pending_head_++];
      o->state = OrderState::Working;
      if (o->type == OrderType::Stop) {
        o->type = OrderType::Market;
        execute_market(o);
      } else {
        o->type = OrderType::Limit;
        execute_limit(o);
      }
    }
    pending_.clear();
    pending_head_ = 0;
  }

  std::size_t levels_;
  Listener& listener_;
  MemoryPool<Order> pool_;
  OrderMap map_;
  std::vector<PriceLevel> bids_, asks_, buy_stops_, sell_stops_;
  PriceBitset bid_bits_, ask_bits_, buy_stop_bits_, sell_stop_bits_;
  std::vector<Order*> pending_;
  std::size_t pending_head_ = 0;
  Price best_bid_ = kNoPrice;
  Price best_ask_ = kNoPrice;
  Price last_trade_ = kNoPrice;
  std::uint64_t seq_ = 0;
};

}  // namespace apex
