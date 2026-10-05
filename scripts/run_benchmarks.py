"""Runs the C++ and Python benchmarks on one dataset and writes the results.

  python scripts/run_benchmarks.py aapl_msft     # AAPL + MSFT, full day: C++ and Python
  python scripts/run_benchmarks.py full_day      # every stock, full day: C++ only
  python scripts/run_benchmarks.py fixture       # quick check on the test fixture

Each variant runs in its own process, so memory measurements don't leak between variants.
Raw results go to results/bench/<dataset>.jsonl and a Markdown table to results/bench/<dataset>.md.
"""

import json
import platform
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / "build" / "release" / ("lob_bench.exe" if sys.platform == "win32" else "lob_bench")
OUT = ROOT / "results" / "bench"

CPP_BOOKS = ["baseline"] + [f"pool/{i}/{l}" for i in ("std-hash", "flat-hash", "direct") for l in ("map", "vector")]

DATASETS = {
    "fixture": dict(file="tests/fixtures/sample.itch.gz", symbols="", repeat=3,
                    cpp=["frame", "parse"] + CPP_BOOKS,
                    python=[("frame", "on"), ("parse", "on"), ("book/sorteddict", "on"), ("book/sorteddict", "off"),
                            ("book/naive", "on")]),
    "aapl_msft": dict(file="data/filtered/01302019_AAPL_MSFT.itch.gz", symbols="AAPL,MSFT", repeat=5,
                      cpp=["frame", "parse"] + CPP_BOOKS,
                      python=[("frame", "on"), ("parse", "on"), ("book/sorteddict", "on"), ("book/sorteddict", "off"),
                              ("book/naive", "on")]),
    "full_day": dict(file="data/01302019.NASDAQ_ITCH50.gz", symbols="", repeat=1,
                     cpp=["frame", "parse"] + CPP_BOOKS, python=[]),
}


def cpu_name():
    if sys.platform == "win32":
        import winreg

        key = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DESCRIPTION\System\CentralProcessor\0")
        return winreg.QueryValueEx(key, "ProcessorNameString")[0].strip()
    for line in Path("/proc/cpuinfo").read_text().splitlines():
        if line.startswith("model name"):
            return line.split(":", 1)[1].strip()
    return platform.processor() or platform.machine()


def run(cmd):
    print("  $", " ".join(str(c) for c in cmd), flush=True)
    out = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if out.returncode != 0:
        sys.exit(f"failed: {out.stderr}")
    return json.loads(out.stdout.strip().splitlines()[-1])


def table(results):
    rows = ["| Variant | M msg/s | ns/msg | p50 ns | p99 ns | p99.9 ns | Memory growth | Checksum |",
            "|---|---:|---:|---:|---:|---:|---:|---|"]
    for r in results:
        lat = r["latency_ns"]
        mem = f"{r['mem_growth_mb']:.0f} MB" if "mem_growth_mb" in r else "-"
        rows.append(f"| {r['variant']} | {r['msgs_per_sec'] / 1e6:.2f} | {r['ns_per_msg']:.1f} | {lat['p50']:.0f} | "
                    f"{lat['p99']:.0f} | {lat['p99.9']:.0f} | {mem} | `{r.get('checksum', '')}` |")
    return "\n".join(rows)


def main():
    if len(sys.argv) != 2 or sys.argv[1] not in DATASETS:
        sys.exit(__doc__)
    name = sys.argv[1]
    d = DATASETS[name]
    results = []
    for v in d["cpp"]:
        cmd = [EXE, d["file"], "--variant", v, "--repeat", str(d["repeat"])]
        if d["symbols"]:
            cmd += ["--symbols", d["symbols"]]
        results.append(run(cmd))
    for v, gc in d["python"]:
        cmd = [sys.executable, "scripts/bench_python.py", d["file"], "--variant", v, "--gc", gc,
               "--repeat", str(min(d["repeat"], 3)), "--symbols", d["symbols"]]
        results.append(run(cmd))
    memory = run([sys.executable, "scripts/bench_python.py", d["file"], "--memory"]) if d["python"] else None

    books = {r["checksum"] for r in results if r.get("checksum") and r["variant"] not in ("frame", "parse")
             and r.get("top_changes")}
    OUT.mkdir(parents=True, exist_ok=True)
    with open(OUT / f"{name}.jsonl", "w") as f:
        for r in results + ([memory] if memory else []):
            f.write(json.dumps(r) + "\n")

    first = results[0]
    md = [f"# Benchmark: {name}", "",
          f"- File: `{d['file']}`" + (f", symbols {d['symbols']}" if d["symbols"] else ", all stocks"),
          f"- Messages: {first['messages']:,}",
          f"- Machine: {cpu_name()}, {platform.system()} {platform.release()}; "
          f"C++ {first.get('compiler', '')}, pinned to CPU {first.get('cpu', '')}; Python {platform.python_version()}",
          f"- Every {first['sample_every']}th message timed individually; timer overhead subtracted. "
          "Python's clock on Windows ticks every 100 ns.",
          f"- All book variants agree on the top-of-book checksum: {'yes' if len(books) == 1 else 'NO: ' + str(books)}",
          "", table(results), ""]
    if memory:
        md.append(f"Python order dict: {memory['bytes_per_order']} bytes per resting order "
                  f"(C++ pool: 32 bytes per order plus the index).")
    (OUT / f"{name}.md").write_text("\n".join(md) + "\n", encoding="utf-8")
    print("\n".join(md))


if __name__ == "__main__":
    main()
