# Architecture-to-implementation map

This page is a code-reading index for the implemented system.

## End-to-end composition

| Concern | Implementation | Primary verification |
|---|---|---|
| Public Python entry point | `python/back_tester/__init__.py`, `src/python/bindings.cpp` | `python/tests/test_runtime.py`, `python/tests/test_end_to_end.py` |
| Runtime composition | `src/runtime/BacktestRuntime.cpp` | `test/RuntimeTest.cpp` |
| Streaming JSONL source | `src/market/JsonlReader.*`, runtime `JsonlScheduledSource` | `test/CoreMarketTest.cpp`, `test/RuntimeTest.cpp` |
| L2 conversion, trust preflight, and replay | `scripts/convert_l2_csv.py`, `src/market/L2CacheReader.*`, `HistoricalL2Book.*`, runtime `L2CacheScheduledSource` | `python/tests/test_l2_pipeline.py`, L2 cases in `test/CoreMarketTest.cpp` |
| Historical L3 state | `src/market/LimitOrderBook.*`, `HistoricalLOBStore.*` | `test/CoreMarketTest.cpp` |
| Scheduled ordering | `src/scheduler/ChronologicalScheduler.*` | `test/SchedulerTest.cpp` |
| Threading and queues | `SchedulerRuntime.hpp`, `SpscRing.hpp`, `ReadyBarrier.hpp` | `test/SchedulerTest.cpp` |
| Strategy commands, lifecycle, and positions | `src/trading/TradingEngine.*`, `PositionKeeper.*` | `test/TradingTest.cpp` |
| Private orders, matching, and synthetic fills | `src/trading/SimulatedLOB.*` | `test/TradingTest.cpp`, `test/TypedSimulatedLOBTest.cpp` |
| Position accounting | `src/trading/PositionKeeper.*` | `test/TradingTest.cpp` |
| Result columns and PnL | `src/results/ResultRecorder.*` | `test/ResultsTest.cpp` |
| Run summary and replay audit | runtime `RunStatistics`, `src/python/bindings.cpp` | summary cases in `python/tests/test_runtime.py`, `python/tests/test_l2_pipeline.py` |
| Python callbacks and GIL | `src/python/bindings.cpp` | `python/tests/test_runtime.py` |
| Real two-instrument workflow | `examples/mean_reversion.py` | `python/tests/test_end_to_end.py` |

## Public contracts

| Contract | Source |
|---|---|
| Numeric types and stable enum values | `src/core/Types.hpp` |
| Instrument metadata, latency config, date range | `src/core/BacktestConfig.hpp` |
| Callback views, commands, scheduled events, query rows | `src/core/Events.hpp` |
| Fill, order-log, and PnL rows | `src/core/ResultSchemas.hpp` |
| Strategy and StrategyContext interfaces | `src/trading/Strategy.hpp` |
| Frozen column views | `src/results/ResultRecorder.hpp` |

`src/common/BasicTypes.hpp` is a compatibility include. New engine contracts
come from `src/core`.

## Runtime event path

```text
JsonlReader
  -> JsonlScheduledSource::next() stages one atomic group and ordering key
  -> SchedulerRuntime compares the market key with queued commands
  -> JsonlScheduledSource::prepare_for_dispatch() applies HistoricalLOBStore
  -> SchedulerRuntime publishes ScheduledEvent
  -> TradingEngine advances virtual time and handles the payload
  -> SimulatedLOB returns ordered SyntheticFill decisions
  -> ResultRecorder appends typed columns
  -> TradingEngine invokes Python through PythonStrategyAdapter
  -> ReadyBarrier acknowledges completion
```

The L2 alternative replaces the first four lines with:

```text
convert_l2_csv.py -> daily Parquet + replay.l2cache + manifest.json
L2CacheReader
  -> strict manifest and consent validation
  -> all-cache counts/bounds/date/sequence reconciliation
  -> trusted DateRange selection and selected-cache SHA-256
  -> L2CacheScheduledSource::prepare_for_dispatch()
  -> HistoricalL2Book::replace_snapshot() or one typed TradeView
```

The preflight completes on the caller thread before `SchedulerRuntime` starts
its dispatcher and trading threads. See
[`12_l2_manifest_cache_validation.md`](12_l2_manifest_cache_validation.md).

## Command path

```text
Python Strategy
  -> PythonStrategyHandle
  -> TradingEngine::submit_limit() / cancel_order()
  -> CommandSink
  -> SPSC command ring
  -> ChronologicalScheduler
  -> ScheduledEvent arrival
  -> SimulatedLOB matching decision
  -> TradingEngine state transition
```

## Build and packaging map

| File | Responsibility |
|---|---|
| `pyproject.toml` | Python metadata, locked dependency surface, scikit-build-core configuration |
| `uv.lock` | Reproducible Python/build dependency resolution |
| root `CMakeLists.txt` | Project options and top-level targets |
| `src/*/CMakeLists.txt` | Native source composition and pybind11 module |
| `test/CMakeLists.txt` | Native CTest registrations and benchmark target |
| `python/back_tester/__init__.py` | Stable import surface |

For requirement-level mapping from both source documents to code and tests, see
[`11_requirements_traceability.md`](11_requirements_traceability.md).
