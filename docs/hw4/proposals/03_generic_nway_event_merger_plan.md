# Generic N-way Event Merger implementation plan

## Status

- **State:** restricted flat implementation delivered and independently
  reviewed without unresolved P0/P1; hierarchy deferred; not required for
  Homework 4
- **Prepared against:**
  `c6675253bf5472e5875afcf5060584266db4c0c7`
- **Current traceability status:**
  `RESTRICTED FLAT IMPLEMENTED / HIERARCHY DEFERRED`
- **Activation condition:** satisfied by the requested multi-L2 replay path
- **Estimated effort:** 2–4 working days for the restricted flat merger;
  5–10 working days for multi-venue consolidation, mixed L2/L3 state, or a
  public recursive-manifest API

This proposal incorporates the architecture review of the initial generic
merger idea. Its restricted flat L2 scope has now been implemented; the
current HW4 path itself still does not require this extension.

Architecture documents describe implemented behavior and remain authoritative.
The delivered behavior is documented in
[`../architecture/13_restricted_nway_event_merger.md`](../architecture/13_restricted_nway_event_merger.md).

## Implementation outcome

Delivered:

- strict `cmf-multi-source-v1` flat parent manifest and JSON Schema;
- L2-manifest child support with child-manifest SHA-256/bytes/counts/bounds
  reconciliation;
- typed child timestamp semantics with cross-source compatibility validation;
- fail-closed sign and target-range validation for every integer field before
  C++ conversion;
- unique source IDs/priorities and disjoint `instrument_id` ownership;
- fixed-capacity heap selection with the planned leaf-global key;
- globally ordered warm-up through the same merger;
- full winner-identity validation after leaf preparation;
- separate source-local, global-input, global-market, and Scheduler dispatch
  sequences;
- callback/fill/result provenance;
- per-source conservation counters and versioned composite digest;
- native lifecycle/ordering tests, Python end-to-end/adversarial tests, and
  20-fold deterministic replay.

Still deferred:

- wrapped JSONL leaves;
- overlapping ownership, consolidated multi-venue books, or mixed L2/L3;
- public nested manifests and internal hierarchy composition;
- generic live/plugin source support.

## Reference contracts

- [`01_homework_4_assignment.md`](../source/01_homework_4_assignment.md) — the
  written assignment does not require a generic multi-source merger.
- [`01_scope_and_decisions.md`](../architecture/01_scope_and_decisions.md) —
  current one-timeline scope and the prohibition on mixing L2/L3 state for one
  instrument.
- [`04_event_time_and_concurrency.md`](../architecture/04_event_time_and_concurrency.md)
  — two-phase source preparation, span lifetime, and ready-barrier contract.
- [`11_requirements_traceability.md`](../architecture/11_requirements_traceability.md)
  — current Event Merger disposition: one market source plus delayed commands.
- [`SchedulerRuntime.hpp`](../../../src/scheduler/SchedulerRuntime.hpp) —
  implemented `next()`, optional `prepare_for_dispatch()`, command merge, and
  acknowledgement lifecycle.
- [`Events.hpp`](../../../src/core/Events.hpp) — current market provenance,
  scheduled keys, and dispatch sequence.

## 1. Executive decision

Do not build a generic Event Merger for the current HW4 submission.

The existing runtime has two distinct ordering layers:

1. conversion establishes a deterministic `merged_sequence` for the local L2
   snapshot and trade rows;
2. `SchedulerRuntime` merges one prefetched market delivery with delayed new
   order and cancel commands.

That behavior is sufficient for the written assignment and is correctly
classified as `PARTIAL / COLLAPSED` relative to the broader original diagram.

If a real multi-source use case appears, implement only the restricted scope
defined below. Do not begin with public nested manifests, consolidated books,
or mixed L2/L3 state.

## 2. Current contracts that must remain intact

### 2.1 Two-phase source lifecycle

The source contract is not a simple iterator. It protects causality and the
lifetime of non-owning spans:

```text
next()
  -> stage one complete group and immutable scheduled key
  -> do not mutate the historical book

prepare_for_dispatch()
  -> called only after the staged group wins against strategy commands
  -> apply the group to historical state
  -> materialize trades, price-cross signals, and top-N spans

publish to event ring
  -> trading thread processes matching, state, results, and callbacks

processed_seq acknowledgement
  -> only now may the dispatcher/source reuse staged buffers

next()
  -> implicitly releases the previous winner and stages another group
```

