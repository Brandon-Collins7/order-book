#pragma once

// A configurable market maker covering the 2x2 experiment in PLAN.md:
//
//   baseline         skew_k = 0, imbalance filter off: join the best bid and best ask
//   inventory skew   shift both quotes by -skew_k * (position / size) ticks, rounded:
//                    when long, quote lower to sell more and buy less (and the reverse when short)
//   imbalance filter imbalance I = (bid shares - ask shares) / (bid shares + ask shares) over the
//                    top `imbalance_levels` levels. If I < -theta the bid side is thin and the price
//                    is likely to tick down, so don't bid; if I > theta, don't offer.
//
// Quotes never cross the market (they are clamped one tick inside the opposite best price), and
// a side is not quoted if a fill there could push |position| past max_position.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>

#include "lob/sim.hpp"

namespace lob::sim {

struct MarketMakerParams {
  std::uint32_t size = 100;          // shares per quote (one round lot)
  std::int64_t max_position = 500;   // shares
  double skew_k = 0.0;               // ticks of quote shift per `size` shares of inventory
  double imbalance_theta = 2.0;      // > 1 disables the filter (|I| <= 1 always)
  std::size_t imbalance_levels = 1;  // book levels per side used for the imbalance
  Price tick = 100;                  // $0.01 in ITCH price units
};

class MarketMaker : public Strategy {
 public:
  explicit MarketMaker(MarketMakerParams p) : p_(p) {}

  std::string name() const override {
    std::ostringstream s;
    s << "mm(size=" << p_.size << ",max_pos=" << p_.max_position << ",k=" << p_.skew_k << ",theta=" << p_.imbalance_theta
      << ",levels=" << p_.imbalance_levels << ")";
    return s.str();
  }

  QuoteRequest quote(const MarketState& m) override {
    QuoteRequest q;
    const auto tick = static_cast<std::int64_t>(p_.tick);
    const auto shift = static_cast<std::int64_t>(
        std::llround(-p_.skew_k * static_cast<double>(m.position) / static_cast<double>(p_.size)));

    std::int64_t bid = static_cast<std::int64_t>(m.top.bid_px) + shift * tick;
    std::int64_t ask = static_cast<std::int64_t>(m.top.ask_px) + shift * tick;
    bid = std::min(bid, static_cast<std::int64_t>(m.top.ask_px) - tick);  // never cross the market
    ask = std::max(ask, static_cast<std::int64_t>(m.top.bid_px) + tick);

    bool want_bid = bid > 0 && m.position + static_cast<std::int64_t>(p_.size) <= p_.max_position;
    bool want_ask = m.position - static_cast<std::int64_t>(p_.size) >= -p_.max_position;

    if (p_.imbalance_theta <= 1.0) {
      const double imb = imbalance(m);
      if (imb < -p_.imbalance_theta) want_bid = false;
      if (imb > p_.imbalance_theta) want_ask = false;
    }
    if (want_bid) q.bid = {true, static_cast<Price>(bid), p_.size};
    if (want_ask) q.ask = {true, static_cast<Price>(ask), p_.size};
    return q;
  }

 private:
  double imbalance(const MarketState& m) const {
    double b = 0, a = 0;
    for (std::size_t k = 0; k < p_.imbalance_levels; ++k) {
      if (const auto* lv = m.book.level('B', k)) b += static_cast<double>(lv->shares);
      if (const auto* lv = m.book.level('S', k)) a += static_cast<double>(lv->shares);
    }
    return b + a > 0 ? (b - a) / (b + a) : 0.0;
  }

  MarketMakerParams p_;
};

}  // namespace lob::sim
