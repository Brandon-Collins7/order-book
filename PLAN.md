# Plan: ITCH Order Book Engine & Market-Making Backtester

**Goal:** one project that shows two things:
- **Quant SWE:** a fast, well-engineered C++ engine, with latency and throughput numbers.
- **Quant research:** a careful backtest result about adverse selection.

The same book logic is also written in Python. It checks the C++ engine for correctness, and it gives a like-for-like performance comparison.

## 1. Deliverables

| Piece | Language | Purpose |
|---|---|---|
| ITCH 5.0 parser + book builder | C++20 | Throughput and latency numbers (systems bullet) |
| Reference parser + book builder | Python | Correctness oracle; C++ vs. Python comparison |
| Replay simulator + market-making strategy | C++20 | Produces the quotes and fills |
| Analysis: markouts, PnL decomposition, plots | Python | Research bullet |
| README that leads with numbers + one plot | — | What recruiters actually read |

## 2. Data

- Nasdaq publishes free full-day TotalView-ITCH 5.0 files at `https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/`. There are 15 days available between 2018 and 2020. Each is 3.5–5.6 GB gzipped, roughly 9–14 GB uncompressed, and holds hundreds of millions of messages.
- **Storage:** keep only the `.gz` files, in `data/` (gitignored). The disk has limited free space. Start with one day (`01302019`) and add days when M5 starts. Tools find the folder through a `ITCH_DATA_DIR` env var, which defaults to `data/`.
- **Read the `.gz` files directly:** the C++ and Python tools decompress while reading (zlib in C++, `gzip` in Python), so an uncompressed file never needs to be written to disk.
- **Per-symbol filter (M1):** a one-time tool writes just the chosen symbols' messages to a small binary file per day. The research backtests run on these files.
- **Throughput benchmarks** must not include decompression time. For those, decompress one day, or the filtered file, into memory or a temporary file, and delete it afterwards.
- Commit a small test file (a few MB, sliced from a real day) to `tests/fixtures/` so CI and unit tests can run on it.
- Symbol universe: roughly 5–10 liquid names that trade at different prices (e.g. AAPL, MSFT, AMD, INTC, plus a few mid-caps). Use every available day for the research result, and use at least 3.

## 3. Repo layout

```
OrderBook/
├── CMakeLists.txt
├── cpp/
│   ├── include/lob/        itch.hpp, book.hpp, order_store.hpp, sim.hpp, strategy.hpp
│   ├── src/                parser, book, replay engine, simulator, strategies
│   ├── apps/               itch_stats, build_book, backtest (CLI entry points)
│   ├── bench/              Google Benchmark micro-benches + end-to-end replay timer
│   └── tests/              GoogleTest
├── python/
│   ├── pylob/              reference parser + book (pure Python, then optimized variant)
│   ├── analysis/           markouts, PnL decomposition, plots
│   └── tests/              pytest; includes C++-vs-Python cross-check
├── notebooks/              exploratory analysis only; final plots come from scripts
├── scripts/                download_itch.sh, slice_fixture.py, run_all.sh
├── results/                small CSV/JSON summaries + README plots (committed)
└── .github/workflows/ci.yml  build + test on ubuntu with the fixture file
```

## 4. C++ engine design (the talking points)

**Parser**
- Memory-map the decompressed file. Messages are length-prefixed, big-endian binary.
- Zero-copy decode: read the fields in place with byte-swaps, and dispatch on the message type with a `switch`. Handle `S R A F E C X D U P Q` and ignore the rest.
- Numbers to record: messages/sec and GB/sec for parsing alone, measured separately from book updates.

**Order store** (order ref → order)
- ITCH order reference numbers increase through the day, so a flat `std::vector<Order>` indexed by ref is much faster than a `std::unordered_map`. Benchmark both. This comparison is a good interview story.

**Price levels**
- Baseline: `std::map<Price, Level>` for each side.
- Optimized: a flat array of levels indexed by tick offset from a moving anchor, with a separate pointer to the best price. Alternatively, a sorted `std::vector` with the best price at the back.
- Each level has an intrusive doubly linked FIFO of orders, which gives price-time priority and O(1) cancels.
- Keep one book per stock locate code, and only build books for the chosen symbols.

