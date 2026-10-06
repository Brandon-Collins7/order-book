# Simulated cache behavior per book variant (valgrind cachegrind)

AAPL + MSFT, 2019-01-30, 3,331,057 messages. gcc 16.2 `-O3 -march=native`, built with `LOB_CACHEGRIND=ON`, so cachegrind counts only the timed replay, not decompression or setup. The cache is **simulated** (valgrind 3.27, which models the host's L1 and last-level caches), because WSL2 doesn't expose the CPU's hardware counters to `perf`. Cachegrind counts misses; it doesn't model their latency or branch prediction.

| Variant | Instructions / msg | Data refs / msg | L1 data misses / msg | Last-level data misses / msg | Measured ns / msg (gcc, WSL2) |
|---|---:|---:|---:|---:|---:|
| parse only | 52 | 14.6 | 0.48 | 0.48 | 8.6 |
| baseline | 563 | 266 | 3.71 | 0.51 | 79.4 |
| pool/std-hash/map | 542 | 260 | 3.88 | 0.52 | 90.2 |
| pool/std-hash/vector | 559 | 286 | 4.81 | 0.51 | 65.9 |
| pool/flat-hash/map | 414 | 182 | 2.60 | 0.53 | 68.3 |
| pool/flat-hash/vector | 431 | 208 | 3.31 | 0.52 | 54.0 |
| pool/direct/map | 1,132 | 884 | 33.5 | 32.5 | 572 |
| pool/direct/vector | 1,148 | 910 | 34.4 | 32.5 | 491 |

What this shows:

- **On this dataset, misses to RAM don't separate the normal variants.** All of them see about 0.5 last-level misses per message, the same as parse-only: that's the cost of streaming the input itself. The book (65K live orders at peak) fits in the 36 MB last-level cache.
- **The flat open-addressing hash wins by doing less work:** 27% fewer instructions than `std::unordered_map` (563 → 414 per message) and fewer L1 misses.
- **Sorted-vector levels win with about the same instruction count as `std::map`.** The likely reason is that a tree lookup is a chain of dependent pointer loads (each waits for the one before, even when it hits L1/L2), whereas a vector scan from the best price touches adjacent memory. Cachegrind doesn't model latency, so this remains an inference.
- **The direct index's collapse on filtered data is measured: 32.5 last-level misses per message**, about 65x the others, from touching a 2.3 GB array indexed by market-wide order refs while only a few thousand orders are live.
- **Not measured:** the full trading day (368M messages, 1.74M live orders), where the book no longer fits in cache. Cachegrind is about 50x slower than native, so that would take hours per variant. "Misses to RAM explain the full-day slowdown of node-based containers" is therefore still an inference, not a measurement.
