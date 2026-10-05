# M5 confirmation round: results

Tests exactly what [confirmation_plan.md](confirmation_plan.md) registered (commit 85f5b43, amended in 09924c0 before any confirmation data was downloaded). Confirmation days: 08302019, 12302019, 01302020. Units: basis points of the pre-fill mid, per share, equal-weighted across stocks in a group. 95% paired block-bootstrap intervals, 2,000 resamples, seed 11.

| Hypothesis | Statistic | Estimate (bps) | 95% interval | Verdict |
|---|---|---:|---:|---|
| H1 (primary) | Filter improves net 1 s markout on large-tick stocks: Δ_L | +0.026 | [+0.014, +0.041] | **confirmed** |
| H2 (primary) | Filter helps large-tick more than small-tick: Δ_L − Δ_S | +0.036 | [+0.018, +0.055] | **confirmed** |
| H3 | Large-tick AS worse when our side is thin: G_L | -1.127 | [-1.300, -0.989] | **confirmed** |
| H4 | That gradient is steeper for large-tick: G_L − G_S | -1.105 | [-1.294, -0.936] | **confirmed** |
| H5 | Large-tick fills from deep in the queue are more adverse: Q_L | -0.262 | [-0.324, -0.205] | **confirmed** |

*Confirmed* means the interval lies entirely on the predicted side of zero; *contradicted*, entirely on the other side; *inconclusive*, it includes zero.

## Per stock (descriptive)

| Stock | Tick size | Baseline shares (M) | Filter volume kept | Baseline net 1 s (bps) | Filter Δ net 1 s (bps) |
|---|---|---:|---:|---:|---:|
| MSFT | large | 2.47 | 82% | -0.213 | -0.014 [-0.029, -0.002] |
| AMD | large | 1.94 | 63% | -0.705 | +0.013 [-0.018, +0.054] |
| INTC | large | 1.32 | 68% | -0.514 | +0.024 [+0.005, +0.048] |
| CSCO | large | 0.84 | 60% | -0.511 | +0.064 [+0.027, +0.104] |
| CMCSA | large | 0.75 | 69% | -0.443 | +0.043 [+0.011, +0.076] |
| AAPL | small | 1.65 | 89% | -0.231 | -0.006 [-0.016, +0.005] |
| NVDA | small | 0.31 | 83% | -0.172 | -0.012 [-0.044, +0.022] |
| FB | small | 1.14 | 84% | -0.259 | -0.011 [-0.028, +0.007] |

## Per day (descriptive)

| Day | Δ_L (bps, no interval) | Baseline shares, all stocks (M) |
|---|---:|---:|
| 08302019 | +0.028 | 3.04 |
| 12302019 | +0.019 | 2.82 |
| 01302020 | +0.035 | 4.53 |
