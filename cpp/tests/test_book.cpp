#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "lob/book.hpp"
#include "lob/gz_reader.hpp"

using namespace lob;

namespace {

constexpr std::uint16_t kLoc = 7;

itch::Symbol sym(const std::string& s) {
  itch::Symbol out;
  for (std::size_t i = 0; i < 8; ++i) out.chars[i] = i < s.size() ? s[i] : ' ';
  return out;
}

itch::Header hdr(char type, std::uint16_t locate = kLoc) { return {type, locate, 0}; }

// A builder tracking one stock, "TEST", at locate kLoc.
struct BookTest : ::testing::Test {
  BookBuilder b{{"TEST"}};

  void SetUp() override { b.on(itch::StockDirectory{hdr('R'), sym("TEST"), 'Q', 100}); }

  void add(std::uint64_t ref, char side, std::uint32_t shares, Price price, std::uint16_t loc = kLoc) {
    b.on(itch::AddOrder{hdr('A', loc), ref, side, shares, sym("TEST"), price, {' ', ' ', ' ', ' '}});
  }
  void exec(std::uint64_t ref, std::uint32_t shares) { b.on(itch::OrderExecuted{hdr('E'), ref, shares, 1}); }
  void cancel(std::uint64_t ref, std::uint32_t shares) { b.on(itch::OrderCancel{hdr('X'), ref, shares}); }
  void del(std::uint64_t ref) { b.on(itch::OrderDelete{hdr('D'), ref}); }
  void replace(std::uint64_t orig, std::uint64_t ref, std::uint32_t shares, Price price) {
    b.on(itch::OrderReplace{hdr('U'), orig, ref, shares, price});
  }

  const Book& book() { return *b.book(kLoc); }
  // Refs at one price level, front of the queue first.
  std::vector<std::uint64_t> queue(char side, Price price) {
    std::vector<std::uint64_t> refs;
    const Level& lv = side == 'B' ? book().bids().at(price) : book().asks().at(price);
    for (const Order* o = lv.head; o; o = o->next) refs.push_back(o->ref);
    return refs;
  }
};

}  // namespace

TEST_F(BookTest, EmptyBookHasZeroTop) {
  EXPECT_EQ(book().top(), Top{});
}