**Replay simulator**
- Event-driven and in timestamp order. The strategy sees top-of-book or depth updates and sends quote add/cancel requests. Those requests reach the "exchange" after a configurable latency (0 / 10 / 100 µs).
- **Queue position model** (the thing that makes the backtest believable):
  - When a simulated order joins a level, its queue-ahead equals the displayed size already at that level.
  - Executions at that level reduce queue-ahead first.
  - For cancels at that level, assume a fixed fraction came from ahead of us. Report both the pessimistic setting (0 from ahead) and the proportional one.
  - A fill happens when an execution reaches our place in the queue, or when the price trades through our level.
- State the assumption plainly: **no market impact.** Our simulated orders never change what the replayed market does.

**Strategies**
1. Fixed-spread quoter (baseline).
2. **Inventory skew:** shift both quotes by −k·inventory. This controls inventory risk.
3. **Imbalance filter:** compute top-of-book imbalance I = (bid_sz − ask_sz) / (bid_sz + ask_sz), optionally over the top 3 levels. When I < −θ, the bid side is thin and the price is likely to tick down, so pull or widen the bid. Do the reverse for the ask. This targets adverse selection directly.
4. Skew + filter combined.
5. Avellaneda–Stoikov, with σ estimated online from mid-price changes (stretch).

Strategies are small classes behind one interface, and every parameter comes from a config file, so an experiment is just a config sweep.

**Benchmarking method** (interviewers ask about this)
- Release build (`-O3 -march=native`) with the input already in memory, so I/O isn't timed.
- Warm up first and pin the process to one core.
- **Throughput:** time the whole replay loop, not individual messages.
- **Latency:** timing every update adds tens of nanoseconds of overhead, which is about the size of the update itself. So time a sample instead: every Nth message, or batches of 64–1024 messages. Record the timer's own overhead and subtract it.
- Report p50/p99/p99.9 plus the hardware and compiler used.
- **Toolchain:** develop on Windows with MSVC and the CMake + Ninja that ship with VS 2022 Build Tools. Run commands from a "Developer PowerShell for VS 2022" shell. Keep the code portable (no MSVC-only intrinsics without a gcc/clang fallback) so it also builds on Linux.
- **Before quoting final numbers:** rebuild under WSL2 with gcc/clang, profile with `perf`, and report both platforms or the Linux one. CI builds on Ubuntu from the start, which catches portability problems early.

## 5. The Python version (comparison + oracle)

Build it at three levels of optimization, so the comparison shows *where* time goes and not just "C++ is faster":

| Variant | Order store | Price levels | Parse |
|---|---|---|---|
| `naive` | `dict` of objects | `dict` + `sorted()` on demand | `struct.unpack` per field |
| `optimized` | `dict` of `__slots__` / tuples | `sortedcontainers.SortedDict` | `struct.unpack_from` on `mmap` |
| `numba` (stretch) | preallocated NumPy arrays | array ladder, same as C++ | vectorized header scan |

What to measure and write up:
- Throughput (msg/s) and per-update latency (p50/p99) for each variant, using the same fixture and the same symbols.
- **Tail latency:** Python's p99/p99.9 gets worse because of GC and allocation. Measure it with `gc.disable()` and again with `gc.enable()`.
- Memory per resting order (`tracemalloc` for Python; `sizeof` plus allocator overhead for C++).
- A profile breakdown (parse vs. book update) for both languages.
- What *changed* between languages: byte-swapping, object vs. struct layout, choice of container, and GC.

**Correctness:** both engines write the top-of-book stream (`ts, locate, bid_px, bid_sz, ask_px, ask_sz`) to a file, and a pytest checks the two outputs are identical over the fixture and a full symbol-day. Also check invariants: no crossed book during continuous trading, every E/C/X/D message refers to a live order, and sizes never go negative.

## 6. Research analysis (Python)

