#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "lob/gz_reader.hpp"
#include "lob/itch.hpp"

using namespace lob::itch;

namespace {

// Builds a message of the right length for its type, with fields written big-endian at the
// byte offsets given in the ITCH 5.0 spec.
struct Msg {
  std::vector<std::uint8_t> b;

  Msg(char type, std::uint16_t locate, std::uint64_t ts) : b(kLengths[static_cast<std::uint8_t>(type)], 0) {
    b[0] = static_cast<std::uint8_t>(type);
    put(1, locate, 2);
    put(5, ts, 6);
  }
  Msg& put(std::size_t off, std::uint64_t v, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i) b[off + i] = static_cast<std::uint8_t>(v >> (8 * (width - 1 - i)));
    return *this;
  }
  Msg& chr(std::size_t off, char c) {
    b[off] = static_cast<std::uint8_t>(c);
    return *this;
  }
  Msg& str(std::size_t off, const std::string& s, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i) b[off + i] = static_cast<std::uint8_t>(i < s.size() ? s[i] : ' ');
    return *this;
  }
};

using Any = std::variant<std::monostate, SystemEvent, StockDirectory, TradingAction, AddOrder, OrderExecuted,
                         OrderExecutedWithPrice, OrderCancel, OrderDelete, OrderReplace, Trade, CrossTrade,
                         BrokenTrade, Other>;

// Records the last decoded message.
struct Capture {
  Any last;
  template <class M>
  void on(const M& m) { last = m; }
};

template <class M>
M decode_as(const Msg& m) {
  Capture c;
  dispatch(m.b.data(), static_cast<std::uint16_t>(m.b.size()), c);
  EXPECT_TRUE(std::holds_alternative<M>(c.last));
  return std::get<M>(c.last);
}

constexpr std::uint64_t kTs = 0x0000123456789ABCull;  // uses all 6 timestamp bytes

}  // namespace

