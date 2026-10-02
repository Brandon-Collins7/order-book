#pragma once

// Decoding of Nasdaq TotalView-ITCH 5.0 messages.
//
// Every message starts with the same 11-byte header:
//   [0] type  [1..2] stock locate  [3..4] tracking number  [5..10] timestamp (ns since midnight)
// Field offsets below are byte offsets from the type byte, as in the ITCH 5.0 specification.
//
// dispatch() checks the length, decodes the message into one of the small structs below, and
// calls handler.on(msg). The handler type is a template parameter, so every call is resolved at
// compile time and can be inlined: no virtual calls and no allocation per message. Prices are
// kept as integers in units of 1/10,000 of a dollar, exactly as sent.

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include "lob/endian.hpp"

namespace lob::itch {

using Price = std::uint32_t;  // $ x 10^4
constexpr double kPriceScale = 10'000.0;

// An 8-character, right-space-padded stock symbol.
struct Symbol {
  std::array<char, 8> chars{};

  std::string_view view() const {
    std::size_t n = chars.size();
    while (n > 0 && chars[n - 1] == ' ') --n;
    return {chars.data(), n};
  }
  friend bool operator==(const Symbol&, const Symbol&) = default;
};

struct Header {
  char type;
  std::uint16_t locate;
  std::uint64_t timestamp;
};

struct SystemEvent : Header {  // 'S'
  char event;                  // O start of messages, S start of system hours, Q market open, M close, E end of system hours, C end of messages
};
struct StockDirectory : Header {  // 'R'
  Symbol stock;
  char market_category;  // Q/G/S = Nasdaq tiers, N = NYSE, A = NYSE American, P = NYSE Arca, Z = Cboe BZX, V = IEX
  std::uint32_t round_lot_size;
};
struct TradingAction : Header {  // 'H'
  Symbol stock;
  char state;  // H halted, P paused, Q quotation only, T trading
};
struct AddOrder : Header {  // 'A' (no attribution) and 'F' (with MPID)
  std::uint64_t ref;
  char side;  // B or S
  std::uint32_t shares;
  Symbol stock;
  Price price;
  std::array<char, 4> mpid;  // all spaces for 'A'
};
struct OrderExecuted : Header {  // 'E': executes at the resting order's price
  std::uint64_t ref;
  std::uint32_t shares;
  std::uint64_t match;
};
struct OrderExecutedWithPrice : Header {  // 'C': executes at a different price (e.g. in a cross)
  std::uint64_t ref;
  std::uint32_t shares;
  std::uint64_t match;
  char printable;  // N = not printed to the tape (avoid double counting with the cross)
  Price price;
};
struct OrderCancel : Header {  // 'X': partial cancel
  std::uint64_t ref;
  std::uint32_t shares;
};
struct OrderDelete : Header {  // 'D'
  std::uint64_t ref;
};
struct OrderReplace : Header {  // 'U': cancel orig_ref and add new_ref at the same side; loses queue priority
  std::uint64_t orig_ref;
  std::uint64_t new_ref;
  std::uint32_t shares;
  Price price;
};
struct Trade : Header {  // 'P': execution against a non-displayed order; does not change the visible book
  std::uint64_t ref;
  char side;
  std::uint32_t shares;
  Symbol stock;
  Price price;
  std::uint64_t match;
};
struct CrossTrade : Header {  // 'Q': opening, closing, IPO or halt cross
  std::uint64_t shares;
  Symbol stock;
  Price price;
  std::uint64_t match;
  char cross_type;  // O open, C close, H halt/IPO, I intraday
};
struct BrokenTrade : Header {  // 'B'
  std::uint64_t match;
};
// Any other valid message type, decoded only as far as the header.
struct Other : Header {};

// Expected total length of each message type (the type byte included), or 0 if unknown.
constexpr std::array<std::uint8_t, 256> kLengths = [] {
  std::array<std::uint8_t, 256> t{};
  t['S'] = 12; t['R'] = 39; t['H'] = 25; t['Y'] = 20; t['L'] = 26; t['V'] = 35; t['W'] = 12;
  t['K'] = 28; t['J'] = 35; t['h'] = 21; t['A'] = 36; t['F'] = 40; t['E'] = 31; t['C'] = 36;
  t['X'] = 23; t['D'] = 19; t['U'] = 35; t['P'] = 44; t['Q'] = 40; t['B'] = 19; t['I'] = 50;
  t['N'] = 20; t['O'] = 48;
  return t;
}();

class DecodeError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

namespace detail {

inline Header header(const std::uint8_t* m) {
  return {static_cast<char>(m[0]), be16(m + 1), be48(m + 5)};
}
inline Symbol symbol(const std::uint8_t* p) {
  Symbol s;
  for (std::size_t i = 0; i < s.chars.size(); ++i) s.chars[i] = static_cast<char>(p[i]);
  return s;
}
inline char ch(const std::uint8_t* p) { return static_cast<char>(*p); }

}  // namespace detail

// Decodes one message (msg[0] is the type byte, len is its length) and calls handler.on(...)
// with the decoded struct. Throws DecodeError for an unknown type or a wrong length.
template <class Handler>
inline void dispatch(const std::uint8_t* m, std::uint16_t len, Handler& h) {
  using namespace detail;
  const std::uint8_t want = kLengths[m[0]];
  if (want == 0) throw DecodeError(std::string("unknown message type '") + ch(m) + "'");
  if (len != want)
    throw DecodeError(std::string("message '") + ch(m) + "' has length " + std::to_string(len) +
                      ", expected " + std::to_string(want));
  const Header hd = header(m);
  switch (m[0]) {
    case 'A':
      h.on(AddOrder{hd, be64(m + 11), ch(m + 19), be32(m + 20), symbol(m + 24), be32(m + 32), {' ', ' ', ' ', ' '}});
      break;
    case 'F':
      h.on(AddOrder{hd, be64(m + 11), ch(m + 19), be32(m + 20), symbol(m + 24), be32(m + 32),
                    {ch(m + 36), ch(m + 37), ch(m + 38), ch(m + 39)}});
      break;
    case 'E': h.on(OrderExecuted{hd, be64(m + 11), be32(m + 19), be64(m + 23)}); break;
    case 'C':
      h.on(OrderExecutedWithPrice{hd, be64(m + 11), be32(m + 19), be64(m + 23), ch(m + 31), be32(m + 32)});
      break;
    case 'X': h.on(OrderCancel{hd, be64(m + 11), be32(m + 19)}); break;
    case 'D': h.on(OrderDelete{hd, be64(m + 11)}); break;
    case 'U': h.on(OrderReplace{hd, be64(m + 11), be64(m + 19), be32(m + 27), be32(m + 31)}); break;
    case 'P':
      h.on(Trade{hd, be64(m + 11), ch(m + 19), be32(m + 20), symbol(m + 24), be32(m + 32), be64(m + 36)});
      break;
    case 'Q': h.on(CrossTrade{hd, be64(m + 11), symbol(m + 19), be32(m + 27), be64(m + 31), ch(m + 39)}); break;
    case 'B': h.on(BrokenTrade{hd, be64(m + 11)}); break;
    case 'S': h.on(SystemEvent{hd, ch(m + 11)}); break;
    case 'R': h.on(StockDirectory{hd, symbol(m + 11), ch(m + 19), be32(m + 21)}); break;
    case 'H': h.on(TradingAction{hd, symbol(m + 11), ch(m + 19)}); break;
    default: h.on(Other{hd}); break;
  }
}

// Base for handlers that only care about some message types. Derive from it, add
// `using lob::itch::Handler::on;`, and overload on() for the types you want.
struct Handler {
  void on(const SystemEvent&) {}
  void on(const StockDirectory&) {}
  void on(const TradingAction&) {}
  void on(const AddOrder&) {}
  void on(const OrderExecuted&) {}
  void on(const OrderExecutedWithPrice&) {}
  void on(const OrderCancel&) {}
  void on(const OrderDelete&) {}
  void on(const OrderReplace&) {}
  void on(const Trade&) {}
  void on(const CrossTrade&) {}
  void on(const BrokenTrade&) {}
  void on(const Other&) {}
};

}  // namespace lob::itch