- **Markouts:** for each fill, compute signed mid-price moves at +100 ms, 1 s, 5 s, and 30 s. Plot the average markout curve by strategy.
- **PnL decomposition:** spread capture vs. adverse selection (markout loss) vs. inventory mark-to-market.
- **Main experiment (2×2):** run baseline, skew only, filter only, and skew + filter, holding the quoted spread, latency, days and symbols fixed. Each strategy has a different job, so measure each on its own axis:
  - Skew is judged on inventory variance and inventory PnL. It may barely change markouts, and that is a finding worth reporting.
  - The filter is judged on 1 s adverse selection per share vs. lost fill volume and lost spread capture. Sweep θ to trace the whole tradeoff curve, instead of reporting only the best point.
- **Avoid overfitting:** choose θ and k on the first days (in-sample) and report results only on the later days (held out). With few days available, use leave-one-day-out.
- **Robustness:** report results by day and by symbol, with bootstrap confidence intervals. Sweep latency (0/10/100 µs) and the queue-model assumption.
- **Claims discipline:** say "on N held-out days × M symbols of ITCH replay, the imbalance filter cut 1 s adverse selection by Z% (95% CI …) at a cost of W% of fill volume". Never claim "the strategy is profitable."
- **State the limitation:** ITCH covers only Nasdaq, so the book and mid price come from one venue, and trades on other venues can't be seen.

## 7. Milestones

| # | Milestone | Done when | Est. |
|---|---|---|---|
| M0 | Setup | Toolchain installed, GitHub repo + CI green, one day downloaded, fixture sliced | ½ day |
| M1 | Parsers | C++ + Python read `.gz` directly and count every message type in a full day, and the two counts match; per-symbol filter tool written | 1–2 days |
| M2 | Book builders | Top-of-book streams from C++ and Python match; invariant tests pass | 2 days |
| M3 | Benchmarks | Throughput, latency percentiles, and memory table for C++ (map vs. flat) and Python variants | 1 day |
| M4 | Simulator + strategies | Queue model with unit tests on hand-built scenarios; fills exported | 3 days |
| M5 | Analysis | Markout curves, decomposition, 2×2 experiment + θ sweep with CIs on held-out days | 2 days |
| M6 | Write-up | README (numbers + one plot at the top), resume bullets filled in | ½ day |

**Systems bullet is ready after M3. Research bullet is ready after M5.** Stretch goals: Avellaneda–Stoikov, a latency-sensitivity study, a parser→book SPSC lock-free queue on two threads, pybind11 bindings, and the Numba variant.

## Ideas for later

- **Exchange rebates (maker-taker fees):** Nasdaq pays a rebate when a resting order is filled. In 2019 this was roughly $0.002–0.003/share, depending on volume tier. Taking liquidity costs about $0.003/share. Against a 1-cent spread, the rebate is a large part of a market maker's revenue. If added, it should be its own line in the M5 PnL breakdown, at a stated assumed rate, with results reported with and without it.
- **From the comparison with similar public repos (2026-10-05):** several projects already do an ITCH parser and book in C++ with a pool, a flat hash and a Python oracle (e.g. KareemJandali/itchbook, cjramsey/NASDAQ-ITCH-LOB, Shashank231205/ItchBook). The engine alone doesn't stand out, so:
  - **Lead with the research result** in the README and resume: many days (Nasdaq has 15), held-out evaluation, confidence intervals, tradeoff curves.
  - **Adverse selection by queue position:** measure how much worse fills are at the back of the queue than at the front. This uses the exact queue model below and ties to the "value of queue position" literature.
  - **Explain engineering findings with measurements:** rerun under WSL2/gcc with `perf stat` cache-miss counts to confirm the pointer-chasing explanation for each variant.
  - **Sanitizers in CI:** an AddressSanitizer + UndefinedBehaviorSanitizer job (cheap; competitors have it).
  - Optional: memory-map an uncompressed file as an input mode.
  - Low priority: a matching engine or an SPSC parser-to-book pipeline. Common in this genre, so it differentiates least.
  - Interview prep: be ready for "how is yours different from X?"
- **Order-store and queue-model alternatives:** once every part has a first version, test aggregate price levels (no per-level linked lists) against the current design, and an exact queue-position model based on order-ref ordering against the assumed cancel fraction.

## Progress log

