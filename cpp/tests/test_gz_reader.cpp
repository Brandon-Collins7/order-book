#include <gtest/gtest.h>
#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "lob/gz_reader.hpp"

namespace fs = std::filesystem;

namespace {

// Writes raw bytes to a gzipped temp file that is deleted when the object goes away.
struct TempGz {
  fs::path path;
  explicit TempGz(const std::vector<std::uint8_t>& bytes, const std::string& name) {
    path = fs::temp_directory_path() / ("lob_test_" + name + ".gz");
    gzFile f = gzopen(path.string().c_str(), "wb");
    if (!bytes.empty()) gzwrite(f, bytes.data(), static_cast<unsigned>(bytes.size()));
    gzclose(f);
  }
  ~TempGz() { fs::remove(path); }
};

void append_message(std::vector<std::uint8_t>& out, const std::vector<std::uint8_t>& body) {
  out.push_back(static_cast<std::uint8_t>(body.size() >> 8));
  out.push_back(static_cast<std::uint8_t>(body.size() & 0xff));
  out.insert(out.end(), body.begin(), body.end());
}

// Message i has length 1000 + i % 37 and bytes (i + k) % 251, so every message is distinct.
std::vector<std::uint8_t> patterned_body(std::size_t i) {
  std::vector<std::uint8_t> b(1000 + i % 37);
  for (std::size_t k = 0; k < b.size(); ++k) b[k] = static_cast<std::uint8_t>((i + k) % 251);
  return b;
}

}  // namespace

TEST(GzItchReader, MessagesStraddlingTheBufferArriveIntact) {
  constexpr std::size_t kMessages = 1000;  // ~1 MB, many times the 70 KB buffer below
  std::vector<std::uint8_t> stream;
  for (std::size_t i = 0; i < kMessages; ++i) append_message(stream, patterned_body(i));
  TempGz file(stream, "straddle");

  lob::GzItchReader reader(file.path, 70'000);
  std::size_t i = 0;
  const auto n = reader.for_each([&](const std::uint8_t* msg, std::uint16_t len) {
    const auto want = patterned_body(i);
    ASSERT_EQ(len, want.size()) << "message " << i;
    ASSERT_TRUE(std::equal(want.begin(), want.end(), msg)) << "message " << i;
    ++i;
  });
  EXPECT_EQ(n, kMessages);
  EXPECT_EQ(i, kMessages);
}

TEST(GzItchReader, TruncatedFileThrows) {
  std::vector<std::uint8_t> stream;
  append_message(stream, {'S', 1, 2, 3});
  stream.push_back(0);
  stream.push_back(10);  // declares a 10-byte message
  stream.push_back('A');  // but only one byte follows
  TempGz file(stream, "truncated");

  lob::GzItchReader reader(file.path);
  EXPECT_THROW(reader.for_each([](const std::uint8_t*, std::uint16_t) {}), std::runtime_error);
}

TEST(GzItchReader, EmptyFileHasNoMessages) {
  TempGz file({}, "empty");
  lob::GzItchReader reader(file.path);
  EXPECT_EQ(reader.for_each([](const std::uint8_t*, std::uint16_t) {}), 0u);
}

TEST(GzItchReader, MissingFileThrows) {
  EXPECT_THROW(lob::GzItchReader("does/not/exist.gz"), std::runtime_error);
}

// The fixture is a real day sliced by scripts/slice_fixture.py, which also writes the
// expected per-type counts next to it.
TEST(Fixture, CountsMatchSlicer) {
  const fs::path dir = LOB_FIXTURE_DIR;
  if (!fs::exists(dir / "sample.itch.gz")) GTEST_SKIP() << "fixture not present";

  std::map<std::string, std::uint64_t> want;
  std::ifstream in(dir / "sample.counts.txt");
  std::string key;
  std::uint64_t value = 0;
  while (in >> key >> value) want[key] = value;
  ASSERT_FALSE(want.empty());

  std::map<std::string, std::uint64_t> got;
  std::vector<char> system_events;
  lob::GzItchReader reader(dir / "sample.itch.gz");
  got["total"] = reader.for_each([&](const std::uint8_t* msg, std::uint16_t len) {
    ++got[std::string(1, static_cast<char>(msg[0]))];
    if (msg[0] == 'S') {
      ASSERT_EQ(len, 12);
      system_events.push_back(static_cast<char>(msg[11]));
    }
  });
  EXPECT_EQ(got, want);

  // A full day opens with "start of messages" and closes with "end of messages".
  ASSERT_FALSE(system_events.empty());
  EXPECT_EQ(system_events.front(), 'O');
  EXPECT_EQ(system_events.back(), 'C');
}
