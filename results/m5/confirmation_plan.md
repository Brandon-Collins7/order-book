# M5 confirmation round: pre-registered plan

Written and committed on 2026-10-05, **before** any confirmation-day data was analyzed. (The files were still downloading when this was committed.) Everything below is fixed: the days, the groups, the parameters, the metrics, the tests and the decision rules. The results will be reported whatever they show.

## Amendment 1 (2026-10-05, also before any confirmation-day data was analyzed)

The four confirmation days above don't exist. The 2018 entries in Nasdaq's directory are checksum files (`.md5sum`) only, and the data files return 404. Only 7 full days are downloadable in total: the 4 development days plus **2019-08-30, 2019-12-30 and 2020-01-30**. Those three replace the original list, and they are the only fresh data available. Everything else stays exactly as registered: groups, θ = 0.8, metrics, hypotheses and decision rules.

Known limitation: 2019-08-30 (the Friday before Labor Day) and 2019-12-30 (holiday week) are quieter than typical days, and 3 days give less statistical power than 4. If a hypothesis fails, the results will say whether the interval was simply too wide to decide (inconclusive) or pointed the other way (contradicted).

## Why

In the first round (`report.md`), splitting stocks by tick size suggested that top-of-book imbalance predicts adverse selection in large-tick stocks but not small-tick ones, and that the imbalance filter only helps large-tick stocks. That split was found **after** looking at the held-out days, so it is a hypothesis. This round tests it on data that played no part in finding it.

## Data

- **Confirmation days (new, never used):** 2019-08-30, 2019-12-30, 2020-01-30 (see Amendment 1; the original list was 2018-05-30, 2018-10-30, 2019-05-30, 2020-01-30).
- **Development days (used to choose everything below):** 2019-01-30, 2019-03-27, 2019-07-30, 2019-10-30.
- **Symbols:** AAPL MSFT AMD INTC CSCO CMCSA NVDA FB.
- **Tick-size groups, fixed as defined on 2019-01-30** (median quoted spread at baseline fills = 1 tick means large-tick). Stocks are not re-classified on the new days, even if their prices differ.
  - large-tick: MSFT, AMD, INTC, CSCO, CMCSA
  - small-tick: AAPL, NVDA, FB

## Strategies and settings

Identical to round 1: 100-share quotes, max position 500, 10 us latency, proportional cancel model, no skew (k = 0), no fees or rebates.

- **Baseline:** join the best bid and ask.
- **Filter:** the same, with the imbalance filter at **θ = 0.8**. Chosen on the development days as the threshold that maximizes the equal-weight mean, over the five large-tick stocks, of the per-stock change in net 1 s markout (bps). It must keep at least 25% of every large-tick stock's volume. Development-day values: θ = 0.8 → +0.029 bps (min volume kept 56%); 0.6 → +0.015; 0.4 → −0.004; 0.2 → −0.038. Small-tick stocks were negative at every θ (−0.017 bps at 0.8).

## Metrics

All are per share, share-weighted within a stock, then averaged **with equal weight across the stocks in a group** (so stocks with different prices don't mix), in basis points of the mid just before the fill:

- **Net 1 s markout** = 10^4 · s · (mid_{t+1s} − fill price) / mid_t, with s = +1 for buys and −1 for sells.
- **AS 1 s** = 10^4 · s · (mid_{t+1s} − mid_t) / mid_t.
- **Imbalance against us** = −I for our bids and +I for our asks, where I = (bid size − ask size) / (bid size + ask size) at the top of book just before the fill. +1 means our side of the book was thin.

## Hypotheses and decision rules

Intervals are 95% percentile intervals from a block bootstrap over (day, symbol, 30-minute) blocks, 2,000 resamples, seed 11, with the same resampled blocks for baseline and filter (paired).

| # | Hypothesis | Statistic | Confirmed if |
|---|---|---|---|
| H1 (primary) | The filter improves fill quality on large-tick stocks | Δ_L = mean over large-tick stocks of [net 1 s (filter) − net 1 s (baseline)] | 95% interval lower bound > 0 |
| H2 (primary) | The filter helps large-tick stocks more than small-tick stocks | Δ_L − Δ_S | 95% interval lower bound > 0 |
| H3 | In large-tick stocks, adverse selection is worse when our side of the book is thin | G_L = mean over large-tick stocks of [AS 1 s with imbalance against us in (0.6, 1]] − [AS 1 s in [−1, −0.6]], baseline fills | 95% interval upper bound < 0 |
| H4 | That gradient is steeper for large-tick than for small-tick stocks | G_L − G_S | 95% interval upper bound < 0 |
| H5 | In large-tick stocks, fills that waited behind more displayed shares are more adverse | Q_L = mean over large-tick stocks of [AS 1 s with > 2,000 shares ahead on arrival] − [AS 1 s with ≤ 100 ahead], baseline fills | 95% interval upper bound < 0 |

H1 and H2 are the primary tests. H3–H5 check the mechanism and the queue-position result. Partial confirmation will be reported as partial. If H1 fails but H3 holds, the conclusion is "the signal is real but the filter doesn't monetize it at 10 us", not a confirmation of the filter.