The merger must preserve the existing `next()` plus
`prepare_for_dispatch()` protocol. A `consume()` operation immediately after
winner selection is forbidden because it could invalidate spans before the
trading thread acknowledges the delivery.

The first implementation should keep release implicit: the scheduler already
does not call `next()` again until `processed_seq` acknowledges the current
delivery. An explicit `release_after_ack()` may be added only if a later source
needs resources that cannot follow this convention.

### 2.2 Atomic market group

The unit of comparison is one complete market group, never an individual raw
MBO row. One L2 snapshot row and one L2 trade row are naturally single-record
groups. One MBO group may contain multiple raw records ending at its atomic
boundary.

The winning group is prepared and published as one `MarketDelivery`. No event
from another source and no strategy command may split its raw records.

### 2.3 Ready barrier

The merger runs on the dispatcher side. It must not weaken the existing
release/acquire protocol:

```text
prepare winner
  -> publish delivery
  -> trading thread reacts completely
  -> processed_seq.store(dispatch_sequence, release)
  -> dispatcher observes acknowledgement with acquire
  -> merger may advance the winning leaf
```

Prefetching one immutable group key per leaf is allowed. Applying or consuming
the next winning group before acknowledgement is not allowed.

## 3. Restricted first implementation scope

### In scope

- two or more finite, independently chronological offline market sources;
- one flat multi-source manifest;
- one staged atomic group per leaf source;
- deterministic N-way ordering before the existing command scheduler;
- globally ordered warm-up through the same merger;
- separate leaf provenance, global market sequence, and dispatch sequence;
- strict source metadata, counts, bounds, and SHA-256 in multi-source mode;
- per-source/global selected-record conservation and full-replay exact-once
  checks;
- internal hierarchy-equivalence tests (future completion criterion, not part
  of the delivered flat runtime).

### Book ownership rule

For the first version, each runtime `instrument_id` must be owned by exactly one
leaf source. The sets of instrument IDs declared by different leaves must be
disjoint.

This is intentionally stricter than a future `(venue_id, instrument_id)` rule
because the current runtime routes books only by `instrument_id`. It avoids
silently combining:

- two venues for one instrument;
- L2 and L3 state for one instrument;
- a snapshot feed and an independently stateful book feed;
- two sources that can both mutate the same historical book.

A current L2 dataset containing snapshots and trades remains one leaf because
its converter has already established one immutable merged chronology.

### Explicitly out of scope

- consolidated books across venues;
- `venue_id` routing in strategy callbacks, orders, positions, or results;
- mixed L2/L3 state for one instrument;
- ownership hand-off between sources during a run;
- live or unbounded feeds and watermark/late-event policies;
- network sources, reconnects, or distributed clocks;
- public nested manifests;
- recursive manifest inclusion and cycle detection;
- changing market-versus-command priority in `SchedulerRuntime`.

## 4. Terminology and types

### 4.1 Leaf identity

```cpp
using SourceId = std::uint32_t;
using SourcePriority = std::uint32_t;

struct MarketGroupKey {
  TimestampNs event_ts_ns;
  SourcePriority source_priority;
  SourceId source_id;
  Sequence source_local_group_sequence;
};
```

Ordering is lexicographic in the field order shown above.

`source_local_group_sequence` is meaningful only inside its leaf. It is the
last raw source sequence in an MBO group or the row/group sequence supplied by
an atomic L2 event. It must never decide ordering between feeds before
`source_priority` and `source_id`.

Strict mode requires a unique `source_priority` per leaf. `source_id` and
`source_local_group_sequence` remain deterministic fail-safe tie-breakers and
make corrupted duplicate priorities observable rather than dependent on heap
insertion order.

All four fields belong to the leaf and must pass unchanged through internal
nested mergers. An internal `MergeSource` must not assign a parent priority or
replace leaf identity; otherwise flat and hierarchical arrangements could
produce different results.

### 4.2 Separate sequence domains

The design must retain four distinct concepts:

