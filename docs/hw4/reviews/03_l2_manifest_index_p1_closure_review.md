# L2 manifest-index P1 closure review

## Technical summary

The manifest-index P1 from the preceding review is **closed** on
`feat/l2-parquet-data-pipeline` at base/HEAD
`e738bc349ff6f9fe031d65718e059bd9cf3d4e86` plus the current uncommitted L2
diff. No P0 or P1 finding remains in the reviewed manifest/cache
reconciliation scope.

Before `DateRange` pruning, `L2CacheReader` now scans every schema-v1 replay
cache and independently derives record-kind counts, exact timestamp and
merged-sequence bounds, UTC partition date, and global sequence continuity
starting at one. It reconciles partition, source-file, and conversion totals
with the manifest. Only then does it select the replay range and hash selected
cache files.

The code is ready to proceed to the repository's final merge process, subject
to the remaining non-code gates in `AGENTS.md`: create a reviewable commit,
verify the intended base/rebase state, and obtain Team Lead approval. Existing
P2 items remain tracked but do not block this focused P1 closure.

## The P1 invariants now fail closed

| Requirement | Status | Production evidence | Independent evidence |
|---|---|---|---|
| Validate all caches before range pruning | **Implemented** | `validate_manifest_cache_index()` precedes `select_cache_paths()` in the reader constructor | A false index outside the requested `DateRange` was rejected rather than pruned |
| Partition kind counts match cache | **Implemented** | `scan_cache_index()` counts snapshot/trade records and compares both declared values | Adversarial false `snapshot_rows` and `trade_rows` tests passed |
| Exact timestamp bounds match cache | **Implemented** | First/last scanned timestamps are compared with declared bounds | False minima/maxima were rejected; the former silent empty replay no longer reproduces |
| Exact sequence bounds match cache | **Implemented** | First/last scanned merged sequences are compared with declared bounds | False sequence bounds were rejected |
| UTC partition date matches records | **Implemented** | Ordered endpoints must both map to the declared UTC date | A false date was rejected |
| Global sequence starts at 1 and is contiguous | **Implemented** | Preflight requires first sequence 1 and every next sequence to be previous plus one across files | An independently injected sequence gap was rejected |
| Source/conversion totals match caches | **Implemented** | Kind totals are compared with `source_files[].rows`; their sum with `conversion_stats.rows` | False source and aggregate totals were rejected |
| Failure precedes scheduler threads | **Implemented** | `L2CacheScheduledSource` constructs and validates its reader before `execute_source()` constructs `SchedulerRuntime` | Independent corrupt-index/cache probes failed with zero strategy callbacks |

## Scope and method

The review compared the current uncommitted implementation with the accepted
L2 proposal, runtime/file contract, implementation brief, and HW4
architecture. The closure criterion was narrow: false index metadata must not
silently remove or alter replay selection, and failure must precede scheduler
execution.

Evidence came from production construction order, the committed Python
adversarial suite, independent temporary fixtures, and the real six-partition
diagnostic dataset under `data_normalized/l2_parquet`. No web sources or
external architectural assumptions were used. No production, test, fixture,
or generated-data file was modified by this review.

## Verification and robustness checks

### Clean native build

```bash
uv run --no-sync cmake -S . \
  -B /tmp/back-tester-l2-review.mwJaJZ \
  -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
uv run --no-sync cmake --build \
  /tmp/back-tester-l2-review.mwJaJZ -j 4
uv run --no-sync ctest \
  --test-dir /tmp/back-tester-l2-review.mwJaJZ --output-on-failure
```

Clean configure/build succeeded and **6/6 CTest registrations passed** in
0.95 seconds. Only the existing Apple archive and duplicate-library linker
warnings appeared.

### Python integration suite

```bash
uv sync --reinstall-package back-tester-cmf
uv run --no-sync pytest -q
```

The extension rebuilt successfully and **45 tests passed** in 18.46 seconds,
including adversarial false counts, bounds, date, and aggregate totals.

### Independent adversarial probes

Two additional temporary-dataset probes were run outside the committed suite:

1. Manifest timestamp bounds were changed so the partition appeared outside a
   disjoint requested range. The run failed with `partition index does not
   match replay cache`; no strategy callback was invoked.
2. A cache sequence was changed to create a gap. The run failed with
   `globally ordered and contiguous`; no strategy callback was invoked.

The first probe directly covers the former P1 failure: manifest metadata can
no longer cause a real cache file to be silently omitted by pruning.

