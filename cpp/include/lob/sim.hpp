#pragma once

// Market-making simulator: replays real ITCH data and lets a strategy rest simulated orders
// in the replayed book.
//
// Assumptions (stated in the README, because the results depend on them):
// - No market impact. Our orders never enter the replayed book and never change what the real
//   market did. A fill only means "the real order flow would have reached our order".
// - Latency. New orders and cancels reach the exchange `latency_ns` after the strategy decides.
//   An order can therefore fill while its cancel is in flight. Orders are post-only: one that
//   would cross the market when it arrives is rejected.
// - Queue position. When our order arrives, every share already displayed at its price is ahead
//   of it. At our price and side:
//     executions eat through the shares ahead first, then fill us;
//     cancels come from ahead of us either never (pessimistic) or in proportion to the shares
//       ahead of us (proportional);
//     the shares ahead can never exceed what is displayed at the level.
//   We are also filled in full when an execution happens at a worse price on our side (someone
//   traded through our price), and filled when an opposite-side order is added at a price that
//   crosses ours.
// - Quoting window. Only after the stock's opening cross, while it is trading ('T'), not within
//   `reopen_cooldown_ns` after a halt or LULD pause ends, and not in the last
//   `stop_before_close_ns` before 16:00. Outside the window all our orders are cancelled.
// - Fills are at our limit price. No fees or rebates yet.

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "lob/fast_book.hpp"
#include "lob/itch.hpp"

namespace lob::sim {

using itch::Price;
using MarketBook = fast::Book<fast::VectorLevels>;

enum class CancelModel { kPessimistic, kProportional };

constexpr std::size_t kHorizons = 4;  // markout horizons per fill

struct Config {
  std::uint64_t latency_ns = 10'000;
  CancelModel cancel_model = CancelModel::kProportional;
  std::uint64_t reopen_cooldown_ns = 1'000'000'000;
  std::uint64_t stop_before_close_ns = 60'000'000'000;
  std::array<std::uint64_t, kHorizons> markout_ns = {100'000'000, 1'000'000'000, 5'000'000'000, 30'000'000'000};
};

// What the strategy sees each time a tracked stock's book changes.
struct MarketState {
  std::uint64_t ts;
  std::uint16_t locate;
  const std::string& symbol;
  const MarketBook& book;
  Top top;
  std::int64_t position;  // shares; positive = long
};

struct Quote {
  bool active = false;
  Price price = 0;
  std::uint32_t shares = 0;
};

// The quotes the strategy wants resting right now. The simulator works out the orders and
// cancels needed to get there; an existing order at the same price is kept, so it keeps its place.
struct QuoteRequest {
  Quote bid;
  Quote ask;
};

class Strategy {
 public:
  virtual ~Strategy() = default;
  virtual QuoteRequest quote(const MarketState& s) = 0;
  virtual std::string name() const = 0;
};

// Why an order filled.
enum class FillReason : char { kQueue = 'Q', kTradeThrough = 'T', kCross = 'X' };

struct Fill {
  std::uint64_t ts;
  std::uint16_t locate;
  char side;  // our side: 'B' = we bought
  Price price;
  std::uint32_t shares;
  std::int64_t position_after;
  double mid;  // mid price just before the fill, $ x 10^4; 0 if one side was empty
  double imbalance;  // top-of-book (bid - ask) / (bid + ask) shares just before the fill
  Price spread;      // best ask - best bid just before the fill, $ x 10^4
  std::array<double, kHorizons> mid_after{};  // mid at each markout horizon
  FillReason reason;
  std::uint64_t ahead_at_arrival;  // displayed shares ahead of our order when it arrived
  std::uint64_t order_age_ns;      // time from arrival at the exchange to this fill
};

struct SymbolStats {
  std::uint64_t orders_sent = 0;
  std::uint64_t cancels_sent = 0;
  std::uint64_t rejects = 0;
  std::uint64_t fills = 0;
  std::uint64_t bought = 0;
  std::uint64_t sold = 0;
};

class Simulator {
 public:
  Simulator(const std::vector<std::string>& symbols, Strategy& strategy, Config config = {});

