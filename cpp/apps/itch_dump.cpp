// Prints every message of a gzipped ITCH file as one comma-separated line, for comparison
// with the Python decoder (pylob.itch.dump_line produces the identical format).
//
//   itch_dump <file.gz> > out.txt
//
// Each line starts with type,locate,timestamp. The fields that follow depend on the type, in
// the order they appear in the structs of lob/itch.hpp. Other message types print the header only.

#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>

#include "lob/gz_reader.hpp"
#include "lob/itch.hpp"

namespace {

using namespace lob::itch;
using ull = unsigned long long;

struct Printer {
  std::FILE* out;

  void head(const Header& h) { std::fprintf(out, "%c,%u,%llu", h.type, h.locate, static_cast<ull>(h.timestamp)); }
  static std::string sym(const Symbol& s) { return std::string(s.view()); }

  void on(const SystemEvent& m) { head(m); std::fprintf(out, ",%c\n", m.event); }
  void on(const StockDirectory& m) {
    head(m);
    std::fprintf(out, ",%s,%c,%u\n", sym(m.stock).c_str(), m.market_category, m.round_lot_size);
  }
  void on(const TradingAction& m) { head(m); std::fprintf(out, ",%s,%c\n", sym(m.stock).c_str(), m.state); }
  void on(const AddOrder& m) {
    head(m);
    std::fprintf(out, ",%llu,%c,%u,%s,%u,%.4s\n", static_cast<ull>(m.ref), m.side, m.shares,
                 sym(m.stock).c_str(), m.price, m.mpid.data());
  }
  void on(const OrderExecuted& m) {
    head(m);
    std::fprintf(out, ",%llu,%u,%llu\n", static_cast<ull>(m.ref), m.shares, static_cast<ull>(m.match));
  }
  void on(const OrderExecutedWithPrice& m) {
    head(m);
    std::fprintf(out, ",%llu,%u,%llu,%c,%u\n", static_cast<ull>(m.ref), m.shares, static_cast<ull>(m.match),
                 m.printable, m.price);
  }
  void on(const OrderCancel& m) { head(m); std::fprintf(out, ",%llu,%u\n", static_cast<ull>(m.ref), m.shares); }
  void on(const OrderDelete& m) { head(m); std::fprintf(out, ",%llu\n", static_cast<ull>(m.ref)); }
  void on(const OrderReplace& m) {
    head(m);
    std::fprintf(out, ",%llu,%llu,%u,%u\n", static_cast<ull>(m.orig_ref), static_cast<ull>(m.new_ref), m.shares,
                 m.price);
  }
  void on(const Trade& m) {
    head(m);
    std::fprintf(out, ",%llu,%c,%u,%s,%u,%llu\n", static_cast<ull>(m.ref), m.side, m.shares, sym(m.stock).c_str(),
                 m.price, static_cast<ull>(m.match));
  }
  void on(const CrossTrade& m) {
    head(m);
    std::fprintf(out, ",%llu,%s,%u,%llu,%c\n", static_cast<ull>(m.shares), sym(m.stock).c_str(), m.price,
                 static_cast<ull>(m.match), m.cross_type);
  }
  void on(const BrokenTrade& m) { head(m); std::fprintf(out, ",%llu\n", static_cast<ull>(m.match)); }
  void on(const Other& m) { head(m); std::fputc('\n', out); }
};

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <file.gz>\n", argv[0]);
    return 2;
  }
  try {
    Printer p{stdout};
    lob::GzItchReader reader(argv[1]);
    reader.for_each([&](const std::uint8_t* m, std::uint16_t len) { dispatch(m, len, p); });
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
  return 0;
}
