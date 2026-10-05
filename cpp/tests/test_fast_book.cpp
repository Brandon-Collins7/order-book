#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <random>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "lob/book.hpp"
#include "lob/fast_book.hpp"
#include "lob/gz_reader.hpp"

using namespace lob;

// Random inserts, lookups and deletes against std::unordered_map as the reference. Keys are
// drawn from a small range so probe runs collide, wrap around the table and get shifted back.
TEST(FlatHashIndex, MatchesStdUnorderedMap) {
  fast::FlatHashIndex flat;
  std::unordered_map<std::uint64_t, std::uint32_t> ref;
  std::mt19937_64 rng(42);
  for (int step = 0; step < 2'000'000; ++step) {
    const std::uint64_t key = rng() % 200'000;
    switch (rng() % 3) {
      case 0: {
        const auto v = static_cast<std::uint32_t>(step);
        ASSERT_EQ(flat.insert(key, v), ref.try_emplace(key, v).second) << step;
        break;
      }
      case 1:
        flat.erase(key);
        ref.erase(key);
        break;
      default: {
        auto it = ref.find(key);
        ASSERT_EQ(flat.find(key), it == ref.end() ? fast::kNil : it->second) << step;
      }
    }
  }
  EXPECT_EQ(flat.size(), ref.size());
  for (const auto& [k, v] : ref) ASSERT_EQ(flat.find(k), v);
}

TEST(VectorLevels, KeepsBestAtBackForBothSides) {
  fast::VectorLevels<true> bids;
  fast::VectorLevels<false> asks;
  for (Price p : {100u, 300u, 200u, 50u, 1000u, 120u, 130u, 140u, 150u, 160u, 170u, 180u}) {
    bids.get_or_add(p);
    asks.get_or_add(p);
  }
  EXPECT_EQ(bids.best()->price, 1000u);
  EXPECT_EQ(asks.best()->price, 50u);
  bids.erase(1000);
  asks.erase(50);
  EXPECT_EQ(bids.best()->price, 300u);
  EXPECT_EQ(asks.best()->price, 100u);
  EXPECT_EQ(&bids.at(120), &bids.get_or_add(120));  // existing level, not a new one
}

namespace {

struct TopChange {
  std::uint16_t locate;
  Top top;
  friend bool operator==(const TopChange&, const TopChange&) = default;
};

// Every change to any book's top, in order, as build_book would print it.
template <class Builder>
std::vector<TopChange> top_changes(const std::filesystem::path& path, Builder& builder) {
  std::vector<TopChange> out;
  std::vector<Top> last(65536);
  GzItchReader reader(path);
  reader.for_each([&](const std::uint8_t* m, std::uint16_t len) {
    builder.process(m, len);
    if (const auto* b = builder.touched()) {
      const Top t = b->top();
      if (!(t == last[builder.touched_locate()])) {
        last[builder.touched_locate()] = t;
        out.push_back({builder.touched_locate(), t});
      }
    }
  });
  return out;
}

}  // namespace

template <class Builder>
class FastBookVariant : public ::testing::Test {};

using Variants = ::testing::Types<
    fast::BookBuilder<fast::StdHashIndex, fast::MapLevels>, fast::BookBuilder<fast::StdHashIndex, fast::VectorLevels>,
    fast::BookBuilder<fast::FlatHashIndex, fast::MapLevels>, fast::BookBuilder<fast::FlatHashIndex, fast::VectorLevels>,
    fast::BookBuilder<fast::DirectIndex, fast::MapLevels>, fast::BookBuilder<fast::DirectIndex, fast::VectorLevels>>;
TYPED_TEST_SUITE(FastBookVariant, Variants);

TYPED_TEST(FastBookVariant, MatchesBaselineOnFixtureDay) {
  const auto path = std::filesystem::path(LOB_FIXTURE_DIR) / "sample.itch.gz";
  if (!std::filesystem::exists(path)) GTEST_SKIP() << "fixture not present";

  BookBuilder baseline;
  TypeParam variant;
  const auto want = top_changes(path, baseline);
  const auto got = top_changes(path, variant);

  ASSERT_EQ(got.size(), want.size());
  for (std::size_t i = 0; i < want.size(); ++i) ASSERT_EQ(got[i], want[i]) << "change " << i;
  EXPECT_EQ(variant.stats().unknown_ref, 0u);
  EXPECT_EQ(variant.stats().duplicate_ref, 0u);
  EXPECT_EQ(variant.stats().overfill, 0u);
  EXPECT_EQ(variant.stats().max_live_orders, baseline.stats().max_live_orders);
  EXPECT_EQ(variant.live_orders(), baseline.live_orders());
}