| Field | Scope | Meaning |
|---|---|---|
| `source_id` | leaf | Immutable feed/dataset identity. |
| `source_local_sequence` | leaf record | Original raw record provenance used to identify a quote/trade trigger. |
| `global_market_sequence` | merged market groups | Monotonic `1..N` order assigned by the N-way merger. |
| `dispatch_sequence` | all scheduled events | Existing Scheduler sequence covering market deliveries and strategy commands. |

The merger must not overwrite `source_local_sequence`. In particular, a fill
must preserve the pair `(trigger_source_id, trigger_source_local_sequence)`.
The global market sequence proves merged ordering but cannot identify which raw
feed record caused a fill.

The existing single-source public fields may remain compatibility aliases
during migration, but ambiguous reuse of `source_sequence` must not enter the
new implementation. Contract changes require coordinated updates to
`MarketDelivery`, `PriceCrossSignal`, `TradeView`, `FillView`, result schemas,
Python bindings, and documentation.

### 4.3 Digest input

The replay digest must not hash only `global_market_sequence`, because every
apparently contiguous replay would then have the same digest. For each raw
record in final merged group order, hash a domain-separated fixed-width
encoding of at least:

```text
source_id
source_local_sequence
event_ts_ns
event_kind
instrument_id
```

Hash an explicit group-boundary marker after the final raw record of each
atomic group. For an L2 leaf, every snapshot or trade row is both one raw
record and one complete group.

Include explicit byte order and an algorithm/version label in the audit
contract. This digest is a determinism fingerprint, not a replacement for
manifest SHA-256.

## 5. Source and merger lifecycle

### 5.1 Leaf-source requirements

Each leaf must:

1. validate its manifest metadata before replay threads start;
2. stage exactly one complete group in `next()`;
3. return an immutable group key without mutating book state;
4. retain the group and its buffers until the next `next()` after
   acknowledgement;
5. apply state and materialize delivery spans only in
   `prepare_for_dispatch()`;
6. reject local timestamp or local group-sequence regression;
7. distinguish clean EOF from detectable truncation;
8. expose immutable source ID, priority, ownership set, and expected audit
   bounds.

Leaves participating in N-way mode must expose their full chronological range.
They must not independently consume or apply pre-range records; range ownership
and warm-up move to the merger.

Legacy direct JSONL remains supported as the current single-source mode. It
cannot participate in strict multi-source mode unless wrapped in a manifest
that supplies the required integrity and bounds metadata.

### 5.2 `NWayMarketMerger`

The flat merger owns or references N leaf sources and stores one staged head
per non-exhausted leaf.

```text
initialization:
  call next() once on each leaf
  validate every returned MarketGroupKey
  insert leaf heads into a fixed-capacity min-heap

NWayMarketMerger::next(out):
  this call occurs only after acknowledgement of the prior winner
  advance only the prior winning leaf
  validate its new key or mark it exhausted
  choose the minimum leaf MarketGroupKey
  assign the next global_market_sequence with overflow checking
  stage a lightweight market event without applying state

NWayMarketMerger::prepare_for_dispatch(out):
  verify that out still identifies the staged winner and key
  call prepare_for_dispatch() only on that leaf
  attach unchanged leaf provenance and global_market_sequence
  verify preparation did not change the ordering key
```

The heap capacity is fixed at startup from the manifest source count. The hot
path must not allocate once leaves and delivery buffers have been reserved.

### 5.3 Relationship to strategy commands

`NWayMarketMerger` produces only `EventPriority::MarketData`. The existing
`SchedulerRuntime` remains the sole authority that compares the staged market
winner against `NewOrderCommand` and `CancelCommand`.

At equal scheduled time, the existing priority remains:

1. market delivery;
2. new-order arrival;
3. cancel arrival.

Tests must include a strategy command whose arrival timestamp equals an event
from one leaf. The market event must win regardless of leaf priority.

## 6. Globally ordered warm-up

Warm-up is part of N-way ordering, not a per-leaf preprocessing step.

For `event_ts_ns < DateRange.start_ts_ns`, the same merger must repeatedly:

1. select the globally earliest group;
2. call the winning leaf's preparation path;
3. apply the historical state mutation;
4. suppress matching, strategy callbacks, public market deliveries, result
   rows, and strategy commands;
5. release the prepared buffers and advance only that leaf;
6. update per-source and global warm-up counters and digest state as defined by
   the audit contract.

