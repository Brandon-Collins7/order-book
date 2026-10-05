"""Tables and figures for M5. Run through `python scripts/m5.py report`."""

import json

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
import pandas as pd  # noqa: E402

from m5 import (  # noqa: E402
    B, HORIZONS, K_GRID, OUT, SEED, SYMBOLS, TEST_DAYS, THETA_GRID, TUNE_DAYS, blocks, bootstrap, load, pnl_by_day_symbol,
    stats, tag,
)

FIG = OUT / "figures"

# Reference palette from the dataviz guidance: categorical slots in fixed order, light surface.
SURFACE, INK, INK2, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#e4e3df"
SERIES = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100"]
METRICS = ["shares", "spread_c", "as_1s_c", "net_1s_c", "as_30s_c", "net_30s_c", "inv_rms"] + [
    f"net_{h}_c" for h in ("100ms", "5s")]


def test_blocks(name):
    return pd.concat([blocks(d, name) for d in TEST_DAYS])


def ci(c, digits=3, scale=1.0, diff=False):
    if diff:
        return f"{c['diff'] * scale:+.{digits}f} [{c['dlo'] * scale:+.{digits}f}, {c['dhi'] * scale:+.{digits}f}]"
    return f"{c['point'] * scale:.{digits}f} [{c['lo'] * scale:.{digits}f}, {c['hi'] * scale:.{digits}f}]"


def md_table(header, rows):
    out = ["| " + " | ".join(header) + " |", "|" + "|".join("---" if i == 0 else "---:" for i in range(len(header))) + "|"]
    out += ["| " + " | ".join(str(x) for x in r) + " |" for r in rows]
    return "\n".join(out)


def style(ax, title, xlabel, ylabel):
    ax.set_facecolor(SURFACE)
    ax.figure.set_facecolor(SURFACE)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(INK2)
    ax.tick_params(colors=INK2, labelsize=9)
    ax.grid(axis="y", color=GRID, linewidth=0.8)
    ax.set_axisbelow(True)
    ax.set_title(title, color=INK, fontsize=11, loc="left", pad=10)
    ax.set_xlabel(xlabel, color=INK2, fontsize=9)
    ax.set_ylabel(ylabel, color=INK2, fontsize=9)


