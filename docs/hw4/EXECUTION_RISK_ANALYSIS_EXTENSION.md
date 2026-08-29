# Execution, Risk, and Analysis Extension

## Why this extension exists

The Homework 4 runtime already provides deterministic replay, a private engine
view, delayed commands, queue-aware matching, lifecycle callbacks, and frozen
results. A useful backtester must also answer three questions that an optimistic
matching prototype cannot answer:

1. What price and explicit cost would the strategy realistically pay?
2. Could the strategy exceed exposure limits while orders are still in flight?
3. Can execution quality and net results be explained without rerunning the test?

This extension closes those gaps as one vertical slice. It does not apply an
after-the-fact haircut. Execution costs and pre-trade risk participate in the
same deterministic event timeline as market data, order arrival, fills, and
cancels.

## Runtime flow

```text
Python strategy
    |
    | submit_limit
    v
validation -> pre-trade risk check + reservation
    | accepted                         | rejected
    v                                  v
global delayed command timeline     reject callback/result
    |
    v
SimulatedLOB (sole fill authority)
    |
    v
ExecutionCostModel
    | reference price -> bounded adverse slippage
    | maker/taker role -> exact fee in currency micros
    v
TradingEngine lifecycle + risk release/update
    |
    v
ResultRecorder exact net PnL + immutable result columns
    |
    v
ExecutionReport / comparison DataFrame
```

## Execution-cost semantics

- A quote cross is a taker execution; a trade cross is a maker execution.
- `SlippageModel.NONE` preserves the original behavior.
- `SlippageModel.FIXED_TICKS` moves taker fills in the adverse direction.
- A buy fill never exceeds its limit and a sell fill never falls below its
  limit. Therefore slippage can be smaller than the configured amount.
- Maker fees may be negative to model rebates. Taker fees must be non-negative.
- Fees are stored as signed millionths of account currency and are subtracted
  from exact rational PnL before conversion to `float64`.

Each fill records both reference and effective price, liquidity role, applied
slippage, fee, submit time, arrival time, and time-to-fill. Trigger source and
global market sequence remain available, so the complete decision is auditable.

## Pre-trade risk semantics

Risk is evaluated synchronously during `submit_limit`, before a command is
inserted into the scheduler. The engine supports deterministic limits for:

- quantity of one order;
- absolute worst-case position;
- total open quantity;
- active order count.

Accepted pending orders reserve their entire remaining quantity. Partial fills
move quantity from the reservation to the net position. A full fill, cancel, or
reject releases the remaining reservation. A pending cancel remains reserved
until its delayed cancel command reaches the engine. This prevents latency from
becoming a risk-limit loophole.

The strategy can inspect `risk(instrument_id)` during a callback. Rejections use
stable, machine-readable reasons and are available both through callbacks and
`Result.rejects_df`.

## Result and analysis API

The frozen Python result now exposes:

- `fills_df`: execution price, reference price, cost, role, latency, queue, and
  market-event provenance;
- `order_log_df`: the complete lifecycle transition log;
- `rejects_df`: all new-order and command rejections;
- `pnl_series`: net mark-to-market PnL after fees;
- `final_positions_df`: realized, unrealized, and total PnL by instrument.

`build_execution_report(result)` derives fill ratio, maker/taker volume, fees,
slippage tick-volume, latency percentiles, queue-at-accept, final net PnL, and
maximum drawdown. `compare_results({...})` places multiple configurations in one
DataFrame for model sensitivity analysis.

## Demo

From the repository root:

```bash
uv run --no-sync python examples/execution_quality.py
```

The deterministic fixture produces:

- one accepted marketable limit order;
- one risk rejection that never enters the scheduler;
- a taker fill whose reference price is `101`, effective price is `103`, and
  fixed adverse slippage is two ticks;
- a `0.5` account-currency fee and a final PnL decomposition of `-0.5` realized
  cost plus `-5.0` unrealized PnL.

The demo also runs the same strategy against the optimistic zero-cost,
unlimited-risk baseline and prints both reports side by side.

The queue-aware demo remains available separately:

```bash
uv run --no-sync python examples/queue_aware.py
```

## Suggested 15-minute presentation

1. **Problem and architecture (2 minutes).** Show the global event timeline and
   explain why post-processing cannot enforce risk or affect execution.
2. **Queue-aware fill model (3 minutes).** Contrast fill-at-touch with displayed
   FIFO queue, aggressor-side trade semantics, and partial fills.
3. **Execution costs (3 minutes).** Trace reference price to bounded slippage,
   maker/taker role, exact fees, and net PnL.
4. **Pre-trade risk (3 minutes).** Show pending-order reservation, a machine-
   readable rejection, partial-fill accounting, and delayed cancel semantics.
5. **Observability and analysis (2 minutes).** Join lifecycle, fill provenance,
   latency, rejects, PnL, and final positions from immutable result columns.
6. **Live deterministic demo and tests (2 minutes).** Run both examples, then
   show native and Python regression counts.

## Explicit scope

This extension deliberately does not implement option pricing, feature
generation, or delta hedging. Those are strategy-layer concerns in the supplied
big-picture diagram. The implemented slice strengthens the shared simulation
kernel: it is strategy-independent, deterministic, testable, and directly
changes fill feasibility and net performance.
