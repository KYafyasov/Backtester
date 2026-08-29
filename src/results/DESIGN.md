# Native result ownership and PnL

`ResultRecorder` is a `trading::Recorder` implementation plus an explicit
`on_book_mark()` input. It consumes decisions made by the trading engine; it
does not match orders or create transitions.

Each tabular result column has one typed `std::vector`. All affected vectors
are reserved before a row is committed, so allocation failure cannot expose a
partial row. FIFO transitions are first analyzed read-only. The ledger uses a
contiguous lot vector with a logical head, so closures advance an index instead
of erasing/shifting the live history. Any required lot/result capacity is
prepared before the no-throw mutation commit. `position_lots_per_instrument`
allows the caller to reserve the expected lot count up front.

For a closed FIFO lot, realized PnL is:

```text
(exit_ticks - entry_ticks) * signed_closed_quantity
    * contract_multiplier / price_scale
```

An open lot marked at the exact midpoint uses:

```text
((bid_ticks + ask_ticks) / 2 - entry_ticks) * signed_open_quantity
    * contract_multiplier / price_scale
```

Every multiplication, addition, and rational normalization is checked before
state mutation. Missing-sided books retain the last valid mark. Fill samples
and changed marks at the same engine timestamp replace the last aggregate
sample.

Marked unrealized PnL is computed from the checked aggregate signed open cost,
not by walking or copying every FIFO lot. Same-side opens are therefore
amortized constant-time after reservation; opposite fills inspect only the lots
they close.

During a run, PnL remains a reduced `int64 numerator / positive int64
denominator`. `freeze()` performs the only bulk conversion to `double`, marks
the storage immutable, and returns `FrozenResults`:

```text
ResultRecorder --shared ownership--> immutable native column storage
                                      ^
FrozenResults -----------------------|
```

The read-only spans remain valid while any copied `FrozenResults` handle is
alive, even after the recorder and engine are destroyed.

## Dataset provenance

`FrozenResults::Storage` also owns optional `DatasetMetadata` containing
`dataset_id` and `verified_metadata`. This metadata is immutable and shares the
same lifetime as the result columns, but it is not represented as a tabular
vector.

For single or strict multi-source manifest-backed L2 replay, the runtime copies
the manifest identity and verification state into the recorder before the run
starts. For JSONL input, the optional metadata remains empty because no dataset
manifest established those facts. The Python `Result` consequently exposes
the two L2 values and returns `None` for both properties on JSONL runs.

Fill columns preserve local and merged trigger provenance through
`trigger_source_sequence`, `trigger_source_id`, and
`trigger_global_market_sequence`. Legacy single-source fills retain zero in
the two added multi-source fields.

Order-log columns preserve a monotonic `transition_sequence`, both
`previous_state` and resulting `state`, and the queue estimate after the
transition. This keeps lifecycle reconstruction numeric and columnar without
introducing per-event strings or file I/O in the trading loop.

## Native/Python ownership boundary

The results component itself creates no Python, NumPy, pandas, or Arrow
objects. The pybind11 layer constructs NumPy arrays that point directly at the
frozen native columns. Each array carries a capsule containing a shared
`FrozenResults` owner, so pandas objects created with `copy=False` cannot
outlive their native storage.

The optional `run_summary.json` is not owned by `ResultRecorder`. Runtime and
callback audit counters are collected outside the result hot path, and the
Python binding publishes the summary only after native threads have joined.
The public schemas, provenance behavior, zero-copy boundary, and run-summary
contract are documented in
[`docs/hw4/architecture/06_python_api_and_results.md`](../../docs/hw4/architecture/06_python_api_and_results.md).
