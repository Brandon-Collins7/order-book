# Benchmark: full_day (WSL2 / gcc)

- Messages: 368,366,634
- Machine: same i9-13900HX, inside WSL2 (Ubuntu 22.04, kernel 6.18 WSL2, 7 GB VM). gcc 16.2, -O3 -march=native; pinned to vCPU 4 inside the VM (the host can still move that vCPU between performance and efficiency cores).
- Same method as the Windows runs. C++ only.
- All book variants agree on the top-of-book checksum, and with the Windows/MSVC build: yes

| Variant | M msg/s | ns/msg | p50 ns | p99 ns | p99.9 ns | Memory growth | Checksum |
|---|---:|---:|---:|---:|---:|---:|---|
| frame | 267.70 | 3.7 | 1 | 7 | 18 | 99 MB | `000000084e49d4e8` |
| parse | 117.38 | 8.5 | 10 | 23 | 196 | 99 MB | `16f011a7a382d826` |
| baseline | 3.46 | 289.4 | 280 | 1204 | 1906 | 277 MB | `f9771e4496388206` |
| pool/std-hash/map | 3.95 | 253.2 | 257 | 1088 | 1615 | 282 MB | `f9771e4496388206` |
| pool/std-hash/vector | 5.68 | 176.0 | 177 | 701 | 1092 | 255 MB | `f9771e4496388206` |
| pool/flat-hash/map | 4.94 | 202.5 | 187 | 903 | 1376 | 275 MB | `f9771e4496388206` |
| pool/flat-hash/vector | 7.39 | 135.3 | 156 | 510 | 852 | 250 MB | `f9771e4496388206` |
| pool/direct/map | 4.14 | 241.7 | 146 | 1006 | 2018 | 2568 MB | `f9771e4496388206` |
| pool/direct/vector | 7.22 | 138.5 | 69 | 449 | 6588 | 2544 MB | `f9771e4496388206` |
