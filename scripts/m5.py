"""M5: market-making experiments on held-out days, with block-bootstrap confidence intervals.

  python scripts/m5.py tune                 # parameter grid on the tuning day
  python scripts/m5.py select               # apply the pre-registered rules -> results/m5/selection.json
  python scripts/m5.py test                 # 2x2, sweeps and robustness runs on the held-out days
  python scripts/m5.py report               # tables + figures -> results/m5/report.md

Design (fixed before looking at the held-out days):
- Tuning day 2019-01-30 (already used for the M4 sanity checks). Held-out days: 2019-03-27,
  2019-07-30, 2019-10-30. Universe: AAPL MSFT AMD INTC CSCO CMCSA NVDA FB.
- theta*: maximizes net 1 s markout per share (spread capture + 1 s adverse selection) on the
  tuning day, among thresholds that keep >= 25% of the baseline's traded volume.
- k*: minimizes time-weighted RMS inventory on the tuning day, among skews whose net 1 s markout
  per share is within 0.1 cents of the baseline's.
- Confidence intervals: bootstrap over (day, symbol, 30-minute window) blocks, 2,000 resamples,
  percentile 95% intervals. Strategy comparisons resample the same blocks for both (paired).
"""

import argparse
import json
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import numpy as np
import pandas as pd

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / "build" / "release" / ("backtest.exe" if sys.platform == "win32" else "backtest")
OUT = ROOT / "results" / "m5"
RUNS = OUT / "runs"

TUNE_DAYS = ["01302019"]
TEST_DAYS = ["03272019", "07302019", "10302019"]
SYMBOLS = ["AAPL", "MSFT", "AMD", "INTC", "CSCO", "CMCSA", "NVDA", "FB"]
K_GRID = [0.0, 0.5, 1.0, 2.0]
THETA_GRID = [None, 0.8, 0.6, 0.4, 0.2]  # None = filter off
HORIZONS = ["100ms", "1s", "5s", "30s"]
OPEN_NS, STOP_NS = 34_200 * 10**9, 57_540 * 10**9  # 09:30:00 and 15:59:00
BLOCK_NS = 30 * 60 * 10**9
B = 2000
SEED = 7


def tag(k=0.0, theta=None, latency=10, cancel="proportional"):
    t = f"k{k:g}_th{'off' if theta is None else f'{theta:g}'}"
    if latency != 10:
        t += f"_lat{latency:g}"
    if cancel != "proportional":
        t += f"_{cancel}"
    return t


# ---- running backtests -----------------------------------------------------------------

def run_one(day, k=0.0, theta=None, latency=10, cancel="proportional"):
    name = tag(k, theta, latency, cancel)
    dest = RUNS / day / f"{name}.parquet"
    if dest.exists():
        return name
    dest.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        csv = Path(tmp) / "fills.csv"
        cmd = [str(EXE), str(ROOT / "data" / "filtered" / f"{day}_universe.itch.gz"), "--symbols", ",".join(SYMBOLS),
               "--out", str(csv), "--k", str(k), "--theta", str(2.0 if theta is None else theta),
               "--latency-us", str(latency), "--cancel-model", cancel]
        out = subprocess.run(cmd, capture_output=True, text=True)
        if out.returncode != 0:
            raise RuntimeError(f"{day} {name}: {out.stderr}")
        summary = json.loads(out.stdout)
        summary.update(day=day, tag=name, k=k, theta=theta, latency=latency, cancel=cancel)
        pd.read_csv(csv).to_parquet(dest, compression="zstd")
    (RUNS / day / f"{name}.json").write_text(json.dumps(summary))
    print(f"  {day} {name}: {summary['fills']} fills in {summary['seconds']:.1f} s", flush=True)
    return name


def run_many(jobs):
    with ThreadPoolExecutor(max_workers=8) as pool:
        for f in [pool.submit(run_one, *j) for j in jobs]:
            f.result()


def tune_jobs():
    return [(d, k, th) for d in TUNE_DAYS for k in K_GRID for th in THETA_GRID]


def test_jobs(k_star, theta_star):
    jobs = set()
    for d in TEST_DAYS:
        for k in (0.0, k_star):  # the 2x2
            for th in (None, theta_star):
                jobs.add((d, k, th, 10, "proportional"))
        for th in THETA_GRID:  # filter sweep
            jobs.add((d, 0.0, th, 10, "proportional"))
        for k in K_GRID:  # skew sweep
            jobs.add((d, k, None, 10, "proportional"))
        for th in (None, theta_star):  # robustness: latency and cancel model
            for lat in (0, 100):
                jobs.add((d, 0.0, th, lat, "proportional"))
            jobs.add((d, 0.0, th, 10, "pessimistic"))
    return sorted(jobs, key=lambda j: (j[0], str(j[1:])))


# ---- per-block aggregates ---------------------------------------------------------------

def load(day, name):
    fills = pd.read_parquet(RUNS / day / f"{name}.parquet")
    summary = json.loads((RUNS / day / f"{name}.json").read_text())
    return fills, summary


