from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

import pyarrow.parquet as pq
import pytest

from back_tester import (
    BacktestConfig,
    DateRange,
    LiquiditySource,
    Side,
    Strategy,
    backtest,
)


ROOT = Path(__file__).resolve().parents[2]


def convert_fixture(tmp_path: Path, *extra: str) -> Path:
    output = tmp_path / "normalized"
    command = [
        sys.executable,
        str(ROOT / "scripts" / "convert_l2_csv.py"),
        str(ROOT / "test" / "data" / "l2_csv"),
        str(output),
        "--dataset-id",
        "tiny-l2",
        "--instrument-id",
        "7",
        "--symbol",
        "TEST",
        "--source-provider",
        "fixture",
        "--venue",
        "fixture",
        "--timestamp-unit",
        "us",
        "--timestamp-semantics",
        "exchange",
        "--price-scale",
        "1",
        "--tick-size-ticks",
        "1",
        "--contract-multiplier",
        "1",
        "--trade-side-semantics",
        "aggressor",
        "--same-timestamp-policy",
        "snapshot_first",
        "--depth",
        "2",
        "--batch-rows",
        "1",
        *extra,
    ]
    completed = subprocess.run(
        command, cwd=ROOT, check=True, text=True, capture_output=True
    )
    assert completed.stdout.strip() == str(output / "manifest.json")
    return output / "manifest.json"


def test_converter_writes_typed_parquet_cache_and_manifest(tmp_path: Path) -> None:
    manifest_path = convert_fixture(tmp_path)
    manifest = json.loads(manifest_path.read_text())
    assert manifest["format"] == "cmf-l2-parquet-cache-v1"
    assert manifest["same_timestamp_policy"] == "snapshot_first"
    assert manifest["partitions"][0]["snapshot_rows"] == 2
    assert manifest["partitions"][0]["trade_rows"] == 1

    partition = manifest["partitions"][0]
    cache_path = manifest_path.parent / partition["replay_cache"]
    assert partition["replay_cache_bytes"] == cache_path.stat().st_size
    books = pq.read_table(manifest_path.parent / partition["book_parquet"])
    trades = pq.read_table(manifest_path.parent / partition["trade_parquet"])
    assert books.column("merged_sequence").to_pylist() == [1, 3]
    assert books.column("bid_price_ticks_00").to_pylist() == [99, 99]
    assert trades.column("merged_sequence").to_pylist() == [2]
    assert trades.column("side").to_pylist() == [-1]


def test_l2_manifest_replays_through_public_runtime(tmp_path: Path) -> None:
    manifest_path = convert_fixture(tmp_path)

    class Capture(Strategy):
        def __init__(self) -> None:
            super().__init__()
            self.callbacks: list[str] = []
            self.order_id = 0

        def on_book_update(self, update) -> None:
            self.callbacks.append("book")
            if not self.order_id:
                self.order_id = self.submit_limit(
                    update.instrument_id, Side.BUY, 101, 2
                )

        def on_trade(self, trade) -> None:
            self.callbacks.append("trade")

        def on_fill(self, fill) -> None:
            self.callbacks.append("fill")

    strategy = Capture()
    result = backtest.run(
        strategy,
        str(manifest_path),
        DateRange(),
        BacktestConfig(order_latency_ns=1, book_depth=2),
    )

    assert strategy.callbacks == ["book", "fill", "trade", "book"]
    assert len(result.fills_df) == 1
    assert result.fills_df.iloc[0].price_ticks == 100
    assert result.fills_df.iloc[0].liquidity_source == LiquiditySource.TRADE_CROSS.value
    assert result.fills_df.iloc[0].trigger_source_sequence == 2


def test_l2_range_warms_snapshot_without_false_book_callback(tmp_path: Path) -> None:
    manifest_path = convert_fixture(tmp_path)

    class Capture(Strategy):
        def __init__(self) -> None:
            super().__init__()
            self.callbacks: list[str] = []

        def on_trade(self, trade) -> None:
            self.callbacks.append("trade")
            self.submit_limit(trade.instrument_id, Side.BUY, 101, 1)

        def on_fill(self, fill) -> None:
            self.callbacks.append("fill")

        def on_book_update(self, update) -> None:
            self.callbacks.append("book")

    strategy = Capture()
    result = backtest.run(
        strategy,
        str(manifest_path),
        DateRange(start_ts_ns=150_000, end_ts_ns=200_000),
        BacktestConfig(order_latency_ns=1, book_depth=2),
    )

    assert strategy.callbacks == ["trade", "fill", "book"]
    assert result.fills_df["price_ticks"].tolist() == [101]
    assert result.fills_df["liquidity_source"].tolist() == [1]


def test_l2_reader_rejects_cache_trailing_bytes(tmp_path: Path) -> None:
    manifest_path = convert_fixture(tmp_path)
    manifest = json.loads(manifest_path.read_text())
    cache_path = manifest_path.parent / manifest["partitions"][0]["replay_cache"]
    with cache_path.open("ab") as stream:
        stream.write(b"corrupt")

    with pytest.raises(RuntimeError, match="trailing bytes"):
        backtest.run(Strategy(), str(manifest_path), DateRange())


def test_converter_rejects_unverified_metadata_without_explicit_mode(
    tmp_path: Path,
) -> None:
    output = tmp_path / "bad"
    command = [
        sys.executable,
        str(ROOT / "scripts" / "convert_l2_csv.py"),
        str(ROOT / "test" / "data" / "l2_csv"),
        str(output),
        "--dataset-id",
        "bad",
        "--instrument-id",
        "7",
        "--timestamp-unit",
        "us",
        "--timestamp-semantics",
        "unknown",
        "--price-scale",
        "1",
        "--tick-size-ticks",
        "1",
        "--contract-multiplier",
        "1",
        "--trade-side-semantics",
        "unknown",
        "--same-timestamp-policy",
        "snapshot_first",
        "--depth",
        "2",
    ]
    completed = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
    assert completed.returncode != 0
    assert "--allow-unverified-metadata" in completed.stderr
    assert not output.exists()