  // Applies one ITCH message (msg[0] is the type byte).
  void process(const std::uint8_t* msg, std::uint16_t len);

  // Settles markouts still pending at the end of the data, using the last known mid.
  void finish();

  const std::vector<Fill>& fills() const { return fills_; }
  const Config& config() const { return config_; }

  struct SymbolResult {
    std::string symbol;
    std::uint16_t locate;
    SymbolStats stats;
    std::int64_t position;
    double cash;      // $ (sales minus purchases)
    double last_mid;  // $, the mid at 16:00 (or the last mid seen)
    double pnl;       // $, cash + position marked at last_mid
  };
  std::vector<SymbolResult> results() const;

  // Called by the book builder for every add, execution and cancel of a real resting order.
  void on_book_event(fast::BookEvent kind, std::uint16_t locate, char side, Price price, std::uint32_t shares,
                     std::uint64_t ref);

 private:
  struct SimOrder {
    std::uint64_t id;
    char side;
    Price price;
    std::uint32_t shares;  // remaining
    std::uint64_t ahead = 0;
    std::uint64_t ahead_at_arrival = 0;
    std::uint64_t arrives;  // when it reaches the exchange
    bool live = false;      // arrived and resting
    bool cancel_pending = false;
    std::uint64_t cancel_arrives = 0;
    bool done = false;
  };

  struct Symbol {
    std::string name;
    bool tracked = false;
    std::vector<SimOrder> orders;  // working orders: in flight, resting, or being cancelled
    std::int64_t position = 0;
    std::int64_t cash = 0;  // $ x 10^4
    double mid = 0;         // $ x 10^4
    double imbalance = 0;   // top-of-book share imbalance
    Price spread = 0;
    char trading_state = 'T';
    bool opened = false;
    std::uint64_t reopened_at = 0;
    double close_mid = 0;  // mid at the end-of-market-hours event, used to mark the final position
    SymbolStats stats;
  };

  struct PendingMarkout {
    std::uint64_t due;
    std::size_t fill;
  };

  // A small handler for the messages the simulator needs beyond the book.
  struct Events : itch::Handler {
    using itch::Handler::on;
    Simulator* sim;
    void on(const itch::SystemEvent& m);
    void on(const itch::StockDirectory& m);
    void on(const itch::TradingAction& m);
    void on(const itch::CrossTrade& m);
    void on(const itch::Trade& m);
  };

  void advance(std::uint64_t ts);
  bool can_quote(const Symbol& s, std::uint64_t ts) const;
  void requote(std::uint16_t locate, std::uint64_t ts);
  void reconcile(Symbol& s, char side, const Quote& want, std::uint64_t ts);
  void arrive(Symbol& s, std::uint16_t locate, SimOrder& o);
  void fill(Symbol& s, std::uint16_t locate, SimOrder& o, std::uint32_t shares, FillReason reason);
  void schedule_next_action();

  Config config_;
  Strategy& strategy_;
  fast::BookBuilder<fast::FlatHashIndex, fast::VectorLevels, Simulator> builder_;
  Events events_;
  std::vector<Symbol> symbols_ = std::vector<Symbol>(65536);  // by locate
  std::vector<std::uint16_t> tracked_;                        // locates of tracked symbols
  std::vector<std::string> wanted_;
  bool regular_hours_ = false;
  bool stopped_ = false;  // past the end-of-day stop time
  std::uint64_t now_ = 0;
  std::uint64_t next_action_ = ~0ull;  // earliest pending order arrival or cancel
  std::uint64_t next_id_ = 1;
  std::vector<Fill> fills_;
  std::array<std::deque<PendingMarkout>, kHorizons> markouts_;
};

}  // namespace lob::sim
