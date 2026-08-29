# L2 Parquet pipeline implementation brief

## Status and base

- **Branch:** `feat/l2-parquet-data-pipeline`
- **Starting commit:** `1ef94a3235e016c8ac16d2b9a673cdbbf9fcc6f2`
- **Design source:**
  [`../proposals/02_l2_parquet_data_pipeline.md`](../proposals/02_l2_parquet_data_pipeline.md)

## Accepted implementation scope

Implement the proposal's Option B as one deterministic end-to-end path:

```text
raw L2 CSV
  -> validated, daily wide-schema Parquet
  -> versioned little-endian native replay cache
  -> L2 scheduled source
  -> existing scheduler and TradingEngine
```

The existing MBO JSONL path and public callback/result schemas remain
compatible. An L2 snapshot atomically replaces aggregated levels and emits one
final quote-cross signal; it is never expanded into fabricated L3 orders.

## Frozen technical choices

- Parquet schema version 1 uses 100 scalar depth columns. This avoids an Arrow
  C++ dependency and makes validation/debugging straightforward.
- Runtime uses a native cache referenced by the dataset manifest. Parquet is
  the canonical audit/analysis representation, not the matching-loop format.
- The converter supports both `snapshot_first` and `trade_first`; the selected
  policy is mandatory metadata and becomes immutable `merged_sequence`.
- Timestamps, instrument metadata, price scale, tick size, multiplier, trade
  side semantics, and provenance are explicit converter inputs. Unknown
  semantics require an explicit diagnostic/unverified flag.
- Raw files are read-only. Output is built in a temporary directory, reopened
  and validated, then atomically published.

## Deliverables

1. bounded streaming CSV converter with exact decimal-to-ticks parsing;
2. daily Parquet partitions, native replay caches, hashes, and final manifest;
3. native cache/manifest reader with fail-fast schema and chronology checks;
4. honest per-instrument historical L2 state;
5. runtime auto-selection between existing `.jsonl` MBO and L2 manifest;
6. native and Python compatibility/integration tests;
7. conversion/replay usage documentation and benchmark evidence.

## Deferred facts, not guessed defaults

The local files still lack confirmed provider, venue, symbol, timestamp
semantics, trade-side semantics, and multiplier. The implementation can be
completed and tested with explicit fixture metadata, but conversion of the
full local dataset is diagnostic until those values are supplied or accepted
as unverified.