The simplest safe implementation performs warm-up synchronously before
starting scheduler and trading threads, using the same `MarketGroupKey`
comparison and leaf preparation logic as replay. When the first staged key is
inside the requested range, ownership of the staged heads passes unchanged to
normal scheduler execution.

Even though the first scope requires disjoint `instrument_id` ownership, the
implementation must use global warm-up. This prevents a future optimization
from silently becoming incorrect when ownership rules evolve and proves that
warm-up/replay share one chronology.

The following sequence must produce the same final state and digest as a
single pre-merged reference stream:

```text
A @ 09:59:58
B @ 09:59:59
A @ 09:59:59.5
start = 10:00
```

## 7. Flat multi-source manifest

Only a flat manifest is public in the first version. A sketch follows; exact
field names and types require a versioned JSON Schema before implementation.

```json
{
  "format": "cmf-multi-source-v1",
  "manifest_version": 1,
  "dataset_id": "example-two-feed-run",
  "sources": [
    {
      "source_id": 1,
      "source_priority": 10,
      "format": "cmf-l2-parquet-cache-v1",
      "path": "instrument_1/manifest.json",
      "sha256": "<64 lowercase hex characters>",
      "bytes": 1234,
      "record_count": 100,
      "group_count": 100,
      "min_event_ts_ns": 1000000,
      "max_event_ts_ns": 2000000,
      "min_source_sequence": 1,
      "max_source_sequence": 100,
      "instrument_ids": [1]
    },
    {
      "source_id": 2,
      "source_priority": 20,
      "format": "mbo-jsonl-v1",
      "path": "instrument_2/data.jsonl",
      "sha256": "<64 lowercase hex characters>",
      "bytes": 5678,
      "record_count": 120,
      "group_count": 80,
      "min_event_ts_ns": 1100000,
      "max_event_ts_ns": 2100000,
      "min_source_sequence": 1,
      "max_source_sequence": 120,
      "instrument_ids": [2]
    }
  ],
  "expected_global_group_count": 180
}
```

Strict validation must reject:

- unknown or duplicate fields;
- duplicate/zero `source_id`;
- duplicate `source_priority`;
- absolute paths or `..` path traversal;
- unsupported source format/schema versions;
- missing file, byte-size mismatch, or SHA-256 mismatch;
- zero or contradictory counts and bounds;
- overlapping `instrument_ids` ownership;
- child metadata inconsistent with the parent declaration;
- source-local chronology outside declared bounds;
- cleanly parsed prefixes whose final counts/bounds do not match the manifest.

For JSONL without a strict manifest, syntax and unterminated atomic groups can
be detected, but truncation after a complete valid line cannot be distinguished
from legitimate EOF. Documentation and errors must not claim otherwise.

## 8. Internal hierarchy composition

An internal `MergeSource` may implement the same staged-source protocol and
accept either leaves or other merge nodes. This is valuable for proving that
ordering metadata remains leaf-global and the algorithm is associative.

Hierarchy is a test arrangement, not a public configuration feature:

```text
flat = Merge(A, B, C, D)

hierarchical = Merge(
  Merge(A, B),
  Merge(C, D)
)
```

Both arrangements must produce identical ordered tuples, global sequences,
digests, callbacks, fills, and results. Parent nodes may not assign priority,
source ID, or local sequence.

Public nested manifests remain deferred because they require recursive schema
validation, include-cycle detection, duplicate leaf detection, metadata
inheritance rules, and more complex provenance errors without helping the
initial offline use case.

## 9. Selected-record and full-replay audit invariants

Maintain counters per leaf and globally. The conservation identity applies to
records selected and staged by `DateRange` processing, not automatically to
every record declared by each manifest.

For each source:

```text
read = warmed + replayed + after_end
```

Globally:

```text
sum(source.read) = global_read
sum(source.warmed) = global_warmed
sum(source.replayed) = global_replayed
sum(source.after_end) = global_after_end
global_read = global_warmed + global_replayed + global_after_end
global_market_sequence = 1..global_replayed_groups
```

Only a full-range run may additionally claim exact-once coverage of the child
manifests:

```text
source.read = source.replayed = source.expected_records
source.warmed = source.after_end = 0
```

For bounded ranges, the reader begins at the nearest prior full-snapshot
partition and stops after classifying the staged after-end heads. It neither
reads all earlier history nor scans the remaining tail merely for accounting.

