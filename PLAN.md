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

## Progress log

- **M0 done (2026-09-28):** CMake/CI skeleton, gzip reader in C++ and Python, FLIR+IIVI fixture. Both readers agree on all 368,366,634 messages of 2019-01-30.
- **M1 done (2026-10-02):** C++ and Python decoders for every message type the book needs; `itch_dump` cross-check agrees on every field of the fixture; the full day passes the length, timestamp-order and symbol checks; `itch_filter` written and verified against the scan counts.
  - Note for M2: the stock directory can repeat (CFG-D's `R` message is sent twice), so handle it idempotently.
  - Note for M3: timings on this machine swing 2x (86 s vs 154 s for the same run) while OneDrive is syncing. Benchmark with sync paused and the data already in memory.

## 8. Resume bullets (fill in after M3/M5)

- Built a C++20 engine that reconstructs Nasdaq TotalView-ITCH order books, replaying **N M** messages at **X M msg/s** (p99 update **Y ns**); **Kx** faster than a reference Python implementation used as a correctness oracle
- Backtested market-making strategies with queue-position modeling on **N days × M symbols** of replay; an order-book-imbalance filter cut 1 s adverse selection by **Z%** for **W%** less fill volume, while inventory skew reduced inventory variance by **V%**
