# OrderBook

A C++20 engine that rebuilds Nasdaq TotalView-ITCH 5.0 order books and replays them through a market-making simulator. A reference implementation in Python checks its correctness and gives a performance comparison. See [PLAN.md](PLAN.md) for the design and milestones.

> Status: the engine, benchmarks, simulator and research (including a pre-registered confirmation on fresh days) are done (M0–M5). Final benchmark numbers on Linux/gcc are still to come.

## Results so far

Measured on Windows 11 with MSVC, on an i9-13900HX pinned to one performance core, with the data decompressed into memory before timing. A gcc build under WSL2 is shown further down as a cross-check.

**Full trading day, all 8,713 securities (2019-01-30, 368M messages, 1.74M live orders at peak):**

| Book | M msg/s | ns/msg | p50 | p99 | p99.9 | Memory growth |
|---|---:|---:|---:|---:|---:|---:|
| Baseline: `std::unordered_map` + `std::map` + heap-allocated orders | 3.27 | 306 | 287 ns | 1074 ns | 1430 ns | 353 MB |
| Order pool + flat open-addressing hash + sorted-vector levels | 9.41 | 106 | 125 ns | 392 ns | 548 ns | 226 MB |
| Order pool + direct ref index + sorted-vector levels | **13.92** | **72** | 62 ns | 334 ns | 582 ns | 2517 MB |

Decoding alone runs at 118M msg/s (8.5 ns/msg). Every variant reproduces the baseline's top-of-book stream exactly (checked by checksum), and so does the independent Python implementation, which runs at 0.19M msg/s on AAPL + MSFT.

Which design wins depends on the data. The direct index is fastest on the full feed, where order refs are dense, but on a two-symbol file it is 5x slower than the flat hash and uses about 2 GB, because refs are numbered across the whole market. Full tables: [results/bench/](results/bench/).

**gcc cross-check (WSL2, gcc 16.2):** every variant produces the same checksums as the MSVC build. On AAPL + MSFT, gcc is faster across the board, most of all for the baseline (79 vs 127 ns/msg, since libstdc++'s `std::map`/`std::unordered_map` are quicker), so the best variant's advantage there shrinks to 1.5x (54 ns/msg). On the full day, WSL2 is *slower* for the fast variants (flat-hash/vector 135 ns, direct/vector 139 ns with a 6.6 us p99.9). WSL2 is a virtual machine: first-touch memory is costlier, and the host can move the pinned vCPU onto an efficiency core. These are a compiler cross-check, not bare-metal Linux numbers. Tables: [results/bench/wsl2_gcc/](results/bench/wsl2_gcc/).

**Why the variants differ (valgrind cachegrind, simulated cache, AAPL + MSFT):** the flat hash runs 27% fewer instructions per message than `std::unordered_map` (563 → 414). Misses to RAM are the same for every normal variant (about 0.5 per message, which is just reading the input), because a two-stock book fits in cache. The direct index's collapse on filtered data is measured: 32.5 last-level misses per message, 65x the others. Full-day cache behavior is not measured (cachegrind is too slow for 368M messages). Details: [cachegrind.md](results/bench/wsl2_gcc/cachegrind.md).

### Market-making research

A market maker quoting 100 shares at the best bid and ask was simulated on 8 Nasdaq stocks (AAPL MSFT AMD INTC CSCO CMCSA NVDA FB). The simulator models queue position, 10 µs latency, and post-only orders. Parameters were tuned on one day (2019-01-30) and results reported on three held-out days (2019-03-27, 07-30, 10-30). Intervals are 95% block-bootstrap intervals. Full report: [results/m5/report.md](results/m5/report.md).

- **Adverse selection outweighs the spread.** Joining the touch captures +0.66 ¢/share of spread but loses 1.01 ¢/share to the mid moving against us within 1 s, a net loss of −0.34 ¢/share [−0.36, −0.32] before fees. Break-even would need a maker rebate of about 0.34 ¢/share.
- **Inventory skew (k = 1)** cuts time-weighted inventory by 76% (291 → 70 shares RMS). It costs 0.045 ¢/share [0.026, 0.065] in fill quality.
- **The order-book-imbalance filter (θ = 0.8)** cuts the daily loss by $3.7K [2.6K, 4.8K]. It does this by trading 28% less, not by getting better fills: per-share net markout is slightly worse (−0.018 ¢ [−0.024, −0.011]).
- **Tick size decides whether the order book's imbalance carries information.** This was found as a post-hoc split in the first round, then **confirmed on three fresh days with a pre-registered test** ([plan](results/m5/confirmation_plan.md), [results](results/m5/confirmation.md)). Comparing within each stock in basis points: in large-tick stocks (MSFT AMD INTC CSCO CMCSA), fills when our side of the book was thin are 1.1 bps more adverse than fills when it was thick (fresh days: −1.13 bps [−1.30, −0.99]). In small-tick stocks (AAPL NVDA FB) the gradient is about zero. Pooling all stocks in cents per share reverses the sign of the relationship (Simpson's paradox). In large-tick stocks, fills that waited behind more than 2,000 displayed shares are 0.26 bps [0.21, 0.32] more adverse than fills with 100 or fewer ahead.