TEST_F(BookTest, BestPricesAndLevelTotals) {
  add(1, 'B', 100, 10'0000);
  add(2, 'B', 200, 10'0100);  // better bid
  add(3, 'B', 50, 10'0100);
  add(4, 'S', 300, 10'0300);
  add(5, 'S', 100, 10'0200);  // better ask
  EXPECT_EQ(book().top(), (Top{10'0100, 250, 10'0200, 100}));
  EXPECT_EQ(book().bids().size(), 2u);
  EXPECT_EQ(book().bids().at(10'0100).orders, 2u);
  EXPECT_EQ(book().validate(), 0u);
}

TEST_F(BookTest, QueueIsFirstInFirstOut) {
  add(1, 'B', 100, 10'0000);
  add(2, 'B', 100, 10'0000);
  add(3, 'B', 100, 10'0000);
  EXPECT_EQ(queue('B', 10'0000), (std::vector<std::uint64_t>{1, 2, 3}));
}

TEST_F(BookTest, PartialExecutionKeepsPlaceInLine) {
  add(1, 'S', 100, 10'0000);
  add(2, 'S', 100, 10'0000);
  exec(1, 40);
  EXPECT_EQ(queue('S', 10'0000), (std::vector<std::uint64_t>{1, 2}));
  EXPECT_EQ(book().top().ask_sz, 160u);
  EXPECT_EQ(b.live_orders(), 2u);
}

TEST_F(BookTest, FullExecutionRemovesOrderAndEmptyLevel) {
  add(1, 'S', 100, 10'0000);
  add(2, 'S', 100, 10'0100);
  exec(1, 100);
  EXPECT_EQ(book().top(), (Top{0, 0, 10'0100, 100}));
  EXPECT_EQ(b.live_orders(), 1u);
  EXPECT_EQ(book().validate(), 0u);
}

TEST_F(BookTest, CancelFromMiddleRelinksQueue) {
  add(1, 'B', 100, 10'0000);
  add(2, 'B', 100, 10'0000);
  add(3, 'B', 100, 10'0000);
  del(2);
  EXPECT_EQ(queue('B', 10'0000), (std::vector<std::uint64_t>{1, 3}));
  del(1);
  del(3);
  EXPECT_TRUE(book().bids().empty());
  cancel(99, 1);  // unknown ref
  EXPECT_EQ(b.stats().unknown_ref, 1u);
}

TEST_F(BookTest, PartialCancelReducesShares) {
  add(1, 'B', 100, 10'0000);
  cancel(1, 30);
  EXPECT_EQ(book().top().bid_sz, 70u);
  cancel(1, 70);  // cancelling the rest removes it
  EXPECT_TRUE(book().bids().empty());
  EXPECT_EQ(b.stats().overfill, 0u);
}

TEST_F(BookTest, ReplaceLosesPriorityAndKeepsSide) {
  add(1, 'B', 100, 10'0000);
  add(2, 'B', 100, 10'0000);
  replace(1, 10, 300, 10'0000);  // same price, new size: goes to the back
  EXPECT_EQ(queue('B', 10'0000), (std::vector<std::uint64_t>{2, 10}));
  replace(2, 11, 100, 10'0500);  // new price
  EXPECT_EQ(book().top(), (Top{10'0500, 100, 0, 0}));
  EXPECT_EQ(b.live_orders(), 2u);
  EXPECT_EQ(book().validate(), 0u);
}

TEST_F(BookTest, IntegrityCounters) {
  add(1, 'B', 100, 10'0000);
  add(1, 'B', 100, 10'0000);  // duplicate ref
  exec(1, 150);               // more than the order has
  EXPECT_EQ(b.stats().duplicate_ref, 1u);
  EXPECT_EQ(b.stats().overfill, 1u);
  EXPECT_EQ(b.live_orders(), 0u);
}

TEST_F(BookTest, UntrackedStocksAndRepeatedDirectoryAreIgnored) {
  b.on(itch::StockDirectory{hdr('R', 8), sym("OTHER"), 'Q', 100});
  add(1, 'B', 100, 10'0000, 8);
  EXPECT_EQ(b.book(8), nullptr);
  EXPECT_EQ(b.live_orders(), 0u);

  add(2, 'B', 100, 10'0000);
  b.on(itch::StockDirectory{hdr('R'), sym("TEST"), 'Q', 100});  // repeated: must keep the book
  EXPECT_EQ(book().top().bid_sz, 100u);
}

// A full real day for two stocks: every reference must resolve and every book must stay
// consistent. Nasdaq matches incoming orders, so its own book should never be crossed or
// locked while a stock is trading in regular hours.
TEST(FixtureDay, BooksStayConsistent) {
  const auto path = std::filesystem::path(LOB_FIXTURE_DIR) / "sample.itch.gz";
  if (!std::filesystem::exists(path)) GTEST_SKIP() << "fixture not present";

  BookBuilder builder;
  GzItchReader reader(path);
  reader.for_each([&](const std::uint8_t* m, std::uint16_t len) { builder.process(m, len); });

  const BookStats& s = builder.stats();
  EXPECT_EQ(s.unknown_ref, 0u);
  EXPECT_EQ(s.duplicate_ref, 0u);
  EXPECT_EQ(s.overfill, 0u);
  EXPECT_EQ(s.crossed, 0u);
  EXPECT_EQ(s.locked, 0u);
  EXPECT_GT(s.max_live_orders, 500u);  // 877 at peak for FLIR + IIVI
  for (int loc = 0; loc < 65536; ++loc) {
    if (const Book* b = builder.book(static_cast<std::uint16_t>(loc))) {
      EXPECT_EQ(b->validate(), 0u) << loc;
    }
  }
}
