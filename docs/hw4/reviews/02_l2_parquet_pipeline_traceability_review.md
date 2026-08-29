# L2 Parquet pipeline implementation review and traceability

> **Superseded for the manifest-index P1:** the follow-up implementation and
> independent revalidation are recorded in
> [`03_l2_manifest_index_p1_closure_review.md`](03_l2_manifest_index_p1_closure_review.md).
> The P2 findings and broader feature assessment below remain applicable.

## Technical summary

The trust-gap update materially improves the L2 pipeline. Independent QA
confirmed that selected replay-cache SHA-256 values are checked before replay,
same-size corruption is rejected, unverified datasets require explicit runtime
consent, L2 results retain dataset identity and verification state, the
converter no longer downgrades complete metadata merely because an opt-in flag
was supplied, and both equal-time policies affect replay causality as designed.

The real six-day dataset also replayed successfully after the changes:
1,033,084 book callbacks, 21,864,989 trade callbacks, one fill, no rejects, and
final position one. Wall time was 229.90 seconds and peak resident size was
142,737,408 bytes (136.1 MiB) on the review host.

The implementation is nevertheless **not merge-ready**. One P1 trust issue
remains in the new strict manifest path: JSON shape, types, enums, paths, and
local constraints are checked, but manifest-to-cache cross-field consistency
is not. An adversarial manifest with false partition timestamp bounds silently
pruned all cache events that were actually inside the requested `DateRange`.
Incorrect partition and conversion row counts were also accepted. This can
change a backtest without an error even though the selected cache bytes and
their SHA-256 are valid.

Two lower-severity cache-contract checks also remain absent: a rehashed cache
with non-zero reserved flags is accepted, and snapshot prices are not checked
for tick alignment during replay. The already documented offline
persisted-Parquet validation and negative converter matrix remain incomplete.

Verdict by milestone:

| Milestone | Status | Reason |
|---|---|---|
| Trust-gap fixes 2–5 | **Passed** | SHA, explicit consent, result provenance, converter verification derivation, invalid policy rejection, and both tie policies were independently exercised. |
| Trust-gap fix 1: strict manifest | **Partial** | Required fields/types/enums/paths pass, but counts and bounds are not reconciled to cache content. |
| Real-data functional replay | **Passed** | Full six-day Python → pybind11 → C++ replay completed after the changes. |
| Diagnostic research use | **Passed with warning** | Result is visibly marked `verified_metadata=False`; economic metadata remains unresolved. |
| Proposal-complete implementation | **Partial** | Offline persisted validation, complete negative coverage, and some cache semantic checks remain open. |
| Merge-ready under `AGENTS.md` | **No** | Independent review has one unresolved P1 finding; the branch must be fixed and reviewed again. |

## Audit scope and baseline

This re-review compares:

- repository reference baseline
  `c4f4c02916f5a9fb5f2636926fd93cd28af0f46d`;
- requested branch and implementation base
  `feat/l2-parquet-data-pipeline` at
  `e738bc349ff6f9fe031d65718e059bd9cf3d4e86` plus the uncommitted diff;
- the [L2 proposal](../proposals/02_l2_parquet_data_pipeline.md),
  [implementation brief](../tasks/01_l2_pipeline_implementation.md),
  [file/runtime contract](../contracts/01_l2_python_pipeline_contract.md), and
  accepted architecture;
- production code, Python/native tests, the real manifest and six generated
  partitions under `data_normalized/l2_parquet`.

The review treats this as a single-instrument, optimistic snapshot-based
student backtester. L3 queue reconstruction, market impact, distributed
services, databases, Greeks, and a production options-risk platform are not
counted as gaps.

Status terms:

| Status | Meaning |
|---|---|
| **Implemented** | Production behavior exists and direct executable evidence passed. |
| **Partial** | The main path exists, but a promised invariant, test, or supported case is missing. |
| **Not implemented** | No production behavior or direct evidence was found. |
| **Blocked by data facts** | Code works diagnostically, but external market metadata is required for economic interpretation. |

## Trust-gap fixes 1–5