![Adverse selection by imbalance, large- vs small-tick](results/m5/figures/as_by_imbalance.png)

- **The imbalance filter only monetizes a small part of that signal.** On fresh days it improves large-tick fills by +0.026 bps [+0.014, +0.041] and helps large-tick more than small-tick stocks (+0.036 bps [+0.018, +0.055]). That is about 0.01 ¢/share on a $40 stock, far smaller than the 1.1 bps signal. A likely reason (not yet tested): the book usually thins in the same event that fills us, before a cancel can arrive.

Caveats: no market impact (our orders never change the replayed market), a single venue (Nasdaq only), no fees or rebates, and 7 days in total (Nasdaq's full public set), of which 2 of the 3 confirmation days are quiet holiday-period days.

## Data

Download full-day ITCH files from Nasdaq's [public directory](https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/) into `data/`. That folder is gitignored, and the tools read the `.gz` files directly:

```sh
curl -L -o data/01302019.NASDAQ_ITCH50.gz "https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/01302019.NASDAQ_ITCH50.gz"
```

`tests/fixtures/sample.itch.gz` is a small slice of a real day, committed so tests and CI can run without downloading anything. To regenerate it:

```sh
python scripts/slice_fixture.py scan  data/01302019.NASDAQ_ITCH50.gz results/01302019_counts.json
python scripts/slice_fixture.py slice data/01302019.NASDAQ_ITCH50.gz tests/fixtures/sample.itch.gz \
       --summary results/01302019_counts.json --auto 2
```

## Build (C++)

Requires CMake ≥ 3.28, Ninja, and a C++20 compiler. On Windows, run from a *Developer PowerShell for VS 2022*, which puts MSVC, CMake and Ninja on the path.

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
```

Tools (in `build/release/`):

| Tool | What it does |
|---|---|
| `itch_stats <file.gz>` | Decodes every message, counts by type, and checks lengths, timestamp order and symbols |
| `itch_dump <file.gz>` | Prints every decoded message as one CSV line (same format as `pylob.dump_line`) |
| `itch_filter <in.gz> <out.gz> SYM...` | Writes a small file with just the chosen symbols, plus market-wide and directory messages |
| `build_book [--quiet] <file.gz> [SYM...]` | Rebuilds the order books and prints every top-of-book change; reports integrity checks |
| `backtest <file.gz> --symbols A,B [options]` | Runs the market maker in the replayed market; writes every fill with its markouts (see `--help`-style usage in `cpp/apps/backtest.cpp`) |
| `lob_bench <file.gz> --variant NAME` | Benchmarks parsing and the book variants (see `scripts/run_benchmarks.py`) |

`ctest` includes two cross-language checks on the fixture: `cross_check_decoders` (every decoded field of every message) and `cross_check_books` (every top-of-book change). Run them on any file with `scripts/cross_check.py`.

## Python

```sh
pip install -e ".[dev]"
pytest
```
