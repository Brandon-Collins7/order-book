"""Benchmarks the Python implementation with the same method and output as cpp/bench/lob_bench.

  python scripts/bench_python.py <file.gz> --variant NAME [--symbols A,B] [--repeat N]
         [--sample-every K] [--gc on|off]
  python scripts/bench_python.py <file.gz> --memory      # bytes per resting order

Variants:
  frame             walk the message framing only
  parse             decode every message into a NamedTuple
  book/sorteddict   reference book (dict of orders, SortedDict levels) + top-of-book tracking
  book/naive        same with plain dict levels; finding the best price scans every level

The file is decompressed into memory before timing. Every K-th message is timed on its own
with perf_counter_ns, minus the timer's overhead. Book variants report the same FNV-1a
checksum over the top-of-book stream as the C++ benchmark, so equal checksums mean the
Python and C++ books produced identical output.
"""

import argparse
import gc
import gzip
import json
import platform
import sys
import time
import tracemalloc
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "python"))
from pylob.book import Book, BookBuilder, NaiveBook  # noqa: E402
from pylob.itch import AddOrder, decode  # noqa: E402

FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
MASK = (1 << 64) - 1


def make_work(variant, symbols):
    """Returns (work(buf, off, end), result()) for one fresh run."""
    if variant == "frame":
        acc = [0]

        def work(buf, off, end):
            acc[0] += buf[off] + end - off

        return work, lambda: (acc[0] & MASK, 0, None)

    if variant == "parse":

        def work(buf, off, end):
            decode(buf, off, end)

        return work, lambda: (0, 0, None)

    book_type = {"book/sorteddict": Book, "book/naive": NaiveBook}[variant]
    builder = BookBuilder(symbols, book_type=book_type)
    apply = builder.apply
    books = builder.books
    last = {}
    state = [FNV_OFFSET, 0]  # checksum, top-of-book changes

    def work(buf, off, end):
        m = decode(buf, off, end)
        locate = apply(m)
        if locate is None:
            return
        top = books[locate].top()
        if last.get(locate) != top:
            last[locate] = top
            h = state[0]
            for v in (m.timestamp, locate, *top):
                h = ((h ^ v) * FNV_PRIME) & MASK
            state[0] = h
            state[1] += 1

    return work, lambda: (state[0], state[1], builder)


def replay(data, work, sample_every):
    samples = []
    countdown = sample_every
    clock = time.perf_counter_ns
    n = 0
    off = 0
    total = len(data)
    t0 = time.perf_counter()
    while off < total:
        end = off + 2 + ((data[off] << 8) | data[off + 1])
        countdown -= 1
        if countdown == 0:
            countdown = sample_every
            a = clock()
            work(data, off + 2, end)
            samples.append(clock() - a)
        else:
            work(data, off + 2, end)
        off = end
        n += 1
    return time.perf_counter() - t0, n, samples


def timer_overhead_ns():
    clock = time.perf_counter_ns
    best = 1 << 62
    for _ in range(100_000):
        a = clock()
        best = min(best, clock() - a)
    return best


def bench(args):
    data = gzip.open(args.file, "rb").read()
    symbols = [s for s in args.symbols.split(",") if s]
    overhead = timer_overhead_ns()
    if args.gc == "off":
        gc.disable()

    runs = []
    for _ in range(args.repeat):
        gc.collect()
        work, result = make_work(args.variant, symbols)
        secs, n, samples = replay(data, work, args.sample_every)
        checksum, changes, builder = result()
        runs.append((secs, n, samples, checksum, changes, builder))

    secs = [r[0] for r in runs]
    median = sorted(secs)[len(secs) // 2]
    n = runs[0][1]
    if len({r[3] for r in runs}) != 1:
        sys.exit("checksum differs between repeats")
    s = sorted(max(0, x - overhead) for x in runs[-1][2])

    def pct(q):
        return float(s[min(len(s) - 1, int(q * len(s)))]) if s else 0.0

    builder = runs[0][5]
    out = {
        "variant": f"python {args.variant}" + (" (gc off)" if args.gc == "off" else ""),
        "file": args.file,
        "symbols": args.symbols,
        "messages": n,
        "runs_seconds": [round(x, 4) for x in secs],
        "median_seconds": round(median, 4),
        "msgs_per_sec": round(n / median),
        "ns_per_msg": round(median * 1e9 / n, 2),
        "latency_ns": {"p50": pct(0.5), "p90": pct(0.9), "p99": pct(0.99), "p99.9": pct(0.999), "max": float(s[-1]) if s else 0.0},
        "latency_samples": len(s),
        "sample_every": args.sample_every,
        "timer_overhead_ns": overhead,
        "top_changes": runs[0][4],
        "checksum": f"{runs[0][3]:016x}" if builder else "",
        "peak_live_orders": builder.stats.max_live_orders if builder else 0,
        "gc": args.gc,
        "python": platform.python_version(),
    }
    print(json.dumps(out))


def memory_per_order(args, sample=200_000):
    """Bytes per resting order in the reference book's order dict, using real orders from the file."""
    adds = []
    data = gzip.open(args.file, "rb").read()
    off = 0
    while off < len(data) and len(adds) < sample:
        end = off + 2 + ((data[off] << 8) | data[off + 1])
        if data[off + 2] in (ord("A"), ord("F")):
            adds.append(decode(data, off + 2, end))
        off = end
    del data
    tracemalloc.start()
    before = tracemalloc.get_traced_memory()[0]
    # Built exactly as BookBuilder stores them: ref -> [locate, side, price, shares]
    orders = {}
    for m in adds:
        orders[int(str(m.ref))] = [int(str(m.locate)), m.side, int(str(m.price)), int(str(m.shares))]
    after = tracemalloc.get_traced_memory()[0]
    tracemalloc.stop()
    assert all(isinstance(a, AddOrder) for a in adds)
    print(json.dumps({"variant": "python order dict", "orders": len(orders),
                      "bytes_per_order": round((after - before) / len(orders), 1)}))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("file")
    p.add_argument("--variant", choices=["frame", "parse", "book/sorteddict", "book/naive"])
    p.add_argument("--symbols", default="")
    p.add_argument("--repeat", type=int, default=3)
    p.add_argument("--sample-every", type=int, default=64)
    p.add_argument("--gc", choices=["on", "off"], default="on")
    p.add_argument("--memory", action="store_true")
    args = p.parse_args()
    if args.memory:
        memory_per_order(args)
    elif args.variant:
        bench(args)
    else:
        p.error("give --variant or --memory")


if __name__ == "__main__":
    main()
