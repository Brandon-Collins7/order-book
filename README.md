# OrderBook

A C++20 engine that rebuilds Nasdaq TotalView-ITCH 5.0 order books and replays them through a market-making simulator. A reference implementation in Python checks its correctness and gives a performance comparison. See [PLAN.md](PLAN.md) for the design and milestones.

> Work in progress: reading, decoding and order-book reconstruction are done and cross-checked between C++ and Python (M0–M2). Benchmarks come next (M3).

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

`ctest` includes two cross-language checks on the fixture: `cross_check_decoders` (every decoded field of every message) and `cross_check_books` (every top-of-book change). Run them on any file with `scripts/cross_check.py`.

## Python

```sh
pip install -e ".[dev]"
pytest
```
