# Benchmark: aapl_msft

- File: `data/filtered/01302019_AAPL_MSFT.itch.gz`, symbols AAPL,MSFT
- Messages: 3,331,057
- Machine: 13th Gen Intel(R) Core(TM) i9-13900HX, Windows 11; C++ MSVC 194435209, pinned to CPU 4; Python 3.12.7
- Every 64th message timed individually; timer overhead subtracted. Python's clock on Windows ticks every 100 ns.
- All book variants agree on the top-of-book checksum: yes

| Variant | M msg/s | ns/msg | p50 ns | p99 ns | p99.9 ns | Memory growth | Checksum |
|---|---:|---:|---:|---:|---:|---:|---|
| frame | 350.60 | 2.9 | 2 | 3 | 5 | 1 MB | `000000001335d901` |
| parse | 115.30 | 8.7 | 13 | 18 | 50 | 1 MB | `11a16f29040d8fbb` |
| baseline | 7.90 | 126.5 | 125 | 340 | 439 | 13 MB | `fc72321ff2af9320` |
| pool/std-hash/map | 7.96 | 125.6 | 127 | 340 | 516 | 11 MB | `fc72321ff2af9320` |
| pool/std-hash/vector | 9.35 | 107.0 | 109 | 352 | 570 | 10 MB | `fc72321ff2af9320` |
| pool/flat-hash/map | 12.67 | 78.9 | 79 | 248 | 354 | 8 MB | `fc72321ff2af9320` |
| pool/flat-hash/vector | 16.56 | 60.4 | 62 | 249 | 460 | 7 MB | `fc72321ff2af9320` |
| pool/direct/map | 2.65 | 377.0 | 73 | 308 | 1043 | 1709 MB | `fc72321ff2af9320` |
| pool/direct/vector | 2.87 | 348.9 | 46 | 291 | 581 | 2261 MB | `fc72321ff2af9320` |
| python frame | 3.74 | 267.4 | 200 | 300 | 500 | - | `` |
| python parse | 0.84 | 1197.4 | 1100 | 1600 | 3900 | - | `` |
| python book/sorteddict | 0.19 | 5197.5 | 4900 | 8100 | 12900 | - | `fc72321ff2af9320` |
| python book/sorteddict (gc off) | 0.20 | 5101.8 | 4900 | 8200 | 13400 | - | `fc72321ff2af9320` |
| python book/naive | 0.01 | 96853.1 | 90600 | 199200 | 327200 | - | `fc72321ff2af9320` |

Python order dict: 210.1 bytes per resting order (C++ pool: 32 bytes per order plus the index).
