#pragma once

// Limit order book rebuilt from ITCH messages (first version: standard containers).
//
// - Order store: std::unordered_map<ref, Order>. Its nodes never move, so the intrusive list
//   pointers below stay valid when the table rehashes.
// - Price levels: std::map per side, best price first.
// - Each level keeps its orders in an intrusive doubly linked FIFO (oldest at head), which is
//   price-time priority: appending, and unlinking any order found by ref, are both O(1).
//
// Nasdaq does the matching and every execution names the order it hit, so the builder never
// matches orders itself; it only applies adds, executions, cancels, deletes and replaces.

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "lob/itch.hpp"

namespace lob {

using itch::Price;

struct Order {
  std::uint64_t ref = 0;
  Price price = 0;
  std::uint32_t shares = 0;
  std::uint16_t locate = 0;
  char side = 0;  // 'B' or 'S'
  Order* prev = nullptr;
  Order* next = nullptr;
};

struct Level {
  std::uint64_t shares = 0;
  std::uint32_t orders = 0;
  Order* head = nullptr;  // oldest: first in line to be filled
  Order* tail = nullptr;
};

// Best bid and ask. An empty side has price 0 and size 0.
struct Top {
  Price bid_px = 0;
  std::uint64_t bid_sz = 0;
  Price ask_px = 0;
  std::uint64_t ask_sz = 0;
  friend bool operator==(const Top&, const Top&) = default;
};

class Book {
 public:
  using Bids = std::map<Price, Level, std::greater<Price>>;
  using Asks = std::map<Price, Level>;

  void add(Order& o);                           // append o to the back of its price level
  void reduce(Order& o, std::uint32_t shares);  // shares < o.shares; o keeps its place in line
  void remove(Order& o);                        // unlink o; drop the level if it empties

  Top top() const;
  bool crossed() const;  // best bid > best ask
  bool locked() const;   // best bid == best ask
  const Bids& bids() const { return bids_; }
  const Asks& asks() const { return asks_; }

  // Walks every level and checks that its share and order totals match its list, and that
  // the list links are consistent. Returns the number of problems found (0 if consistent).
  std::size_t validate() const;

 private:
  Level& level_of(const Order& o);

  Bids bids_;
  Asks asks_;
};

struct BookStats {
  std::uint64_t unknown_ref = 0;    // execution/cancel/delete/replace for an order we don't have
  std::uint64_t duplicate_ref = 0;  // add for a ref that is already live
  std::uint64_t overfill = 0;       // executed or cancelled more shares than the order had
  std::uint64_t crossed = 0;        // messages after which a book was crossed, regular hours only
  std::uint64_t locked = 0;         // same, for a locked book
  std::uint64_t max_live_orders = 0;
  std::map<std::uint16_t, std::uint64_t> crossed_or_locked_by_locate;  // regular hours, trading
};

// Applies ITCH messages to one Book per tracked stock.
class BookBuilder : public itch::Handler {
 public:
  using itch::Handler::on;

  // Tracks the given symbols, or every stock if the list is empty.
  explicit BookBuilder(const std::vector<std::string>& symbols = {});

  // Decodes and applies one message. Afterwards touched() is the book it changed, if any.
  void process(const std::uint8_t* msg, std::uint16_t len);

  Book* touched() const { return touched_; }
  std::uint16_t touched_locate() const { return touched_locate_; }
  const Book* book(std::uint16_t locate) const { return books_[locate].get(); }
  const std::string& symbol(std::uint16_t locate) const { return symbols_[locate]; }
  const BookStats& stats() const { return stats_; }
  std::size_t live_orders() const { return orders_.size(); }

  void on(const itch::SystemEvent& m);
  void on(const itch::StockDirectory& m);
  void on(const itch::TradingAction& m);
  void on(const itch::AddOrder& m);
  void on(const itch::OrderExecuted& m);
  void on(const itch::OrderExecutedWithPrice& m);
  void on(const itch::OrderCancel& m);
  void on(const itch::OrderDelete& m);
  void on(const itch::OrderReplace& m);

 private:
  Book* tracked(std::uint16_t locate) { return books_[locate].get(); }
  void add(Book& b, std::uint16_t locate, std::uint64_t ref, char side, std::uint32_t shares, Price price);
  void take_shares(std::uint16_t locate, std::uint64_t ref, std::uint32_t shares);  // E, C and X
  void touch(Book& b, std::uint16_t locate);

  std::unordered_map<std::uint64_t, Order> orders_;
  std::vector<std::unique_ptr<Book>> books_ = std::vector<std::unique_ptr<Book>>(65536);  // by locate
  std::vector<std::string> symbols_ = std::vector<std::string>(65536);
  std::vector<char> trading_state_ = std::vector<char>(65536, 'T');
  std::set<std::string> wanted_;
  bool all_symbols_;
  bool regular_hours_ = false;  // between the market-open and market-close system events
  Book* touched_ = nullptr;
  std::uint16_t touched_locate_ = 0;
  BookStats stats_;
};

}  // namespace lob
