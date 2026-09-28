"""Scan a gzipped ITCH 5.0 day file and cut a small, self-contained test fixture from it.

Two subcommands:

  scan    One pass over the whole day. Counts messages by type and by stock locate,
          counts executions per locate, and records the locate -> symbol directory.
          Writes a JSON summary (also used later to cross-check the parsers).

  slice   Writes a gzipped fixture containing every market-wide message (locate 0),
          every stock directory ('R') message, and every message for the chosen symbols.
          The result is a valid ITCH stream covering the full trading day.

Usage:
  python scripts/slice_fixture.py scan  data/01302019.NASDAQ_ITCH50.gz results/01302019_counts.json
  python scripts/slice_fixture.py slice data/01302019.NASDAQ_ITCH50.gz tests/fixtures/sample.itch.gz \
         --summary results/01302019_counts.json --symbols AAA BBB
  python scripts/slice_fixture.py slice ... --auto 2    # pick symbols automatically from the summary
"""

import argparse
import gzip
import json
import sys
import time
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "python"))
from pylob.reader import iter_messages  # noqa: E402

EXEC_TYPES = {ord("E"), ord("C"), ord("P")}
R = ord("R")


def scan(src, out):
    t0 = time.perf_counter()
    by_type = [0] * 256
    by_locate = [0] * 65536
    execs = [0] * 65536
    symbols = {}
    total = 0
    for buf, off, _ in iter_messages(src):
        t = buf[off]
        loc = (buf[off + 1] << 8) | buf[off + 2]
        by_type[t] += 1
        by_locate[loc] += 1
        if t in EXEC_TYPES:
            execs[loc] += 1
        elif t == R:
            symbols[loc] = buf[off + 11 : off + 19].decode("ascii").rstrip()
        total += 1
        if total % 50_000_000 == 0:
            print(f"  {total / 1e6:.0f}M messages, {time.perf_counter() - t0:.0f}s", flush=True)
    secs = time.perf_counter() - t0
    summary = {
        "file": str(src),
        "total_messages": total,
        "scan_seconds": round(secs, 1),
        "by_type": {chr(t): c for t, c in enumerate(by_type) if c},
        "stocks": {
            str(loc): {"symbol": sym, "messages": by_locate[loc], "executions": execs[loc]}
            for loc, sym in sorted(symbols.items())
        },
        "locate0_messages": by_locate[0],
    }
    with open(out, "w") as f:
        json.dump(summary, f, indent=1)
    print(f"{total:,} messages in {secs:.0f}s ({total / secs / 1e6:.2f}M msg/s). Wrote {out}")


def auto_pick(stocks, k, lo=20_000, hi=80_000, min_execs=1_000):
    """Pick k symbols with a moderate message count and real trading activity, busiest first."""
    cands = [
        (s["messages"], s["symbol"])
        for s in stocks.values()
        if lo <= s["messages"] <= hi and s["executions"] >= min_execs
    ]
    cands.sort(reverse=True)
    return [sym for _, sym in cands[:k]]


def slice_(src, out, summary_path, symbols, auto):
    with open(summary_path) as f:
        stocks = json.load(f)["stocks"]
    if auto:
        symbols = auto_pick(stocks, auto)
    by_symbol = {s["symbol"]: int(loc) for loc, s in stocks.items()}
    missing = [s for s in symbols if s not in by_symbol]
    if missing:
        raise SystemExit(f"unknown symbols: {missing}")
    keep = {by_symbol[s] for s in symbols}
    print("fixture symbols:", {s: by_symbol[s] for s in symbols})

    kept = Counter()
    with gzip.open(out, "wb", compresslevel=9) as w:
        for buf, off, end in iter_messages(src):
            loc = (buf[off + 1] << 8) | buf[off + 2]
            t = buf[off]
            if loc == 0 or t == R or loc in keep:
                w.write(buf[off - 2 : end])
                kept[chr(t)] += 1
    total = sum(kept.values())
    # Expected counts for the C++ and Python tests, in the same format itch_stats prints.
    counts_path = Path(out).with_name(Path(out).name.replace(".itch.gz", ".counts.txt"))
    with open(counts_path, "w", newline="\n") as f:
        for t in sorted(kept):
            f.write(f"{t} {kept[t]}\n")
        f.write(f"total {total}\n")
    print(f"wrote {total:,} messages to {out} and counts to {counts_path}")
    print("by type:", dict(sorted(kept.items())))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("scan")
    s.add_argument("src")
    s.add_argument("out")
    c = sub.add_parser("slice")
    c.add_argument("src")
    c.add_argument("out")
    c.add_argument("--summary", required=True)
    g = c.add_mutually_exclusive_group(required=True)
    g.add_argument("--symbols", nargs="+")
    g.add_argument("--auto", type=int, metavar="K")
    a = p.parse_args()
    if a.cmd == "scan":
        scan(a.src, a.out)
    else:
        slice_(a.src, a.out, a.summary, a.symbols or [], a.auto)


if __name__ == "__main__":
    main()
