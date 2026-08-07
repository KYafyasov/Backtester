# L2 Python conversion pipeline contract

## 1. Status and audience

- **Contract version:** 1
- **Data schema version:** 1
- **Manifest format:** `cmf-l2-parquet-cache-v1`
- **Audience:** the developer rebuilding the Python CSV-to-Parquet/cache path
- **Machine-readable manifest contract:**
  [`l2_dataset_manifest.schema.json`](l2_dataset_manifest.schema.json)
- **Architecture source:**
  [`../proposals/02_l2_parquet_data_pipeline.md`](../proposals/02_l2_parquet_data_pipeline.md)

The keywords **MUST**, **MUST NOT**, **SHOULD**, and **MAY** are normative.
This is a file/data contract, not an HTTP API, so JSON Schema is used instead
of OpenAPI. Internal Python classes and functions are not public APIs; the CLI,
persisted schemas, ordering, validation, and publication behavior are public.

## 2. Ownership boundary

The Python pipeline owns:

1. immutable CSV ingestion;
2. exact normalization into native numeric units;
3. deterministic snapshot/trade merge;
4. daily Parquet generation;
5. native replay-cache generation;
6. persisted-artifact validation;
7. atomic dataset publication and manifest generation.

The native runtime owns manifest/cache validation, date-range selection,
replay, scheduling, matching, callbacks, and result creation. It MUST NOT parse
the source CSV or infer missing market metadata.

`microprice_table.csv` is not a chronological market-data input and is outside
this contract. If retained, it belongs to a separately versioned strategy
model artifact.

## 3. Required source files

The input directory MUST contain exactly the expected inputs by name:

```text
lob.csv
trades.csv
```

Additional files MAY be present but MUST NOT silently affect conversion.

### 3.1 `lob.csv`

The header MUST be exact. The first two columns are an unnamed source index and
`local_timestamp`. They are followed by price/amount pairs for all asks and
then all bids:

```text
"",local_timestamp,
asks[0].price,asks[0].amount,...,asks[D-1].price,asks[D-1].amount,
bids[0].price,bids[0].amount,...,bids[D-1].price,bids[D-1].amount
```

`D` MUST equal configured `book_depth`. Production data uses `D=25`; smaller
depths are permitted only for fixtures.

### 3.2 `trades.csv`

The exact header is:

```text
"",local_timestamp,side,price,amount
```

The only accepted side tokens are lowercase-normalized `buy` and `sell`.
Their market meaning is determined by `trade_side_semantics` in the manifest.

## 4. Stable CLI

The conversion entry point is:

```bash
python scripts/convert_l2_csv.py INPUT_DIR OUTPUT_DIR \
  --dataset-id DATASET_ID \
  --instrument-id INSTRUMENT_ID \
  --symbol SYMBOL \
  --source-provider PROVIDER \
  --venue VENUE \
  --timestamp-unit us \
  --timestamp-semantics local_receive \
  --price-scale 10000000 \
  --tick-size-ticks 1 \
  --contract-multiplier MULTIPLIER \
  --trade-side-semantics aggressor \
  --same-timestamp-policy trade_first \
  --depth 25 \
  --batch-rows 65536
```

Uppercase values are facts supplied for a dataset, not defaults. The values
shown for the local files are evidence-backed candidates except for symbol,
provider, venue, market type, and multiplier, which remain externally
unconfirmed.

The CLI MUST print only the final `manifest.json` path to stdout on success.
It MUST exit non-zero with file and physical-row context on a contract error.
It MUST NOT publish a partial output directory.

## 5. Metadata and verification gate

The following facts are required for verified output:

- non-empty `dataset_id`, `symbol`, `source_provider`, and `venue`;
- positive `instrument_id`, `price_scale`, `tick_size_ticks`, and
  `contract_multiplier`;
- confirmed timestamp unit and semantics;
- confirmed trade-side semantics;
- accepted same-timestamp policy;
- `timezone=UTC` for schema version 1.

