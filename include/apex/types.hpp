#pragma once
// Core value types shared by every Apex component.
//
// Prices are integers measured in ticks (e.g. 1 tick = $0.01). Integer prices
// avoid floating-point rounding and let a price index straight into an array of
// price levels.

#include <cstdint>

namespace apex {

using OrderId = std::uint64_t;
using Price = std::int64_t;  // in ticks, valid range [0, BookConfig::price_levels)
using Qty = std::uint32_t;

inline constexpr Price kNoPrice = -1;

enum class Side : std::uint8_t { Buy, Sell };

enum class OrderType : std::uint8_t {
  Limit,      // rest at `price` if not fully filled
  Market,     // take liquidity at any price; unfilled remainder is cancelled
  Stop,       // dormant until last trade crosses `stop_price`, then becomes Market
  StopLimit,  // dormant until last trade crosses `stop_price`, then becomes Limit at `price`
};

enum class Action : std::uint8_t { New, Cancel, Modify, Shutdown };

enum class RejectReason : std::uint8_t {
  None,
  DuplicateId,
  UnknownId,
  InvalidPrice,
  InvalidQty,
  NotModifiable,
  BookFull,
};

// Where an order currently lives. Needed so cancel knows which list to unlink from.
enum class OrderState : std::uint8_t { Resting, PendingStop, Triggered, Working };

// An order as stored inside the book. Orders are intrusive doubly-linked list
// nodes: prev/next link orders at the same price level in arrival (FIFO) order,
// so unlinking on cancel is O(1) with no search.
struct Order {
  OrderId id;
  Price price;       // limit price (Limit / StopLimit)
  Price stop_price;  // trigger price (Stop / StopLimit)
  std::uint64_t seq; // arrival sequence number = time priority
  Qty qty;           // remaining quantity
  Side side;
  OrderType type;
  OrderState state;
  Order* prev;
  Order* next;
};

// A message arriving at the matching engine.
struct Request {
  Action action;
  Side side;
  OrderType type;
  OrderId id;
  Price price;
  Price stop_price;
  Qty qty;
  std::uint64_t ts_ns;  // gateway timestamp, used only for latency measurement
};

struct Trade {
  OrderId taker_id;
  OrderId maker_id;
  Price price;  // always the resting (maker) order's price
  Qty qty;
  Side taker_side;
};

// Event sink with no-op defaults. The book is templated on its listener so
// these calls compile down to nothing when unused (no virtual dispatch).
struct NullListener {
  void on_accept(const Order&) {}
  void on_reject(OrderId, RejectReason) {}
  void on_trade(const Trade&) {}
  void on_cancel(OrderId, Qty /*remaining*/) {}
  void on_modify(OrderId) {}
  void on_trigger(OrderId) {}
  void on_processed(const Request&) {}
};

}  // namespace apex
