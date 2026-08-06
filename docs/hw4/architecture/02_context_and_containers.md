# Runtime architecture and data flow

## System context

```mermaid
flowchart LR
    USER["Python strategy"]
    DATA["MBO JSONL or L2 dataset manifest"]
    API["backtest.run"]
    NATIVE["Native C++ runtime"]
    RESULT["Result<br/>fills, order log, PnL"]

    USER --> API
    DATA --> NATIVE
    API --> NATIVE
    NATIVE --> RESULT --> USER
```

`backtest.run()` is the public entry point. It accepts a Strategy instance,
input path, date range, optional configuration, and optional instrument
metadata. When metadata is omitted, the binding performs one discovery pass to
find numeric instrument IDs and then runs the streaming replay with Databento
nanounit price scaling.

## Process and thread model

```mermaid
flowchart TB
    subgraph CALLER["Python caller thread"]
        RUN["pybind11 run()"]
        ADAPTER["PythonStrategyAdapter"]
        PYRESULT["Python Result wrapper"]
    end

    subgraph NATIVE["Native runtime"]
        subgraph DISPATCHER["Dispatcher thread"]
            READER["JsonlReader or L2CacheReader"]
            SOURCE["JSONL or L2 scheduled source"]
            BOOKS["HistoricalLOBStore<br/>L3 or honest L2 state"]
            SCHED["ChronologicalScheduler"]

            READER --> SOURCE
            SOURCE --> BOOKS
            SOURCE --> SCHED
        end

        subgraph CONSUMER["Trading thread"]
            ENGINE["TradingEngine"]
            SIM["SimulatedLOB / EngineView"]
            POSITION["PositionKeeper"]
            RECORDER["ResultRecorder"]

            ENGINE --> SIM
            ENGINE --> POSITION
            ENGINE --> RECORDER
        end

        EVENTQ[["SPSC ScheduledEvent ring"]]
        COMMANDQ[["SPSC OrderCommand ring"]]
        READY[("processed_seq")]

        SCHED --> EVENTQ --> ENGINE
        ENGINE --> COMMANDQ --> SCHED
        ENGINE --> READY
        READY -. "acknowledgement" .-> SCHED
    end

    RUN --> NATIVE
    ENGINE <--> ADAPTER
    RECORDER --> PYRESULT
```

`SchedulerRuntime::run()` starts and joins both native threads. The Python
binding releases the GIL around the native run. The trading thread reacquires
the GIL only while calling a Python strategy method.

## Ownership

| State | Writer | Read access |
|---|---|---|
| Physical reader and staged MBO group or L2 row | Dispatcher thread | Dispatcher only |
| `HistoricalLOBStore` | Dispatcher thread | Trading thread while dispatcher waits for acknowledgement |
| Scheduler heap and source merge state | Dispatcher thread | Dispatcher only |
| Event ring | Dispatcher producer | Trading consumer |
| Command ring | Trading producer | Dispatcher consumer |
| Virtual clock and private orders | Trading thread | Strategy during an active callback |
| Positions and private resting orders | Trading thread | Strategy during an active callback |
| Mutable result recorder | Trading thread | Runtime marking code executes in the same consumer callback |
| Frozen result storage | No writers after `freeze()` | Python arrays/DataFrames |
| Python Strategy object | Python owns lifetime | Trading thread invokes it while holding the GIL |

Single-writer ownership and the ready barrier avoid hot-path book mutexes in the
mandatory runtime.

## Market-delivery flow

```mermaid
sequenceDiagram
    participant R as JsonlReader
    participant D as Dispatcher
    participant H as HistoricalLOBStore
    participant T as TradingEngine
    participant S as SimulatedLOB
    participant P as Python Strategy
    participant O as ResultRecorder

    R->>D: stage one complete atomic group
    D->>D: compare market key with queued commands
    D->>H: apply group when market event wins
    D->>T: publish ScheduledEvent(dispatch_seq)
    T->>T: set virtual clock
    T->>S: re-evaluate resting orders against H
    S-->>T: SyntheticFill decisions
    T->>O: record state/fills
    T->>P: on_fill() zero or more times
    T->>P: on_trade() in source order
    T->>P: on_book_update() if top-N changed
    T->>D: publish processed_seq
```

The source stages a group before chronological selection but delays historical
book mutation until `prepare_for_dispatch()`. A strategy command scheduled
before that market group therefore observes the old book. Equal-time market
data still wins because market priority is lower numerically.

## Order and cancel flow

```mermaid
sequenceDiagram
    participant P as Python Strategy
    participant T as TradingEngine
    participant Q as Command ring
    participant D as Dispatcher
    participant H as HistoricalLOBStore
    participant S as SimulatedLOB
    participant R as ResultRecorder

    P->>T: submit_limit(...)
    T->>R: PendingNew / Submit
    T->>Q: NewOrderCommand(now + order latency)
    Q->>D: drain command
    D->>T: NewOrder arrival
    T->>R: Open / Accepted
    T->>S: accept order against H
    S-->>T: SyntheticFill decisions
    T->>R: zero or more Fill transitions
    T->>P: on_fill() after state update

    opt remaining quantity
        S->>S: rest in EngineView price-time index
    end

    P->>T: cancel_order(id)
    T->>R: PendingCancel / CancelRequest
    T->>Q: CancelCommand(now + order latency)
    Q->>D: drain command
    D->>T: Cancel arrival
    T->>R: Cancelled or typed reject
```

Commands submitted by a callback are fully enqueued before the current event
is acknowledged. The dispatcher drains them while waiting and merges them with
future market deliveries.

## End of run

At normal end-of-data, commands at or before the inclusive range end are
processed, rings are closed, threads join, and `ResultRecorder::freeze()`
produces immutable shared storage. On failure, the first exception triggers the
same wake-and-join path and is rethrown after both threads exit.