If any provenance or semantic fact is unknown, conversion MUST fail unless the
caller explicitly passes `--allow-unverified-metadata`. Such output MUST set
`verified_metadata=false`. Merely passing that flag MUST NOT turn otherwise
confirmed metadata into unverified metadata.

The native runtime MUST require separate explicit consent before replaying a
manifest with `verified_metadata=false`. Until that runtime consent exists,
unverified output is suitable only for converter/schema diagnostics.

## 6. Exact normalization

All conversion happens before the event loop:

| Source | Target | Normative rule |
|---|---|---|
| unnamed CSV index | `source_row` | Parse as `uint64`; strictly increasing within its source stream. |
| `local_timestamp` | `event_ts_ns` | Parse integer and multiply by the configured unit factor with checked `int64` overflow. |
| `local_timestamp` | `receive_ts_ns` | Copy normalized timestamp only for `receive` or `local_receive`; otherwise null. |
| decimal price | `PriceTicks` | Parse decimal characters exactly as `price * price_scale`; binary floating point is forbidden. |
| amount | `Quantity` | Require an exact positive `int64`; fractional, zero, and negative values fail. |
| `buy` | `side=1` | Valid only under the recorded side semantics. |
| `sell` | `side=-1` | Valid only under the recorded side semantics. |

Every normalized price MUST be positive and divisible by `tick_size_ticks`.
Bids MUST be strictly descending, asks strictly ascending, and
`best_bid < best_ask`.

For the current local files, observed evidence supports
`timestamp_unit=us`, `price_scale=10_000_000`, `tick_size_ticks=1`, depth 25,
integral quantities, and likely aggressor-side encoding. The name
`local_timestamp` supports `local_receive`; it does not prove exchange event
time.

## 7. Merge and causality

The source streams are individually chronological but have no shared exchange
sequence. The converter MUST assign one immutable `merged_sequence`, starting
at 1, using:

```text
(event_ts_ns, same_timestamp_priority(source_kind), source_row)
```

Allowed policies are:

- `snapshot_first`: snapshot rows precede trade rows at equal timestamps;
- `trade_first`: trade rows precede snapshot rows at equal timestamps.

The chosen policy MUST be recorded in the manifest and MUST NOT depend on
Python iteration timing, filesystem layout, Parquet row groups, or runtime
thread scheduling. Changing the policy creates a different dataset identity.

## 8. Output layout

The converter MUST produce:

```text
OUTPUT_DIR/
  manifest.json
  schema_version=1/
    instrument_id=<instrument_id>/
      date=<YYYY-MM-DD>/
        book_snapshots.parquet
        trades.parquet
        replay.l2cache
```

Dates are derived from normalized timestamps in UTC. Files MUST use Zstandard
compression and Parquet statistics. Raw CSV files MUST remain unchanged.

## 9. Parquet schema version 1

All fields are non-null unless explicitly marked nullable.

### 9.1 `book_snapshots.parquet`

| Column | Arrow type | Constraint |
|---|---|---|
| `schema_version` | `uint16` | Always 1. |
| `instrument_id` | `int64` | Equals manifest. |
| `event_ts_ns` | `int64` | Non-decreasing. |
| `receive_ts_ns` | `int64`, nullable | See timestamp semantics. |
| `merged_sequence` | `uint64` | Strictly increasing globally. |
| `source_row` | `uint64` | Strictly increasing for snapshots. |
| `depth` | `uint8` | Equals manifest `book_depth`. |

For every `i` from `00` through `D-1`, append these non-null `int64` columns:

```text
bid_price_ticks_<i>
bid_quantity_<i>
ask_price_ticks_<i>
ask_quantity_<i>
```

For top-25 production data this is 7 fixed columns plus 100 level columns.

### 9.2 `trades.parquet`

| Column | Arrow type | Constraint |
|---|---|---|
| `schema_version` | `uint16` | Always 1. |
| `instrument_id` | `int64` | Equals manifest. |
| `event_ts_ns` | `int64` | Non-decreasing. |
| `receive_ts_ns` | `int64`, nullable | See timestamp semantics. |
| `merged_sequence` | `uint64` | Strictly increasing globally. |
| `source_row` | `uint64` | Strictly increasing for trades. |
| `side` | `int8` | `-1` or `1`. |
| `price_ticks` | `int64` | Positive and tick-aligned. |
| `quantity` | `int64` | Positive. |