If audit policy counts warm-up groups in `global_market_sequence`, this must be
decided before coding and represented by a schema version. The recommended
choice is two explicit counters:

- `global_input_sequence`: every merged group including warm-up;
- `global_replay_sequence`: only groups delivered inside the requested range.

The run summary must report:

- multi-source dataset ID and manifest verification state;
- per-source identity, format, priority, ownership, counts, and bounds;
- global warm-up/replay/after-end counts;
- first/last global input and replay sequences;
- composite replay digest and its algorithm/version;
- selected-record conservation and full-replay exact-once booleans;
- callback/result counts and failure text using the existing summary policy.

## 10. Failure and shutdown behavior

All validation that can be completed without replay must fail before threads
start. Runtime failures must preserve the existing stop protocol.

Required behavior:

- the first leaf/merger/native/Python exception wins;
- stop closes and wakes both rings and the ready waiter;
- no dispatcher waits for an acknowledgement that can no longer arrive;
- both threads join before the exception reaches Python;
- a staged leaf group is never prepared twice;
- no other leaf advances after failure is committed;
- failed `run_summary.json` retains counters reached before stop;
- global market, input, replay, and dispatch sequence overflow fails before
  wraparound.

## 11. Implementation phases

### Phase 0 — activation and contract freeze

- [x] Record the concrete multi-source dataset/use case.
- [x] Confirm that generic merger work is outside mandatory HW4 scope.
- [x] Confirm disjoint `instrument_id` ownership is sufficient.
- [x] Decide whether strict multi-source mode accepts L2 manifests, wrapped
      JSONL, or both.
- [x] Freeze `MarketGroupKey`, provenance fields, sequence domains, and digest
      encoding.
- [x] Write and review the flat manifest JSON Schema.
- [x] Define migration compatibility for existing `source_sequence` fields.

Exit criterion: architecture and public-contract review has no unresolved
ordering, ownership, provenance, or warm-up question.

### Phase 1 — provenance and leaf adapters

- [x] Add fixed-width `SourceId` and `SourcePriority` aliases.
- [x] Add source ID/local sequence/global market sequence without repurposing
      existing provenance.
- [x] Extend quote/trade signals and fill results with trigger source ID.
- [x] Adapt L2 leaves to expose immutable group keys and ownership; wrapped
      JSONL leaves remain deferred.
- [x] Preserve current two-phase buffer lifetime.
- [x] Add manifest-derived integrity and expected-bound metadata.

Exit criterion: each leaf passes standalone lifecycle, provenance, corruption,
and exact-count tests without changing single-source results.

### Phase 2 — flat `NWayMarketMerger`

- [x] Validate unique IDs/priorities and disjoint ownership before replay.
- [x] Reserve fixed head/heap storage from source count.
- [x] Prefetch one atomic group per leaf.
- [x] Implement lexicographic `MarketGroupKey` selection.
- [x] Advance only the acknowledged prior winner.
- [x] Assign checked global input/replay sequences.
- [x] Forward preparation only to the winning leaf.
- [x] Reject key mutation during preparation.

Exit criterion: flat merge matches a pre-merged oracle for all unit fixtures.
The implementation reserves head/heap capacity; a zero-allocation claim still
requires a dedicated allocation test or benchmark.

### Phase 3 — global warm-up and scheduler integration

- [x] Route every selected pre-range warm-up group through the same merger.
- [x] Apply warm-up state without callbacks, matching, deliveries, or results.
- [x] Transfer staged in-range heads into normal scheduling without reread.
- [x] Keep Scheduler market/new/cancel priority unchanged.
- [x] Verify span lifetime through `processed_seq` acknowledgement.
- [x] Propagate stop and exceptions through merger and leaves.

Exit criterion: warm-up plus replay is byte-equivalent to one full pre-merged
reference stream followed by range filtering.

### Phase 4 — audit, Python, and documentation

- [x] Add per-source/global counters and conservation checks.
- [x] Implement the composite provenance digest.
- [x] Extend `run_summary.json` with versioned multi-source audit fields.
- [x] Expose provenance additions in Python callbacks/results where required.
- [x] Document manifest preparation, limitations, and diagnostic commands.
- [x] Update architecture and traceability only for behavior actually merged.