def blocks(day, name):
    """Sums per (day, symbol, 30-minute block): everything a per-share ratio needs."""
    f, _ = load(day, name)
    keys = [(day, s, b) for s in SYMBOLS for b in range(int(OPEN_NS // BLOCK_NS), int(STOP_NS // BLOCK_NS) + 1)]
    out = pd.DataFrame(0.0, index=pd.MultiIndex.from_tuples(keys, names=["day", "symbol", "block"]),
                       columns=["shares", "fills", "spread", "inv_sq_dt", "dt"]
                       + [f"as_{h}" for h in HORIZONS] + [f"shares_{h}" for h in HORIZONS])
    inventory_fills = f.copy()  # every fill moves the position, even if the mid was undefined
    f = f[f.mid > 0].copy()
    if len(f):
        f["block"] = (f.ts_ns // BLOCK_NS).astype(int)
        sign = f.side.map({"B": 1, "S": -1})
        q = f.shares
        f["spread_usd"] = sign * (f.mid - f.price) * q
        cols = {"shares": q, "fills": 1, "spread": f.spread_usd}
        for h in HORIZONS:
            ok = f[f"mid_{h}"] > 0
            cols[f"as_{h}"] = np.where(ok, sign * (f[f"mid_{h}"] - f.mid) * q, 0.0)
            cols[f"shares_{h}"] = np.where(ok, q, 0)
        g = pd.DataFrame(cols).assign(day=day, symbol=f.symbol.values, block=f.block.values)
        out = out.add(g.groupby(["day", "symbol", "block"])[list(cols)].sum(), fill_value=0)
    # Time-weighted inventory: position is constant between fills, from 09:30 to 15:59. Each
    # interval is credited to the block it starts in (fills are frequent, so intervals are short).
    parts = []
    for s in SYMBOLS:
        g = inventory_fills[inventory_fills.symbol == s]
        ts = np.concatenate([[OPEN_NS], np.clip(g.ts_ns.to_numpy(), OPEN_NS, STOP_NS), [STOP_NS]]).astype(float)
        pos = np.concatenate([[0], g.position.to_numpy()]).astype(float)
        dt = np.diff(ts)
        parts.append(pd.DataFrame({"day": day, "symbol": s, "block": (ts[:-1] // BLOCK_NS).astype(int),
                                   "inv_sq_dt": pos * pos * dt, "dt": dt}))
    inv = pd.concat(parts).groupby(["day", "symbol", "block"])[["inv_sq_dt", "dt"]].sum()
    return out.add(inv, fill_value=0).fillna(0)


def stats(m, counts=None):
    """Per-share metrics (cents) and inventory from block sums, optionally bootstrap-weighted."""
    w = np.ones(len(m)) if counts is None else counts
    tot = lambda c: w @ m[c].to_numpy()
    shares = tot("shares")
    r = {"shares": shares, "spread_c": 100 * tot("spread") / shares}
    for h in HORIZONS:
        r[f"as_{h}_c"] = 100 * tot(f"as_{h}") / tot(f"shares_{h}")
        r[f"net_{h}_c"] = r["spread_c"] + r[f"as_{h}_c"]
    r["inv_rms"] = np.sqrt(tot("inv_sq_dt") / tot("dt"))
    return r


def bootstrap(tables, metric_fns, rng):
    """tables: name -> block frame (same index). Returns name -> metric -> (point, lo, hi), plus
    paired differences vs. the first table."""
    names = list(tables)
    idx = tables[names[0]].index
    n = len(idx)
    counts = rng.multinomial(n, np.full(n, 1 / n), size=B).astype(float)
    point = {k: stats(tables[k]) for k in names}
    draws = {k: [stats(tables[k], c) for c in counts] for k in names}
    out = {}
    for k in names:
        out[k] = {}
        for m in metric_fns:
            v = np.array([d[m] for d in draws[k]])
            base = np.array([d[m] for d in draws[names[0]]])
            lo, hi = np.percentile(v, [2.5, 97.5])
            dlo, dhi = np.percentile(v - base, [2.5, 97.5])
            out[k][m] = dict(point=point[k][m], lo=lo, hi=hi, diff=point[k][m] - point[names[0]][m], dlo=dlo, dhi=dhi)
    return out


def pnl_by_day_symbol(day, name):
    _, s = load(day, name)
    return {(day, x["symbol"]): x["pnl"] for x in s["symbols"]}


# ---- selection ------------------------------------------------------------------------------

def select():
    rows = []
    for d in TUNE_DAYS:
        for k in K_GRID:
            for th in THETA_GRID:
                st = stats(blocks(d, tag(k, th)))
                rows.append(dict(k=k, theta=th, **st))
    t = pd.DataFrame(rows)
    base = t[(t.k == 0) & t.theta.isna()].iloc[0]
    filt = t[(t.k == 0) & t.theta.notna() & (t.shares >= 0.25 * base.shares)]
    theta_star = float(filt.loc[filt["net_1s_c"].idxmax(), "theta"])
    skew = t[t.theta.isna() & (t["net_1s_c"] >= base["net_1s_c"] - 0.1)]
    k_star = float(skew.loc[skew["inv_rms"].idxmin(), "k"])
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "selection.json").write_text(json.dumps({"theta_star": theta_star, "k_star": k_star}, indent=1))
    t.to_csv(OUT / "tuning_grid.csv", index=False)
    print(t.round(3).to_string(index=False))
    print(f"\ntheta* = {theta_star}, k* = {k_star}")
    return theta_star, k_star


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("cmd", choices=["tune", "select", "test", "report"])
    a = p.parse_args()
    if a.cmd == "tune":
        run_many(tune_jobs())
    elif a.cmd == "select":
        select()
    elif a.cmd == "test":
        sel = json.loads((OUT / "selection.json").read_text())
        run_many(test_jobs(sel["k_star"], sel["theta_star"]))
    else:
        from m5_report import report  # noqa: E402  (kept separate: tables and figures)

        report()


if __name__ == "__main__":
    main()