- **M0 done (2026-09-28):** CMake/CI skeleton, gzip reader in C++ and Python, FLIR+IIVI fixture. Both readers agree on all 368,366,634 messages of 2019-01-30.
- **M1 done (2026-10-02):** C++ and Python decoders for every message type the book needs; `itch_dump` cross-check agrees on every field of the fixture; the full day passes the length, timestamp-order and symbol checks; `itch_filter` written and verified against the scan counts.
  - Note for M2: the stock directory can repeat (CFG-D's `R` message is sent twice), so handle it idempotently.
  - Note for M3: timings on this machine swing 2x (86 s vs 154 s for the same run) while OneDrive is syncing. Benchmark with sync paused and the data already in memory.

- **M2 done (2026-10-02):** first-version book (`unordered_map` order store, `std::map` levels, intrusive FIFO per level) and an independent Python reference (dict + `SortedDict`, no queues).
  - C++ and Python agree on every top-of-book change: 58,209 on the fixture, 1,600,405 for AAPL + MSFT over the full day.
  - Full day, all 8,713 stocks: 0 unknown refs, 0 duplicate refs, 0 overfills, 0 structure problems, 0 live orders at the end of the day, 1.74M live orders at peak.
  - Crossed/locked books in regular hours appear only for SXTC and PHUN, during LULD pauses or within ~100 µs after the reopening cross while Nasdaq publishes the cross results. Note for M4: don't quote during pauses or right after a reopening.
  - Note for M3: the first version manages ~0.5M msg/s on the full day (736 s) but 3.8M msg/s on AAPL + MSFT. Throughput falls as the live order count grows (1.74M vs 65K), which points at cache misses from node-based containers. Printing the stream added another 40%.
  - **Correction (M3):** measured properly (pinned to a performance core, decompression excluded), the same baseline does 3.27M msg/s on the full day, not 0.5M. The 736 s run was unpinned (so it could land on an efficiency core), included decompression, and competed with OneDrive. Live-order growth is real but accounts for a 2.4x slowdown (7.9M msg/s on AAPL + MSFT), not 7x.
- **M3 done (2026-10-05), Windows/MSVC numbers:** six fast variants (order pool x {std::unordered_map, flat open-addressing hash, direct array} x {std::map, sorted vector}) plus the C++ and Python harnesses. Every variant reproduces the baseline's top-of-book checksum, and so does the Python book. Full tables are in `results/bench/`.
  - Full day, all stocks: baseline 3.27M msg/s (306 ns/msg, p99.9 1.43 us, +353 MB); pool/flat-hash/vector 9.41M (106 ns, p99.9 548 ns, +226 MB); pool/direct/vector 13.92M (72 ns, p99.9 582 ns, +2.5 GB), 4.3x the baseline.
  - AAPL + MSFT: flat-hash/vector is best (60 ns/msg, 2.1x the baseline). The direct index is 5x slower there and uses 1.7-2.3 GB, because refs are numbered across the whole market, so a filtered file uses a sparse slice of a huge array. Which index wins depends on the workload.
  - The pool alone (with std::unordered_map and std::map) is no faster than the baseline. The gains come from removing pointer chasing in the lookups: flat hash or direct index, plus vector levels (about 2x on the full day).
  - Python reference book: 5.2 us/msg on AAPL + MSFT (86x slower than the best C++), 210 bytes per resting order vs 32 in the C++ pool. The naive dict book is 19x slower again (97 us/msg). Turning the GC off halved p99.9 on the fixture but made no difference on AAPL + MSFT.
  - Still to do: rerun under WSL2/gcc with `perf` cache-miss counts before quoting final numbers.
- **M4 done (2026-10-05):** replay simulator (`lob/sim.hpp`) on the flat-hash/vector book, plus a configurable market maker (`lob/strategy.hpp`) and the `backtest` app, which writes per-fill CSVs with markouts at 100 ms / 1 s / 5 s / 30 s.
  - Model: no market impact; 10 us latency on new orders and cancels (orders can fill while a cancel is in flight); post-only rejects; the queue model as planned (pessimistic or proportional cancels, ahead clamped to the displayed level); trade-through and crossing-add fills; quoting only after the opening cross, while trading, 1 s after a reopening, and until 15:59. The fast book got an optional event listener (compiles to nothing when unused) so the simulator sees executions and cancels with price and side resolved.
  - Found: ITCH 5.0 reports every hidden ('P') trade with side 'B' (22,262/22,262 for AAPL + MSFT), so hidden trades are used only by price.
  - Tests: 14 hand-built scenarios (queue fills, latency, both cancel models, clamping, trade-through, hidden trades, crossing adds, post-only, fill during cancel, halt + cooldown, markouts, skew clamping, limits). 49 C++ tests total.
  - Sanity grid on AAPL + MSFT (`results/m4/sanity_grid.md`): baseline captures +0.70 c/share of spread but loses 0.93 c/share to 1 s adverse selection (-$9.0K before rebates). Skew cuts max inventory 594 -> 385. The imbalance filter halves the loss by trading less, but per-share adverse selection doesn't improve at theta = 0.5, even though the signal itself is valid (P(next move down) is 0.61 when the bid side is thin vs 0.35 when it's heavy). Open question for M5.
  - Caveats for the write-up: the position limit is soft (in-flight cancels); no-impact fills can double-count an aggressor's volume (queue fill at our level plus trade-through at the next level).