### Real-data replay regression

```bash
/usr/bin/time -lp uv run --no-sync python examples/run_local_l2.py
/usr/bin/time -lp uv run --no-sync python \
  examples/run_local_l2.py --duration-seconds 518400
```

| Run | Book callbacks | Trade callbacks | Fills | Rejects | Wall time | Peak RSS |
|---|---:|---:|---:|---:|---:|---:|
| 10-second smoke | 16 | 364 | 1 | 0 | 5.16 s | 92.9 MiB |
| Full six-day replay | 1,033,084 | 21,864,989 | 1 | 0 | 234.04 s | 139.7 MiB |

Both runs retained `dataset_id=hw_previous_tasks_unverified`,
`verified_metadata=False`, final position one, and three order-log rows. The
full callback/fill totals exactly match the earlier independent run. The full
wall-time increase from 229.90 to 234.04 seconds is 4.14 seconds and agrees
with the new fixed preflight scan.

### Formatting and diff hygiene

```bash
uv run --no-sync ruff check scripts/convert_l2_csv.py \
  examples/run_local_l2.py python/tests/test_l2_pipeline.py \
  python/tests/test_runtime.py
uv run --no-sync ruff format --check scripts/convert_l2_csv.py \
  examples/run_local_l2.py python/tests/test_l2_pipeline.py \
  python/tests/test_runtime.py
/Users/sergei/.cache/pre-commit/reporpy3vzc6/py_env-python3.12/bin/clang-format \
  --dry-run --Werror src/core/BacktestConfig.hpp \
  src/market/L2CacheReader.cpp src/market/L2CacheReader.hpp \
  src/python/bindings.cpp src/results/ResultRecorder.cpp \
  src/results/ResultRecorder.hpp src/runtime/BacktestRuntime.cpp
git diff --check
```

Ruff, Ruff format, scoped clang-format, and diff whitespace checks passed.

## Remaining limitations and risk classification

No P0/P1 remains in this review's scope. Previously identified P2 or
external-data limitations remain:

- schema-v1 preflight reads record headers but skips snapshot payload values;
  reserved flags and tick alignment are still not validated;
- post-write conversion does not independently certify every persisted
  Parquet invariant and source sample;
- preflight is an O(total cache records) fixed cost, measured at roughly four
  seconds for 22.9 million rows on this host;
- real instrument/provider/venue/timestamp/trade-side/tick and multiplier
  facts remain economically unverified;
- commit-level merge-base/rebase and Team Lead checks cannot be completed
  while the L2 stages remain one uncommitted working-tree diff.

None of these recreates the silent manifest-bounds pruning defect.

## Recommendation

Proceed from independent QA to a reviewable commit and Team Lead merge check.
Do not claim economic PnL validity until real metadata is verified. Track
reserved flags, tick alignment, and complete offline Parquet validation as P2
follow-ups without expanding the student project's architecture.

## Review handoff

- **Task and base:** independent revalidation of the manifest-index P1 closure
  on `feat/l2-parquet-data-pipeline`, base/HEAD
  `e738bc349ff6f9fe031d65718e059bd9cf3d4e86` plus the current L2 diff.
- **Files changed by this review:** this closure report and a superseded notice
  in the preceding review; implementation files were not changed.
- **User-visible behavior reviewed:** corrupt counts, bounds, dates, aggregate
  totals, and sequence gaps now fail before range selection and replay.
- **Design choices and assumptions:** a full schema-v1 header scan is accepted
  as a student-project correctness tradeoff; selected-cache hashing and the
  optimistic L2 fill model are unchanged.
- **Build/tests:** clean Release build, 6/6 native tests, extension rebuild, 45
  Python tests, independent probes, real smoke/full replay, and scoped format
  checks passed; exact commands and results are above.
- **Benchmarks:** smoke 5.16 seconds and 92.9 MiB peak RSS; six-day replay
  234.04 seconds and 139.7 MiB peak RSS.
- **Remaining risks:** P2 reserved/tick checks, incomplete offline Parquet
  certification, fixed preflight latency, and unverified real metadata.
- **Suggested QA focus:** preserve corrupt-index cases in CI and next test
  reserved flags/tick alignment without weakening selected-cache hashing.
- **Commit/diff:** no commit created. The tree contains the user's combined
  uncommitted L2 implementation plus these two review-document changes;
  generated Parquet/cache files were not modified.
