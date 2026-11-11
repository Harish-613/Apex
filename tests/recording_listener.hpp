#pragma once
// Listener that records every event so tests can assert on them.

#include <cstdint>
#include <ostream>
#include <tuple>
#include <utility>
#include <vector>

#include "apex/types.hpp"

namespace apex_test {

struct TradeRec {
  apex::OrderId taker, maker;
  apex::Price price;
  apex::Qty qty;
  bool operator==(const TradeRec& o) const {
    return std::tie(taker, maker, price, qty) == std::tie(o.taker, o.maker, o.price, o.qty);
  }
};

inline std::ostream& operator<<(std::ostream& os, const TradeRec& t) {
  return os << "{taker " << t.taker << ", maker " << t.maker << ", px " << t.price << ", qty "
            << t.qty << "}";
}

struct RecordingListener {
  std::vector<TradeRec> trades;
  std::vector<std::pair<apex::OrderId, apex::Qty>> cancels;
  std::vector<std::pair<apex::OrderId, apex::RejectReason>> rejects;
  std::vector<apex::OrderId> accepts, triggers, modifies;

  void on_accept(const apex::Order& o) { accepts.push_back(o.id); }
  void on_reject(apex::OrderId id, apex::RejectReason r) { rejects.emplace_back(id, r); }
  void on_trade(const apex::Trade& t) { trades.push_back({t.taker_id, t.maker_id, t.price, t.qty}); }
  void on_cancel(apex::OrderId id, apex::Qty q) { cancels.emplace_back(id, q); }
  void on_modify(apex::OrderId id) { modifies.push_back(id); }
  void on_trigger(apex::OrderId id) { triggers.push_back(id); }
  void on_processed(const apex::Request&) {}

  void clear() { *this = RecordingListener{}; }
};

// Request builders.
inline apex::Request limit(apex::OrderId id, apex::Side s, apex::Price px, apex::Qty q) {
  return {apex::Action::New, s, apex::OrderType::Limit, id, px, apex::kNoPrice, q, 0};
}
inline apex::Request market(apex::OrderId id, apex::Side s, apex::Qty q) {
  return {apex::Action::New, s, apex::OrderType::Market, id, apex::kNoPrice, apex::kNoPrice, q, 0};
}
inline apex::Request stop(apex::OrderId id, apex::Side s, apex::Price stop_px, apex::Qty q) {
  return {apex::Action::New, s, apex::OrderType::Stop, id, apex::kNoPrice, stop_px, q, 0};
}
inline apex::Request stop_limit(apex::OrderId id, apex::Side s, apex::Price stop_px,
                                apex::Price px, apex::Qty q) {
  return {apex::Action::New, s, apex::OrderType::StopLimit, id, px, stop_px, q, 0};
}
inline apex::Request cancel(apex::OrderId id) {
  return {apex::Action::Cancel, apex::Side::Buy, apex::OrderType::Limit, id, 0, 0, 0, 0};
}
inline apex::Request modify(apex::OrderId id, apex::Price px, apex::Qty q) {
  return {apex::Action::Modify, apex::Side::Buy, apex::OrderType::Limit, id, px, 0, q, 0};
}

}  // namespace apex_test