def ratio_by_group(fills_by_day, group_col, labels, rng):
    """Share-weighted 1 s adverse selection (cents/share) per group, with block-bootstrap CIs."""
    parts = []
    for day, f in fills_by_day.items():
        f = f[(f.mid > 0) & (f.mid_1s > 0)].copy()
        sign = f.side.map({"B": 1, "S": -1})
        f["num"] = sign * (f.mid_1s - f.mid) * f.shares
        f["key"] = list(zip([day] * len(f), f.symbol, f.ts_ns // (30 * 60 * 10**9)))
        parts.append(f[["key", group_col, "num", "shares"]])
    f = pd.concat(parts)
    keys = sorted(set(f.key))
    pos = {k: i for i, k in enumerate(keys)}
    counts = rng.multinomial(len(keys), np.full(len(keys), 1 / len(keys)), size=B).astype(float)
    rows = []
    for lab in labels:
        g = f[f[group_col] == lab].groupby("key")[["num", "shares"]].sum()
        num = np.zeros(len(keys))
        den = np.zeros(len(keys))
        for k, r in g.iterrows():
            num[pos[k]], den[pos[k]] = r.num, r.shares
        point = 100 * num.sum() / den.sum()
        draws = 100 * (counts @ num) / np.maximum(counts @ den, 1)
        lo, hi = np.percentile(draws, [2.5, 97.5])
        rows.append(dict(group=lab, shares=den.sum(), point=point, lo=lo, hi=hi))
    return pd.DataFrame(rows)


def tick_regimes():
    """Large-tick = the median quoted spread at our baseline fills on the tuning day is one tick ($0.01).
    Defined on the tuning day so the held-out days don't choose the grouping."""
    f = load(TUNE_DAYS[0], tag(0, None))[0]
    med = f[f.spread > 0].groupby("symbol").spread.median()
    return {s: ("large-tick" if round(med[s] / 0.01) <= 1 else "small-tick") for s in SYMBOLS}


def within_symbol_bps(fills_by_day, group_col, labels, symbols_by_regime, rng):
    """1 s adverse selection in basis points of price, computed within each symbol and then averaged
    with equal weight per symbol (so stocks with different prices and books don't mix).
    Block bootstrap over (day, symbol, 30-minute) blocks."""
    parts = []
    for day, f in fills_by_day.items():
        f = f[(f.mid > 0) & (f.mid_1s > 0)].copy()
        sign = f.side.map({"B": 1, "S": -1})
        f["num"] = 1e4 * sign * (f.mid_1s - f.mid) / f.mid * f.shares
        f["key"] = list(zip([day] * len(f), f.symbol, f.ts_ns // (30 * 60 * 10**9)))
        parts.append(f[["key", "symbol", group_col, "num", "shares"]])
    f = pd.concat(parts)
    keys = sorted(set(f.key))
    pos = {k: i for i, k in enumerate(keys)}
    counts = rng.multinomial(len(keys), np.full(len(keys), 1 / len(keys)), size=B).astype(float)
    rows = []
    for regime, syms in symbols_by_regime.items():
        for lab in labels:
            point, draws = [], []
            for s in syms:
                g = f[(f.symbol == s) & (f[group_col] == lab)].groupby("key")[["num", "shares"]].sum()
                num, den = np.zeros(len(keys)), np.zeros(len(keys))
                for k, r in g.iterrows():
                    num[pos[k]], den[pos[k]] = r.num, r.shares
                if den.sum() == 0:
                    continue
                point.append(num.sum() / den.sum())
                draws.append((counts @ num) / np.maximum(counts @ den, 1))
            if not point:
                continue
            d = np.mean(draws, axis=0)
            lo, hi = np.percentile(d, [2.5, 97.5])
            rows.append(dict(regime=regime, group=lab, point=float(np.mean(point)), lo=lo, hi=hi, n_symbols=len(point)))
    return pd.DataFrame(rows)


def label_ends(ax, ends, x):
    """Direct labels for line ends at position x. Labels are pushed apart to a minimum gap,
    re-centered on the line ends, and the y-axis is widened if a label would fall outside it."""
    lo, hi = ax.get_ylim()
    gap = 0.07 * (hi - lo)
    ends = sorted([float(y), lab] for y, lab in ends)
    mean_before = np.mean([e[0] for e in ends])
    for j in range(1, len(ends)):
        ends[j][0] = max(ends[j][0], ends[j - 1][0] + gap)
    shift = mean_before - np.mean([e[0] for e in ends])
    for e in ends:
        e[0] += shift
    ax.set_ylim(min(lo, ends[0][0] - gap / 2), max(hi, ends[-1][0] + gap / 2))
    for y, lab in ends:
        ax.annotate(lab, (x, y), xytext=(10, 0), textcoords="offset points", va="center", fontsize=8, color=INK2)


def regime_figure(df, title, xlabel, path):
    fig, ax = plt.subplots(figsize=(7, 3.8), dpi=150)
    labels = list(dict.fromkeys(df.group))
    x = np.arange(len(labels))
    ends = []
    for i, (regime, g) in enumerate(df.groupby("regime", sort=False)):
        g = g.set_index("group").reindex(labels)
        off = (i - 0.5) * 0.12
        ax.errorbar(x + off, g.point, yerr=[g.point - g.lo, g.hi - g.point], color=SERIES[i], linewidth=2,
                    marker="o", markersize=6, elinewidth=1, capsize=3, label=regime)
        ends.append((g.point.iloc[-1], regime))
    label_ends(ax, ends, x[-1] + 0.06)
    ax.axhline(0, color=INK2, linewidth=0.8)
    ax.set_xticks(x, labels)
    ax.set_xlim(-0.4, len(labels) - 0.1)
    ax.legend(frameon=False, fontsize=8, labelcolor=INK2, loc="lower left")
    style(ax, title, xlabel, "1 s adverse selection (bps of price)")
    fig.tight_layout()
    fig.savefig(path)
    plt.close(fig)


def bar_figure(df, title, xlabel, path):
    fig, ax = plt.subplots(figsize=(7, 3.6), dpi=150)
    x = np.arange(len(df))
    ax.bar(x, df.point, width=0.55, color=SERIES[0], zorder=2)
    ax.errorbar(x, df.point, yerr=[df.point - df.lo, df.hi - df.point], fmt="none", ecolor=INK2, elinewidth=1,
                capsize=3, zorder=3)
    ax.axhline(0, color=INK2, linewidth=0.8)
    ax.set_xticks(x, df.group)
    style(ax, title, xlabel, "1 s adverse selection (cents/share)")
    fig.tight_layout()
    fig.savefig(path)
    plt.close(fig)


def report():
    FIG.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(SEED)
    sel = json.loads((OUT / "selection.json").read_text())
    k_star, theta_star = sel["k_star"], sel["theta_star"]
    md = ["# M5: market-making experiments", ""]
    md += [
        f"Tuning day: {', '.join(TUNE_DAYS)}. Held-out days: {', '.join(TEST_DAYS)}. Symbols: {' '.join(SYMBOLS)}.",
        "Defaults: 100-share quotes, max position 500, 10 us latency, proportional cancel model, no fees or rebates.",
        "Cents per share are share-weighted. *Spread* = captured spread vs. the mid just before the fill. *AS h* = how "
        "far the mid moved in our favor (positive) or against us (negative) h after the fill. *Net h* = spread + AS h. "
        "*Inventory RMS* = time-weighted root-mean-square position, shares. Brackets: 95% block-bootstrap intervals "
        f"over (day, symbol, 30-minute) blocks, {B:,} resamples.",
        "",
    ]

    # ---- tuning grid and selection
    t = pd.read_csv(OUT / "tuning_grid.csv")
    rows = []
    for _, r in t.iterrows():
        mark = " ←" if (r.k == k_star and pd.isna(r.theta)) or (r.k == 0 and r.theta == theta_star) else ""
        rows.append([f"{r.k:g}", "off" if pd.isna(r.theta) else f"{r.theta:g}", f"{r.shares / 1e6:.2f}",
                     f"{r.spread_c:.3f}", f"{r.as_1s_c:+.3f}", f"{r.net_1s_c:+.3f}", f"{r.inv_rms:.0f}{mark}"])
    md += ["## 1. Parameter selection on the tuning day", "",
           "Rules fixed in advance: θ\\* maximizes net 1 s markout per share among thresholds keeping ≥ 25% of baseline "
           "volume; k\\* minimizes inventory RMS among skews whose net 1 s markout is within 0.1 ¢ of the baseline's.",
           f"Selected: **θ\\* = {theta_star:g}**, **k\\* = {k_star:g}**.", "",
           md_table(["k", "θ", "Shares (M)", "Spread ¢", "AS 1 s ¢", "Net 1 s ¢", "Inventory RMS"], rows), ""]

    # ---- 2x2 on held-out days
    names = {"Baseline": tag(0, None), f"Skew k={k_star:g}": tag(k_star, None),
             f"Filter θ={theta_star:g}": tag(0, theta_star), "Skew + filter": tag(k_star, theta_star)}
    tables = {lab: test_blocks(n) for lab, n in names.items()}
    res = bootstrap(tables, METRICS, rng)
    nd = len(TEST_DAYS)
    rows, drows = [], []
    for lab in names:
        c = res[lab]
        rows.append([lab, f"{c['shares']['point'] / nd / 1e6:.2f}", ci(c["spread_c"]), ci(c["as_1s_c"]),
                     ci(c["net_1s_c"]), ci(c["net_30s_c"]), ci(c["inv_rms"], 0)])
        if lab != "Baseline":
            drows.append([lab, f"{(c['shares']['point'] / res['Baseline']['shares']['point'] - 1) * 100:+.0f}%",
                          ci(c["as_1s_c"], diff=True), ci(c["net_1s_c"], diff=True), ci(c["net_30s_c"], diff=True),
                          ci(c["inv_rms"], 0, diff=True)])

    # PnL marked to the close, per (day, symbol); bootstrap over those 24 units.
    pnl = {lab: pnl_by_day_symbol_all(n) for lab, n in names.items()}
    units = sorted(pnl["Baseline"])
    cnt = rng.multinomial(len(units), np.full(len(units), 1 / len(units)), size=B).astype(float)
    prow = []
    for lab in names:
        v = np.array([pnl[lab][u] for u in units])
        base = np.array([pnl["Baseline"][u] for u in units])
        per_day = cnt @ v / nd
        dlt = cnt @ (v - base) / nd
        prow.append([lab, f"{v.sum() / nd:+,.0f} [{np.percentile(per_day, 2.5):+,.0f}, {np.percentile(per_day, 97.5):+,.0f}]",
                     "" if lab == "Baseline" else
                     f"{(v - base).sum() / nd:+,.0f} [{np.percentile(dlt, 2.5):+,.0f}, {np.percentile(dlt, 97.5):+,.0f}]",
                     f"{-res[lab]['net_1s_c']['point']:.3f}"])

    md += ["## 2. The 2×2 on held-out days", "",
           md_table(["Strategy", "Shares/day (M)", "Spread ¢", "AS 1 s ¢", "Net 1 s ¢", "Net 30 s ¢", "Inventory RMS"], rows), "",
           "Differences vs. baseline (paired bootstrap):", "",
           md_table(["Strategy", "Volume", "Δ AS 1 s ¢", "Δ Net 1 s ¢", "Δ Net 30 s ¢", "Δ Inventory RMS"], drows), "",
           "PnL marked to the 16:00 mid, $ per day across all 8 symbols (bootstrap over the 24 day×symbol units), and the "
           "maker rebate per share that would make the net 1 s markout break even:", "",
           md_table(["Strategy", "PnL $/day", "Δ vs baseline $/day", "Break-even rebate ¢/share"], prow), ""]

    # Figure 1: markout curves
    fig, ax = plt.subplots(figsize=(7.5, 4), dpi=150)
    xs = ["at fill", "100 ms", "1 s", "5 s", "30 s"]
    ends = []
    for i, lab in enumerate(names):
        c = res[lab]
        y = [c["spread_c"]["point"]] + [c[f"net_{h}_c"]["point"] for h in HORIZONS]
        lo = [c["spread_c"]["lo"]] + [c[f"net_{h}_c"]["lo"] for h in HORIZONS]
        hi = [c["spread_c"]["hi"]] + [c[f"net_{h}_c"]["hi"] for h in HORIZONS]
        ax.plot(range(5), y, color=SERIES[i], linewidth=2, marker="o", markersize=5, label=lab, zorder=3)
        ax.fill_between(range(5), lo, hi, color=SERIES[i], alpha=0.12, linewidth=0, zorder=2)
        ends.append([y[-1], lab])
    label_ends(ax, ends, 4)
    ax.axhline(0, color=INK2, linewidth=0.8)
    ax.set_xticks(range(5), xs)
    ax.set_xlim(-0.2, 5.3)
    ax.legend(frameon=False, fontsize=8, labelcolor=INK2, loc="lower left")
    style(ax, "Net markout after a fill (held-out days, 8 symbols)", "Time after fill",
          "Spread + adverse selection (cents/share)")
    fig.tight_layout()
    fig.savefig(FIG / "markout_curves.png")
    plt.close(fig)

    # ---- filter sweep
    sweep = {("off" if th is None else f"{th:g}"): test_blocks(tag(0, th)) for th in THETA_GRID}
    sres = bootstrap(sweep, METRICS, rng)
    base_sh = sres["off"]["shares"]["point"]
    rows = [[th, f"{c['shares']['point'] / base_sh * 100:.0f}%", ci(c["as_1s_c"]), ci(c["net_1s_c"]),
             ci(c["net_1s_c"], diff=True)] for th, c in sres.items()]
    md += ["## 3. Imbalance-filter sweep (held-out days, k = 0)", "",
           md_table(["θ", "Volume vs. off", "AS 1 s ¢", "Net 1 s ¢", "Δ Net 1 s ¢ vs. off"], rows), ""]
    fig, ax = plt.subplots(figsize=(7, 4), dpi=150)
    vol = [c["shares"]["point"] / base_sh * 100 for c in sres.values()]
    net = [c["net_1s_c"]["point"] for c in sres.values()]
    err = [[c["net_1s_c"]["point"] - c["net_1s_c"]["lo"] for c in sres.values()],
           [c["net_1s_c"]["hi"] - c["net_1s_c"]["point"] for c in sres.values()]]
    ax.errorbar(vol, net, yerr=err, color=SERIES[0], linewidth=2, marker="o", markersize=6, elinewidth=1, capsize=3)
    for v, n, th in zip(vol, net, sres):
        ax.annotate(f"θ={th}", (v, n), xytext=(6, 6), textcoords="offset points", fontsize=8, color=INK2)
    ax.axhline(0, color=INK2, linewidth=0.8)
    style(ax, "Imbalance filter: fill quality vs. volume kept (held-out days)", "Volume traded (% of no filter)",
          "Net 1 s markout (cents/share)")
    fig.tight_layout()
    fig.savefig(FIG / "filter_tradeoff.png")
    plt.close(fig)

    # ---- skew sweep
    ksw = {f"{k:g}": test_blocks(tag(k, None)) for k in K_GRID}
    kres = bootstrap(ksw, METRICS, rng)
    rows = [[k, f"{c['shares']['point'] / nd / 1e6:.2f}", ci(c["inv_rms"], 0), ci(c["net_1s_c"]), ci(c["net_30s_c"])]
            for k, c in kres.items()]
    md += ["## 4. Inventory-skew sweep (held-out days, filter off)", "",
           md_table(["k", "Shares/day (M)", "Inventory RMS", "Net 1 s ¢", "Net 30 s ¢"], rows), ""]

    # ---- robustness
    rows = []
    for lat, cancel, lab in [(0, "proportional", "0 µs"), (10, "proportional", "10 µs (default)"),
                             (100, "proportional", "100 µs"), (10, "pessimistic", "10 µs, pessimistic cancels")]:
        pair = {"base": test_blocks(tag(0, None, lat, cancel)), "filt": test_blocks(tag(0, theta_star, lat, cancel))}
        r = bootstrap(pair, METRICS, rng)
        rows.append([lab, f"{r['base']['shares']['point'] / nd / 1e6:.2f}", ci(r["base"]["net_1s_c"]),
                     ci(r["base"]["as_1s_c"]), ci(r["filt"]["as_1s_c"], diff=True), ci(r["filt"]["net_1s_c"], diff=True),
                     f"{(r['filt']['shares']['point'] / r['base']['shares']['point'] - 1) * 100:+.0f}%"])
    md += ["## 5. Robustness: latency and queue model", "",
           f"Baseline, and the effect of the filter (θ = {theta_star:g}) under each setting.", "",
           md_table(["Setting", "Baseline shares/day (M)", "Baseline net 1 s ¢", "Baseline AS 1 s ¢",
                     "Filter Δ AS 1 s ¢", "Filter Δ net 1 s ¢", "Filter volume"], rows), ""]

    # ---- conditional analyses on baseline fills
    fills = {d: load(d, tag(0, None))[0] for d in TEST_DAYS}
    for f in fills.values():
        against = np.where(f.side == "B", -f.imbalance, f.imbalance)  # +1 = our side of the book is thin
        f["imb_bucket"] = pd.cut(against, [-1.0001, -0.6, -0.2, 0.2, 0.6, 1.0001],
                                 labels=["-1 to -0.6", "-0.6 to -0.2", "-0.2 to 0.2", "0.2 to 0.6", "0.6 to 1"])
        f["queue_bucket"] = pd.cut(f.ahead_at_arrival, [-1, 100, 500, 2000, np.inf],
                                   labels=["0-100", "101-500", "501-2000", ">2000"])
    imb_labels = ["-1 to -0.6", "-0.6 to -0.2", "-0.2 to 0.2", "0.2 to 0.6", "0.6 to 1"]
    que_labels = ["0-100", "101-500", "501-2000", ">2000"]
    imb = ratio_by_group(fills, "imb_bucket", imb_labels, rng)
    regimes = tick_regimes()
    by_regime = {r: [s for s in SYMBOLS if regimes[s] == r] for r in ("large-tick", "small-tick")}
    imb_w = within_symbol_bps(fills, "imb_bucket", imb_labels, by_regime, rng)
    que_w = within_symbol_bps(fills, "queue_bucket", que_labels, by_regime, rng)
    regime_figure(imb_w, "Adverse selection by book imbalance just before the fill (baseline)",
                  "Imbalance against us (+1 = our side of the book is thin)", FIG / "as_by_imbalance.png")
    regime_figure(que_w, "Adverse selection by queue position on arrival (baseline)",
                  "Shares displayed ahead of our order when it arrived", FIG / "as_by_queue.png")
    pooled = [[r.group, f"{r.shares / 1e6:.2f}", f"{r.point:+.3f} [{r.lo:+.3f}, {r.hi:+.3f}]"] for r in imb.itertuples()]
    wfmt = lambda df: [[r.regime, r.group, r.n_symbols, f"{r.point:+.3f} [{r.lo:+.3f}, {r.hi:+.3f}]"]
                       for r in df.itertuples()]
    md += ["## 6. Where adverse selection comes from (baseline fills, held-out days)", "",
           "Pooled across symbols in cents per share, adverse selection looks *smaller* when our side of the book is "
           "thin. That is a composition effect (Simpson's paradox): thin-side fills come mostly from low-priced, "
           "thick-book stocks whose moves are small in cents. The tables below compare like with like: basis points of "
           "price, computed within each symbol, then averaged with equal weight per symbol, separately for large-tick "
           "and small-tick stocks.", "",
           "Tick-size groups, fixed on the tuning day (median quoted spread at our fills = 1 tick → large-tick): " +
           "; ".join(f"**{r}**: {', '.join(s)}" for r, s in by_regime.items()) + ".", "",
           "By top-of-book imbalance just before the fill (+1 = our side was thin):", "",
           md_table(["Group", "Imbalance against us", "Symbols", "AS 1 s (bps)"], wfmt(imb_w)), "",
           "![Adverse selection by imbalance](figures/as_by_imbalance.png)", "",
           "By shares displayed ahead of our order when it arrived:", "",
           md_table(["Group", "Shares ahead", "Symbols", "AS 1 s (bps)"], wfmt(que_w)), "",
           "![Adverse selection by queue position](figures/as_by_queue.png)", "",
           "For reference, the misleading pooled version (cents per share, all symbols together):", "",
           md_table(["Imbalance against us", "Shares (M)", "AS 1 s ¢"], pooled), ""]

    # ---- exploratory: the filter effect by stock and by tick-size group
    base_t, filt_t = tables["Baseline"], tables[f"Filter θ={theta_star:g}"]
    rows = []
    groups = [(s, [s]) for r in by_regime for s in by_regime[r]] + [(f"all {r}", by_regime[r]) for r in by_regime]
    for lab, syms in groups:
        mask = base_t.index.get_level_values("symbol").isin(syms)
        r = bootstrap({"base": base_t[mask], "filt": filt_t[mask]}, ["shares", "as_1s_c", "net_1s_c"], rng)
        rows.append([lab, regimes.get(lab, ""), f"{r['filt']['shares']['point'] / r['base']['shares']['point'] * 100:.0f}%",
                     ci(r["base"]["net_1s_c"]), ci(r["filt"]["as_1s_c"], diff=True), ci(r["filt"]["net_1s_c"], diff=True)])
    md += [f"## 7. Exploratory: the filter (θ = {theta_star:g}) by stock and tick size", "",
           "**Post hoc**: this split was looked for after seeing the held-out results, so treat it as a hypothesis "
           "to confirm on new days, not as a finding. Positive Δ = the filter improves fills.", "",
           md_table(["Stock / group", "Tick size", "Volume kept", "Baseline net 1 s ¢", "Filter Δ AS 1 s ¢",
                     "Filter Δ net 1 s ¢"], rows), "",
           "## Figures", "", "![Markout curves](figures/markout_curves.png)", "",
           "![Filter tradeoff](figures/filter_tradeoff.png)", ""]
    (OUT / "report.md").write_text("\n".join(md), encoding="utf-8")
    print(f"wrote {OUT / 'report.md'} and {FIG}")


def pnl_by_day_symbol_all(name):
    out = {}
    for d in TEST_DAYS:
        out.update(pnl_by_day_symbol(d, name))
    return out