Exit criterion: an operator can prove which source records were warmed or
replayed and which leaf record triggered every fill.

### Phase 5 — internal hierarchy proof and hardening

- [ ] Compose internal merge nodes without parent ordering metadata.
- [ ] Prove flat/hierarchy equivalence on fixed and randomized fixtures.
- [x] Run 20-fold determinism checks.
- [x] Run ASan and TSan suites.
- [ ] Measure throughput, fixed startup validation, and peak RSS.
- [x] Obtain independent adversarial QA and close all P0/P1 findings.

Exit criterion: internal hierarchy changes neither ordering nor results, and
the branch satisfies repository quality gates.

## 12. Test matrix

### Unit tests

| Case | Required assertion |
|---|---|
| Two interleaved sources | Output follows `MarketGroupKey` exactly. |
| Three or more sources | Heap selection matches a stable sorted oracle. |
| Equal timestamps | Unique priority wins; source ID is deterministic fallback. |
| Atomic MBO group | No other source or command splits raw rows. |
| Empty leaf | Other leaves replay normally. |
| Early clean EOF | Replay continues and exact manifest counts match. |
| Timestamp regression | Fail before publishing the invalid group. |
| Local-sequence regression | Fail without assigning a global sequence. |
| Duplicate ID/priority | Fail before threads start. |
| Overlapping instrument ownership | Fail before leaf replay. |
| Preparation changes timestamp | Fail immediately. |
| Preparation changes local sequence | Fail immediately. |
| Preparation changes instrument | Fail immediately. |
| Incompatible timestamp semantics | Reject before worker threads start. |
| Global sequence overflow | Fail before wraparound. |
| Detectable truncation | Size/hash/count/bounds mismatch fails closed. |
| Valid JSONL prefix with truncated tail | Strict manifest count/hash detects it. |

### Warm-up and causality tests

| Case | Required assertion |
|---|---|
| Interleaved pre-range groups | Warm-up applies in global chronological order. |
| Boundary timestamp | `event_ts == start` is replayed, not warmed. |
| Warmed snapshot | Seeds state without a false public book callback. |
| First in-range trade | Sees globally warmed state. |
| Market/command equal time | Market delivery wins under existing Scheduler priority. |
| Callback exception | Stop, wake, join, and rethrow without invalidating spans. |

### Provenance and audit tests

| Case | Required assertion |
|---|---|
| Fill from each leaf | `(trigger_source_id, local_sequence)` is preserved. |
| Global sequence | Contiguous and unique in the declared sequence domain. |
| Per-source conservation | `read = warmed + replayed + after_end`. |
| Global conservation | Sums of leaf counters equal global counters. |
| Digest sensitivity | Swapping two provenance tuples changes the digest. |
| Repeat run | Counts, digest, callbacks, fills, and results are identical. |
| Failed run summary | Contains partial counters and the original error. |

### Flat/hierarchy equivalence tests

For identical leaf fixtures, compare:

- ordered leaf keys;
- source IDs and local sequences;
- global input/replay sequences;
- composite digest;
- callback order and payloads;
- fills, order log, positions, and PnL;
- per-source and global audit counters.

The test must cover at least two different internal tree shapes. No nested
manifest is required.

### Performance tests

- N = 2, 4, 8, and 32 leaves;
- disjoint and heavily tied timestamp distributions;
- warmed and steady-state samples reported separately;
- throughput in groups/second and records/second;
- selection latency percentiles;
- zero steady-state allocation after initialization;
- fixed manifest/hash validation time reported separately;
- peak RSS proportional to leaf heads plus source buffers, not total rows.

## 13. Acceptance criteria

Criteria 1–11, 13–14, and 16 describe the delivered restricted flat behavior.
Criterion 12 is the future hierarchy-composition gate. Criterion 15 is a
repository merge-readiness gate and cannot be satisfied by implementation
alone.

1. Current single-source JSONL and L2 results remain unchanged.
2. Every leaf stages groups without mutating state before it wins.
3. No group or span is released before `processed_seq` acknowledgement.
4. Every warm-up and replay group passes through one global ordering rule.
5. Leaf ownership sets are disjoint and validated before replay.
6. The merger preserves source ID and raw local sequence through fill results.
7. Global market and Scheduler dispatch sequences are distinct and checked.
8. Market-versus-command ordering remains the current Scheduler contract.
9. Strict manifests detect byte/hash/count/bounds inconsistencies.
10. Per-source/global selected-record conservation holds, and full-replay
    exact-once is claimed only when read/replayed counts equal manifest totals.
