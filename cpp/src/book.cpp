#include "lob/book.hpp"

#include <algorithm>

namespace lob {

// ---- Book -------------------------------------------------------------------

Level& Book::level_of(const Order& o) {
  return o.side == 'B' ? bids_[o.price] : asks_[o.price];
}

void Book::add(Order& o) {
  Level& lv = level_of(o);
  o.prev = lv.tail;
  o.next = nullptr;
  if (lv.tail) lv.tail->next = &o;
  else lv.head = &o;
  lv.tail = &o;
  lv.shares += o.shares;
  ++lv.orders;
}

void Book::reduce(Order& o, std::uint32_t shares) {
  level_of(o).shares -= shares;
  o.shares -= shares;
}

void Book::remove(Order& o) {
  auto unlink = [&o](Level& lv) {
    if (o.prev) o.prev->next = o.next;
    else lv.head = o.next;
    if (o.next) o.next->prev = o.prev;
    else lv.tail = o.prev;
    lv.shares -= o.shares;
    --lv.orders;
    return lv.orders == 0;
  };
  if (o.side == 'B') {
    auto it = bids_.find(o.price);
    if (unlink(it->second)) bids_.erase(it);
  } else {
    auto it = asks_.find(o.price);
    if (unlink(it->second)) asks_.erase(it);
  }
  o.prev = o.next = nullptr;
}

Top Book::top() const {
  Top t;
  if (!bids_.empty()) {
    t.bid_px = bids_.begin()->first;
    t.bid_sz = bids_.begin()->second.shares;
  }
  if (!asks_.empty()) {
    t.ask_px = asks_.begin()->first;
    t.ask_sz = asks_.begin()->second.shares;
  }
  return t;
}

bool Book::crossed() const {
  return !bids_.empty() && !asks_.empty() && bids_.begin()->first > asks_.begin()->first;
}

bool Book::locked() const {
  return !bids_.empty() && !asks_.empty() && bids_.begin()->first == asks_.begin()->first;
}

std::size_t Book::validate() const {
  std::size_t problems = 0;
  auto check = [&](Price price, const Level& lv, char side) {
    std::uint64_t shares = 0;
    std::uint32_t orders = 0;
    const Order* prev = nullptr;
    for (const Order* o = lv.head; o; prev = o, o = o->next) {
      if (o->prev != prev || o->price != price || o->side != side || o->shares == 0) ++problems;
      shares += o->shares;
      if (++orders > lv.orders) break;  // guards against a cycle
    }
    if (prev != lv.tail || shares != lv.shares || orders != lv.orders || orders == 0) ++problems;
  };
  for (const auto& [p, lv] : bids_) check(p, lv, 'B');
  for (const auto& [p, lv] : asks_) check(p, lv, 'S');
  return problems;
}

// ---- BookBuilder --------------------------------------------------------------

BookBuilder::BookBuilder(const std::vector<std::string>& symbols)
    : wanted_(symbols.begin(), symbols.end()), all_symbols_(symbols.empty()) {}

void BookBuilder::process(const std::uint8_t* msg, std::uint16_t len) {
  touched_ = nullptr;
  itch::dispatch(msg, len, *this);
}

void BookBuilder::touch(Book& b, std::uint16_t locate) {
  touched_ = &b;
  touched_locate_ = locate;
  if (regular_hours_ && trading_state_[locate] == 'T') {
    const bool crossed = b.crossed();
    if (crossed || b.locked()) {
      ++(crossed ? stats_.crossed : stats_.locked);
      ++stats_.crossed_or_locked_by_locate[locate];
    }
  }
}

void BookBuilder::on(const itch::SystemEvent& m) {
  if (m.event == 'Q') regular_hours_ = true;   // start of market hours
  if (m.event == 'M') regular_hours_ = false;  // end of market hours
}

void BookBuilder::on(const itch::StockDirectory& m) {
  // The directory can repeat a stock (seen for CFG-D on 2019-01-30); keep the existing book.
  symbols_[m.locate] = std::string(m.stock.view());
  if (!books_[m.locate] && (all_symbols_ || wanted_.count(symbols_[m.locate])))
    books_[m.locate] = std::make_unique<Book>();
}

void BookBuilder::on(const itch::TradingAction& m) { trading_state_[m.locate] = m.state; }

void BookBuilder::add(Book& b, std::uint16_t locate, std::uint64_t ref, char side, std::uint32_t shares,
                      Price price) {
  auto [it, inserted] = orders_.try_emplace(ref);
  if (!inserted) {
    ++stats_.duplicate_ref;
    return;
  }
  Order& o = it->second;
  o.ref = ref;
  o.price = price;
  o.shares = shares;
  o.locate = locate;
  o.side = side;
  b.add(o);
  stats_.max_live_orders = std::max<std::uint64_t>(stats_.max_live_orders, orders_.size());
  touch(b, locate);
}

void BookBuilder::on(const itch::AddOrder& m) {
  if (Book* b = tracked(m.locate)) add(*b, m.locate, m.ref, m.side, m.shares, m.price);
}

void BookBuilder::take_shares(std::uint16_t locate, std::uint64_t ref, std::uint32_t shares) {
  Book* b = tracked(locate);
  if (!b) return;
  auto it = orders_.find(ref);
  if (it == orders_.end()) {
    ++stats_.unknown_ref;
    return;
  }
  Order& o = it->second;
  if (shares < o.shares) {
    b->reduce(o, shares);
  } else {
    if (shares > o.shares) ++stats_.overfill;
    b->remove(o);
    orders_.erase(it);
  }
  touch(*b, locate);
}

void BookBuilder::on(const itch::OrderExecuted& m) { take_shares(m.locate, m.ref, m.shares); }
void BookBuilder::on(const itch::OrderExecutedWithPrice& m) { take_shares(m.locate, m.ref, m.shares); }
void BookBuilder::on(const itch::OrderCancel& m) { take_shares(m.locate, m.ref, m.shares); }

void BookBuilder::on(const itch::OrderDelete& m) {
  Book* b = tracked(m.locate);
  if (!b) return;
  auto it = orders_.find(m.ref);
  if (it == orders_.end()) {
    ++stats_.unknown_ref;
    return;
  }
  b->remove(it->second);
  orders_.erase(it);
  touch(*b, m.locate);
}

void BookBuilder::on(const itch::OrderReplace& m) {
  Book* b = tracked(m.locate);
  if (!b) return;
  auto it = orders_.find(m.orig_ref);
  if (it == orders_.end()) {
    ++stats_.unknown_ref;
    return;
  }
  const char side = it->second.side;  // a replace keeps the side; the new order joins the back of the queue
  b->remove(it->second);
  orders_.erase(it);
  add(*b, m.locate, m.new_ref, side, m.shares, m.price);
  touch(*b, m.locate);
}

}  // namespace lob
