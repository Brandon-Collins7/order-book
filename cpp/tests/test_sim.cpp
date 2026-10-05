#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "lob/itch.hpp"
#include "lob/sim.hpp"
#include "lob/strategy.hpp"

using namespace lob;
using namespace lob::sim;

namespace {

constexpr std::uint16_t kLoc = 9;
constexpr std::uint64_t kOpen = 34'200'000'000'000ull;  // 09:30:00
constexpr std::uint64_t kUs = 1'000;

// Encodes ITCH messages byte by byte, at the offsets in the ITCH 5.0 spec.
struct Wire {
  std::vector<std::uint8_t> b;
  Wire(char type, std::uint16_t locate, std::uint64_t ts) : b(itch::kLengths[static_cast<std::uint8_t>(type)], 0) {
    b[0] = static_cast<std::uint8_t>(type);
    put(1, locate, 2);
    put(5, ts, 6);
  }
  Wire& put(std::size_t off, std::uint64_t v, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i) b[off + i] = static_cast<std::uint8_t>(v >> (8 * (width - 1 - i)));
    return *this;
  }
  Wire& chr(std::size_t off, char c) {
    b[off] = static_cast<std::uint8_t>(c);
    return *this;
  }
  Wire& sym(std::size_t off, const std::string& s) {
    for (std::size_t i = 0; i < 8; ++i) b[off + i] = static_cast<std::uint8_t>(i < s.size() ? s[i] : ' ');
    return *this;
  }
};

// A strategy that quotes whatever the test sets.
struct Scripted : Strategy {
  QuoteRequest want;
  int calls = 0;
  QuoteRequest quote(const MarketState&) override {
    ++calls;
    return want;
  }
  std::string name() const override { return "scripted"; }
};

Quote bid(Price p, std::uint32_t n = 100) { return {true, p, n}; }

// One stock, "TEST", open and trading, with a resting bid of 300 at $10.00 (ref 1) and a resting
// ask of 200 at $10.01 (ref 2). Latency 10 us.
struct SimTest : ::testing::Test {
  Scripted strat;
  Config cfg = [] {
    Config c;
    c.latency_ns = 10 * kUs;
    return c;
  }();
  std::unique_ptr<Simulator> sim;
  std::uint64_t t = kOpen;
  std::uint64_t next_ref = 100;

  void SetUp() override { start(); }