- **M5 first round done (2026-10-05):** 4 days (tuning 01-30; held out 03-27, 07-30, 10-30) x 8 symbols (AAPL MSFT AMD INTC CSCO CMCSA NVDA FB). Pipeline: `scripts/m5.py` (tune / select / test / report) and `scripts/m5_report.py`. The selection rules and split were fixed in the script docstring before running the held-out days. Report: `results/m5/report.md`.
  - Selected on the tuning day: theta* = 0.8 (every filter setting lowered per-share net markout; 0.8 lowered it least), k* = 1.
  - Held out: baseline net 1 s markout -0.343 c/share [-0.361, -0.324] (spread +0.66, AS -1.01), -$12.9K/day across the 8 stocks; break-even maker rebate about 0.34 c/share. Skew k=1: inventory RMS 291 -> 70 shares, net -0.045 c/share vs baseline, -$6.0K/day. Filter: volume -28%, net -0.018 c/share [-0.024, -0.011], but +$3.7K/day [2.6K, 4.8K] from trading less.
  - Found, then corrected: the pooled cents-per-share view of adverse selection by imbalance (and the filter's per-share effect) is distorted by composition across stocks with different prices and tick regimes (Simpson's paradox). Within-symbol bps analysis: imbalance predicts adverse selection strongly for large-tick stocks (MSFT AMD INTC CSCO CMCSA; -0.46 -> -1.74 bps) and not for small-tick ones (AAPL NVDA FB; flat about -0.9 bps). Filter effect: large-tick +0.005 c [-0.001, +0.012] (INTC, CSCO significantly positive), small-tick -0.024 c [-0.036, -0.014]. Queue position: behind >2,000 shares on arrival is more adverse (large-tick -1.71 vs -1.54 bps; small-tick -1.65 vs -0.95). Tick groups were defined on the tuning day, but the split itself is **post hoc**.
  - Robustness: baseline net worsens with latency (-0.315 at 0 us, -0.343 at 10 us, -0.406 at 100 us); the pessimistic cancel model cuts volume 38% and worsens net to -0.377. The filter's per-share effect stays slightly negative in every setting.
  - **Next (proposed):** confirm the tick-size hypothesis on fresh days, fixed in advance: "on large-tick stocks the imbalance filter improves net 1 s markout per share; on small-tick stocks it does not". Choose theta per tick group on the current 4 days, then test on 3-4 new days from the 11 unused ones. Then add the rebate line to the PnL breakdown (see Ideas for later).

## 8. Resume bullets (fill in after M3/M5)

- Built a C++20 engine that reconstructs Nasdaq TotalView-ITCH order books, replaying **N M** messages at **X M msg/s** (p99 update **Y ns**); **Kx** faster than a reference Python implementation used as a correctness oracle
- Backtested market-making strategies with queue-position modeling on **N days × M symbols** of replay; an order-book-imbalance filter cut 1 s adverse selection by **Z%** for **W%** less fill volume, while inventory skew reduced inventory variance by **V%**