## 10. Manifest contract

`manifest.json` MUST validate against
[`l2_dataset_manifest.schema.json`](l2_dataset_manifest.schema.json).
JSON Schema validates shape, types, enums, and local constraints. The offline
validator MUST additionally enforce cross-field invariants that JSON Schema
cannot express conveniently:

1. source row counts equal persisted Parquet counts;
2. partition `snapshot_rows + trade_rows` equals cache record count;
3. partition timestamp and sequence bounds equal persisted values;
4. partitions and all persisted rows are globally ordered;
5. the partition directory date matches every contained timestamp;
6. hashes and byte sizes match every referenced file;
7. all relative paths remain inside the dataset root;
8. each manifest instrument/schema value matches Parquet and cache headers;
9. deterministic sampled source rows equal their normalized Parquet values.

SHA-256 is lowercase hexadecimal over the exact persisted bytes. Generated
hashes are part of dataset identity. The runtime MUST verify selected replay
cache hashes before starting scheduler threads.

## 11. Replay cache boundary

The replay cache is a derived runtime artifact. Parquet remains canonical.
Cache version 1 is little-endian and MUST remain byte-compatible with
`L2CacheReader`:

```text
header: <8sHHqqqqQ
  magic="CMFL2C01", version, depth, instrument_id,
  price_scale, tick_size_ticks, contract_multiplier, record_count

record header: <BbHqQQ
  kind, side, reserved=0, event_ts_ns, merged_sequence, source_row

snapshot payload repeated depth times: <qqqq
  bid_price, bid_quantity, ask_price, ask_quantity

trade payload: <qq
  price_ticks, quantity
```

`kind=1` is snapshot with `side=0`; `kind=2` is trade with side `-1` or `1`.
Reserved fields MUST be zero. Cache records MUST exactly fill the declared file
size; truncation and trailing bytes are errors.

## 12. Persistence and failure semantics

The converter MUST:

1. stream input with bounded memory and MUST NOT load the dataset into pandas;
2. write to a temporary sibling directory;
3. close every writer;
4. reopen and independently validate persisted Parquet and cache artifacts;
5. write `manifest.json` only after validation succeeds;
6. atomically rename the completed temporary tree to `OUTPUT_DIR`;
7. remove temporary output on failure;
8. refuse to overwrite an existing `OUTPUT_DIR`.

It MUST fail rather than sort, round, repair, drop, deduplicate, or silently
replace malformed rows.

## 13. Minimum acceptance suite for the Python rewrite

The handoff is incomplete until automated tests cover:

- exact fixture conversion and manifest-schema validation;
- both equal-time policies with different expected merged sequences;
- missing, reordered, and extra CSV columns;
- duplicate/regressing source rows and timestamps;
- timestamp and numeric overflow;
- non-representable and tick-misaligned prices;
- fractional, zero, and negative quantities;
- invalid side, book order, and locked/crossed snapshots;
- verified metadata rejection and explicit unverified output;
- cross-day partitioning and global sequence continuity;
- persisted row counts, bounds, ordering, date placement, and sampled values;
- truncated, trailing-byte, and same-size cache corruption;
- interrupted conversion leaving no published dataset;
- deterministic byte-identical normalized data across repeated runs, excluding
  explicitly non-deterministic audit fields such as `created_at` and measured
  throughput.

## 14. Known current deviations

At contract publication time the implementation is a fixture-backed prototype:

- runtime consent for unverified data is not implemented;
- runtime does not verify recorded cache SHA-256;
- persisted Parquet validation does not yet cover every cross-field invariant;
- the negative converter test matrix is incomplete;
- real-data provenance and full-data benchmarks are unresolved;
- one manifest represents one L2 instrument.

The Python rewrite SHOULD close offline validation/test gaps without changing
schema version 1. Any persisted field, enum, ordering, or cache-layout change
requires a new contract decision and coordinated native-reader update.
