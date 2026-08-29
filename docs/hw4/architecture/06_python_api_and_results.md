# Python Strategy API and result ownership

## Public package

The distribution name is `back-tester-cmf`; the import package is
`back_tester`.

```python
from back_tester import (
    BacktestConfig,
    DateRange,
    FillModel,
    InstrumentMeta,
    Side,
    Strategy,
    backtest,
)


class BuyTouch(Strategy):
    def on_book_update(self, update):
        if update.asks:
            self.submit_limit(
                update.instrument_id,
                Side.BUY,
                update.asks[0].price,
                1,
            )


result = backtest.run(
    BuyTouch(),
    "test/data/tiny_mbo.jsonl",
    DateRange(),
    BacktestConfig(
        market_data_latency_ns=0,
        order_latency_ns=1,
        book_depth=15,
        fill_model=FillModel.QUEUE_AWARE,
    ),
    [
        InstrumentMeta(
            instrument_id=1,
            tick_size_ticks=1,
            price_scale=1_000_000_000,
            contract_multiplier=1,
        )
    ],
    run_summary_path="artifacts/run_summary.json",
)
```

The last two arguments are optional. Without explicit instruments, the binding
performs a metadata discovery pass and uses Databento nanounits, tick size 1,
and multiplier 1. Explicit metadata enables a one-pass replay and real
instrument tick sizes/multipliers.

## Strategy callbacks

Subclass `Strategy` and override any of:

```python
def on_book_update(self, update): ...
def on_trade(self, trade): ...
def on_fill(self, fill): ...
def on_reject(self, reject): ...
```

Payloads are immutable Python-visible objects.

| Payload | Important fields |
|---|---|
| `BookUpdate` | `instrument_id`, exchange/engine time, local sequence, source ID, global market sequence, snapshot flag, bids, asks |
| `Trade` | instrument, exchange/engine time, local sequence, source ID, global market sequence, aggressor side, price, quantity |
| `Fill` | instrument, client order ID, side, price, quantity, remaining quantity, times, sequence, trigger provenance |
| `Reject` | instrument, client order ID, reason, times, sequence |

The native `BookUpdateView` contains callback-scoped spans. The binding copies
its top-N levels into an owned `BookUpdate` before calling Python, so Python may
safely retain the payload. Other callback payloads are numeric value types.

## Strategy context

The bound Strategy object exposes:

```python
client_order_id = self.submit_limit(instrument_id, side, price_ticks, quantity)
accepted = self.cancel_order(client_order_id)
position = self.position(instrument_id)
orders = self.open_orders(instrument_id)
now_ns = self.now_ns
```

These methods are valid only on the callback thread while a callback is active.
Calling them before a run, after a callback returns, or from another Python
thread raises an error. `open_orders()` returns Python-owned rows rather than
references into mutable native indexes.

## GIL and exception handling

The binding:

1. validates and retains the Python Strategy object;
2. releases the GIL before discovery/native execution;
3. reacquires it immediately around each Python callback;
4. activates the Strategy context only for that callback;
5. releases it again before native scheduling, waits, or joins.

A Python exception leaves the callback, is captured by the native runtime,
requests stop, wakes both queues and the ready barrier, joins both native
threads, and is rethrown by `backtest.run()`. The same Strategy object cannot
run concurrently. A new run remains possible after a failed run.

## Native result storage

`ResultRecorder` writes structure-of-arrays columns during the run and
`freeze()` converts them into immutable reference-counted storage.

### `fills_df`

| Column | dtype |
|---|---|
| `exchange_ts_ns` | `int64` |
| `engine_ts_ns` | `int64` |
| `instrument_id` | `int64` |
| `client_order_id` | `uint64` |
| `side` | `int8` |
| `price_ticks` | `int64` |
| `quantity` | `int64` |
| `remaining_quantity` | `int64` |
| `liquidity_source` | `uint8` |
| `trigger_source_sequence` | `uint64` |
| `trigger_source_id` | `uint32` |
| `trigger_global_market_sequence` | `uint64` |
| `reference_price_ticks` | `int64` |
| `liquidity_role` | `uint8` |
| `slippage_ticks` | `uint32` |
| `fee_micros` | `int64` |
| `order_submit_ts_ns` | `int64` |
| `order_arrival_ts_ns` | `int64` |
| `time_to_fill_ns` | `int64` |

### `order_log_df`

| Column | dtype / meaning |
|---|---|
| `engine_ts_ns` | `int64` callback-visible time |
| `transition_sequence` | `uint64` stable run-wide lifecycle order |
| `instrument_id` | `int64` |
| `client_order_id` | `uint64` |
| `event_type` | `uint8` stable enum encoding |
| `previous_state` | `uint8` state before the transition |
| `state` | `uint8` resulting state |
| `side` | `int8` |
| `limit_price_ticks` | `int64` |
| `order_quantity` | `int64` |
| `filled_quantity` | `int64` cumulative fill |
| `remaining_quantity` | `int64` |
| `queue_ahead_quantity` | `int64` estimated FIFO quantity ahead |
| `reject_reason` | `uint8` stable enum encoding |

