# CMF Advanced Backtesting Engine for Options

Deterministic C++20/Python backtesting engine for replaying Databento-like MBO
data or normalized L2 snapshot/trade datasets, simulating latency-aware private
limit orders, and returning fills, order transitions, positions, and PnL to
Python.

```text
MBO JSONL -> historical L3 books -> chronological scheduler
L2 CSV -> Parquet + native cache -> historical L2 books -> scheduler
          -> trading engine -> Python Strategy callbacks
          -> frozen native columns -> pandas results
```

The runtime is deterministic and multi-instrument. It uses one dispatcher
thread, one trading thread, fixed market-data/order latency, an atomic processed
sequence barrier, full-fill-on-price-cross matching, and bulk
NumPy/pandas result hand-off.

- [New contributor guide](docs/hw4/GETTING_STARTED.md)
- [Architecture overview](docs/hw4/architecture/README.md)
- [Assignment-to-code traceability](docs/hw4/architecture/11_requirements_traceability.md)
- [Homework 4 source requirements](docs/hw4/source/01_homework_4_assignment.md)

## Prerequisites

- a C++20 compiler;
- [UV](https://docs.astral.sh/uv/).

UV is the repository's environment and dependency workflow. The locked
development environment includes CMake and Ninja, so no project-specific
system CMake installation is required.

## Set up and install

Create the locked environment and build the editable native Python extension:

```bash
curl -LsSf https://astral.sh/uv/install.sh | sh
uv sync --locked
uv run pip install -e .
uv run python -c "import back_tester; print(back_tester.__file__); print(back_tester.version())"
```

The distribution name is `back-tester-cmf`. Its import package is
`back_tester`, which loads `back_tester._backtester`.

## Python strategy API

`backtest.run()` drives the streaming JSONL or manifest-selected L2 reader, historical book,
scheduler, trading engine, and native result recorder:

```python
from back_tester import DateRange, Side, Strategy, backtest


class BuyTouch(Strategy):
    def on_book_update(self, update):
        if update.asks:
            self.submit_limit(
                update.instrument_id, Side.BUY, update.asks[0].price, 1
            )


result = backtest.run(BuyTouch(), "data/sample.mbo.jsonl", DateRange())
print(result.fills_df)
print(result.order_log_df)
print(result.pnl_series)
```

The three-argument form discovers only the unique numeric instrument IDs in a
metadata pass and then replays the file as a stream. It uses Databento
nanounits (`price_scale=1_000_000_000`, tick size 1, multiplier 1). Pass an
explicit `instruments=[InstrumentMeta(...)]` list for a one-pass replay and
real tick sizes or option multipliers.

The optional `BacktestConfig` defaults to zero market-data latency, a strictly
positive one-nanosecond order latency, and top-15 callbacks. Strategy context
methods are intentionally available only while a callback is active. Result
DataFrames and the PnL Series are built in bulk from frozen typed native
columns; callback-scoped book levels are copied into immutable Python-owned
payloads.

## Deterministic end-to-end example

Run the checked-in two-instrument mean-reversion example after installation:

```bash
uv run python examples/mean_reversion.py
```

It exercises the real `backtest.run` path, including a delayed resting fill,
an independent cancelled order, callback ordering, positions, and PnL.

## Prepare and replay an L2 dataset

The L2 runtime does **not** accept a single Parquet file. Give
`backtest.run()` the generated `manifest.json`; the runtime uses it to select
and validate the native replay caches. Parquet remains the canonical format
for inspection and offline analysis.

### Required source files

The converter expects one directory containing:

```text
INPUT_DIR/
  lob.csv
  trades.csv
```

The exact headers, column meanings, accepted values, normalization rules, and
Parquet schemas are defined in the
[L2 conversion contract](docs/hw4/contracts/01_l2_python_pipeline_contract.md).
The generated manifest must match the
[machine-readable JSON Schema](docs/hw4/contracts/l2_dataset_manifest.schema.json).

### Convert CSV to Parquet and replay cache

Install the locked dependencies first, because conversion requires PyArrow:

```bash
uv sync --locked
```

For a dataset with confirmed metadata:

```bash
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

The uppercase values are required dataset facts, not example defaults. If
provenance or semantics are unknown, conversion requires explicit diagnostic
mode. This is the command originally used for the current `data_trades` files:

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

The converter intentionally refuses to overwrite an existing output
directory. To reproduce the conversion, replace
`data_normalized/l2_parquet` with a new path such as
`data_normalized/l2_parquet-v2`, validate it, and only then switch replay to
its manifest.

Diagnostic values do not establish the real venue, symbol, multiplier, event
timestamp semantics, or trade-side semantics. The generated layout is:

```text
OUTPUT_DIR/
  manifest.json
  schema_version=1/
    instrument_id=<id>/
      date=<YYYY-MM-DD>/
        book_snapshots.parquet
        trades.parquet
        replay.l2cache
```

### Smoke test and full replay

Run a short ten-second check first:

```bash
uv run --no-sync python examples/run_local_l2.py \
  data_normalized/l2_parquet/manifest.json \
  --duration-seconds 10 \
  --run-summary artifacts/l2_smoke_summary.json
```

The current local dataset covers six UTC dates, 2024-08-01 through
2024-08-06. Replay all 22,901,679 normalized records with:

```bash
uv run --no-sync python examples/run_local_l2.py \
  data_normalized/l2_parquet/manifest.json \
  --duration-seconds 518400 \
  --run-summary artifacts/l2_full_summary.json
```

For a different dataset, use `backtest.run(..., DateRange())` to replay its
entire manifest range, or calculate a bounded range from the manifest
timestamps. Unverified datasets require
`BacktestConfig(allow_unverified_metadata=True)`; the local example sets this
explicitly.

Confirm that the full replay completed and reconciled with the manifest:

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

The command must print `true` and exit with status 0. See the
[replay-audit contract](docs/hw4/architecture/06_python_api_and_results.md#optional-run-summary-and-replay-audit)
and [manifest/cache validation order](docs/hw4/architecture/12_l2_manifest_cache_validation.md)
for the meaning and limits of these checks. A detailed walkthrough is in the
[HW4 getting-started guide](docs/hw4/GETTING_STARTED.md#6-input-data-contracts).

### Merge multiple disjoint L2 datasets

Create a strict flat parent manifest from two or more generated child
manifests. Priorities must be unique; the lower number wins equal timestamps:

```bash
uv run python scripts/create_multi_source_manifest.py \
  data_normalized/multi_source_manifest.json \
  --dataset-id multi-instrument-run \
  --source 10:data_normalized/instrument_1/manifest.json \
  --source 20:data_normalized/instrument_2/manifest.json
```

Pass the produced parent path to the unchanged `backtest.run()` API. Child
manifests must live below the parent directory and own disjoint
`instrument_id` values. They must also use identical `timestamp_semantics`;
exchange, receive, and local-receive timelines cannot be mixed. The runtime
validates parent declarations, child manifest SHA-256/bytes/counts/bounds, and
each selected replay cache before starting threads. See the
[multi-source architecture](docs/hw4/architecture/13_restricted_nway_event_merger.md)
and [JSON Schema](docs/hw4/contracts/multi_source_manifest.schema.json).

## Native build and tests

From a clean checkout, install the editable extension, verify the import, and
run the complete Release and Python suites:

```bash
uv sync --locked
uv run pip install -e .
uv run python -c "import back_tester; print(back_tester.__file__); print(back_tester.version())"
uv run cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
uv run cmake --build build-release -j
uv run ctest --test-dir build-release --output-on-failure
uv run pytest -q python/tests
uv run python examples/mean_reversion.py
```

Run the required benchmarks only from that Release setup:

```bash
build-release/bin/test/back-tester-scheduler-benchmark
build-release/bin/test/back-tester-price-cross-benchmark
uv run python python/benchmarks/callback_overhead.py
uv run python scripts/benchmark_l2_replay.py PATH_TO_MANIFEST
```

The scheduler benchmark reports warmed dispatcher-to-consumer round trips,
including ring capacity, waiting strategy, build/compiler/platform metadata,
and min/mean/p50/p95/p99. The price-cross benchmark reports construction and
native replay cost for 8- and 64-signal groups, including trigger-buffer
reallocations. The Python benchmark reports 20 warmed samples of
exactly 1,000 no-op callbacks for top-1 and top-15 payloads. A synthetic empty
native loop is not reported because the Release compiler elides it; callback
totals remain unadjusted. Parsing, process startup, logging, and DataFrame
construction are outside the timed region. Results are machine-specific
observations, not universal pass thresholds.

The HW4 runtime tests use a small checked-in test runner. The separate
component suite inherited from `cmf-team/main` uses Catch2 and exercises the
Feather ingestion, queue, order-book, pricing, and hedging modules.

## CLI smoke run

The CLI requires exactly one data path. With no path it prints usage and exits
with status 64:

```bash
build-release/bin/back-tester
```

Run ingestion against the deterministic checked-in fixture:

```bash
build-release/bin/back-tester test/data/tiny_mbo.jsonl
```

A missing or unreadable path exits with status 2.

## Feather ingestion prototype

The component pipeline from `cmf-team/main` is preserved as the separate
`back-tester-feather` executable. Convert a directory of `.mbo.json` files and
then pass either one generated file or its containing directory:

```bash
uv run python scripts/convert_to_feather.py PATH_TO_DATA_DIRECTORY
build-release/bin/back-tester-feather PATH_TO_DATA_DIRECTORY
```

The converter creates `.mbo.json.feather` files. This prototype is independent
of the deterministic Python `backtest.run()` and L2 manifest contracts above.

## Model limitations

- A same-instrument best quote or trade price crossing the limit fills the
  complete remaining order at the trigger price. Historical quote/trade size,
  queue position, market impact, probabilistic passive fills, slippage, and
  source-book mutation are not modeled.
- The mandatory runtime uses one process, one dispatcher, and one EngineView
  with deterministic fixed latency. Multi-engine simulation is only an
  extension point.
- Only limit GTC submission and cancel are supported. Replace, IOC/FOK,
  post-only, stops, pegs, and multi-leg orders are out of scope.
- Input support is Databento-like MBO JSONL, one L2 manifest generated by
  `scripts/convert_l2_csv.py`, or a strict flat parent of disjoint L2
  manifests. Parquet is canonical storage; the runtime reads its numeric
  cache. Overlapping feeds, hierarchy, options exercise, assignment, expiry
  settlement, Greeks, and a full risk engine are not implemented.

## Development checks

Install the pre-commit hooks or run all configured formatters and linters:

```bash
uv run pre-commit install
uv run pre-commit run --all-files
```

The hooks:

- format and lint C++ code with `clang-format`;
- format and lint Python code with `ruff`;
- strip outputs from Jupyter notebooks.
