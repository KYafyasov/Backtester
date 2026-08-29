# Homework 4: Trading Engine Consumer & Simulation, Python Strategy Layer

> Faithful transcription of the provided Homework 4 PDF. Formatting and line wrapping were normalized; the requirements were not expanded with project assumptions.

## Before starting

Agree together and commit a short note covering:

- **The Strategy callback contract:** what granularity `on_book_update()` fires at — every raw L3 event, top-of-book only, top-N depth, or rate-limited. This is a joint decision: Group B needs it for the Python API, Group A needs it to shape C++-side filtering before invoking a callback.
- **The latency model surface:** two independent parameters — market-data latency (exchange → trading engine) and order latency (trading engine → exchange). Fixed values for this homework, no jitter.

# Group A — Trading Engine Consumer & Simulation

## Objective

Implement the trading-engine-side components that consume market data from the backtest engine thread, simulate this engine's private view of the book with its own order matching and fills, and keep track of the resulting orders and positions.

## Task

### 1. Market Data Consumer

Subscribe to Group 2's feed and receive `BookUpdate` / `BookSnapshot` / `Trade` messages from the backtest engine thread.

- Advance a virtual clock: this engine's current time = last received event's timestamp + configured market-data latency.
- Implement the ready signal as a lightweight atomic sequence counter, not a queue message: bump `processed_seq` once the engine has finished reacting to an event; the backtest dispatcher only releases the next event once `processed_seq >= N`.
- Prefetching the next event into the queue while waiting is fine — gate consumption, not enqueue.

### 2. LOB Simulation with Order Matching & Fills

Build this engine's `SimulatedLOB` by overlaying its own resting orders (`EngineView`) on top of the shared `HistoricalLOB`.

- Implement fill simulation: start with fill-at-touch — when this engine's own limit order is at or through the best historical price, generate a synthetic fill.
- Timestamp outgoing orders using the order-latency parameter so fills reflect a realistic, non-instant round trip rather than the backtest's raw in-process queue speed.

### 3. Order Manager / Position Keeper

Track this engine's own open orders and the resulting position as fills arrive from the LOB simulation.

- Update position and open-order state on every fill.
- Expose current position and open orders in a form a strategy can query; this is what Group B's Python Strategy layer will read from and submit orders through.

## Deliverables

- Market Data Consumer receiving real messages from the backtest engine thread, with virtual-clock and ready-signal synchronization.
- `SimulatedLOB` combining `HistoricalLOB + EngineView` with fill-at-touch matching.
- Order Manager / Position Keeper reflecting fills correctly.
- A small synthetic test harness — a handful of hardcoded test orders — proving matching and fills work correctly in isolation, ahead of the joint checkpoint.
- Benchmark: ready-signal round-trip latency.

## Bonus

N concurrent `EngineView`s trading against one shared `HistoricalLOB`, each with its own virtual clock and ready-signal, with the dispatcher's barrier logic extended to handle multiple consumers advancing at different rates.

# Group B — Python Strategy Layer (continues Group 4)

## Objective

Build the pybind11 boundary and a minimal Python Strategy/Backtest API so a strategy written in Python can drive Group A's trading engine and get results back as pandas DataFrames.

## Task

### 1. Packaging

Resolve the packaging setup: `../../../pyproject.toml` is currently configured for PDM alongside a `../../../uv.lock` file — pick one, and set up `scikit-build-core` so `pip install -e .` builds the C++ extension.

### 2. Bindings

Bind the core types (`BasicTypes.hpp`) and the order-submission API via pybind11.

### 3. Strategy base class

Implement the Strategy base class:

- `on_book_update()`;
- `on_trade()`;
- `on_fill()`;
- `on_reject()`.

Use the agreed callback granularity and release the GIL outside the callback invocation window.

### 4. Result and entry point

Implement:

- `Result.pnl_series`;
- `Result.fills_df`;
- `Result.order_log_df`;
- a minimal `backtest.run(strategy, data_path, date_range)` entry point.

The pandas objects must be built via zero-copy NumPy/pyarrow hand-off, not per-row Python appends.

## Deliverables

- Resolved packaging using one tool, with a working `pip install -e .`.
- pybind11 module binding core types and the order-submission API.
- Strategy base class with correct GIL discipline.
- Result object using zero-copy hand-off, with a working `backtest.run()` entry point.
- Benchmark: Python callback overhead per 1,000 invocations.

# Integration checkpoint — joint milestone

Once Group A's components pass their synthetic test harness and Group B's bindings work against a stub, run a first real end-to-end test:

- a Python-defined strategy, for example a simple mean-reversion strategy;
- submitting real orders through Group A's `Market Data Consumer → LOB Simulation → Order Manager` pipeline.

This is where interface mismatches from the frozen callback contract will surface.
