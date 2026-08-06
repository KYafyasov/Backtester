#!/usr/bin/env python3
"""Measure end-to-end no-op strategy replay for an L2 dataset manifest."""

from __future__ import annotations

import argparse
import json
import statistics
import time
from pathlib import Path

from back_tester import BacktestConfig, DateRange, Strategy, backtest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--iterations", type=int, default=5)
    args = parser.parse_args()
    if args.iterations <= 0:
        parser.error("--iterations must be positive")

    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    events = sum(
        partition["snapshot_rows"] + partition["trade_rows"]
        for partition in manifest["partitions"]
    )
    config = BacktestConfig(order_latency_ns=1, book_depth=manifest["book_depth"])
    samples: list[float] = []
    for _ in range(args.iterations):
        started = time.perf_counter()
        backtest.run(Strategy(), str(args.manifest), DateRange(), config)
        samples.append(time.perf_counter() - started)

    median = statistics.median(samples)
    print(
        json.dumps(
            {
                "manifest": str(args.manifest),
                "iterations": args.iterations,
                "events_per_iteration": events,
                "median_seconds": median,
                "median_events_per_second": events / median,
                "samples_seconds": samples,
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    main()
