#include "lob/sim.hpp"

#include <algorithm>
#include <cmath>

#include "lob/endian.hpp"

namespace lob::sim {

namespace {
constexpr std::uint64_t kClose = 16ull * 3600 * 1'000'000'000;  // 16:00:00 in ns since midnight
}

Simulator::Simulator(const std::vector<std::string>& symbols, Strategy& strategy, Config config)
    : config_(config), strategy_(strategy), builder_(symbols), wanted_(symbols) {
  builder_.set_listener(this);
  events_.sim = this;
}

void Simulator::process(const std::uint8_t* msg, std::uint16_t len) {
  const std::uint64_t ts = be48(msg + 5);
  advance(ts);
  builder_.process(msg, len);       // real book; calls on_book_event for each resting-order change
  itch::dispatch(msg, len, events_);  // system events, directory, trading state, crosses, hidden trades
  if (const auto* b = builder_.touched()) {
    const std::uint16_t loc = builder_.touched_locate();
    Symbol& s = symbols_[loc];
    const Top t = b->top();
    s.mid = t.bid_px && t.ask_px ? (static_cast<double>(t.bid_px) + t.ask_px) / 2.0 : 0.0;
    if (s.tracked) requote(loc, ts);
  }
}

// Everything due before the message at `ts`: markouts, order arrivals, cancel arrivals, and the
// end-of-day stop. Runs before the message is applied, so it sees the market as of that moment.
void Simulator::advance(std::uint64_t ts) {
  now_ = ts;
  for (std::size_t h = 0; h < kHorizons; ++h) {
    auto& q = markouts_[h];
    while (!q.empty() && q.front().due <= ts) {
      Fill& f = fills_[q.front().fill];
      f.mid_after[h] = symbols_[f.locate].mid;
      q.pop_front();
    }
  }
  if (!stopped_ && ts + config_.stop_before_close_ns >= kClose) {
    stopped_ = true;
    for (std::uint16_t loc : tracked_) requote(loc, ts);
  }
  if (ts < next_action_) return;
  for (std::uint16_t loc : tracked_) {
    Symbol& s = symbols_[loc];
    for (SimOrder& o : s.orders) {
      if (!o.live && !o.done && o.arrives <= ts) arrive(s, loc, o);
      if (o.cancel_pending && !o.done && o.cancel_arrives <= ts) o.done = true;
    }
    std::erase_if(s.orders, [](const SimOrder& o) { return o.done; });
  }
  schedule_next_action();
}

void Simulator::schedule_next_action() {
  next_action_ = ~0ull;
  for (std::uint16_t loc : tracked_) {
    for (const SimOrder& o : symbols_[loc].orders) {
      if (o.done) continue;
      if (!o.live) next_action_ = std::min(next_action_, o.arrives);
      if (o.cancel_pending) next_action_ = std::min(next_action_, o.cancel_arrives);
    }
  }
}

bool Simulator::can_quote(const Symbol& s, std::uint64_t ts) const {
  return regular_hours_ && !stopped_ && s.opened && s.trading_state == 'T' &&
         ts >= s.reopened_at + config_.reopen_cooldown_ns;
}

void Simulator::requote(std::uint16_t locate, std::uint64_t ts) {
  Symbol& s = symbols_[locate];
  std::erase_if(s.orders, [](const SimOrder& o) { return o.done; });
  QuoteRequest want;
  const MarketBook* book = builder_.book(locate);
  if (book && can_quote(s, ts)) {
    const Top t = book->top();
    if (t.bid_px && t.ask_px && t.bid_px < t.ask_px)
      want = strategy_.quote(MarketState{ts, locate, s.name, *book, t, s.position});
  }
  reconcile(s, 'B', want.bid, ts);
  reconcile(s, 'S', want.ask, ts);
  schedule_next_action();
}

void Simulator::reconcile(Symbol& s, char side, const Quote& want, std::uint64_t ts) {
  const bool wanted = want.active && want.shares > 0;
  for (SimOrder& o : s.orders) {
    if (o.side != side || o.done || o.cancel_pending) continue;
    if (wanted && o.price == want.price) return;  // keep it, and its place in line
    o.cancel_pending = true;
    o.cancel_arrives = ts + config_.latency_ns;
    ++s.stats.cancels_sent;
  }
  if (!wanted) return;
  SimOrder o{};
  o.id = next_id_++;
  o.side = side;
  o.price = want.price;
  o.shares = want.shares;
  o.arrives = ts + config_.latency_ns;
  s.orders.push_back(o);
  ++s.stats.orders_sent;
}

void Simulator::arrive(Symbol& s, std::uint16_t locate, SimOrder& o) {
  const MarketBook* book = builder_.book(locate);
  const Top t = book->top();
  const bool crosses = o.side == 'B' ? t.ask_px && o.price >= t.ask_px : t.bid_px && o.price <= t.bid_px;
  if (crosses || s.trading_state != 'T') {  // post-only: never take liquidity
    ++s.stats.rejects;
    o.done = true;
    return;
  }
  o.live = true;
  o.ahead = o.ahead_at_arrival = book->shares_at(o.side, o.price);
}

void Simulator::on_book_event(fast::BookEvent kind, std::uint16_t locate, char side, Price price,
                              std::uint32_t shares, std::uint64_t /*ref*/) {
  Symbol& s = symbols_[locate];
  if (!s.tracked || s.orders.empty()) return;
  const MarketBook* book = builder_.book(locate);
  for (SimOrder& o : s.orders) {
    if (!o.live || o.done) continue;
    if (side == o.side) {
      if (price == o.price) {
        if (kind == fast::BookEvent::kExecute) {
          if (o.ahead >= shares) {
            o.ahead -= shares;
          } else {
            const std::uint64_t reaches_us = shares - o.ahead;
            o.ahead = 0;
            fill(s, locate, o, static_cast<std::uint32_t>(std::min<std::uint64_t>(reaches_us, o.shares)),
                 FillReason::kQueue);
          }
        } else if (kind == fast::BookEvent::kCancel && config_.cancel_model == CancelModel::kProportional) {
          const std::uint64_t level_before = book->shares_at(side, price) + shares;
          if (level_before > 0) {
            const auto from_ahead = static_cast<std::uint64_t>(
                std::llround(static_cast<double>(shares) * static_cast<double>(o.ahead) / level_before));
            o.ahead -= std::min(o.ahead, from_ahead);
          }
        }
        // Whatever the model, the shares ahead of us are part of the displayed level.
        o.ahead = std::min(o.ahead, book->shares_at(side, price));
      } else if (kind == fast::BookEvent::kExecute && (side == 'B' ? price < o.price : price > o.price)) {
        // Someone sold below our bid (or bought above our ask): they would have hit us first.
        fill(s, locate, o, std::min(shares, o.shares), FillReason::kTradeThrough);
      }
    } else if (kind == fast::BookEvent::kAdd && (o.side == 'B' ? price <= o.price : price >= o.price)) {
      // A real order was added at a price that crosses ours: it would have traded with us.
      fill(s, locate, o, std::min(shares, o.shares), FillReason::kCross);
    }
  }
}

void Simulator::fill(Symbol& s, std::uint16_t locate, SimOrder& o, std::uint32_t shares, FillReason reason) {
  if (shares == 0) return;
  o.shares -= shares;
  if (o.shares == 0) o.done = true;
  const std::int64_t signed_shares = o.side == 'B' ? shares : -static_cast<std::int64_t>(shares);
  s.position += signed_shares;
  s.cash -= signed_shares * static_cast<std::int64_t>(o.price);
  ++s.stats.fills;
  (o.side == 'B' ? s.stats.bought : s.stats.sold) += shares;

  Fill f{};
  f.ts = now_;
  f.locate = locate;
  f.side = o.side;
  f.price = o.price;
  f.shares = shares;
  f.position_after = s.position;
  f.mid = s.mid;  // updated only after the whole message is applied, so this is the pre-fill mid
  f.reason = reason;
  f.ahead_at_arrival = o.ahead_at_arrival;
  f.order_age_ns = now_ - o.arrives;
  fills_.push_back(f);
  for (std::size_t h = 0; h < kHorizons; ++h) markouts_[h].push_back({now_ + config_.markout_ns[h], fills_.size() - 1});
}

void Simulator::finish() {
  for (std::size_t h = 0; h < kHorizons; ++h) {
    for (const PendingMarkout& p : markouts_[h]) fills_[p.fill].mid_after[h] = symbols_[fills_[p.fill].locate].mid;
    markouts_[h].clear();
  }
}

std::vector<Simulator::SymbolResult> Simulator::results() const {
  std::vector<SymbolResult> out;
  for (std::uint16_t loc : tracked_) {
    const Symbol& s = symbols_[loc];
    const double mark = (s.close_mid > 0 ? s.close_mid : s.mid) / itch::kPriceScale;
    const double cash = static_cast<double>(s.cash) / itch::kPriceScale;
    out.push_back({s.name, loc, s.stats, s.position, cash, mark, cash + static_cast<double>(s.position) * mark});
  }
  return out;
}

// ---- non-book messages ----------------------------------------------------------

void Simulator::Events::on(const itch::SystemEvent& m) {
  if (m.event == 'Q') sim->regular_hours_ = true;  // start of market hours
  if (m.event == 'M') {                            // end of market hours
    sim->regular_hours_ = false;
    for (std::uint16_t loc : sim->tracked_) {
      Symbol& s = sim->symbols_[loc];
      s.close_mid = s.mid;
      sim->requote(loc, m.timestamp);
    }
  }
}

void Simulator::Events::on(const itch::StockDirectory& m) {
  Symbol& s = sim->symbols_[m.locate];
  s.name = std::string(m.stock.view());
  if (!s.tracked && std::find(sim->wanted_.begin(), sim->wanted_.end(), s.name) != sim->wanted_.end()) {
    s.tracked = true;
    sim->tracked_.push_back(m.locate);
  }
}

void Simulator::Events::on(const itch::TradingAction& m) {
  Symbol& s = sim->symbols_[m.locate];
  if (m.state == 'T' && s.trading_state != 'T') s.reopened_at = m.timestamp;
  s.trading_state = m.state;
  if (s.tracked) sim->requote(m.locate, m.timestamp);
}

void Simulator::Events::on(const itch::CrossTrade& m) {
  Symbol& s = sim->symbols_[m.locate];
  if (m.cross_type == 'O') s.opened = true;
  if (m.cross_type == 'H') s.reopened_at = m.timestamp;
  if (s.tracked) sim->requote(m.locate, m.timestamp);
}

// A non-displayed execution. ITCH 5.0 always reports these with side 'B', so the side is unknown;
// what matters is the price: a hidden trade below our bid means a seller was willing to sell below
// our bid (and above our ask, a buyer above our ask), and they would have traded with us.
void Simulator::Events::on(const itch::Trade& m) {
  Symbol& s = sim->symbols_[m.locate];
  if (!s.tracked) return;
  for (SimOrder& o : s.orders) {
    if (!o.live || o.done) continue;
    if (o.side == 'B' ? m.price < o.price : m.price > o.price)
      sim->fill(s, m.locate, o, std::min(m.shares, o.shares), FillReason::kTradeThrough);
  }
}

}  // namespace lob::sim
