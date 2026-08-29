"""Smoke-test the unverified local L2 dataset through the public Python API."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import back_tester as bt


DEFAULT_MANIFEST = (
    Path(__file__).resolve().parents[1] / "data_normalized/l2_parquet/manifest.json"
)


class BuyFirstAsk(bt.Strategy):
    """Submit one lot at the first visible ask and record callback counts."""

    def __init__(self) -> None:
        super().__init__()
        self.order_id: int | None = None
        self.callback_counts = {"book": 0, "trade": 0, "fill": 0, "reject": 0}
        self.position_after_fill: int | None = None

    def on_book_update(self, update) -> None:
        self.callback_counts["book"] += 1
        if self.order_id is None and update.asks:
            self.order_id = self.submit_limit(
                update.instrument_id,
                bt.Side.BUY,
                update.asks[0].price,
                1,
            )

    def on_trade(self, trade) -> None:
        self.callback_counts["trade"] += 1

    def on_fill(self, fill) -> None:
        self.callback_counts["fill"] += 1
        self.position_after_fill = self.position(fill.instrument_id).net_quantity

    def on_reject(self, reject) -> None:
        self.callback_counts["reject"] += 1


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", nargs="?", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument(
        "--duration-seconds",
        type=int,
        default=10,
        help="Replay this many seconds from the first partition (default: 10).",
    )
    parser.add_argument(
        "--run-summary",
        type=Path,
        help="Atomically write replay audit counters to this JSON path.",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    if args.duration_seconds <= 0:
        raise SystemExit("--duration-seconds must be positive")

    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    start_ts_ns = manifest["partitions"][0]["min_event_ts_ns"]
    end_ts_ns = start_ts_ns + args.duration_seconds * 1_000_000_000

    strategy = BuyFirstAsk()
    result = bt.backtest.run(
        strategy,
        str(args.manifest),
        bt.DateRange(start_ts_ns=start_ts_ns, end_ts_ns=end_ts_ns),
        bt.BacktestConfig(
            order_latency_ns=1,
            book_depth=1,
            allow_unverified_metadata=True,
        ),
        run_summary_path=(
            str(args.run_summary) if args.run_summary is not None else None
        ),
    )

    print(f"manifest={args.manifest}")
    print(f"dataset_id={result.dataset_id}")
    print(f"verified_metadata={result.verified_metadata}")
    print(f"range_ns=[{start_ts_ns}, {end_ts_ns}]")
    print(f"callbacks={strategy.callback_counts}")
    print(f"fills={len(result.fills_df)}")
    print(f"position_after_fill={strategy.position_after_fill}")
    print(f"order_log_rows={len(result.order_log_df)}")
    if args.run_summary is not None:
        print(f"run_summary={args.run_summary}")


if __name__ == "__main__":
    main()
