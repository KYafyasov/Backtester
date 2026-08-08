# Restricted flat N-way market merger

## Implemented scope

The runtime accepts a strict `cmf-multi-source-v1` parent manifest and merges
two or more independently ordered L2 child datasets in one replay. This is an
optional extension beyond the HW4 requirement; the existing single JSONL and
single L2 paths remain supported.

The implemented first version is intentionally narrow:

- every child is a `cmf-l2-parquet-cache-v1` manifest;
- each schema-v1 child owns exactly one `instrument_id`, disjoint from every
  other child;
- the public configuration is one flat parent manifest;
- source priorities and source IDs are positive and unique;
- one event from the L2 cache is one atomic market group;
- multi-venue consolidation, overlapping books, mixed L2/L3 state, wrapped
  JSONL children, live feeds, and public nested manifests are unsupported.

The runtime rejects a parent manifest before worker threads start when its
identity, ownership, path, hash, byte size, counts, bounds, timestamp semantics,
or child metadata is inconsistent. Every child must declare the same typed
`timestamp_semantics`; for example, `exchange` and `receive` cannot share one
global causal timeline. The machine-readable contract is
[`../contracts/multi_source_manifest.schema.json`](../contracts/multi_source_manifest.schema.json).

All integer fields are range-checked before conversion to their C++ type.
Unsigned fields reject negative values and values above the target type's
maximum instead of relying on narrowing JSON conversions. Zero is then rejected
where the schema requires a positive value.

## Lifecycle and ordering

Each leaf retains the existing two-phase source contract:

```text
next (stage immutable group and key)
  -> merger selects winner
  -> Scheduler compares winner with strategy commands
  -> prepare_for_dispatch (apply book and materialize spans)
  -> TradingEngine processes delivery
  -> processed_seq acknowledgement
  -> merger advances only the acknowledged winning leaf
```

The merger keeps one staged head per non-empty leaf in a fixed-capacity heap.
The leaf group key is ordered lexicographically by:

```text
event_ts_ns
source_priority
source_id
source_local_group_sequence
```

Preparation is allowed to populate delivery payloads but may not change the
scheduled key or winning identity. The merger rechecks `MarketGroupKey`,
`instrument_id`, `source_id`, `global_input_sequence`, and
`global_market_sequence` after preparation. Scheduler priority is unchanged:
market data wins over a new-order arrival, which wins over a cancel arrival at
the same scheduled timestamp.

## Global warm-up

Date-range filtering belongs to the merger in multi-source mode. For every
child, `L2CacheReader` selects a warm-up suffix beginning at the nearest prior
partition containing a full snapshot. All selected pre-range groups then pass
through the same global ordering rule and the winning leaf's warm-up path.
Warm-up applies snapshot state and seeds callback depth caches, but emits no
matching, callbacks, deliveries, results, or strategy commands. Earlier
partitions are intentionally pruned rather than counted as read.

The first group at the start timestamp is replayed. Once a globally minimal
head is after the end timestamp, all already staged heads are classified as
after-end and replay stops. L2 child readers retain their existing partition
pruning and nearest-snapshot partition selection.

## Sequence domains and provenance

The runtime does not overwrite a leaf's sequence:

| Field | Meaning |
|---|---|
| `source_id` | Stable leaf identity from the parent manifest. |
| `source_sequence` / callback `sequence` | Original child-local merged sequence. |
| `global_input_sequence` | Every group selected by the merger, including warm-up and staged after-end heads. |
| `global_market_sequence` | Contiguous `1..N` sequence for delivered in-range market groups. |
| `dispatch_sequence` | Scheduler sequence containing market deliveries and strategy commands. |

`BookUpdate` and `Trade` expose `source_id` and
`global_market_sequence`. A price-cross signal copies both values into a
synthetic fill. `Fill` and `fills_df` therefore expose:

- `trigger_source_id`;
- the compatible local `trigger_source_sequence`;
- `trigger_global_market_sequence`.

For legacy single-source runs the new source/global fields are zero, while all
pre-existing field values and ordering remain unchanged.

## Selected-record conservation and full-replay audit

`run_summary.json.source_audit.multi_source` records global input/replay
sequences, per-source identity/format/ownership/declared bounds, read/warm-up/
replay/after-end counters, selected-record conservation booleans, full-replay
checks, and a replay provenance digest.

For every source:

```text
records_read = records_warmed + records_replayed + records_after_end
```

This identity covers records actually selected and staged by the ranged
reader. After `DateRange.end`, only the already staged head from each source is
classified; the unread tail is not scanned. Consequently it is not a claim
that every manifest record was processed.

`full_replay_exact_once=true` is emitted only when every source also satisfies:

```text
records_read = records_replayed = expected_records
records_warmed = records_after_end = 0
```

The FNV-1a-64 version-1 digest hashes each replayed raw record in final merge
order using little-endian fixed-width fields:

```text
source_id
source_local_sequence
event_ts_ns
event_kind
instrument_id
group-boundary marker 0xff
```

The digest is a deterministic ordering fingerprint, not a replacement for
the SHA-256 integrity checks on parent-declared child manifests and child
replay caches.

## Runtime entry point

The Python call is unchanged; `data_path` points to the parent manifest:

```python
result = backtest.run(
    strategy,
    "data_normalized/multi_source_manifest.json",
    DateRange(),
    BacktestConfig(order_latency_ns=1, book_depth=15),
    run_summary_path="run_summary.json",
)
```

When `instruments` is omitted, runtime discovery reads each verified child
manifest and constructs the combined instrument metadata list. Unverified
metadata still requires `allow_unverified_metadata=True`.

## Verification and remaining limitations

Native tests cover key ordering, equal-time source priority, globally ordered
warm-up, contiguous global sequences, conservation, digest creation,
prepare-before-advance, leaf ordering regressions, and mutation of timestamp,
local sequence, or instrument during preparation. Python integration covers
strict parent validation, incompatible timestamp semantics, two-source replay,
callbacks, fill provenance, run-summary audit, and 20 repeated deterministic
runs.

Internal hierarchical composition is not part of this implementation. After
independent review found no unresolved P0/P1 issues, the status is
`RESTRICTED FLAT IMPLEMENTED / HIERARCHY DEFERRED`. This remains a restricted
flat implementation, not a generic multi-venue merger.
