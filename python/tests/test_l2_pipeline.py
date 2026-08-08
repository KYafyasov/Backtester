from __future__ import annotations

import json
import shutil
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


def convert_fixture(tmp_path: Path, *extra: str, input_dir: Path | None = None) -> Path:
    output = tmp_path / "normalized"
    source = input_dir or ROOT / "test" / "data" / "l2_csv"
    command = [
        sys.executable,
        str(ROOT / "scripts" / "convert_l2_csv.py"),
        str(source),
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
    assert manifest["verified_metadata"] is True
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
    summary_path = tmp_path / "run_summary.json"
    result = backtest.run(
        strategy,
        str(manifest_path),
        DateRange(),
        BacktestConfig(order_latency_ns=1, book_depth=2),
        run_summary_path=str(summary_path),
    )

    assert strategy.callbacks == ["book", "fill", "trade", "book"]
    assert len(result.fills_df) == 1
    assert result.fills_df.iloc[0].price_ticks == 100
    assert result.fills_df.iloc[0].liquidity_source == LiquiditySource.TRADE_CROSS.value
    assert result.fills_df.iloc[0].trigger_source_sequence == 2
    assert result.dataset_id == "tiny-l2"
    assert result.verified_metadata is True
    summary = json.loads(summary_path.read_text(encoding="utf-8"))
    assert summary["source_audit"]["manifest_records"] == {
        "total": 3,
        "snapshots": 2,
        "trades": 1,
    }
    assert summary["source_audit"]["records_read"] == 3
    assert summary["source_audit"]["records_replayed"] == 3
    assert summary["source_audit"]["replayed_book_records"] == 2
    assert summary["source_audit"]["replayed_trade_records"] == 1
    assert summary["source_audit"]["first_replayed_sequence"] == 1
    assert summary["source_audit"]["last_replayed_sequence"] == 3
    assert summary["source_audit"]["checks"] == {
        "read_accounting": True,
        "replay_type_accounting": True,
        "trade_callbacks_match_replayed_trades": True,
        "l2_market_deliveries_match_replayed_records": True,
        "sequence_span_matches_replayed_records": True,
        "full_manifest_replay": True,
        "full_manifest_counts_match": True,
    }


def test_converter_flag_does_not_downgrade_verified_metadata(tmp_path: Path) -> None:
    manifest_path = convert_fixture(tmp_path, "--allow-unverified-metadata")
    manifest = json.loads(manifest_path.read_text())

    assert manifest["verified_metadata"] is True


@pytest.mark.parametrize(
    ("policy", "book_sequences", "trade_sequences"),
    [
        ("snapshot_first", [1, 3], [2]),
        ("trade_first", [2, 3], [1]),
    ],
)
def test_tie_policy_controls_equal_timestamp_order(
    tmp_path: Path,
    policy: str,
    book_sequences: list[int],
    trade_sequences: list[int],
) -> None:
    input_dir = tmp_path / "source"
    shutil.copytree(ROOT / "test" / "data" / "l2_csv", input_dir)
    trades_path = input_dir / "trades.csv"
    trades_path.write_text(trades_path.read_text().replace("0,150,", "0,100,"))
    manifest_path = convert_fixture(
        tmp_path,
        "--same-timestamp-policy",
        policy,
        input_dir=input_dir,
    )
    manifest = json.loads(manifest_path.read_text())
    partition = manifest["partitions"][0]
    books = pq.read_table(manifest_path.parent / partition["book_parquet"])
    trades = pq.read_table(manifest_path.parent / partition["trade_parquet"])

    assert manifest["same_timestamp_policy"] == policy
    assert books.column("merged_sequence").to_pylist() == book_sequences
    assert trades.column("merged_sequence").to_pylist() == trade_sequences


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


def test_l2_reader_rejects_cache_hash_mismatch(tmp_path: Path) -> None:
    manifest_path = convert_fixture(tmp_path)
    manifest = json.loads(manifest_path.read_text())
    manifest["partitions"][0]["replay_cache_sha256"] = "0" * 64
    manifest_path.write_text(json.dumps(manifest))

    with pytest.raises(RuntimeError, match="SHA-256 mismatch"):
        backtest.run(Strategy(), str(manifest_path), DateRange())


def test_l2_reader_rejects_same_size_cache_corruption(tmp_path: Path) -> None:
    manifest_path = convert_fixture(tmp_path)
    manifest = json.loads(manifest_path.read_text())
    cache_path = manifest_path.parent / manifest["partitions"][0]["replay_cache"]
    with cache_path.open("r+b") as stream:
        stream.seek(-1, 2)
        original = stream.read(1)
        stream.seek(-1, 2)
        stream.write(bytes([original[0] ^ 0xFF]))

    with pytest.raises(RuntimeError, match="SHA-256 mismatch"):
        backtest.run(Strategy(), str(manifest_path), DateRange())


def test_l2_reader_rejects_invalid_same_timestamp_policy(tmp_path: Path) -> None:
    manifest_path = convert_fixture(tmp_path)
    manifest = json.loads(manifest_path.read_text())
    manifest["same_timestamp_policy"] = "filesystem_order"
    manifest_path.write_text(json.dumps(manifest))

    with pytest.raises(RuntimeError, match="same_timestamp_policy.*unsupported"):
        backtest.run(Strategy(), str(manifest_path), DateRange())


@pytest.mark.parametrize(
    ("field", "value"),
    [
        ("snapshot_rows", 1),
        ("trade_rows", 0),
        ("min_event_ts_ns", 300_000),
        ("max_event_ts_ns", 99_999),
        ("min_merged_sequence", 2),
        ("max_merged_sequence", 2),
        ("date", "1970-01-02"),
    ],
)
def test_l2_reader_rejects_false_partition_index(
    tmp_path: Path, field: str, value: int | str
) -> None:
    manifest_path = convert_fixture(tmp_path)
    manifest = json.loads(manifest_path.read_text())
    manifest["partitions"][0][field] = value
    manifest_path.write_text(json.dumps(manifest))

    with pytest.raises(RuntimeError, match="partition (index|date).*does not match"):
        backtest.run(
            Strategy(),
            str(manifest_path),
            DateRange(start_ts_ns=100_000, end_ts_ns=200_000),
        )


def test_l2_reader_rejects_false_aggregate_counts(tmp_path: Path) -> None:
    manifest_path = convert_fixture(tmp_path)
    manifest = json.loads(manifest_path.read_text())
    manifest["conversion_stats"]["rows"] += 1
    manifest_path.write_text(json.dumps(manifest))

    with pytest.raises(RuntimeError, match="conversion_stats.rows.*does not match"):
        backtest.run(Strategy(), str(manifest_path), DateRange())


def test_l2_reader_rejects_false_source_counts(tmp_path: Path) -> None:
    manifest_path = convert_fixture(tmp_path)
    manifest = json.loads(manifest_path.read_text())
    manifest["source_files"][0]["rows"] += 1
    manifest_path.write_text(json.dumps(manifest))

    with pytest.raises(RuntimeError, match="source row counts.*do not match"):
        backtest.run(Strategy(), str(manifest_path), DateRange())


def test_unverified_l2_requires_opt_in_and_marks_result(tmp_path: Path) -> None:
    manifest_path = convert_fixture(
        tmp_path,
        "--timestamp-semantics",
        "unknown",
        "--allow-unverified-metadata",
    )

    with pytest.raises(RuntimeError, match="allow_unverified_metadata=true"):
        backtest.run(Strategy(), str(manifest_path), DateRange())

    result = backtest.run(
        Strategy(),
        str(manifest_path),
        DateRange(),
        BacktestConfig(allow_unverified_metadata=True),
    )
    assert result.dataset_id == "tiny-l2"
    assert result.verified_metadata is False


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
