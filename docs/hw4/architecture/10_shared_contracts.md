# Shared native contracts

## 1. Contract location and compatibility

The dependency-free native contract is declared once under `src/core/`.
`core/Contracts.hpp` is the convenience include for the complete contract.
`common/BasicTypes.hpp` remains a compatibility include for legacy targets; it
aliases old names to the core definitions and does not redeclare `Side`,
`Quantity`, `ClOrdId`, or the other public numeric types.

The compatibility LOB under `src/main` retains its earlier representation.
The implemented streaming runtime and every public event-loop contract use
integer `PriceTicks`.

## 2. Price and monetary units

`PriceTicks` is an integer count in the instrument's quoted-price scale.
`InstrumentMeta.price_scale` is the strictly positive number of price ticks per
one quoted currency unit. `tick_size_ticks` is the strictly positive minimum
price increment in that same integer scale. For example, with
`price_scale=10'000` and `tick_size_ticks=5`, `PriceTicks{12'345}` represents
1.2345 quoted currency units and valid prices are multiples of 5.

`contract_multiplier` is a strictly positive integer number of quoted units per
contract. HW4 assumes quoted currency and account currency are the same; FX
conversion is out of scope. Native accounting preserves the exact rational:

```text
pnl_account_currency =
    delta_price_ticks * signed_quantity * contract_multiplier / price_scale
```

The runtime must use checked integer arithmetic for the numerator and must not
round intermediate values. A midpoint is the exact rational
`(bid_ticks + ask_ticks) / 2`; consequently marked PnL has denominator
`2 * price_scale` when the tick sum is odd. Overflow is an error, not wrapping.
`AccountCurrencyAmount` is the minimal native rational value contract:
`numerator / denominator`, with a strictly positive denominator. It does not
implement unchecked arithmetic.

`double` is permitted only in strategy-query snapshots and final PnL result
values. Conversion happens once at that boundary, after rational evaluation,
to the nearest IEEE-754 binary64 value using the default round-to-nearest,
ties-to-even mode. Price, fill, order, and event-loop fields never use floating
point. Positivity is a frozen contract; runtime validation belongs to the
instrument/configuration task.

## 3. Numeric and enum encodings

Timestamps, price ticks, quantity, and instrument IDs are signed 64-bit
integers. Client order IDs, exchange order IDs, and deterministic sequences are
unsigned 64-bit integers.

All enum result columns store the enum's fixed-width underlying value:

| Enum | Underlying type | Stable encoding |
|---|---|---|
| `Side` | `int8` | `Sell=-1`, `None=0`, `Buy=1` |
| `OrderState` | `uint8` | `PendingNew=0`, `Open=1`, `PartiallyFilled=2`, `Filled=3`, `PendingCancel=4`, `Cancelled=5`, `Rejected=6` |
| `RejectReason` | `uint8` | `None=0`, `UnknownInstrument=1`, `InvalidSide=2`, `NonPositiveQuantity=3`, `InvalidPrice=4`, `TickMisalignment=5`, `DuplicateClientOrderId=6`, `UnsupportedOrderType=7`, `UnsupportedTimeInForce=8`, `UnknownOrder=9`, `AlreadyTerminal=10` |
| `EventPriority` | `uint8` | `MarketData=0`, `NewOrder=1`, `Cancel=2` |
| `CommandType` | `uint8` | `NewOrder=0`, `Cancel=1` |
| `OrderLogEventType` | `uint8` | `Submit=0`, `Accepted=1`, `Fill=2`, `CancelRequest=3`, `Cancelled=4`, `Reject=5` |
| `LiquiditySource` | `uint8` | `HistoricalDisplayed=0`, `QuoteCross=1`, `TradeCross=2` |
| `PriceCrossSource` | `uint8` | `BestQuote=0`, `Trade=1` |

These encodings are public serialization/result contracts. Additions or changes
require the decision-change process.

## 4. Scheduled payload and callback contract

`ScheduledEvent` is a closed discriminated value over `MarketDelivery`,
`NewOrderCommand`, and `CancelCommand`. It has no separately writable tag:
priority and stable ordering key are derived from the active variant, so a
market payload cannot carry new-order or cancel priority.

`MarketDelivery` owns only scalar metadata. Its optional book view, trade span,
and ordered `PriceCrossSignal` span are non-owning views into dispatcher-owned
stable event/book storage. That storage must remain valid and unchanged from
enqueue until the Trading Engine publishes `processed_seq` for the delivery.
New-order and cancel alternatives own complete command values; no side table is
part of the contract.

Each price-cross signal carries one raw source sequence and, for restricted
multi-source replay, a source ID plus global market sequence. A book action
records the best bid/ask immediately after that action; a trade records its
price. Signals are strictly source-sequence ordered inside their atomic group
and belong to the delivery's instrument and timestamps.

`FillView.sequence` is the synthetic fill sequence. It is distinct from
`FillView.trigger_source_sequence`, which is the raw quote/trade source row
that won matching. `trigger_source_id` and
`trigger_global_market_sequence` preserve its multi-source identity.
`FillResultRow` and `fills_df` retain the same trigger provenance. For a quote
already crossed when an order arrives,
`trigger_source_sequence` is the latest raw book-action sequence retained by
the historical book; source/global trigger values are zero because that
legacy touch path stores only book-local sequence provenance.

A book callback fires after the complete atomic historical group (for example,
Databento `F_LAST`) and only when the configured top-N view changed. The
default depth is 15; internal replay remains full L3.

For one high-level market event, callback and state order is:

1. replay raw quote/trade cross signals through `SimulatedLOB`;
2. update order, position, and result state, then call `on_fill()`;
3. call `on_trade()` in stable source order;
4. call `on_book_update()` when the top-N view changed;
5. enqueue commands created by callbacks;
6. publish `processed_seq`.

`BookUpdateView` contains non-owning spans. They are valid through processing of
their containing `MarketDelivery`, including the callback, and expire once its
`processed_seq` acknowledgement is published. A consumer must copy data needed
after that point. Other callback and command payloads are numeric value types
with no owning strings or Python objects.

## 5. Range and terminal policies

`DateRange` includes historical source records whose exchange timestamp equals
either the start or end. A command arrival at the end timestamp may execute; an
arrival strictly after the end timestamp does not execute.

Complete atomic historical groups strictly before `start_ts_ns` are replayed
into `HistoricalLOBStore` as warm-up. They do not reach the scheduler or
strategy, do not match private orders, and do not create marks or commands.
The top-N change cache is seeded from the warmed book, so a first in-range
trade-only group does not produce a false book callback. A group exactly at
`start_ts_ns` is delivered normally. Parsing, source ordering, and atomic-group
validation remain fail-fast during warm-up.

If a cancel arrives after its order has already reached terminal `Filled`
state, it produces a reject with `RejectReason::AlreadyTerminal`. It is not a
silent no-op.

## 6. Results boundary

`FillResultRow`, `OrderLogResultRow`, and `PnlPoint` declare the native values
and exact enum storage used by the result schemas. `ResultRecorder` stores each
field in a typed column. `FrozenResults` owns immutable shared storage, and the
Python binding attaches that owner to every zero-copy NumPy view before
constructing pandas objects in bulk.
