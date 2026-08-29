# Queue-aware execution and lifecycle observability

## Why this extension exists

The Homework 4 baseline intentionally uses an optimistic fill-at-touch model:
every eligible private order is filled completely on the first quote or trade
price cross. That model is deterministic and useful as a reference, but it can
materially overstate the executability of passive option orders.

This extension adds an opt-in `FillModel.QueueAware` mode while preserving
`FillModel.FillAtTouch` as the compatibility default.

## Deterministic risk-averse FIFO model

For a passive order at price `P`, the simulator records:

```text
queue threshold = cumulative traded volume at P
                + displayed same-side quantity at P
                + remaining earlier private FIFO quantity at P
```

Only later trades with a known opposite aggressor side advance the threshold.
Adds behind the private order do not move it backwards. Historical cancels and
size reductions do not advance an already-recorded position, which makes the
estimate deliberately risk-averse. Cancelling an earlier private order releases
its remaining FIFO quantity for later private orders.

When cumulative eligible trade volume passes a private order's threshold, only
the excess volume fills that order. This naturally produces deterministic
partial fills and activates the public lifecycle state:

```text
PendingNew -> Open -> PartiallyFilled -> Filled
```

A new opposite quote that crosses the private limit still produces a complete
quote-cross fill. This represents the private order becoming immediately
marketable and is independent of its passive queue estimate.

## Required trade semantics

Queue advancement requires a positive trade quantity and a known aggressor
side:

- sell aggressor consumes the bid queue;
- buy aggressor consumes the ask queue;
- unknown aggressor side does not advance either queue.

The conservative fallback avoids inventing passive-side executions when source
metadata is ambiguous. L2 replay in queue-aware mode therefore fails fast
unless the manifest declares `trade_side_semantics=aggressor`.

## Lifecycle log

`Result.order_log_df` remains a native columnar result and now includes:

| Column | Meaning |
|---|---|
| `transition_sequence` | Stable run-wide order transition order |
| `previous_state` | State before the logged transition |
| `state` | State after the logged transition |
| `queue_ahead_quantity` | Estimated quantity ahead after the transition |

Together with `engine_ts_ns`, order ID, event type, quantities, and reject
reason, these fields make every lifecycle transition reconstructable without
per-event string logging in the hot path.

## Causal timeline

Queue-aware matching does not bypass latency or scheduling:

```text
market delivery = exchange timestamp + market-data latency
order arrival   = callback-visible engine time + order latency
trade delivery  -> queue advancement -> state/result update -> callback
```

Orders remain invisible to matching until their delayed arrival wins the same
global scheduler used by market data and cancels. Equal-time priority remains
market data, new order, then cancel.

## Run the demonstration

```bash
uv run --no-sync python examples/queue_aware.py
```

The checked-in replay starts with five displayed contracts at the bid. A
four-contract private buy joins behind them. A sell-aggressor trade of seven
contracts consumes the displayed queue and partially fills two contracts; the
next two-contract trade completes the order. The printed lifecycle is:

```text
PendingNew -> Open(queue_ahead=5)
Open -> PartiallyFilled(remaining=2, queue_ahead=0)
PartiallyFilled -> Filled(remaining=0)
```

## Explicit limitations

- The model estimates FIFO from displayed level quantity and cumulative trades;
  it does not claim exact exchange queue identity for aggregated L2 input.
- Historical cancellations do not advance existing private orders.
- Hidden liquidity, pro-rata allocation, self-trade prevention, market impact,
  stochastic latency, and venue-specific matching rules remain out of scope.
- Queue volume is tracked independently per instrument, side, and price.

These restrictions are intentional: they provide a deterministic, explainable
improvement over fill-at-touch without pretending to emulate a production
exchange.
