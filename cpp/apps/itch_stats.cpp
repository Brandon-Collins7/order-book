// Counts the messages in a gzipped ITCH 5.0 file by type and reports read throughput.
//
//   itch_stats <file.gz>
//
// The timing includes gzip decompression, so it measures end-to-end file reading,
// not parser speed.

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>

#include "lob/gz_reader.hpp"

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <file.NASDAQ_ITCH50.gz>\n", argv[0]);
    return 2;
  }
  try {
    std::array<std::uint64_t, 256> by_type{};
    std::uint64_t bytes = 0;

    const auto t0 = std::chrono::steady_clock::now();
    lob::GzItchReader reader(argv[1]);
    const std::uint64_t total = reader.for_each([&](const std::uint8_t* msg, std::uint16_t len) {
      ++by_type[msg[0]];
      bytes += 2u + len;
    });
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    for (int t = 0; t < 256; ++t)
      if (by_type[t]) std::printf("%c %llu\n", t, static_cast<unsigned long long>(by_type[t]));
    std::printf("total %llu\n", static_cast<unsigned long long>(total));
    std::fprintf(stderr, "%.2f GB uncompressed in %.1f s: %.2f M msg/s, %.0f MB/s\n", bytes / 1e9,
                 secs, total / secs / 1e6, bytes / secs / 1e6);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
  return 0;
}
