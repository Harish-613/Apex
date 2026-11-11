// Interactive / scriptable front end for the order book. Reads one command per
// line from stdin and prints every event. Handy for demos and for checking a
// scenario by hand.
//
//   BUY  <id> <qty> LIMIT <price>
//   SELL <id> <qty> MARKET
//   BUY  <id> <qty> STOP <stop_price>
//   SELL <id> <qty> STOPLIMIT <stop_price> <limit_price>
//   CANCEL <id>
//   MODIFY <id> <price> <qty>
//   BOOK            print the top 5 levels per side
//
// Example: printf "SELL 1 10 LIMIT 100\nBUY 2 4 MARKET\nBOOK\n" | ./apex_cli

#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>

#include "apex/order_book.hpp"

using namespace apex;

namespace {

const char* reason(RejectReason r) {
  switch (r) {
    case RejectReason::DuplicateId: return "duplicate id";
    case RejectReason::UnknownId: return "unknown id";
    case RejectReason::InvalidPrice: return "invalid price";
    case RejectReason::InvalidQty: return "invalid qty";
    case RejectReason::NotModifiable: return "only resting limit orders can be modified";
    case RejectReason::BookFull: return "book full";
    default: return "?";
  }
}

struct PrintingListener {
  void on_accept(const Order& o) { std::printf("  ACCEPTED  #%llu\n", ull(o.id)); }
  void on_reject(OrderId id, RejectReason r) { std::printf("  REJECTED  #%llu (%s)\n", ull(id), reason(r)); }
  void on_trade(const Trade& t) {
    std::printf("  TRADE     %u @ %lld  (taker #%llu %s, maker #%llu)\n", t.qty,
                static_cast<long long>(t.price), ull(t.taker_id),
                t.taker_side == Side::Buy ? "buy" : "sell", ull(t.maker_id));
  }
  void on_cancel(OrderId id, Qty q) { std::printf("  CANCELLED #%llu (%u unfilled)\n", ull(id), q); }
  void on_modify(OrderId id) { std::printf("  MODIFIED  #%llu\n", ull(id)); }
  void on_trigger(OrderId id) { std::printf("  TRIGGERED #%llu\n", ull(id)); }
  void on_processed(const Request&) {}
  static unsigned long long ull(OrderId id) { return static_cast<unsigned long long>(id); }
};

template <class Book>
void print_book(const Book& book) {
  Price asks[5];
  int na = 0;
  for (Price p = book.best_ask(); p != kNoPrice && na < 5; p = book.next_level(Side::Sell, p)) asks[na++] = p;
  for (int i = na - 1; i >= 0; --i)
    std::printf("            ask %6lld  x %llu\n", static_cast<long long>(asks[i]),
                static_cast<unsigned long long>(book.qty_at(Side::Sell, asks[i])));
  std::printf("            ----------------\n");
  int nb = 0;
  for (Price p = book.best_bid(); p != kNoPrice && nb < 5; p = book.next_level(Side::Buy, p), ++nb)
    std::printf("            bid %6lld  x %llu\n", static_cast<long long>(p),
                static_cast<unsigned long long>(book.qty_at(Side::Buy, p)));
}

}  // namespace

int main() {
  PrintingListener listener;
  OrderBook<PrintingListener> book(BookConfig{1 << 16, 1 << 16}, listener);
  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream in(line);
    std::string cmd;
    if (!(in >> cmd) || cmd[0] == '#') continue;
    std::printf("> %s\n", line.c_str());
    Request r{};
    r.price = r.stop_price = kNoPrice;
    if (cmd == "BUY" || cmd == "SELL") {
      std::string type;
      unsigned long long id = 0;
      long long a = kNoPrice, b = kNoPrice;
      in >> id >> r.qty >> type;
      r.action = Action::New;
      r.id = id;
      r.side = cmd == "BUY" ? Side::Buy : Side::Sell;
      if (type == "LIMIT") { in >> a; r.type = OrderType::Limit; r.price = a; }
      else if (type == "MARKET") { r.type = OrderType::Market; }
      else if (type == "STOP") { in >> a; r.type = OrderType::Stop; r.stop_price = a; }
      else if (type == "STOPLIMIT") { in >> a >> b; r.type = OrderType::StopLimit; r.stop_price = a; r.price = b; }
      else { std::printf("  unknown order type '%s'\n", type.c_str()); continue; }
    } else if (cmd == "CANCEL") {
      unsigned long long id = 0;
      in >> id;
      r.action = Action::Cancel;
      r.id = id;
    } else if (cmd == "MODIFY") {
      unsigned long long id = 0;
      long long px = 0;
      in >> id >> px >> r.qty;
      r.action = Action::Modify;
      r.id = id;
      r.price = px;
    } else if (cmd == "BOOK") {
      print_book(book);
      continue;
    } else {
      std::printf("  unknown command\n");
      continue;
    }
    book.submit(r);
  }
  return 0;
}