| Fix | Review result | Code evidence | Independent QA evidence | Remaining gap |
|---|---|---|---|---|
| 1. Strict manifest v1 | **Partial / P1** | `parse_metadata()`, typed helpers, field allowlists, enum/path/SHA checks in `src/market/L2CacheReader.cpp` | Missing fields, invalid policy, verified/unknown inconsistency, and malformed hashes are rejected by tests or probes | Counts, timestamp bounds, sequence bounds, dates, and aggregate statistics are not reconciled to cache/partition content. False bounds silently changed range selection. `created_at="not-a-date"` was accepted. |
| 2. Selected cache SHA-256 before threads | **Implemented** | `verify_cache_hashes(select_cache_paths(...))` runs in `L2CacheReader` construction | Wrong selected hash and same-size byte corruption fail with `SHA-256 mismatch`; a bad hash outside the selected range is not read | Cache integrity depends on the mutable manifest; semantic validation still matters after a matching hash. |
| 3. Explicit unverified consent | **Implemented** | `BacktestConfig.allow_unverified_metadata`, runtime and reader checks | Unverified fixture fails without opt-in and replays with `allow_unverified_metadata=True` | Real market facts remain unknown; opt-in permits diagnostics only. |
| 4. Result provenance | **Implemented for frozen contract** | `DatasetMetadata` in frozen results and Python `dataset_id` / `verified_metadata` properties | L2 result reports `tiny-l2/True` or `False`; JSONL reports `None/None` | Schema version, semantics, merge policy, and selected hashes are not exposed; current contract requires only the two implemented properties. |
| 5. Converter verification and tie policies | **Implemented** | `metadata_is_verified()` plus frozen `merged_sequence` policy | Passing `--allow-unverified-metadata` no longer downgrades complete metadata. Causal fixture filled at quote 101 for `snapshot_first` and trade 100 for `trade_first` | The committed test checks sequence assignment for both policies, not the causal fill distinction exercised by this review. |

## Feature-to-code traceability matrix

| Feature / requirement | Status | Code evidence | Current evidence | Remaining gap |
|---|---|---|---|---|
| Real interleaved CSV header | **Implemented** | `expected_lob_header()` emits ask/bid pairs per depth | Real 102-column header and fixture conversion pass | Negative reordered/extra header coverage remains thin. |
| Preserve raw CSV and atomic publication | **Implemented, test gap** | temporary sibling root, cleanup, `os.replace()` | Full generated dataset is separate; raw hashes match prior audit | Interrupted-publication behavior is not automated. |
| Bounded streaming conversion | **Partial** | generators and batched `PartitionWriter` | 22,901,679 rows converted at recorded 90,138 rows/s | Converter peak RSS and restart/resume evidence are absent. |
| Exact numeric normalization | **Implemented, limited negative tests** | exact decimal/timestamp/quantity functions | Prior full scan and endpoint samples matched source values | Overflow, fractional quantity, precision, and tick-alignment fixtures remain incomplete. |
| Deterministic equal-time merge | **Implemented** | `merged_rows()` and mandatory enum | Both sequence policies and different causal fill sources passed | Add causal behavior to the committed suite. |
| Daily canonical Parquet | **Implemented; self-certification partial** | typed schemas and `PartitionWriter` | Six daily partitions and prior independent full Parquet scan passed | Converter does not independently rescan all persisted invariants before publish. |
| Versioned replay cache | **Partial** | cache writer/reader, header and chronology checks | All six caches replayed to EOF and selected hashes matched | Reserved flags are ignored; snapshot tick alignment and per-kind source-row chronology are not replay-validated. |
| Strict runtime manifest/index | **Partial / P1** | field/type/enum/path validation and partition ordering | Local schema violations are rejected | Wrong counts are accepted; wrong bounds can silently omit valid events. |
| Explicit diagnostic mode | **Implemented** | config flag, runtime gate, result metadata | Real result reports dataset ID and `False` | No economic claims until instrument/provider/venue/timestamp/side/tick/multiplier are confirmed. |
| Runtime auto-selection | **Implemented** | manifest detection and `run_backtest()` branch | Fixture, smoke, and full real replay pass | Malformed JSON with no detectable L2 format can still fall through to JSONL diagnostics. |
| Date-range pruning and warm-up | **Implemented behavior; automated gap** | `select_cache_paths()` and pre-start replay | Independent two-day fixture produced trade then book without a false warm-up callback | False manifest bounds can corrupt pruning; cross-day case is not committed as a test. |
| Honest atomic L2 state | **Implemented** | dedicated `HistoricalL2Book` replacement | Native suite and real replay pass | No L3 execution fidelity is claimed. |
| Trade/quote-cross matching | **Implemented** | scheduled source and `SimulatedLOB` | Tie-policy causal fixture and real fill pass | Full-fill model remains intentionally optimistic. |
| MBO JSONL compatibility | **Implemented** | unchanged JSONL path | Full Python suite and CTest pass; JSONL metadata is `None` | No regression observed. |
| Repeated-run determinism | **Partial but improved** | immutable merge sequence and deterministic scheduler | 20/20 real 10-second smoke outputs had one identical SHA-256 | This compares observable smoke output, not byte-equivalent full result columns across 20 full-range runs. |
| Failure shutdown/lifetime | **Partial** | shared scheduler stop/join design | native sanitizer suite and existing Python exception tests pass | No L2-specific callback exception during corrupted mid-record/cross-partition replay. |
| Full replay performance | **Measured** | native cache path and public example | 229.90 s, about 99.6k source events/s, 136.1 MiB peak RSS | Cold/warm split, allocation count, and row-group tuning remain absent. |
| Multi-instrument L2 manifest | **Not implemented** | one instrument per manifest | Real dataset has one diagnostic instrument | Acceptable deferred limitation for this project. |

