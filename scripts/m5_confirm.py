"""M5 confirmation round: runs and tests exactly what results/m5/confirmation_plan.md registered.

  python scripts/m5_confirm.py run       # baseline and filter (theta = 0.8) on the confirmation days
  python scripts/m5_confirm.py analyze   # H1-H5 -> results/m5/confirmation.md

Nothing here may change after the plan was committed except to fix bugs; any change is listed
in the results file.
"""

import sys

import numpy as np
import pandas as pd

import m5

CONFIRM_DAYS = ["08302019", "12302019", "01302020"]
THETA = 0.8
LARGE = ["MSFT", "AMD", "INTC", "CSCO", "CMCSA"]
SMALL = ["AAPL", "NVDA", "FB"]
B, SEED = 2000, 11
BLOCK_NS = 30 * 60 * 10**9


def fills(name):
    parts = []
    for d in CONFIRM_DAYS:
        f = pd.read_parquet(m5.RUNS / d / f"{name}.parquet")
        f = f[(f.mid > 0) & (f.mid_1s > 0)].copy()
        s = f.side.map({"B": 1, "S": -1})
        f["net"] = 1e4 * s * (f.mid_1s - f.price) / f.mid * f.shares  # bps x shares
        f["as1"] = 1e4 * s * (f.mid_1s - f.mid) / f.mid * f.shares
        f["against"] = np.where(f.side == "B", -f.imbalance, f.imbalance)
        f["key"] = list(zip([d] * len(f), f.symbol, f.ts_ns // BLOCK_NS))
        parts.append(f)
    return pd.concat(parts)


class Ratios:
    """Per-symbol share-weighted ratios under shared bootstrap resamples of (day, symbol, block) keys."""

    def __init__(self, keys, rng):
        self.keys = keys
        self.pos = {k: i for i, k in enumerate(keys)}
        n = len(keys)
        self.counts = np.vstack([np.ones(n), rng.multinomial(n, np.full(n, 1 / n), size=B)]).astype(float)

    def ratio(self, f, col):
        """Row 0: point estimate; rows 1..B: bootstrap draws. NaN if no shares."""
        g = f.groupby("key")[[col, "shares"]].sum()
        num, den = np.zeros(len(self.keys)), np.zeros(len(self.keys))
        idx = [self.pos[k] for k in g.index]
        num[idx], den[idx] = g[col].to_numpy(), g.shares.to_numpy()
        d = self.counts @ den
        with np.errstate(invalid="ignore", divide="ignore"):
            return np.where(d > 0, (self.counts @ num) / d, np.nan)


def group_mean(per_symbol, symbols):
    return np.nanmean(np.vstack([per_symbol[s] for s in symbols]), axis=0)


def summarize(v, positive_is_good):
    point, draws = v[0], v[1:]
    lo, hi = np.nanpercentile(draws, [2.5, 97.5])
    if positive_is_good:
        verdict = "confirmed" if lo > 0 else ("contradicted" if hi < 0 else "inconclusive")
    else:
        verdict = "confirmed" if hi < 0 else ("contradicted" if lo > 0 else "inconclusive")
    return point, lo, hi, verdict


def analyze():
    rng = np.random.default_rng(SEED)
    base, filt = fills(m5.tag(0, None)), fills(m5.tag(0, THETA))
    keys = sorted(set(base.key) | set(filt.key))
    R = Ratios(keys, rng)
    syms = LARGE + SMALL

    net_b = {s: R.ratio(base[base.symbol == s], "net") for s in syms}
    net_f = {s: R.ratio(filt[filt.symbol == s], "net") for s in syms}
    delta = {s: net_f[s] - net_b[s] for s in syms}
    d_l, d_s = group_mean(delta, LARGE), group_mean(delta, SMALL)

    def gradient(symbols):
        g = {}
        for s in symbols:
            b = base[base.symbol == s]
            g[s] = R.ratio(b[b.against > 0.6], "as1") - R.ratio(b[b.against <= -0.6], "as1")
        return group_mean(g, symbols)

    g_l, g_s = gradient(LARGE), gradient(SMALL)
    q = {}
    for s in LARGE:
        b = base[base.symbol == s]
        q[s] = R.ratio(b[b.ahead_at_arrival > 2000], "as1") - R.ratio(b[b.ahead_at_arrival <= 100], "as1")
    q_l = group_mean(q, LARGE)

    tests = [
        ("H1 (primary)", "Filter improves net 1 s markout on large-tick stocks: Δ_L", d_l, True),
        ("H2 (primary)", "Filter helps large-tick more than small-tick: Δ_L − Δ_S", d_l - d_s, True),
        ("H3", "Large-tick AS worse when our side is thin: G_L", g_l, False),
        ("H4", "That gradient is steeper for large-tick: G_L − G_S", g_l - g_s, False),
        ("H5", "Large-tick fills from deep in the queue are more adverse: Q_L", q_l, False),
    ]
    rows = []
    for h, desc, v, pos in tests:
        point, lo, hi, verdict = summarize(v, pos)
        rows.append(f"| {h} | {desc} | {point:+.3f} | [{lo:+.3f}, {hi:+.3f}] | **{verdict}** |")

    sym_rows = []
    for s in syms:
        kept = filt[filt.symbol == s].shares.sum() / base[base.symbol == s].shares.sum()
        _, lo, hi, _ = summarize(delta[s], True)
        sym_rows.append(f"| {s} | {'large' if s in LARGE else 'small'} | {base[base.symbol == s].shares.sum() / 1e6:.2f} | "
                        f"{kept * 100:.0f}% | {net_b[s][0]:+.3f} | {delta[s][0]:+.3f} [{lo:+.3f}, {hi:+.3f}] |")
    day_rows = []
    for d in CONFIRM_DAYS:
        per = {}
        for s in LARGE:
            b, f = base[(base.symbol == s) & (base.key.map(lambda k: k[0]) == d)], filt[
                (filt.symbol == s) & (filt.key.map(lambda k: k[0]) == d)]
            per[s] = f.net.sum() / f.shares.sum() - b.net.sum() / b.shares.sum()
        day_rows.append(f"| {d} | {np.mean(list(per.values())):+.3f} | "
                        f"{base[base.key.map(lambda k: k[0]) == d].shares.sum() / 1e6:.2f} |")

    md = ["# M5 confirmation round: results", "",
          "Tests exactly what [confirmation_plan.md](confirmation_plan.md) registered (commit 85f5b43, amended in "
          "09924c0 before any confirmation data was downloaded). Confirmation days: " + ", ".join(CONFIRM_DAYS) +
          ". Units: basis points of the pre-fill mid, per share, equal-weighted across stocks in a group. "
          f"95% paired block-bootstrap intervals, {B:,} resamples, seed {SEED}.", "",
          "| Hypothesis | Statistic | Estimate (bps) | 95% interval | Verdict |", "|---|---|---:|---:|---|", *rows, "",
          "*Confirmed* means the interval lies entirely on the predicted side of zero; *contradicted*, entirely on the "
          "other side; *inconclusive*, it includes zero.", "",
          "## Per stock (descriptive)", "",
          "| Stock | Tick size | Baseline shares (M) | Filter volume kept | Baseline net 1 s (bps) | Filter Δ net 1 s (bps) |",
          "|---|---|---:|---:|---:|---:|", *sym_rows, "",
          "## Per day (descriptive)", "",
          "| Day | Δ_L (bps, no interval) | Baseline shares, all stocks (M) |", "|---|---:|---:|", *day_rows, ""]
    (m5.OUT / "confirmation.md").write_text("\n".join(md), encoding="utf-8")
    print(f"wrote {m5.OUT / 'confirmation.md'}")


def main():
    if len(sys.argv) != 2 or sys.argv[1] not in ("run", "analyze"):
        sys.exit(__doc__)
    if sys.argv[1] == "run":
        m5.run_many([(d, 0.0, th) for d in CONFIRM_DAYS for th in (None, THETA)])
    else:
        analyze()


if __name__ == "__main__":
    main()