### `pnl_series`

- index name and dtype: `engine_ts_ns`, `int64`;
- value name and dtype: `total_pnl`, `float64`;
- mark: midpoint when both sides exist;
- missing side: keep the last valid mark;
- sampling: fills and mark-changing book updates for held instruments;
- equal timestamps: deterministically coalesced;
- contract multiplier and price scale: applied before the final `double`
  conversion.

Realized accounting closes FIFO lots. Native arithmetic preserves an exact
rational numerator/denominator and checks overflow before conversion to
`float64`.

### Additional analysis views

`rejects_df` records all typed rejects. `final_positions_df` provides net
quantity and realized/unrealized/total PnL by instrument. The pure-Python
`build_execution_report()` and `compare_results()` functions derive stable
execution-quality tables from frozen results without rerunning the engine.

## NumPy/pandas ownership

Each NumPy array points directly at one frozen native column and carries a
capsule holding a shared owner. Pandas objects are created in bulk with
`copy=False`; there is no per-row Python append path.

The native storage remains alive as long as an exposed array, DataFrame, Series,
or Result wrapper retains it. Result buffers are immutable after `freeze()`.

## Dataset provenance

For manifest-backed L2 replay, `Result.dataset_id` and
`Result.verified_metadata` retain the immutable manifest identity and
verification state. Unverified L2 replay fails before threads start unless the
caller explicitly sets `BacktestConfig.allow_unverified_metadata=True`.
JSONL inputs have no dataset manifest, so both result properties are `None`
rather than claiming a verification state that was not established.

## Optional run summary and replay audit

`backtest.run()` accepts an optional sixth argument,
`run_summary_path`. The default is `None`, which performs no logging and no
filesystem I/O. When a path is supplied, its parent directories are created
and one schema-versioned JSON document is atomically renamed into place after
the native threads have joined:

```python
result = backtest.run(
    strategy,
    data_path,
    date_range,
    config,
    instruments,
    run_summary_path="artifacts/run_summary.json",
)
```

The file contains:

| Section | Meaning |
|---|---|
| `status`, `duration_ns`, `error` | completion state, wall duration, and failure text |
| `data_path`, `dataset_id`, `verified_metadata` | input identity and L2 provenance |
| `date_range`, `config`, `instruments` | effective replay parameters |
| `counts` | scheduled deliveries/commands, callbacks, fills, order rows, and PnL points |
| `source_audit` | source records read, warmed, replayed, and observed after the requested end |

The source accounting identity is:

```text
records_read = records_warmed + records_replayed + records_after_end
records_replayed = replayed_book_records + replayed_trade_records
```

For `cmf-multi-source-v1`, `source_audit.multi_source` adds the parent dataset
identity, global input/replay sequence bounds, a provenance digest, and one
entry per leaf with its timestamp semantics, identity, ownership, bounds, and
counters. `selected_records_conservation` covers only records selected and
staged by the ranged readers. `full_replay_exact_once` additionally requires
every manifest record to be read and replayed with no warm-up or after-end
head. The global replay count must equal the sum of per-source replay counts.
Full details and digest fields are specified in
[`13_restricted_nway_event_merger.md`](13_restricted_nway_event_merger.md).

`records_after_end` is normally zero or one atomic group. The streaming reader
stops at that boundary; it does not scan the rest of the final partition merely
to inflate an audit counter. Partitions wholly outside the range are pruned by
the manifest index and are not reported as physically read records.

For an L2 full-dataset run, `source_audit.checks` additionally establishes:

```text
records_replayed = manifest total rows
replayed_book_records = manifest snapshot rows
replayed_trade_records = manifest trade rows
market_deliveries = records_replayed
trade callbacks = replayed trade records
last sequence - first sequence + 1 = records_replayed
```

`replayed_sequence_digest_fnv1a64` is a deterministic rolling fingerprint of
the replayed source/merged sequences. It is useful for comparing repeated runs,
but it is not a cryptographic integrity proof. L2 cache integrity is separately
protected by the manifest SHA-256 checks.

On a callback or runtime exception, the summary uses `status="failed"`, retains
the counters reached before stop, sets unavailable result-row counts to `null`,
and records the exception message. Failure to write this diagnostic file never
masks the original replay exception. On a successful replay, inability to
publish the requested summary is itself reported as an error.

This is deliberately not per-event logging or distributed tracing. No file I/O,
JSON serialization, or string formatting occurs in the matching/event loop;
the loop only increments fixed-size native counters.