## Proposal validation matrix

| ID | Status after revalidation | Evidence and gap |
|---|---|---|
| `L2D-VAL-01` | **Implemented; negative coverage partial** | Real/fixture headers pass; full reordered/extra matrix is absent. |
| `L2D-VAL-02` | **Converter implemented; replay partial** | Converter enforces source rows; cache reader decodes but does not validate per-kind source-row monotonicity. |
| `L2D-VAL-03` | **Implemented; boundary coverage partial** | Full timestamps and chronology pass; overflow fixtures remain sparse. |
| `L2D-VAL-04` | **Converter implemented; replay partial** | Exact prices pass conversion; a rehashed cache with unticked snapshot prices was accepted. |
| `L2D-VAL-05` | **Implemented; negative coverage partial** | Positive quantities are validated, but the planned converter matrix is incomplete. |
| `L2D-VAL-06` | **Implemented** | `HistoricalL2Book` rejects unordered snapshot levels. |
| `L2D-VAL-07` | **Implemented** | Locked/crossed snapshots are rejected by historical L2 state. |
| `L2D-VAL-08` | **Implemented** | Runtime validates trade side encoding. |
| `L2D-VAL-09` | **Implemented** | Enum is strict; both policies produce deterministic, causally different expected fills. |
| `L2D-VAL-10` | **Implemented / data blocked** | Verification state is validated, gated, and returned; real facts remain unknown. |
| `L2D-VAL-11` | **Partial** | Manifest schema/local constraints and cache header are checked; full Parquet persisted validation remains offline-incomplete. |
| `L2D-VAL-12` | **Externally verified; implementation partial** | Real counts matched in the earlier full scan, but adversarial manifest row counts are accepted. |
| `L2D-VAL-13` | **Externally verified; implementation partial / P1** | Real bounds/date matched earlier; adversarial false bounds silently pruned real cache rows. |
| `L2D-VAL-14` | **Externally verified; converter partial** | Real global order passed prior scan; full post-write scan is not built into conversion. |
| `L2D-VAL-15` | **Externally sampled; converter incomplete** | Endpoint source/Parquet samples matched; no deterministic built-in sampling validator. |
| `L2D-VAL-16` | **Implemented for selected cache** | Runtime rejects wrong selected SHA and same-size corruption before replay threads. |
| `L2D-VAL-17` | **Code present; test missing** | Temporary-root publication is sound; interruption test is absent. |

## Prioritized findings

### P1 — manifest bounds and counts are trusted without reconciliation

`parse_metadata()` validates field presence, types, allowed values, and local
partition ordering. It does not prove that a partition's declared row counts,
timestamp/sequence bounds, or date describe the referenced cache.

Independent probes demonstrated both failure modes:

- changing `snapshot_rows`, `trade_rows`, and `conversion_stats.rows` to false
  values still replayed all fixture events;
- moving the declared timestamp bounds outside an actual event range caused a
  `DateRange` replay to return zero callbacks without any error.

The second case is P1 because it silently changes the backtest data set. SHA
verification does not help: the cache bytes are unchanged and correctly
hashed; the false index metadata determines that the file is never selected.

