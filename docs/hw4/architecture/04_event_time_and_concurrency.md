# Event time, ordering, and concurrency

## 1. Time domains

Store both source and engine time where relevant:

- `exchange_ts_ns`: timestamp from the historical event or scheduled exchange-side command arrival;
- `engine_ts_ns`: time visible to the Trading Engine after configured latency;
- `scheduled_ts_ns`: scheduler ordering timestamp.

For the adopted single-timeline model:

```text
historical delivery scheduled_ts = exchange_ts + market_data_latency
new order arrival scheduled_ts   = submit_engine_ts + order_latency
cancel arrival scheduled_ts      = cancel_submit_engine_ts + order_latency
```

`order_latency` is strictly positive so a callback submission cannot execute
recursively at its submission timestamp. `market_data_latency` may be zero, but
neither configured latency may be negative.

The Trading Engine's virtual clock is set to the consumed event's `scheduled_ts_ns`. It must never move backwards. A source ordering regression is an input error, not something to hide with wall-clock time.

## 2. Scheduled event ordering

```cpp
struct ScheduledKey {
    TimestampNs scheduled_ts_ns;
    EventPriority priority;
    std::uint64_t source_or_command_seq;
};
```

Lexicographic ordering is mandatory:

1. `scheduled_ts_ns` ascending;
2. priority: historical market event, new order, cancel;
3. stable sequence ascending.

Do not depend on heap insertion order or thread timing.

## 3. High-level event atomicity

A raw MBO update may be one record in an exchange event group. The historical book applies all records required to reach a consistent state. A strategy book callback is published only at the agreed atomic boundary, such as `F_LAST`.

A streaming source's `next()` stages one complete typed group and its immutable
scheduled key, but does not mutate the historical book. After that market key
wins chronological selection against pending commands, the dispatcher calls
the source's optional `prepare_for_dispatch()` immediately before publication.
Preparation applies the staged group and materializes trades and top-N views;
it may not change the key or priority. Thus a command ordered before a
prefetched market group sees the old book, while equal-time market data still
wins by the standard priority rule.

For the optional restricted multi-source path, `NWayMarketMerger` keeps one
staged group per leaf and compares leaf-global keys in this order:

```text
event_ts_ns, source_priority, source_id, source_local_group_sequence
```

Only the winning leaf is prepared. It remains the current winner until the
Trading Engine acknowledges the delivery, after which that leaf alone may be
advanced. This preserves the same buffer-lifetime and shared-book rules as the
single-source path. Before the requested start time, every pre-range group in
the selected warm-up suffixes is advanced through this same ordering rule;
warming leaves independently would produce incorrect state when their
timestamps interleave.

While applying the group row by row, preparation also materializes an ordered
`PriceCrossSignal` span:

- every book-mutating row contributes the best bid/ask visible immediately
  after that row;
- every trade row contributes its trade price;
- every signal retains the raw source sequence and group timestamps.

The Trading Engine replays that span in raw source order. The first qualifying
signal fills an eligible order, so a trade before a quote transition can win
inside one atomic group and vice versa. Historical quote/trade size is not part
of the predicate or fill quantity. The winning raw source sequence is copied
into `SyntheticFill`, `FillView`, and `FillResultRow`; for multi-source replay,
the source ID and global market sequence are copied with it. These provenance
fields are distinct from the synthetic fill sequence and Scheduler dispatch
sequence.

A `Trade` callback may be published in the same high-level engine event as a
book update. The callback order is:

1. replay ordered price-cross signals through `SimulatedLOB`;
2. update order state/position and emit `on_fill()` immediately for each
   winning signal;
3. emit `on_trade()` for source trade records in stable source order;
4. emit `on_book_update()` for the final stable top-N book if it changed;
5. process strategy submissions into the command ring;
6. publish `processed_seq`.

This callback order is a project decision and must be covered by integration tests.
Reject notifications caused by a command API call inside any strategy callback
are queued FIFO and delivered only after that initiating callback unwinds; they
must not recursively enter `on_reject()`.

## 4. Queue model

Use two bounded SPSC rings:

- Backtest Engine producer → Trading Engine consumer: `EngineEvent` ring;
- Trading Engine producer → Backtest Engine consumer: `OrderCommand` ring.

Requirements:

- fixed capacity configured at startup;
- no per-event heap allocation in normal operation;
- explicit full/empty behavior;
- stop token checked while waiting;
- producer and consumer indexes separated to reduce false sharing where practical;
- acquire/release semantics documented next to the implementation.

The event producer may enqueue the next event while waiting, but the Trading Engine must gate consumption until the current event is fully acknowledged.

## 5. Ready barrier

Use a monotonically increasing dispatch sequence, starting at 1.

Trading Engine completion:

```cpp
processed_seq.store(event.dispatch_seq, std::memory_order_release);
processed_seq.notify_one(); // when atomic wait/notify is available
```

Dispatcher wait:

```cpp
while (processed_seq.load(std::memory_order_acquire) < expected_seq) {
    // atomic_wait, bounded spin + wait, or another lightweight strategy
    // that also observes stop_requested.
}
```

`processed_seq` is set only after:

- all matching caused by the event;
- all OrderManager/PositionKeeper updates;
- all required Python callbacks;
- all commands created by those callbacks are successfully enqueued;
- any result rows caused by the event are recorded.

## 6. Shared-book safety

Under the strict barrier:

1. dispatcher applies the historical update;
2. dispatcher publishes the event with release semantics;
3. trading thread reads the stable historical book;
4. trading thread publishes `processed_seq` with release semantics;
5. dispatcher observes it with acquire semantics before writing the book again.

Non-owning trade/top-N buffers produced during preparation remain unchanged
through publication and callback processing. They may be reused only when
`next()` is called after the delivery's processed sequence is acknowledged.

This ownership protocol can remove hot-path book mutexes for the mandatory one-engine version. Do not remove existing locks until tests and sanitizer runs demonstrate that all access follows this protocol. Multi-engine bonus mode requires waiting for every engine's ready sequence.

## 7. Command timing

A strategy submission immediately creates local `PendingNew` state and writes a command containing:

- client order ID;
- instrument ID;
- side, limit price, quantity;
- submit engine time;
- scheduled arrival time;
- monotonically increasing command sequence.

The command is not matched until its scheduled arrival event is dispatched. Cancel follows the same path. A command submitted from `on_fill()` cannot execute recursively.

## 8. Stop and exception behavior

Normal end-of-data:

1. stop accepting new source events;
2. process already scheduled commands only if their arrival time is inside the configured backtest end policy;
3. publish an end event;
4. join threads;
5. finalize results.

Python or native failure:

1. store the first exception;
2. set `stop_requested` atomically;
3. notify both queues and the ready waiter;
4. do not wait forever for a sequence that can no longer be produced;
5. join both threads;
6. rethrow on the `backtest.run()` caller thread.

Never detach threads. Never suppress the original exception behind a shutdown timeout.

## 9. Determinism requirements

The same input, config, and strategy must produce byte-equivalent order/fill ordering across repeated runs on the same build. Tests must repeat representative runs at least 20 times and compare normalized results.

The restricted multi-source path additionally requires stable source IDs and
priorities, disjoint book ownership, contiguous global market sequences, and a
replay digest over source identity, local sequence, timestamp, event kind, and
instrument. The complete contract is in
[`13_restricted_nway_event_merger.md`](13_restricted_nway_event_merger.md).
