// Rebuilds order books from a gzipped ITCH file and prints the top-of-book stream.
//
//   build_book [--quiet] <file.gz> [SYMBOL ...]       (no symbols = every stock)
//
// stdout: one line each time a tracked stock's best bid or ask changes (price or size):
//   timestamp,locate,bid_px,bid_sz,ask_px,ask_sz
// pylob.book.top_stream produces the identical stream; scripts/cross_check.py compares them.
// --quiet skips printing the stream (printing dominates the run time for a full day).
// stderr: integrity statistics. The exit code is 1 if any integrity check fails.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

#include "lob/book.hpp"
#include "lob/endian.hpp"
#include "lob/gz_reader.hpp"

int main(int argc, char** argv) {
  const bool quiet = argc > 1 && std::string(argv[1]) == "--quiet";
  const int file_arg = quiet ? 2 : 1;
  if (argc <= file_arg) {
    std::fprintf(stderr, "usage: %s [--quiet] <file.gz> [SYMBOL ...]\n", argv[0]);
    return 2;
  }
  using ull = unsigned long long;
  try {
    lob::BookBuilder builder(std::vector<std::string>(argv + file_arg + 1, argv + argc));
    std::vector<lob::Top> last(65536);
    std::uint64_t lines = 0;

    const auto t0 = std::chrono::steady_clock::now();
    lob::GzItchReader reader(argv[file_arg]);
    const std::uint64_t total = reader.for_each([&](const std::uint8_t* m, std::uint16_t len) {
      builder.process(m, len);
      const lob::Book* b = builder.touched();
      if (!b) return;
      const lob::Top t = b->top();
      lob::Top& prev = last[builder.touched_locate()];
      if (t == prev) return;
      prev = t;
      ++lines;
      if (!quiet)
        std::printf("%llu,%u,%u,%llu,%u,%llu\n", static_cast<ull>(lob::be48(m + 5)), builder.touched_locate(),
                  t.bid_px, static_cast<ull>(t.bid_sz), t.ask_px, static_cast<ull>(t.ask_sz));
    });
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    std::size_t books = 0, problems = 0;
    for (int loc = 0; loc < 65536; ++loc) {
      if (const lob::Book* b = builder.book(static_cast<std::uint16_t>(loc))) {
        ++books;
        problems += b->validate();
      }
    }
    const lob::BookStats& s = builder.stats();
    std::fprintf(stderr, "%llu messages in %.1f s (%.2f M msg/s), %zu books, %llu top-of-book changes\n",
                 static_cast<ull>(total), secs, total / secs / 1e6, books, static_cast<ull>(lines));
    std::fprintf(stderr, "live orders: %zu at end of day, %llu at peak\n", builder.live_orders(),
                 static_cast<ull>(s.max_live_orders));
    std::fprintf(stderr,
                 "integrity: unknown refs %llu, duplicate refs %llu, overfills %llu, structure problems %zu\n",
                 static_cast<ull>(s.unknown_ref), static_cast<ull>(s.duplicate_ref), static_cast<ull>(s.overfill),
                 problems);
    std::fprintf(stderr, "regular hours, trading: crossed after %llu messages, locked after %llu\n",
                 static_cast<ull>(s.crossed), static_cast<ull>(s.locked));
    for (const auto& [loc, n] : s.crossed_or_locked_by_locate)
      std::fprintf(stderr, "  %s (locate %u): crossed or locked after %llu messages\n", builder.symbol(loc).c_str(),
                   loc, static_cast<ull>(n));
    if (s.unknown_ref || s.duplicate_ref || s.overfill || problems) return 1;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
  return 0;
}