Minimal fix: make the cache header/footer carry record-kind counts and exact
timestamp/sequence bounds, validate those fields against the manifest before
range pruning, and compare aggregate/source counts once. Alternatively scan
the necessary cache metadata, but avoid a second full hot-path decode. Add
focused tests for false counts, false bounds/date, gaps/overlaps, and aggregate
statistics.

### P2 — replay cache semantic validation is incomplete

The reader consumes the reserved 16-bit record field but discards it, despite
the contract requiring zero. Snapshot prices/quantities are checked for
positivity/order/spread by `HistoricalL2Book`, but price tick alignment is not
checked. Rehashing these deliberately modified caches and updating the
manifest allowed both to replay.

Reject non-zero reserved flags and enforce tick alignment for snapshot and
trade prices at ingestion. These are simple numeric checks outside matching
and do not require a new abstraction.

### P2 — the converter still does not independently certify every artifact

The contract requires reopening and validating persisted Parquet/cache counts,
bounds, ordering, dates, hashes, and deterministic source samples before
publication. The current writer reopens Parquet schema and derives most
manifest facts from its own counters, but does not execute the complete
independent matrix. The prior review externally confirmed the current real
dataset; that does not protect future conversions.

Keep the validator offline and small. A full ordering-column scan plus bounded
source samples and cache-to-EOF reconciliation is sufficient for the homework.

### P2 — automated coverage trails the independent QA evidence

The branch now has valuable tests for wrong SHA, same-size corruption,
unverified consent/result marking, invalid policy, and both merge sequences.
Independent QA additionally passed causal tie behavior, cross-day warm-up,
20 repeated real smokes, full real replay, and sanitizer suites. Those cases
should be converted into small committed fixtures where practical.

Highest-value missing tests are false manifest counts/bounds, reserved flags,
tick alignment, interrupted publication, L2 callback-exception shutdown,
mid-record truncation, and normalized result equality across repeated L2 runs.

### P2 — reader allocation and scalar-I/O costs remain measurable

`read_record()` assigns `event = L2InputEvent{}`, discarding snapshot vector
capacity, and performs many scalar stream reads. The full run is usable for a
student project, but 229.90 seconds for six days leaves straightforward room
for improvement.

After correctness gates, clear/reuse the level vectors and decode one bounded
record buffer. Benchmark allocations and one day before/after. Do not add
Arrow C++, custom allocators, mmap, or SIMD without evidence.

### P2 — raw source-row provenance is still dropped from public results

The cache and `L2InputEvent` retain `source_row`, but callbacks and fill results
expose the merged sequence. Manual Parquet lookup can recover raw provenance,
but debugging requires an external join. This is lower priority than manifest
correctness; add a small optional audit surface only if a consumer needs it.

## Minimal architecture recommendation

Keep Option B, the dedicated `HistoricalL2Book`, one manifest per local
instrument, and the existing scheduler/engine boundary. Do not add databases,
services, Arrow C++, or a generic source plugin framework.

Recommended sequence:

1. close manifest-to-cache counts/bounds/date reconciliation and test range
   pruning against false metadata;
2. enforce reserved-zero and tick alignment at cache ingestion;
3. add the converter's independent post-write validator;
4. promote causal tie, cross-day, shutdown, and deterministic-result probes to
   automated fixtures;
5. reuse reader buffers and measure one-day allocations/performance;
6. confirm real instrument metadata before interpreting PnL.

## Verification executed in this re-review

All commands were run from the repository root on macOS arm64.

```bash
uv run cmake --build build-release -j 4
uv sync --reinstall-package back-tester-cmf
uv run --no-sync pytest -q
uv run --no-sync ctest --test-dir build-release --output-on-failure
```

Results: Release build and extension rebuild succeeded; **36 Python tests
passed** in 16.21 seconds; **6/6 CTest registrations passed** in 0.60 seconds.
Only the existing Apple archive/duplicate-library linker warnings appeared.

```bash
/usr/bin/time -lp uv run --no-sync python examples/run_local_l2.py
```

Result: 16 book, 364 trade, one fill, zero rejects, position one,
`dataset_id=hw_previous_tasks_unverified`, `verified_metadata=False`; 1.61
seconds and 96,927,744-byte maximum RSS.

```bash
/usr/bin/time -lp uv run --no-sync python \
  examples/run_local_l2.py --duration-seconds 518400
```