TEST(Itch, AddOrderWithoutAttribution) {
  Msg m('A', 4040, kTs);
  m.put(11, 0x0102030405060708ull, 8).chr(19, 'B').put(20, 300, 4).str(24, "IIVI", 8).put(32, 371'500, 4);
  const auto a = decode_as<AddOrder>(m);
  EXPECT_EQ(a.type, 'A');
  EXPECT_EQ(a.locate, 4040);
  EXPECT_EQ(a.timestamp, kTs);
  EXPECT_EQ(a.ref, 0x0102030405060708ull);
  EXPECT_EQ(a.side, 'B');
  EXPECT_EQ(a.shares, 300u);
  EXPECT_EQ(a.stock.view(), "IIVI");
  EXPECT_EQ(a.price, 371'500u);  // $37.15
  EXPECT_EQ(std::string(a.mpid.data(), 4), "    ");
}

TEST(Itch, AddOrderWithAttribution) {
  Msg m('F', 1, kTs);
  m.put(11, 7, 8).chr(19, 'S').put(20, 100, 4).str(24, "FLIR", 8).put(32, 480'000, 4).str(36, "GSCO", 4);
  const auto a = decode_as<AddOrder>(m);
  EXPECT_EQ(a.type, 'F');
  EXPECT_EQ(a.side, 'S');
  EXPECT_EQ(a.stock.view(), "FLIR");
  EXPECT_EQ(std::string(a.mpid.data(), 4), "GSCO");
}

TEST(Itch, Executions) {
  Msg e('E', 2, kTs);
  e.put(11, 99, 8).put(19, 250, 4).put(23, 0xAABBCCDDEEull, 8);
  const auto x = decode_as<OrderExecuted>(e);
  EXPECT_EQ(x.ref, 99u);
  EXPECT_EQ(x.shares, 250u);
  EXPECT_EQ(x.match, 0xAABBCCDDEEull);

  Msg c('C', 2, kTs);
  c.put(11, 99, 8).put(19, 10, 4).put(23, 5, 8).chr(31, 'N').put(32, 123'456, 4);
  const auto y = decode_as<OrderExecutedWithPrice>(c);
  EXPECT_EQ(y.shares, 10u);
  EXPECT_EQ(y.match, 5u);
  EXPECT_EQ(y.printable, 'N');
  EXPECT_EQ(y.price, 123'456u);
}

TEST(Itch, CancelDeleteReplace) {
  Msg x('X', 3, kTs);
  x.put(11, 42, 8).put(19, 60, 4);
  const auto cancel = decode_as<OrderCancel>(x);
  EXPECT_EQ(cancel.ref, 42u);
  EXPECT_EQ(cancel.shares, 60u);

  Msg d('D', 3, kTs);
  d.put(11, 42, 8);
  EXPECT_EQ(decode_as<OrderDelete>(d).ref, 42u);

  Msg u('U', 3, kTs);
  u.put(11, 42, 8).put(19, 43, 8).put(27, 500, 4).put(31, 99'900, 4);
  const auto r = decode_as<OrderReplace>(u);
  EXPECT_EQ(r.orig_ref, 42u);
  EXPECT_EQ(r.new_ref, 43u);
  EXPECT_EQ(r.shares, 500u);
  EXPECT_EQ(r.price, 99'900u);
}

TEST(Itch, TradesAndCross) {
  Msg p('P', 5, kTs);
  p.put(11, 0, 8).chr(19, 'B').put(20, 75, 4).str(24, "AAPL", 8).put(32, 1'650'000, 4).put(36, 777, 8);
  const auto t = decode_as<Trade>(p);
  EXPECT_EQ(t.side, 'B');
  EXPECT_EQ(t.shares, 75u);
  EXPECT_EQ(t.stock.view(), "AAPL");
  EXPECT_EQ(t.price, 1'650'000u);
  EXPECT_EQ(t.match, 777u);

  Msg q('Q', 5, kTs);
  q.put(11, 5'000'000'000ull, 8).str(19, "AAPL", 8).put(27, 1'651'000, 4).put(31, 778, 8).chr(39, 'O');
  const auto c = decode_as<CrossTrade>(q);
  EXPECT_EQ(c.shares, 5'000'000'000ull);  // cross size is 64-bit
  EXPECT_EQ(c.stock.view(), "AAPL");
  EXPECT_EQ(c.price, 1'651'000u);
  EXPECT_EQ(c.cross_type, 'O');
}

TEST(Itch, DirectoryAndSystemMessages) {
  Msg r('R', 13, kTs);
  r.str(11, "QQQ", 8).chr(19, 'G').put(21, 100, 4);
  const auto d = decode_as<StockDirectory>(r);
  EXPECT_EQ(d.stock.view(), "QQQ");
  EXPECT_EQ(d.market_category, 'G');
  EXPECT_EQ(d.round_lot_size, 100u);

  Msg s('S', 0, kTs);
  s.chr(11, 'Q');
  EXPECT_EQ(decode_as<SystemEvent>(s).event, 'Q');

  Msg h('H', 13, kTs);
  h.str(11, "QQQ", 8).chr(19, 'T');
  EXPECT_EQ(decode_as<TradingAction>(h).state, 'T');

  const auto o = decode_as<Other>(Msg('I', 13, kTs));  // NOII: header only
  EXPECT_EQ(o.type, 'I');
  EXPECT_EQ(o.locate, 13);
  EXPECT_EQ(o.timestamp, kTs);
}

TEST(Itch, WrongLengthThrows) {
  Msg m('D', 1, kTs);
  m.b.push_back(0);
  Capture c;
  EXPECT_THROW(dispatch(m.b.data(), static_cast<std::uint16_t>(m.b.size()), c), DecodeError);
}

TEST(Itch, UnknownTypeThrows) {
  const std::uint8_t m[12] = {'Z'};
  Capture c;
  EXPECT_THROW(dispatch(m, 12, c), DecodeError);
}

// Consistency checks over a real day: these fail if any field offset is wrong.
TEST(Fixture, DecodedFieldsAreConsistent) {
  const auto path = std::filesystem::path(LOB_FIXTURE_DIR) / "sample.itch.gz";
  if (!std::filesystem::exists(path)) GTEST_SKIP() << "fixture not present";

  struct Check : Handler {
    using Handler::on;
    std::vector<std::string> dir = std::vector<std::string>(65536);
    std::uint64_t last_ts = 0, backwards = 0, adds = 0, trades = 0;
    std::vector<std::string> errors;

    void ts(const Header& h) {
      if (h.timestamp < last_ts) ++backwards;
      last_ts = h.timestamp;
    }
    void on(const StockDirectory& m) { ts(m); dir[m.locate] = std::string(m.stock.view()); }
    void on(const AddOrder& m) {
      ts(m);
      ++adds;
      if (dir[m.locate] != m.stock.view()) errors.push_back("add symbol " + std::string(m.stock.view()));
      if (m.side != 'B' && m.side != 'S') errors.push_back("add side");
      if (m.shares == 0) errors.push_back("add with zero shares");
    }
    void on(const Trade& m) {
      ts(m);
      ++trades;
      if (dir[m.locate] != m.stock.view()) errors.push_back("trade symbol");
      // FLIR traded near $48 and IIVI near $37 on 2019-01-30.
      if (m.price < 20 * 10'000 || m.price > 100 * 10'000) errors.push_back("trade price " + std::to_string(m.price));
    }
    void on(const OrderExecuted& m) { ts(m); }
    void on(const OrderCancel& m) { ts(m); }
    void on(const OrderDelete& m) { ts(m); }
    void on(const OrderReplace& m) { ts(m); }
    void on(const Other& m) { ts(m); }
  } check;

  lob::GzItchReader reader(path);
  reader.for_each([&](const std::uint8_t* m, std::uint16_t len) { dispatch(m, len, check); });

  EXPECT_EQ(check.backwards, 0u);
  EXPECT_GT(check.adds, 60'000u);
  EXPECT_GT(check.trades, 500u);
  EXPECT_TRUE(check.errors.empty()) << check.errors.size() << " errors, first: " << check.errors.front();
}
