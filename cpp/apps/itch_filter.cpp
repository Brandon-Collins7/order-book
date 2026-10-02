// Writes a smaller ITCH file holding only the chosen symbols.
//
//   itch_filter <in.gz> <out.gz> SYMBOL [SYMBOL ...]
//
// Keeps every market-wide message (stock locate 0), the whole stock directory ('R'), and every
// message whose locate belongs to a chosen symbol. The output is still a valid, framed ITCH
// stream for the full day, so every tool can read it.

#include <zlib.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "lob/endian.hpp"
#include "lob/gz_reader.hpp"
#include "lob/itch.hpp"

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: %s <in.gz> <out.gz> SYMBOL [SYMBOL ...]\n", argv[0]);
    return 2;
  }
  gzFile out = nullptr;
  try {
    std::set<std::string> wanted(argv + 3, argv + argc);
    std::vector<bool> keep(65536, false);  // indexed by stock locate
    std::set<std::string> found;

    out = gzopen(argv[2], "wb6");
    if (!out) throw std::runtime_error(std::string("cannot open ") + argv[2]);
    gzbuffer(out, 1u << 20);

    std::uint64_t kept = 0;
    const auto t0 = std::chrono::steady_clock::now();
    lob::GzItchReader reader(argv[1]);
    const std::uint64_t total = reader.for_each([&](const std::uint8_t* m, std::uint16_t len) {
      const std::uint16_t locate = lob::be16(m + 1);
      if (m[0] == 'R') {
        // The directory is sent before any order for that stock, so locates are known in time.
        const std::string sym(lob::itch::detail::symbol(m + 11).view());
        if (wanted.count(sym)) {
          keep[locate] = true;
          found.insert(sym);
        }
      } else if (locate != 0 && !keep[locate]) {
        return;
      }
      const std::uint8_t prefix[2] = {static_cast<std::uint8_t>(len >> 8), static_cast<std::uint8_t>(len)};
      if (gzwrite(out, prefix, 2) != 2 || gzwrite(out, m, len) != static_cast<int>(len))
        throw std::runtime_error("write failed");
      ++kept;
    });
    if (gzclose(out) != Z_OK) throw std::runtime_error("closing output failed");
    out = nullptr;

    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::fprintf(stderr, "kept %llu of %llu messages in %.1f s\n", static_cast<unsigned long long>(kept),
                 static_cast<unsigned long long>(total), secs);
    for (const auto& s : wanted)
      if (!found.count(s)) std::fprintf(stderr, "warning: symbol %s not in the stock directory\n", s.c_str());
  } catch (const std::exception& e) {
    if (out) gzclose(out);
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
  return 0;
}
