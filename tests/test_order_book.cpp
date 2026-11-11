// Scenario tests for matching rules.

#include <vector>

#include "apex/order_book.hpp"
#include "recording_listener.hpp"
#include "test_framework.hpp"

using namespace apex;
using namespace apex_test;

namespace {
struct Fixture {
  RecordingListener ev;
  OrderBook<RecordingListener> book{BookConfig{1024, 4096}, ev};
  void send(const Request& r) { book.submit(r); }
};
}  // namespace

TEST(limit_order_rests_when_nothing_crosses) {
  Fixture f;
  f.send(limit(1, Side::Buy, 100, 10));
  f.send(limit(2, Side::Sell, 101, 5));
  CHECK_EQ(f.book.best_bid(), 100);
  CHECK_EQ(f.book.best_ask(), 101);
  CHECK(f.ev.trades.empty());
  CHECK_EQ(f.book.live_orders(), 2u);
}

TEST(crossing_limit_trades_at_maker_price) {
  Fixture f;
  f.send(limit(1, Side::Sell, 100, 10));
  f.send(limit(2, Side::Buy, 105, 10));  // willing to pay 105, but trades at the resting 100
  CHECK_EQ(f.ev.trades.size(), 1u);
  CHECK_EQ(f.ev.trades[0], (TradeRec{2, 1, 100, 10}));
  CHECK_EQ(f.book.best_ask(), kNoPrice);
  CHECK_EQ(f.book.best_bid(), kNoPrice);
  CHECK_EQ(f.book.live_orders(), 0u);
}

TEST(partial_fill_leaves_remainder_resting) {
  Fixture f;
  f.send(limit(1, Side::Sell, 100, 4));
  f.send(limit(2, Side::Buy, 100, 10));
  CHECK_EQ(f.ev.trades[0], (TradeRec{2, 1, 100, 4}));
  CHECK_EQ(f.book.best_bid(), 100);
  CHECK_EQ(f.book.qty_at(Side::Buy, 100), 6u);
  CHECK_EQ(f.book.best_ask(), kNoPrice);
}

TEST(price_time_priority_fifo_within_level) {
  Fixture f;
  f.send(limit(1, Side::Sell, 100, 5));
  f.send(limit(2, Side::Sell, 100, 5));
  f.send(limit(3, Side::Sell, 99, 5));   // better price arrives last but fills first
  f.send(limit(4, Side::Buy, 100, 12));
  CHECK_EQ(f.ev.trades.size(), 3u);
  CHECK_EQ(f.ev.trades[0], (TradeRec{4, 3, 99, 5}));
  CHECK_EQ(f.ev.trades[1], (TradeRec{4, 1, 100, 5}));
  CHECK_EQ(f.ev.trades[2], (TradeRec{4, 2, 100, 2}));
  CHECK_EQ(f.book.qty_at(Side::Sell, 100), 3u);
}

TEST(market_order_sweeps_levels_and_cancels_rest) {
  Fixture f;
  f.send(limit(1, Side::Buy, 100, 3));
  f.send(limit(2, Side::Buy, 98, 3));
  f.send(market(3, Side::Sell, 10));
  CHECK_EQ(f.ev.trades.size(), 2u);
  CHECK_EQ(f.ev.trades[0], (TradeRec{3, 1, 100, 3}));
  CHECK_EQ(f.ev.trades[1], (TradeRec{3, 2, 98, 3}));
  CHECK_EQ(f.ev.cancels.size(), 1u);
  CHECK_EQ(f.ev.cancels[0].first, 3u);
  CHECK_EQ(f.ev.cancels[0].second, 4u);
  CHECK_EQ(f.book.live_orders(), 0u);
}

TEST(market_order_into_empty_book_is_cancelled) {
  Fixture f;
  f.send(market(1, Side::Buy, 5));
  CHECK(f.ev.trades.empty());
  CHECK_EQ(f.ev.cancels.size(), 1u);
  CHECK_EQ(f.book.live_orders(), 0u);
}

TEST(cancel_removes_order_and_updates_best) {
  Fixture f;
  f.send(limit(1, Side::Buy, 100, 5));
  f.send(limit(2, Side::Buy, 99, 5));
  f.send(cancel(1));
  CHECK_EQ(f.book.best_bid(), 99);
  CHECK_EQ(f.ev.cancels.size(), 1u);
  f.send(cancel(1));  // already gone
  CHECK_EQ(f.ev.rejects.size(), 1u);
  CHECK(f.ev.rejects[0].second == RejectReason::UnknownId);
}

TEST(rejects_bad_input) {
  Fixture f;
  f.send(limit(1, Side::Buy, 100, 0));
  f.send(limit(2, Side::Buy, -1, 5));
  f.send(limit(3, Side::Buy, 1024, 5));  // book has 1024 levels: 0..1023
  f.send(limit(4, Side::Buy, 100, 5));
  f.send(limit(4, Side::Buy, 100, 5));
  CHECK_EQ(f.ev.rejects.size(), 4u);
  CHECK(f.ev.rejects[0].second == RejectReason::InvalidQty);
  CHECK(f.ev.rejects[1].second == RejectReason::InvalidPrice);
  CHECK(f.ev.rejects[2].second == RejectReason::InvalidPrice);
  CHECK(f.ev.rejects[3].second == RejectReason::DuplicateId);
}

TEST(book_full_is_rejected_not_crashed) {
  RecordingListener ev;
  OrderBook<RecordingListener> book{BookConfig{1024, 2}, ev};
  book.submit(limit(1, Side::Buy, 10, 1));
  book.submit(limit(2, Side::Buy, 10, 1));
  book.submit(limit(3, Side::Buy, 10, 1));
  CHECK_EQ(ev.rejects.size(), 1u);
  CHECK(ev.rejects[0].second == RejectReason::BookFull);
}

