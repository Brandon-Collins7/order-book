# Benchmark: full_day

- File: `data/01302019.NASDAQ_ITCH50.gz`, all stocks
- Messages: 368,366,634
- Machine: 13th Gen Intel(R) Core(TM) i9-13900HX, Windows 11; C++ MSVC 194435209, pinned to CPU 4; Python 3.12.7
- Every 64th message timed individually; timer overhead subtracted. Python's clock on Windows ticks every 100 ns.
- All book variants agree on the top-of-book checksum: yes

| Variant | M msg/s | ns/msg | p50 ns | p99 ns | p99.9 ns | Memory growth | Checksum |
|---|---:|---:|---:|---:|---:|---:|---|
| frame | 367.58 | 2.7 | 2 | 3 | 4 | 68 MB | `000000084e49d4e8` |
| parse | 118.16 | 8.5 | 12 | 16 | 51 | 68 MB | `16f011a7a382d826` |
| baseline | 3.27 | 305.7 | 287 | 1074 | 1430 | 353 MB | `f9771e4496388206` |
| pool/std-hash/map | 3.12 | 320.3 | 299 | 1145 | 1546 | 323 MB | `f9771e4496388206` |
| pool/std-hash/vector | 4.32 | 231.7 | 231 | 734 | 996 | 299 MB | `f9771e4496388206` |
| pool/flat-hash/map | 4.98 | 200.8 | 191 | 867 | 1194 | 250 MB | `f9771e4496388206` |
| pool/flat-hash/vector | 9.41 | 106.2 | 125 | 392 | 548 | 226 MB | `f9771e4496388206` |
| pool/direct/map | 6.50 | 153.9 | 139 | 749 | 1133 | 2543 MB | `f9771e4496388206` |
| pool/direct/vector | 13.92 | 71.8 | 62 | 334 | 582 | 2517 MB | `f9771e4496388206` |

