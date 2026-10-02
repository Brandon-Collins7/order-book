// Decodes every message in a gzipped ITCH 5.0 file, counts them by type, and runs consistency
// checks that would catch a wrong field offset in the decoder:
//   - every message has the length the spec gives for its type (dispatch() throws otherwise)
//   - timestamps never go backwards
//   - the symbol in every add order, trade and cross matches the stock directory for its locate
//
//   itch_stats <file.gz>
//
// Counts go to stdout (same format as tests/fixtures/*.counts.txt); checks and timing go to
// stderr. The exit code is 1 if any check fails. Timing includes gzip decompression.

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <vector>

#include "lob/gz_reader.hpp"
#include "lob/itch.hpp"

namespace {

using namespace lob::itch;

struct Checker : Handler {
  using Handler::on;

  std::vector<Symbol> directory = std::vector<Symbol>(65536);  // indexed by stock locate
  std::vector<bool> listed = std::vector<bool>(65536, false);
  std::uint64_t last_ts = 0;
  std::uint64_t ts_backwards = 0;
  std::uint64_t symbol_mismatch = 0;

  void time(const Header& h) {
    if (h.timestamp < last_ts) ++ts_backwards;
    last_ts = h.timestamp;
  }
  void symbol(const Header& h, const Symbol& s) {
    if (!listed[h.locate] || !(directory[h.locate] == s)) ++symbol_mismatch;
  }

  void on(const StockDirectory& m) {
    time(m);
    directory[m.locate] = m.stock;
    listed[m.locate] = true;
  }
  void on(const AddOrder& m) { time(m); symbol(m, m.stock); }
  void on(const Trade& m) { time(m); symbol(m, m.stock); }
  void on(const CrossTrade& m) { time(m); symbol(m, m.stock); }
  void on(const TradingAction& m) { time(m); symbol(m, m.stock); }
  void on(const SystemEvent& m) { time(m); }
  void on(const OrderExecuted& m) { time(m); }
  void on(const OrderExecutedWithPrice& m) { time(m); }
  void on(const OrderCancel& m) { time(m); }
  void on(const OrderDelete& m) { time(m); }
  void on(const OrderReplace& m) { time(m); }
  void on(const BrokenTrade& m) { time(m); }
  void on(const Other& m) { time(m); }
};

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <file.NASDAQ_ITCH50.gz>\n", argv[0]);
    return 2;
  }
  try {
    std::array<std::uint64_t, 256> by_type{};
    std::uint64_t bytes = 0;
    Checker check;

    const auto t0 = std::chrono::steady_clock::now();
    lob::GzItchReader reader(argv[1]);
    const std::uint64_t total = reader.for_each([&](const std::uint8_t* msg, std::uint16_t len) {
      ++by_type[msg[0]];
      bytes += 2u + len;
      dispatch(msg, len, check);
    });
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    for (int t = 0; t < 256; ++t)
      if (by_type[t]) std::printf("%c %llu\n", t, static_cast<unsigned long long>(by_type[t]));
    std::printf("total %llu\n", static_cast<unsigned long long>(total));

    std::fprintf(stderr, "%.2f GB uncompressed in %.1f s: %.2f M msg/s, %.0f MB/s\n", bytes / 1e9, secs,
                 total / secs / 1e6, bytes / secs / 1e6);
    std::fprintf(stderr, "check: all lengths valid; timestamps backwards: %llu; symbol mismatches: %llu\n",
                 static_cast<unsigned long long>(check.ts_backwards),
                 static_cast<unsigned long long>(check.symbol_mismatch));
    if (check.ts_backwards || check.symbol_mismatch) return 1;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
  return 0;
}
