// Reads order commands from a file (or stdin) and prints the resulting events.
//
//   B <price> <qty> [LIMIT|IOC|FOK|POST]   buy
//   S <price> <qty> [LIMIT|IOC|FOK|POST]   sell
//   MB <qty> / MS <qty>                    market buy / sell
//   C <id>                                 cancel
//   X <id> <price> <qty>                   modify
//   P                                      print the book
//   # comment
//
// Usage: lob_replay [file]

#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

#include "lob/order_book.hpp"

using namespace lob;

namespace {

struct PrintingListener {
  void on_accept(OrderId id, Side s, Price p, Qty q, OrderType t) {
    std::cout << "  ACCEPT  #" << id << ' ' << to_string(s) << ' ' << to_string(t) << ' ' << q;
    if (t != OrderType::Market) std::cout << " @ " << p;
    std::cout << '\n';
  }
  void on_reject(OrderId id, RejectReason r) { std::cout << "  REJECT  #" << id << ' ' << to_string(r) << '\n'; }
  void on_trade(const Trade& t) {
    std::cout << "  TRADE   " << t.qty << " @ " << t.price << "  maker #" << t.maker << "  taker #" << t.taker
              << " (" << to_string(t.aggressor) << " aggressor)\n";
  }
  void on_cancel(OrderId id, Qty q) { std::cout << "  CANCEL  #" << id << " qty " << q << '\n'; }
  void on_modify(OrderId id, Price p, Qty q) { std::cout << "  MODIFY  #" << id << " -> " << q << " @ " << p << '\n'; }
};

OrderType parse_type(const std::string& s) {
  if (s == "IOC") return OrderType::IOC;
  if (s == "FOK") return OrderType::FOK;
  if (s == "POST") return OrderType::PostOnly;
  return OrderType::Limit;
}

template <class Book>
void print_book(const Book& book) {
  const auto asks = book.depth(Side::Sell, 5);
  const auto bids = book.depth(Side::Buy, 5);
  std::cout << "  +------------- BOOK -------------+\n";
  for (auto it = asks.rbegin(); it != asks.rend(); ++it)
    std::cout << "  |           " << std::setw(8) << it->price << "  " << std::setw(6) << it->qty << " ask |\n";
  std::cout << "  |--------------------------------|\n";
  for (const auto& l : bids)
    std::cout << "  | bid " << std::setw(6) << l.qty << "  " << std::setw(8) << l.price << "           |\n";
  std::cout << "  +--------------------------------+\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::ifstream file;
  if (argc > 1) {
    file.open(argv[1]);
    if (!file) { std::cerr << "cannot open " << argv[1] << '\n'; return 1; }
  }
  std::istream& in = argc > 1 ? static_cast<std::istream&>(file) : std::cin;

  PrintingListener listener;
  OrderBook book(BookConfig{1, 1'000'000, 1u << 16}, listener);

  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream ss(line);
    std::string cmd;
    ss >> cmd;
    std::cout << "> " << line << '\n';
    if (cmd == "B" || cmd == "S") {
      Price p; Qty q; std::string t;
      ss >> p >> q >> t;
      book.submit(cmd == "B" ? Side::Buy : Side::Sell, p, q, parse_type(t));
    } else if (cmd == "MB" || cmd == "MS") {
      Qty q; ss >> q;
      book.submit(cmd == "MB" ? Side::Buy : Side::Sell, 0, q, OrderType::Market);
    } else if (cmd == "C") {
      OrderId id; ss >> id;
      if (!book.cancel(id)) std::cout << "  (cancel #" << id << ": not resting)\n";
    } else if (cmd == "X") {
      OrderId id; Price p; Qty q; ss >> id >> p >> q;
      if (!book.modify(id, p, q)) std::cout << "  (modify #" << id << ": not resting or bad price)\n";
    } else if (cmd == "P") {
      print_book(book);
    } else {
      std::cout << "  unknown command\n";
    }
  }
  return 0;
}
