# Benchmark: aapl_msft (WSL2 / gcc)

- Messages: 3,331,057
- Machine: same i9-13900HX, inside WSL2 (Ubuntu 22.04, kernel 6.18 WSL2, 7 GB VM). gcc 16.2, -O3 -march=native; pinned to vCPU 4 inside the VM (the host can still move that vCPU between performance and efficiency cores).
- Same method as the Windows runs. C++ only.
- All book variants agree on the top-of-book checksum, and with the Windows/MSVC build: yes

| Variant | M msg/s | ns/msg | p50 ns | p99 ns | p99.9 ns | Memory growth | Checksum |
|---|---:|---:|---:|---:|---:|---:|---|
| frame | 320.32 | 3.1 | 3 | 6 | 8 | 1 MB | `000000001335d901` |
| parse | 116.05 | 8.6 | 11 | 22 | 127 | 1 MB | `11a16f29040d8fbb` |
| baseline | 12.60 | 79.4 | 83 | 253 | 403 | 11 MB | `fc72321ff2af9320` |
| pool/std-hash/map | 11.08 | 90.2 | 84 | 266 | 460 | 9 MB | `fc72321ff2af9320` |
| pool/std-hash/vector | 15.17 | 65.9 | 69 | 241 | 428 | 8 MB | `fc72321ff2af9320` |
| pool/flat-hash/map | 14.64 | 68.3 | 66 | 196 | 303 | 10 MB | `fc72321ff2af9320` |
| pool/flat-hash/vector | 18.53 | 54.0 | 55 | 211 | 396 | 10 MB | `fc72321ff2af9320` |
| pool/direct/map | 1.75 | 572.2 | 62 | 311 | 694 | 2294 MB | `fc72321ff2af9320` |
| pool/direct/vector | 2.03 | 491.4 | 43 | 291 | 668 | 2294 MB | `fc72321ff2af9320` |