Result: 1,033,084 book, 21,864,989 trade, one fill, zero rejects, position one;
229.90 seconds, approximately 99.6k source events/s, and 142,737,408-byte
maximum RSS.

```bash
uv run --no-sync cmake --build build-asan -j 4
uv run --no-sync ctest --test-dir build-asan --output-on-failure
uv run --no-sync cmake --build build-tsan -j 4
uv run --no-sync ctest --test-dir build-tsan --output-on-failure
```

Results: ASan/UBSan **6/6** in 2.98 seconds; TSan **6/6** in 5.72 seconds.
These builds cover the native suite, not the pybind11/full-data replay.

Additional independent probes:

- selected wrong SHA and same-size corruption were rejected;
- a wrong hash outside the selected range was not hashed;
- unverified metadata failed without opt-in and returned `False` with opt-in;
- complete metadata remained verified when the converter opt-in flag was set;
- `snapshot_first` caused quote-cross at 101 and `trade_first` caused
  trade-cross at 100 on the same logical fixture;
- a two-day fixture warmed the prior snapshot and emitted no false book
  callback for the first in-range trade;
- 20 real 10-second smoke outputs produced one identical SHA-256;
- false counts and invalid `created_at` were accepted;
- false bounds silently pruned actual in-range cache events;
- rehashed non-zero reserved flags and unticked snapshot prices were accepted.

```bash
uv run --no-sync ruff check scripts/convert_l2_csv.py \
  examples/run_local_l2.py python/tests/test_l2_pipeline.py \
  python/tests/test_runtime.py
uv run --no-sync ruff format --check scripts/convert_l2_csv.py \
  examples/run_local_l2.py python/tests/test_l2_pipeline.py \
  python/tests/test_runtime.py
/Users/sergei/.cache/pre-commit/reporpy3vzc6/py_env-python3.12/bin/clang-format \
  --dry-run --Werror src/core/BacktestConfig.hpp src/market/L2CacheReader.cpp \
  src/market/L2CacheReader.hpp src/python/bindings.cpp \
  src/results/ResultRecorder.cpp src/results/ResultRecorder.hpp \
  src/runtime/BacktestRuntime.cpp
git diff --check
```

Results: Ruff passed, four Python files were formatted, scoped clang-format
passed, and `git diff --check` passed.

Not run: full Python replay under sanitizers, 20 full six-day result-column
comparisons, converter peak RSS, cold/warm scan split, allocation count,
row-group tuning, or interruption injection.

## Acceptance recommendation and QA focus

The branch is suitable for explicit diagnostic use and the previous SHA,
unverified-consent, and provenance P1 findings are closed. Do not merge yet:
fix the manifest index reconciliation P1 and repeat independent review.

QA should next focus on:

- false partition counts/bounds/date and range-pruning behavior;
- reserved cache flags, tick alignment, source-row chronology, and mid-record
  truncation;
- converter failure before atomic publication;
- L2 callback exception shutdown and result lifetime;
- automated normalized 20-run L2 equality;
- confirming economic metadata before any PnL interpretation.

## Review handoff

- **Task and base:** independent revalidation of trust-gap fixes 1–5 on
  `feat/l2-parquet-data-pipeline`, base
  `e738bc349ff6f9fe031d65718e059bd9cf3d4e86`.
- **Files changed by this review:** this report only. User production, test,
  fixture, MANIFEST, and generated-data changes were preserved.
- **User-visible behavior reviewed:** strict manifest parsing, selected cache
  SHA, explicit unverified consent, L2 result provenance, correct converter
  verification state, and both equal-time policies.
- **Design choices and assumptions:** single-instrument L2 manifest,
  snapshot-based optimistic fills, explicit diagnostic use for unverified real
  data, and no production-scale architecture expansion.
- **Build/tests:** exact commands and results are listed above.
- **Benchmarks:** full replay 229.90 seconds, about 99.6k source events/s,
  136.1 MiB peak RSS; smoke 1.61 seconds.
- **Remaining risks:** one P1 manifest bounds/count reconciliation gap; P2
  cache semantics, offline validation, automation, and performance gaps;
  unresolved real market metadata.
- **Suggested QA focus:** false manifest index metadata, malformed but rehashed
  cache semantics, publication interruption, L2 shutdown, and deterministic
  result equality.
- **Commit/diff:** no commit created. The working tree is the user's described
  implementation/doc/test diff plus this rewritten review; generated
  Parquet/cache files were not modified.
