# Getting started

This guide takes a new contributor from a clean checkout to a verified local
run, then explains the minimum configuration needed to write a strategy or use
different market data.

## 1. Prerequisites

The project requires a native C++20 toolchain and Python 3.12. You need:

- Git;
- a C++20 compiler (recent Clang, GCC, or MSVC);
- [UV](https://docs.astral.sh/uv/).

The CMake configuration handles Clang, GCC, and MSVC warning policies. The
final documented verification was executed with AppleClang 17 on macOS arm64;
other compiler/platform combinations remain valid QA targets.

UV installs the locked Python environment and supplies CMake, Ninja, and the
Python development dependencies. It can also provision the required Python
version when Python 3.12 is not already installed.

All commands below run from the repository root.

## 2. Install and verify

```bash
uv sync --locked
uv run pip install -e .
uv run python -c "import back_tester; print(back_tester.__file__); print(back_tester.version())"
```

The editable install compiles the pybind11 extension and exposes the
`back_tester` import package. A successful import should print a path inside
the checkout or its editable build and version `0.0.1`.

Run the checked-in end-to-end example:

```bash
uv run python examples/mean_reversion.py
```

The example replays two instruments, submits delayed orders, produces a fill,
cancels an independent order, and prints callback order, final positions,
terminal order states, fill count, and final PnL.

## 3. Build and test the native runtime

```bash
uv run cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
uv run cmake --build build-release -j
uv run ctest --test-dir build-release --output-on-failure
uv run pytest -q python/tests
```

The CTest suite covers native units, runtime integration, and CLI fixtures.
The pytest suite covers the public Python API, callbacks, exception shutdown,
result ownership, determinism, and the end-to-end strategy.

For a quick ingestion-only smoke test:

```bash
build-release/bin/back-tester test/data/tiny_mbo.jsonl
```

## 4. Write a strategy

Subclass `Strategy` and override only the callbacks you need:

```python
from back_tester import BacktestConfig, DateRange, Side, Strategy, backtest


class BuyBestAsk(Strategy):
    def on_book_update(self, update):
        if update.asks:
            self.submit_limit(
                update.instrument_id,
                Side.BUY,
                update.asks[0].price,
                1,
            )

    def on_trade(self, trade):
        pass

    def on_fill(self, fill):
        print(fill.instrument_id, fill.price, fill.quantity)

    def on_reject(self, reject):
        raise RuntimeError(f"order rejected: {reject.reason}")


result = backtest.run(
    BuyBestAsk(),
    "test/data/tiny_mbo.jsonl",
    DateRange(),
    BacktestConfig(),
)
```

Inside a callback, a strategy may call:

- `submit_limit(instrument_id, side, price_ticks, quantity)`;
- `cancel_order(client_order_id)`;
- `position(instrument_id)`;
- `open_orders(instrument_id)`;
- `now_ns`.

These context operations are deliberately unavailable outside an active
callback. Orders and cancels are scheduled at `now_ns + order_latency_ns`; they
do not execute recursively inside the callback.

The returned `Result` contains:

- `fills_df`;
- `order_log_df`;
- `pnl_series`.

For an auditable run, pass an optional summary path. No log file is created by
default:

```python
result = backtest.run(
    strategy,
    path,
    date_range,
    config,
    instruments,
    run_summary_path="artifacts/run_summary.json",
)
```

The JSON records effective parameters, duration, source/replay/callback/result
counts, L2 manifest totals, sequence bounds and a rolling sequence fingerprint.
It is also written with `status="failed"` when replay or a callback raises.

See
[`architecture/06_python_api_and_results.md`](architecture/06_python_api_and_results.md)
for callback payload fields and exact result schemas.

## 5. Configure a run

### Latency and callback depth

```python
config = BacktestConfig(
    market_data_latency_ns=50,
    order_latency_ns=200,
    book_depth=15,
)
```

- `market_data_latency_ns` must be non-negative.
- `order_latency_ns` must be strictly positive.
- `book_depth` must be strictly positive.
- Latencies and all public timestamps are integer nanoseconds.

### Date range

```python
date_range = DateRange(
    start_ts_ns=1_775_553_600_000_000_000,
    end_ts_ns=1_775_553_601_000_000_000,
)
```

The range is inclusive. Records before `start_ts_ns` warm the historical book
without calling the strategy. Market records and command arrivals after
`end_ts_ns` are not processed.

### Instrument metadata

The minimal three-argument `backtest.run(strategy, path, date_range)` call
discovers instrument IDs in a metadata pass and assumes Databento nanounits:

```text
tick_size_ticks=1
price_scale=1_000_000_000
contract_multiplier=1
```

For a one-pass replay or real contract parameters, pass explicit metadata:

```python
from back_tester import InstrumentMeta

instruments = [
    InstrumentMeta(
        instrument_id=42,
        tick_size_ticks=10_000_000,
        price_scale=1_000_000_000,
        contract_multiplier=100,
    )
]

result = backtest.run(strategy, path, date_range, config, instruments)
```

All metadata values must be positive, instrument IDs must be unique, and every
instrument in the input must have metadata. Strategy prices are integer
`price_ticks`, not floating-point currency values.

## 6. Input data contracts

The path passed to `backtest.run()` may be an MBO JSONL file or an L2 dataset
`manifest.json`. Both become the same typed scheduler/callback contract; they
retain different market semantics.

### MBO JSONL

The runtime reads one JSON object per line in Databento-like MBO order. The
checked-in [`test/data/tiny_mbo.jsonl`](../../test/data/tiny_mbo.jsonl) fixture
is the smallest working example.

Every row needs:

- `ts_recv` as a UTC ISO-8601 timestamp ending in `Z`;
- `hd.ts_event` and `hd.instrument_id`;
- one-character `action`;
- strictly increasing non-negative `sequence`;
- `flags`, where bit 128 (`F_LAST`) closes an atomic market group.

Supported actions are `A` (add), `C` (cancel), `M` (modify), `T` (trade), `F`
(fill), and `R` (clear). Depending on the action, the reader also requires
`order_id`, `side`, `price`, and/or positive `size`.

Input is streamed and must already be ordered. Blank rows, malformed JSON,
timestamp or sequence regressions, incomplete atomic groups, unsupported
values, unrepresentable prices, and unknown instruments fail the run with file
and row context. The runtime does not silently sort or repair data.

### Local L2 CSV to Parquet/cache

The normative source headers, persisted columns, and cache layout are in the
[`L2 Python conversion contract`](contracts/01_l2_python_pipeline_contract.md).
The manifest shape is also available as
[`l2_dataset_manifest.schema.json`](contracts/l2_dataset_manifest.schema.json).
In summary, conversion starts from exactly these named inputs:

```text
INPUT_DIR/
  lob.csv
  trades.csv
```

and publishes this dataset tree:

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

Pass `OUTPUT_DIR/manifest.json` to `backtest.run()`. Do not pass an individual
Parquet or cache file. Parquet is the canonical inspection/analysis format;
the runtime resolves its optimized cache files through the manifest.

Install the locked development dependencies, then convert the immutable raw
files with explicit instrument and source metadata:

```bash
uv sync --locked
uv run python scripts/convert_l2_csv.py \
  INPUT_DIR OUTPUT_DIR \
  --dataset-id DATASET_ID \
  --instrument-id INSTRUMENT_ID \
  --symbol SYMBOL \
  --source-provider PROVIDER \
  --venue VENUE \
  --timestamp-unit us \
  --timestamp-semantics exchange \
  --price-scale PRICE_SCALE \
  --tick-size-ticks TICK_SIZE_TICKS \
  --contract-multiplier CONTRACT_MULTIPLIER \
  --trade-side-semantics aggressor \
  --same-timestamp-policy snapshot_first
```

The uppercase values and semantic choices are required facts, not defaults to
copy blindly. If provenance is still unresolved, pass
`--allow-unverified-metadata` together with `unknown` semantic values; the
manifest is then explicitly diagnostic. The converter:

- validates exact headers, ordering, timestamps, decimal scale, tick
  alignment, quantities, depth, spread, and sides;
- assigns one immutable merged sequence using the selected equal-time policy;
- writes daily wide-schema Zstandard Parquet and a numeric native replay
  cache in a temporary tree;
- reopens and validates outputs, records hashes and throughput, and publishes
  the final directory atomically.

For the current `data_trades` files from earlier course tasks, provenance and
market semantics are still unknown. The following is the command originally
used for an explicitly unverified functional conversion; it does not invent a
venue, symbol, exchange timestamp, trade-side meaning, or contract multiplier:

```bash
uv run python scripts/convert_l2_csv.py \
  data_trades data_normalized/l2_parquet \
  --dataset-id hw_previous_tasks_unverified \
  --instrument-id 1 \
  --timestamp-unit us \
  --timestamp-semantics unknown \
  --price-scale 10000000 \
  --tick-size-ticks 1 \
  --contract-multiplier 1 \
  --trade-side-semantics unknown \
  --same-timestamp-policy snapshot_first \
  --allow-unverified-metadata
```

Here `price_scale=10_000_000` exactly preserves the observed seven decimal
places. `tick_size_ticks=1`, `contract_multiplier=1`, and `instrument_id=1`
are diagnostic internal values, not verified exchange metadata. The converter
refuses to overwrite an existing output directory. For a repeat conversion,
replace `data_normalized/l2_parquet` with a fresh output path, validate the new
dataset, and then point replay at its `manifest.json`.

Run a verified normalized dataset without supplying duplicate metadata:

```python
result = backtest.run(strategy, "data_normalized/l2_parquet/manifest.json", DateRange())
```

For an explicitly unverified diagnostic dataset, consent is a separate runtime
flag and the result preserves the warning:

```python
result = backtest.run(
    strategy,
    "data_normalized/l2_parquet/manifest.json",
    DateRange(),
    BacktestConfig(allow_unverified_metadata=True),
)
assert result.dataset_id == "hw_previous_tasks_unverified"
assert result.verified_metadata is False
```

Before starting replay threads, the runtime scans schema-v1 cache record
headers and rejects manifest/cache disagreements in counts, bounds, dates, or
global sequence continuity. It uses those verified bounds for `DateRange`
pruning, then recomputes SHA-256 for every selected native cache file.
The exact validation order, guarantees, cost, and remaining limitations are
documented in
[`architecture/12_l2_manifest_cache_validation.md`](architecture/12_l2_manifest_cache_validation.md).

### Multiple disjoint L2 sources

Build one strict flat parent manifest when a replay needs independently
ordered L2 datasets for different instruments:

```bash
uv run python scripts/create_multi_source_manifest.py \
  data_normalized/multi_source_manifest.json \
  --dataset-id multi-instrument-run \
  --source 10:data_normalized/instrument_1/manifest.json \
  --source 20:data_normalized/instrument_2/manifest.json
```

Then pass `data_normalized/multi_source_manifest.json` as `data_path` to
`backtest.run()`. Lower numeric priority wins when child timestamps are equal.
Source IDs are assigned in command-line order. Children must be L2 manifests
below the parent directory with exactly one disjoint `instrument_id` each and
identical `timestamp_semantics`.

Callbacks expose `source_id` and `global_market_sequence`; fills additionally
expose `trigger_source_id`, local `trigger_source_sequence`, and
`trigger_global_market_sequence`. With `run_summary_path`, inspect
`source_audit.multi_source` for selected-record conservation,
`full_replay_exact_once`, and the versioned provenance digest. The full
contract and unsupported cases are in
[`architecture/13_restricted_nway_event_merger.md`](architecture/13_restricted_nway_event_merger.md)
and
[`contracts/multi_source_manifest.schema.json`](contracts/multi_source_manifest.schema.json).

Run a bounded end-to-end smoke test that submits one order, receives a fill,
and observes the resulting position through the direct pybind11 integration:

```bash
uv run python examples/run_local_l2.py \
  --run-summary artifacts/local_l2_run_summary.json
```

Use `--duration-seconds N` to change the replay window. This example is a
functional check, not evidence of economically correct PnL for the unidentified
instrument.

### Full local replay and audit

The normalized local dataset currently contains 22,901,679 records across
2024-08-01 through 2024-08-06. The interval from its first to last manifest
timestamp fits in 518,400 seconds. Run the complete range and persist its audit
summary with:

```bash
uv run --no-sync python examples/run_local_l2.py \
  data_normalized/l2_parquet/manifest.json \
  --duration-seconds 518400 \
  --run-summary artifacts/l2_full_summary.json
```

This run performs all-cache index reconciliation before range pruning,
SHA-256 verification of selected caches, deterministic scheduler replay, and
one final atomic summary write. Validate the summary:

```bash
jq -e '
  .status == "success"
  and .error == null
  and .source_audit.checks.read_accounting
  and .source_audit.checks.replay_type_accounting
  and .source_audit.checks.trade_callbacks_match_replayed_trades
  and .source_audit.checks.l2_market_deliveries_match_replayed_records
  and .source_audit.checks.sequence_span_matches_replayed_records
  and .source_audit.checks.full_manifest_replay
  and .source_audit.checks.full_manifest_counts_match
' artifacts/l2_full_summary.json
```

Expected output is `true`. `book_update` callback count does not have to equal
the snapshot count because unchanged visible top-N snapshots are filtered.
Trade callbacks must equal replayed trade records. The rolling FNV-1a sequence
digest is useful for repeat-run comparison but is not a substitute for the
cache SHA-256 integrity checks.

An L2 snapshot is one atomic aggregated-book replacement and one final quote
signal. It never fabricates order IDs, queue position, or add/cancel history.
The current fill model is consequently an optimistic snapshot-based model.

## 7. Development workflow

Run the repository checks before handing off a change:

```bash
uv run pre-commit run --all-files
uv run ctest --test-dir build-release --output-on-failure
uv run pytest -q python/tests
```

When changing behavior, update the relevant architecture page, focused native
or Python tests, and
[`architecture/11_requirements_traceability.md`](architecture/11_requirements_traceability.md)
in the same change.

For performance work, use the Release-only benchmarks:

```bash
build-release/bin/test/back-tester-scheduler-benchmark
build-release/bin/test/back-tester-price-cross-benchmark
uv run python python/benchmarks/callback_overhead.py
uv run python scripts/benchmark_l2_replay.py PATH_TO_MANIFEST
```

Benchmark values are machine-specific observations, not pass/fail thresholds.

## 8. Troubleshooting

- `No module named back_tester`: run `uv run pip install -e .` and repeat the
  import verification from section 2.
- C++ compiler not found: install a C++20 compiler and rerun the CMake
  configure command.
- `cannot open source file`: pass a readable JSONL path or L2 manifest path.
- L2 manifest/cache mismatch: do not edit generated partitions; reconvert
  from the immutable CSV source with the intended metadata.
- Configuration validation error: check that market latency is non-negative
  and order latency, depth, instrument IDs, tick sizes, scales, and
  multipliers are positive.
- Strategy context error: call submission, cancellation, position, and
  open-order methods only from a strategy callback.
- Source error with `path:row`: fix the indicated JSONL row; ingestion is
  intentionally fail-fast.

After the first successful run, read
[`architecture/README.md`](architecture/README.md) for the system model and
[`architecture/08_implementation_map.md`](architecture/08_implementation_map.md)
to navigate from concepts to source files and tests.
