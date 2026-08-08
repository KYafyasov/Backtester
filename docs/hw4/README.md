# Homework 4 documentation

This directory contains the final documentation for the deterministic options
backtesting engine delivered for Homework 4.

## Start here

- [`GETTING_STARTED.md`](GETTING_STARTED.md) is the practical onboarding guide
  for installation, configuration, strategy development, input data, testing,
  and troubleshooting.
- [`architecture/README.md`](architecture/README.md) explains the implemented
  architecture, runtime flow, module boundaries, matching, concurrency, Python
  API, results, performance, and verification.
- [`architecture/11_requirements_traceability.md`](architecture/11_requirements_traceability.md)
  maps the written assignment and original diagram to concrete code and tests.
- [`architecture/12_l2_manifest_cache_validation.md`](architecture/12_l2_manifest_cache_validation.md)
  explains the implemented L2 manifest trust boundary, pre-thread cache-index
  reconciliation, range selection, hash checks, and diagnostic-data policy.
- [`architecture/13_restricted_nway_event_merger.md`](architecture/13_restricted_nway_event_merger.md)
  documents the implemented strict flat merge of disjoint L2 child datasets,
  provenance fields, globally ordered selected warm-up, and ranged/full-replay
  audit checks.
- [`proposals/01_price_cross_full_fill_traceability.md`](proposals/01_price_cross_full_fill_traceability.md)
  traces the accepted and implemented trade-or-quote price-cross full-fill
  model from the repository baseline through the candidate code, tests,
  contracts, and benchmark evidence.
- [`proposals/03_generic_nway_event_merger_plan.md`](proposals/03_generic_nway_event_merger_plan.md)
  records the implementation plan and distinguishes the delivered restricted
  flat merger from deferred hierarchy and overlapping-feed work.
- [`source/01_homework_4_assignment.md`](source/01_homework_4_assignment.md) is
  the normalized assignment text.
- [`source/02_original_big_picture_mermaid.md`](source/02_original_big_picture_mermaid.md)
  is the Mermaid transcription of the original big-picture diagram.
- [`../../README.md`](../../README.md) contains installation, build, test,
  example, CLI, and benchmark commands.

The removed development plans, task briefs, handoff templates, status boards,
and agent prompts were implementation-process artifacts. They are not part of
the final system documentation.

## Documentation rule

Architecture pages describe the current implementation, not a proposed design.
When behavior changes, update the relevant architecture page, traceability row,
and executable tests in the same change.
