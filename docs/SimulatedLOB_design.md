# Typed SimulatedLOB and engine-private overlays

## Single fill authority

The production matcher is `cmf::trading::SimulatedLOB` in
`src/trading/SimulatedLOB.*`. It is the only component that decides synthetic
fill price and quantity. `TradingEngine` owns lifecycle transitions, positions,
recording, and callbacks, and applies the ordered `SyntheticFill` decisions
returned by `SimulatedLOB`.

The old `src/main` double/string matcher was removed from the build and source.
Native synthetic tests and Python end-to-end runs now exercise the same typed
implementation.

## State and ownership

`HistoricalLOBStore` owns the shared typed L3 replay. A `SimulatedLOB` owns one
typed `EngineView`, which contains:

- this engine's resting-order price/time indexes.

Synthetic fills never mutate the shared historical book. Separate
`EngineView`s therefore maintain independent private order state.

## Matching

An accepted buy fully fills while `best_ask <= limit_price`; a sell fully fills
while `best_bid >= limit_price`. Otherwise it rests. Later raw best-quote and
trade-price signals are replayed in source-sequence order. A qualifying signal
fills the complete remaining quantity at the trigger price, regardless of the
historical quote or trade size.

Trade aggressor side is ignored. Signals and orders must have the same
`instrument_id`. Every eligible own order is filled in deterministic price-time
order, and own orders never self-match. Result rows and callbacks identify
`QuoteCross` versus `TradeCross` and retain the winning raw
`trigger_source_sequence`.

This is an intentionally optimistic infinite-liquidity model. It does not
model historical capacity, queue position, market impact, or slippage; order
sizing against option liquidity is the strategy user's responsibility.

## Concurrency boundary

The scheduler's strict ready barrier keeps `HistoricalLOBStore` stable while
the Trading Engine calls `SimulatedLOB`. The dispatcher mutates the next market
group only after `processed_seq` acknowledges all matching, state updates, and
callbacks for the current event. The mandatory one-engine path therefore needs
no matcher-side book mutex.