11. The composite digest changes when provenance order changes.
12. Flat and internal hierarchical arrangements are byte-equivalent.
13. Repeated runs are deterministic at least 20 times.
14. Clean build, native/Python tests, ASan, TSan, formatting, and diff checks
    pass.
15. Independent QA has no unresolved P0/P1 finding.
16. Architecture and traceability are updated without claiming multi-venue or
    mixed-book support.

The screenshot may label only the explicitly restricted flat scope as
`IMPLEMENTED`; hierarchy remains `DEFERRED` until criterion 12 and the relevant
remaining Phase 5 work pass. Merge readiness separately requires criterion 15.
Neither status describes a universal
exchange-feed merger.

## 14. Resolved decisions

- The public configuration is a strict flat `cmf-multi-source-v1` manifest.
- Schema v1 accepts only `cmf-l2-parquet-cache-v1` children, exactly one
  `instrument_id` per child, and disjoint ownership across children.
- Parent integer fields are checked against the exact signed or unsigned C++
  target range before conversion; narrowing wraparound is forbidden.
- Every child must expose the same typed `timestamp_semantics`; mixed
  exchange/receive/local-receive timelines fail before worker threads start.
- Source priority is positive and unique. Lower numeric priority wins equal
  timestamps; source ID and local group sequence remain deterministic
  fail-safe tie-breakers.
- `global_input_sequence` counts selected warm-up, replay, and staged after-end
  groups. `global_market_sequence` counts only delivered in-range groups.
- Existing local `source_sequence` remains provenance and is never replaced by
  a global sequence.
- Parent SHA-256 protects each child manifest; the child manifest continues to
  protect its selected replay-cache files and declare all cache metadata.
- `prepare_for_dispatch()` may populate payload buffers but must preserve the
  complete winning identity.
- DateRange warm-up begins at the nearest prior full-snapshot partition rather
  than at the beginning of the dataset.
- Ranged accounting proves conservation of selected records. Exact-once over
  manifest totals is reported only for a full replay.

## 15. Remaining gaps

- Internal hierarchy composition and flat/hierarchy equivalence are deferred.
- Wrapped JSONL leaves, overlapping ownership, consolidated multi-venue books,
  mixed L2/L3 state, and live/plugin sources are unsupported.
- A dedicated N-way allocation/scaling benchmark for 2/4/8/32 leaves and peak
  RSS measurement has not been added. Reserved heap capacity alone is not
  evidence of zero steady-state allocation.
- Global sequence overflow has a checked code path but no practical end-to-end
  exhaustion test.
- Multi-source-specific callback-exception and failed-summary cases are not
  separately duplicated; the shared Scheduler shutdown path is covered by the
  existing callback matrix.
- Independent adversarial QA completed without unresolved P0/P1 findings. Team
  Lead base verification remains a repository merge gate.

Any future request for multiple venues on one instrument, mixed L2/L3 state,
or live feeds requires a separate architecture proposal.

## 16. Verification log

Implementation evidence currently includes:

- native tests for two/three-source ordering, equal-time priority, empty leaf,
  globally ordered selected warm-up, prepare-before-advance, source chronology
  regressions, digest sensitivity, and timestamp/local-sequence/instrument
  mutation during preparation;
- Python integration for parent hash/count/priority validation, incompatible
  timestamp semantics, unsigned boundary matrices, callback/fill provenance,
  audit fields, and 20 repeated deterministic runs;
- clean Release CMake/CTest, complete Python tests, ASan/UBSan, TSan, Ruff,
  JSON Schema parsing, and `git diff --check` in the developer handoff;
- a separate clang-format gate remains environment-dependent and must be
  reported honestly when the executable is unavailable;
- existing Scheduler and price-cross benchmark results, which do not replace
  the missing N-way scaling/RSS benchmark.

The exact commands and measured results belong in the final developer handoff
for the revision being reviewed. Independent QA reported no unresolved P0/P1;
Team Lead verification of the intended base remains outstanding.