TEST(modify_down_keeps_priority) {
  Fixture f;
  f.send(limit(1, Side::Sell, 100, 10));
  f.send(limit(2, Side::Sell, 100, 10));
  f.send(modify(1, 100, 4));  // smaller size, same price: stays first in line
  f.send(market(3, Side::Buy, 5));
  CHECK_EQ(f.ev.trades[0], (TradeRec{3, 1, 100, 4}));
  CHECK_EQ(f.ev.trades[1], (TradeRec{3, 2, 100, 1}));
}

TEST(modify_up_or_reprice_loses_priority) {
  Fixture f;
  f.send(limit(1, Side::Sell, 100, 10));
  f.send(limit(2, Side::Sell, 100, 10));
  f.send(modify(1, 100, 11));  // larger size: goes to the back
  f.send(market(3, Side::Buy, 5));
  CHECK_EQ(f.ev.trades[0], (TradeRec{3, 2, 100, 5}));
}

TEST(modify_to_crossing_price_trades) {
  Fixture f;
  f.send(limit(1, Side::Buy, 95, 10));
  f.send(limit(2, Side::Sell, 100, 4));
  f.send(modify(1, 100, 10));
  CHECK_EQ(f.ev.trades.size(), 1u);
  CHECK_EQ(f.ev.trades[0], (TradeRec{1, 2, 100, 4}));
  CHECK_EQ(f.book.qty_at(Side::Buy, 100), 6u);
  CHECK_EQ(f.book.qty_at(Side::Buy, 95), 0u);
}

TEST(buy_stop_triggers_when_last_trade_reaches_stop) {
  Fixture f;
  f.send(limit(1, Side::Sell, 100, 5));
  f.send(limit(2, Side::Sell, 102, 5));
  f.send(stop(3, Side::Buy, 100, 5));  // dormant: no trade has happened yet
  CHECK(f.ev.trades.empty());
  f.send(limit(4, Side::Buy, 100, 5));  // trade at 100 -> stop fires, buys at 102
  CHECK_EQ(f.ev.triggers.size(), 1u);
  CHECK_EQ(f.ev.trades.size(), 2u);
  CHECK_EQ(f.ev.trades[1], (TradeRec{3, 2, 102, 5}));
}

TEST(sell_stop_cascade) {
  Fixture f;
  f.send(limit(1, Side::Buy, 100, 1));
  f.send(limit(2, Side::Buy, 99, 1));
  f.send(limit(3, Side::Buy, 98, 1));
  f.send(stop(10, Side::Sell, 100, 1));  // fires on a trade at <= 100
  f.send(stop(11, Side::Sell, 99, 1));   // fires once stop 10 trades at 99
  f.send(market(20, Side::Sell, 1));     // trades at 100
  CHECK_EQ(f.ev.triggers.size(), 2u);
  CHECK_EQ(f.ev.trades.size(), 3u);
  CHECK_EQ(f.ev.trades[1], (TradeRec{10, 2, 99, 1}));
  CHECK_EQ(f.ev.trades[2], (TradeRec{11, 3, 98, 1}));
  CHECK_EQ(f.book.best_bid(), kNoPrice);
}

TEST(stop_limit_rests_after_trigger) {
  Fixture f;
  f.send(limit(1, Side::Sell, 100, 1));
  f.send(stop_limit(2, Side::Buy, 100, 101, 5));
  f.send(limit(3, Side::Buy, 100, 1));  // trade at 100 triggers stop-limit at 101
  CHECK_EQ(f.ev.triggers.size(), 1u);
  CHECK_EQ(f.book.best_bid(), 101);
  CHECK_EQ(f.book.qty_at(Side::Buy, 101), 5u);
}

TEST(stop_already_through_market_fires_immediately) {
  Fixture f;
  f.send(limit(1, Side::Sell, 100, 1));
  f.send(limit(2, Side::Buy, 100, 1));  // last trade = 100
  f.send(limit(3, Side::Sell, 105, 5));
  f.send(stop(4, Side::Buy, 99, 5));    // 100 >= 99 already
  CHECK_EQ(f.ev.triggers.size(), 1u);
  CHECK_EQ(f.ev.trades.back(), (TradeRec{4, 3, 105, 5}));
}

TEST(cancel_pending_stop) {
  Fixture f;
  f.send(stop(1, Side::Buy, 100, 5));
  f.send(cancel(1));
  f.send(limit(2, Side::Sell, 100, 1));
  f.send(limit(3, Side::Buy, 100, 1));
  CHECK(f.ev.triggers.empty());
  CHECK_EQ(f.book.live_orders(), 0u);
}

TEST(stops_cannot_be_modified) {
  Fixture f;
  f.send(stop(1, Side::Buy, 100, 5));
  f.send(modify(1, 100, 3));
  CHECK_EQ(f.ev.rejects.size(), 1u);
  CHECK(f.ev.rejects[0].second == RejectReason::NotModifiable);
}

TEST(level_totals_stay_consistent) {
  Fixture f;
  f.send(limit(1, Side::Buy, 100, 5));
  f.send(limit(2, Side::Buy, 100, 7));
  f.send(limit(3, Side::Buy, 100, 2));
  CHECK_EQ(f.book.qty_at(Side::Buy, 100), 14u);
  CHECK_EQ(f.book.orders_at(Side::Buy, 100), 3u);
  f.send(cancel(2));
  CHECK_EQ(f.book.qty_at(Side::Buy, 100), 7u);
  f.send(market(4, Side::Sell, 6));
  CHECK_EQ(f.book.qty_at(Side::Buy, 100), 1u);
  CHECK_EQ(f.book.orders_at(Side::Buy, 100), 1u);
}
