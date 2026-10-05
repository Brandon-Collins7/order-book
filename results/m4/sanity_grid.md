# M4 sanity checks (2019-01-30, AAPL + MSFT, one day)

These checks only show that each simulator setting moves results in the expected direction. They are not research results: there is no held-out evaluation and no confidence interval yet (that is M5).

Defaults: 100-share quotes, max position 500, 10 us latency, proportional cancel model. Spread capture and markout are share-weighted, in cents per share, against the mid just before each fill.

| Run | Fills | Shares | Spread capture | 1 s markout | Max \|position\| | PnL (no fees/rebates) |
|---|---:|---:|---:|---:|---:|---:|
| baseline (join best bid/ask) | 42,729 | 2,201,983 | +0.697 | -0.925 | 594 | -$9,025 |
| inventory skew k=1 | 45,728 | 2,865,862 | +0.468 | -0.758 | 385 | -$9,617 |
| imbalance filter theta=0.5 | 16,591 | 958,122 | +0.719 | -0.989 | 500 | -$4,617 |
| skew + filter | 21,247 | 1,450,486 | +0.447 | -0.747 | 301 | -$5,136 |
| baseline, pessimistic cancels | 20,478 | 1,383,467 | +0.676 | -0.964 | 500 | -$5,310 |
| baseline, 100 us latency | 40,891 | 2,294,707 | +0.536 | -0.825 | 539 | -$10,856 |
| baseline, 0 us latency | 41,804 | 2,162,278 | +0.711 | -0.925 | 500 | -$8,966 |

Observations to follow up in M5:
- Adverse selection (about 0.9 cents/share at 1 s, growing with the horizon) exceeds the spread captured (about 0.7 cents/share), so naive touch-joining loses money before rebates.
- Skew controls inventory (max position 594 -> 385) at the cost of spread capture.
- The imbalance filter halves the loss, but by trading less: per-share adverse selection does not improve at theta = 0.5. The signal itself is valid (table below), so the question for M5 is why it doesn't carry over to our fills (latency? which fills are avoided?).
- The position limit is soft: an order can fill while its cancel is in flight (max 594 vs a 500 limit).

AAPL, regular hours: top-of-book imbalance vs. the direction of the next mid change.

| I = (bid - ask) / (bid + ask) | Updates | P(next move is down) |
|---|---:|---:|
| [-1.0, -0.8] | 60,708 | 0.612 |
| (-0.8, -0.5] | 92,909 | 0.592 |
| (-0.5, 0.0] | 150,294 | 0.533 |
| (0.0, 0.5] | 133,056 | 0.450 |
| (0.5, 0.8] | 89,412 | 0.397 |
| (0.8, 1.0] | 52,965 | 0.352 |