  void start() {
    sim = std::make_unique<Simulator>(std::vector<std::string>{"TEST"}, strat, cfg);
    send(Wire('S', 0, t).chr(11, 'Q'));
    send(Wire('R', kLoc, t).sym(11, "TEST").chr(19, 'Q').put(21, 100, 4));
    add(1, 'B', 300, 10'0000);
    add(2, 'S', 200, 10'0100);
    send(Wire('Q', kLoc, t += kUs).put(11, 0, 8).sym(19, "TEST").put(27, 10'0050, 4).chr(39, 'O'));  // opening cross
  }

  void send(const Wire& w) { sim->process(w.b.data(), static_cast<std::uint16_t>(w.b.size())); }
  void add(std::uint64_t ref, char side, std::uint32_t shares, Price price) {
    send(Wire('A', kLoc, t += kUs).put(11, ref, 8).chr(19, side).put(20, shares, 4).sym(24, "TEST").put(32, price, 4));
  }
  void exec(std::uint64_t ref, std::uint32_t shares) {
    send(Wire('E', kLoc, t += kUs).put(11, ref, 8).put(19, shares, 4).put(23, 1, 8));
  }
  void cancel(std::uint64_t ref, std::uint32_t shares) {
    send(Wire('X', kLoc, t += kUs).put(11, ref, 8).put(19, shares, 4));
  }
  void hidden(Price price, std::uint32_t shares) {
    send(Wire('P', kLoc, t += kUs).chr(19, 'B').put(20, shares, 4).sym(24, "TEST").put(32, price, 4).put(36, 2, 8));
  }
  // A book change far from the touch: makes the strategy re-quote without affecting our level.
  void nudge() { add(next_ref++, 'B', 100, 9'0000); }
  // Lets time pass, by sending harmless messages, so in-flight orders arrive and markouts settle.
  void wait(std::uint64_t ns) {
    const std::uint64_t until = t + ns;
    const std::uint64_t step = ns >= 10'000'000 ? 1'000'000 : kUs;
    while (t < until) send(Wire('I', kLoc, t += step));
  }
  // Quotes a bid and waits until it has arrived at the exchange.
  void rest_bid(Price p, std::uint32_t n = 100) {
    strat.want.bid = bid(p, n);
    nudge();
    wait(cfg.latency_ns);
  }
  const std::vector<Fill>& fills() { return sim->fills(); }
  std::int64_t position() { return sim->results().at(0).position; }
};

}  // namespace

TEST_F(SimTest, JoinsBehindDisplayedSharesAndFillsWhenExecutionsReachIt) {
  rest_bid(10'0000);
  exec(1, 200);  // 100 of the 300 ahead remain
  EXPECT_TRUE(fills().empty());
  add(3, 'B', 100, 10'0000);  // joins behind us
  exec(1, 100);               // the last shares ahead
  EXPECT_TRUE(fills().empty());
  exec(3, 60);  // an execution behind us means it reached us first
  ASSERT_EQ(fills().size(), 1u);
  EXPECT_EQ(fills()[0].shares, 60u);
  EXPECT_EQ(fills()[0].price, 10'0000u);
  EXPECT_EQ(fills()[0].reason, FillReason::kQueue);
  EXPECT_EQ(fills()[0].ahead_at_arrival, 300u);
  EXPECT_EQ(position(), 60);
}

TEST_F(SimTest, ExecutionsBeforeArrivalDoNotCount) {
  strat.want.bid = bid(10'0000);
  nudge();        // order sent; arrives 10 us later
  exec(1, 300);   // the whole level trades while our order is still in flight
  wait(cfg.latency_ns);
  EXPECT_TRUE(fills().empty());
  add(3, 'B', 100, 10'0000);
  exec(3, 100);  // arrived with nothing ahead, so this reaches us
  ASSERT_EQ(fills().size(), 1u);
  EXPECT_EQ(fills()[0].ahead_at_arrival, 0u);
}

TEST_F(SimTest, ProportionalCancelModelMovesUsUp) {
  rest_bid(10'0000);          // 300 ahead
  add(3, 'B', 100, 10'0000);  // 100 behind; level = 400
  cancel(3, 100);             // really from behind us, but the model can't tell
  exec(1, 225);               // proportional: 300 * 100/400 = 75 assumed cancelled ahead -> 225 ahead
  EXPECT_TRUE(fills().empty());
  exec(1, 50);  // 75 left in the level, all of it ahead of us in the model -> reaches us
  ASSERT_EQ(fills().size(), 1u);
  EXPECT_EQ(fills()[0].shares, 50u);
}

TEST_F(SimTest, PessimisticCancelModelKeepsOurPlace) {
  cfg.cancel_model = CancelModel::kPessimistic;
  start();
  rest_bid(10'0000);
  add(3, 'B', 100, 10'0000);
  cancel(3, 100);
  exec(1, 299);
  EXPECT_TRUE(fills().empty());
  exec(1, 1);  // the last share ahead
  EXPECT_TRUE(fills().empty());
}

TEST_F(SimTest, AheadNeverExceedsTheDisplayedLevel) {
  cfg.cancel_model = CancelModel::kPessimistic;
  start();
  rest_bid(10'0000);  // 300 ahead
  cancel(1, 250);     // level now 50: at most 50 can be ahead
  add(3, 'B', 100, 10'0000);
  exec(1, 50);
  exec(3, 10);
  ASSERT_EQ(fills().size(), 1u);
  EXPECT_EQ(fills()[0].shares, 10u);
}

TEST_F(SimTest, TradeThroughAtAWorsePriceFillsUs) {
  add(3, 'B', 500, 9'9900);
  rest_bid(10'0000);
  exec(3, 80);  // someone sold at $9.99 while our bid was at $10.00
  ASSERT_EQ(fills().size(), 1u);
  EXPECT_EQ(fills()[0].shares, 80u);
  EXPECT_EQ(fills()[0].reason, FillReason::kTradeThrough);
}

TEST_F(SimTest, HiddenTradeBelowOurBidFillsUs) {
  rest_bid(10'0000);
  hidden(9'9950, 40);
  ASSERT_EQ(fills().size(), 1u);
  EXPECT_EQ(fills()[0].shares, 40u);
  hidden(10'0000, 40);  // at our price: not evidence either way, so no fill
  EXPECT_EQ(fills().size(), 1u);
}

TEST_F(SimTest, CrossingAddFillsAnImprovedQuote) {
  exec(1, 300);  // market best bid is now gone; our $10.00 bid will be alone at the top
  add(3, 'B', 100, 9'9900);
  rest_bid(10'0000);
  add(4, 'S', 70, 10'0000);  // a real ask added at our bid price would have traded with us
  ASSERT_EQ(fills().size(), 1u);
  EXPECT_EQ(fills()[0].shares, 70u);
  EXPECT_EQ(fills()[0].reason, FillReason::kCross);
}

TEST_F(SimTest, PostOnlyRejectsAnOrderThatWouldCross) {
  strat.want.bid = bid(10'0100);  // at the best ask
  nudge();
  wait(cfg.latency_ns);
  EXPECT_EQ(sim->results().at(0).stats.rejects, 1u);
  exec(2, 200);
  EXPECT_TRUE(fills().empty());
}

TEST_F(SimTest, OrderCanFillWhileItsCancelIsInFlight) {
  cfg.cancel_model = CancelModel::kPessimistic;
  start();
  rest_bid(10'0000);
  exec(1, 300);           // nothing ahead now
  strat.want.bid = {};    // pull the quote: cancel sent now, arrives in 10 us
  nudge();
  add(3, 'B', 100, 10'0000);
  exec(3, 100);  // reaches us before the cancel does
  ASSERT_EQ(fills().size(), 1u);
  wait(cfg.latency_ns);
  add(4, 'B', 100, 10'0000);
  exec(4, 100);  // cancelled by now
  EXPECT_EQ(fills().size(), 1u);
}

TEST_F(SimTest, NoQuotingDuringHaltOrCooldown) {
  rest_bid(10'0000);
  send(Wire('H', kLoc, t += kUs).sym(11, "TEST").chr(19, 'P'));  // LULD pause
  wait(cfg.latency_ns);  // our cancel arrives
  const int calls = strat.calls;
  nudge();
  EXPECT_EQ(strat.calls, calls);  // not asked to quote while paused
  send(Wire('H', kLoc, t += kUs).sym(11, "TEST").chr(19, 'T'));
  nudge();
  EXPECT_EQ(strat.calls, calls);  // still in the 1 s cooldown
  wait(cfg.reopen_cooldown_ns);
  nudge();
  EXPECT_GT(strat.calls, calls);
  exec(1, 300);
  EXPECT_TRUE(fills().empty());  // the old order was cancelled during the pause
}

TEST_F(SimTest, MarkoutsUseTheMidAtEachHorizon) {
  rest_bid(10'0000);
  exec(1, 300);
  add(3, 'B', 100, 10'0000);
  exec(3, 100);  // reaches us: we buy 100 at $10.00
  ASSERT_EQ(fills().size(), 1u);
  EXPECT_DOUBLE_EQ(fills()[0].mid, 10'0050.0);  // mid just before the fill: (10.00 + 10.01) / 2
  // Now the best bid is the $9.00 nudges; a new ask at $10.00 makes the mid (9.00 + 10.00) / 2.
  add(5, 'S', 100, 10'0000);
  wait(200'000'000);  // past the 100 ms horizon
  EXPECT_DOUBLE_EQ(fills()[0].mid_after[0], 9'5000.0);
  EXPECT_DOUBLE_EQ(fills()[0].mid_after[3], 0.0);  // 30 s horizon not reached yet
  sim->finish();                                  // settles it with the last mid
  EXPECT_DOUBLE_EQ(fills()[0].mid_after[3], 9'5000.0);
}

TEST(MarketMaker, BaselineJoinsTheTouchAndRespectsLimits) {
  MarketMakerParams p;
  MarketMaker mm(p);
  MarketBook book;
  const std::string name = "TEST";
  MarketState s{kOpen, kLoc, name, book, Top{10'0000, 300, 10'0100, 200}, 0};
  QuoteRequest q = mm.quote(s);
  EXPECT_EQ(q.bid.price, 10'0000u);
  EXPECT_EQ(q.ask.price, 10'0100u);
  s.position = 500;  // at the long limit: no more buying
  q = mm.quote(s);
  EXPECT_FALSE(q.bid.active);
  EXPECT_TRUE(q.ask.active);
}

TEST(MarketMaker, SkewShiftsQuotesAgainstInventoryWithoutCrossing) {
  MarketMakerParams p;
  p.skew_k = 1.0;
  MarketMaker mm(p);
  MarketBook book;
  const std::string name = "TEST";
  MarketState s{kOpen, kLoc, name, book, Top{10'0000, 300, 10'0300, 200}, 200};  // long 2 lots
  QuoteRequest q = mm.quote(s);
  EXPECT_EQ(q.bid.price, 9'9800u);   // 2 ticks lower
  EXPECT_EQ(q.ask.price, 10'0100u);  // 2 ticks lower, inside the spread
  s.position = -500 + 100;           // short 4 lots: quotes shift up 4 ticks
  q = mm.quote(s);
  EXPECT_EQ(q.bid.price, 10'0200u);  // clamped one tick below the ask
}
