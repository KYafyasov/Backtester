# Global agent rules for Back-tester HW4

These rules apply to every coding, QA, review, and coordination agent working in this repository.

## 1. Read before changing code

Read, in order:

1. `docs/hw4/source/01_homework_4_assignment.md` or the equivalent path where this pack is installed;
2. `docs/hw4/architecture/01_scope_and_decisions.md`;
3. the architecture document for your subsystem;
4. the active task brief;
5. the current implementation and tests in the repository.

Do not use web search to infer repository state or substitute a different architecture. Package managers may resolve declared dependencies, but design decisions must come from this repository and these documents.

## 2. Engineering priorities

In descending order:

1. deterministic correctness and causality;
2. a clean build and repeatable tests;
3. correct ownership, state transitions, and error handling;
4. hot-path performance appropriate for an HFT-school project;
5. clarity and minimal design;
6. optional polish.

This is not a production exchange. Do not add distributed systems, network protocols, databases, generic plugin frameworks, elaborate dependency injection, or speculative abstractions.

## 3. Repository baseline

The reviewed baseline is commit `c4f4c02916f5a9fb5f2636926fd93cd28af0f46d`.

Useful existing pieces:

- `src/common/BasicTypes.hpp`;
- `src/main/MarketDataEvent.*`;
- `src/main/LimitOrderBook.*`;
- `src/main/SimulatedLOB.*`;
- `src/main/LobRouter.*`;
- `docs/SimulatedLOB_design.md`.

Known baseline problems include a missing checked-in Catch2 source, a hard-coded Windows path in `main.cpp`, stale PDM metadata beside `uv.lock`, no pybind11 module, and only a partial one-level SimulatedLOB implementation.

## 4. Coding rules

- Use C++20 and the repository warning policy; warnings are errors.
- Follow the repository formatter. Do not reformat unrelated files.
- Keep prices, timestamps, quantities, and IDs typed and numeric in the hot path.
- Parse JSON strings and timestamps once at ingestion, not during matching.
- Avoid full-book snapshots, strings, shared-pointer churn, and Python object creation in matching/event loops.
- Preserve stable deterministic ordering independent of wall-clock thread scheduling.
- Do not introduce data races or rely on unspecified atomic ordering.
- Public strategy-facing APIs are multi-instrument and always carry `instrument_id`.
- Do not silently catch errors, silently replace duplicate order IDs, or continue after corrupted event ordering.
- Add focused tests with every behavior change.
- Keep changes scoped. No drive-by cleanup or broad rename unless the task explicitly requires it.

## 5. Concurrency rules

- The ready signal is an atomic sequence, not a queue message.
- The dispatcher may prefetch but must not let the consumer process event `N+1` before event `N` is complete.
- A Python exception must request stop, unblock any waiting thread, join cleanly, and then propagate to Python.
- Never call Python without the GIL; never hold the GIL while waiting on the dispatcher or running native loops.
- Never run multiple agents concurrently in the same working tree. Use separate branches/worktrees.

## 6. Required developer handoff

Every developer response must contain:

1. task and base commit;
2. files changed;
3. user-visible behavior implemented;
4. design choices and assumptions;
5. exact build/test commands and results;
6. benchmarks, when relevant;
7. remaining risks or unsupported cases;
8. suggested QA focus;
9. final commit hash or a clean diff description.

Do not claim tests passed unless you ran them and include the command and result.

## 7. Quality gates

A task is not merge-ready until:

- developer tests pass from a clean build;
- independent QA has executed adversarial tests against the branch;
- independent review has no unresolved P0/P1 finding;
- architecture and public contract changes are documented;
- Team Lead verifies that the branch is rebased/merged against the intended base.
