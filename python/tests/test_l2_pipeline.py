from __future__ import annotations

import hashlib
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
    FillModel,
    LiquiditySource,
    Side,
    Strategy,
    backtest,
)


ROOT = Path(__file__).resolve().parents[2]


def convert_fixture(
    tmp_path: Path,
    *extra: str,
    input_dir: Path | None = None,
    trade_side_semantics: str = "aggressor",
) -> Path:
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
        trade_side_semantics,
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


def create_multi_source_fixture(tmp_path: Path) -> Path:
    child_manifests: list[Path] = []
    for instrument_id in (7, 8):
        child_root = tmp_path / f"bounds-child-{instrument_id}"
        child_root.mkdir()
        child_manifests.append(
            convert_fixture(
                child_root,
                "--instrument-id",
                str(instrument_id),
                "--dataset-id",
                f"bounds-child-{instrument_id}",
            )
        )
    parent_path = tmp_path / "bounds_multi_source_manifest.json"
    subprocess.run(
        [
            sys.executable,
            str(ROOT / "scripts" / "create_multi_source_manifest.py"),
            str(parent_path),
            "--dataset-id",
            "bounds-fixture",
            "--source",
            f"10:{child_manifests[0]}",
            "--source",
            f"20:{child_manifests[1]}",
        ],
        cwd=ROOT,
        check=True,
        text=True,
        capture_output=True,
    )
    return parent_path


def write_parent_and_run(parent_path: Path, parent: dict) -> None:
    parent_path.write_text(json.dumps(parent), encoding="utf-8")
    backtest.run(Strategy(), str(parent_path), DateRange())


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


def test_queue_aware_l2_rejects_non_aggressor_trade_semantics(
    tmp_path: Path,
) -> None:
    manifest_path = convert_fixture(tmp_path, trade_side_semantics="maker")

    with pytest.raises(
        RuntimeError,
        match="queue-aware L2 replay requires aggressor trade-side semantics",
    ):
        backtest.run(
            Strategy(),
            str(manifest_path),
            DateRange(),
            BacktestConfig(fill_model=FillModel.QUEUE_AWARE),
        )


def test_strict_multi_source_manifest_replays_with_global_provenance(
    tmp_path: Path,
) -> None:
    child_manifests: list[Path] = []
    for instrument_id in (7, 8):
        child_root = tmp_path / f"child-{instrument_id}"
        child_root.mkdir()
        manifest_path = convert_fixture(
            child_root,
            "--instrument-id",
            str(instrument_id),
            "--dataset-id",
            f"tiny-l2-{instrument_id}",
        )
        child_manifests.append(manifest_path)

    parent_path = tmp_path / "multi_source_manifest.json"
    completed = subprocess.run(
        [
            sys.executable,
            str(ROOT / "scripts" / "create_multi_source_manifest.py"),
            str(parent_path),
            "--dataset-id",
            "two-instrument-fixture",
            "--source",
            f"10:{child_manifests[0]}",
            "--source",
            f"20:{child_manifests[1]}",
        ],
        cwd=ROOT,
        check=True,
        text=True,
        capture_output=True,
    )
    assert completed.stdout.strip() == str(parent_path)

    class Capture(Strategy):
        def __init__(self) -> None:
            super().__init__()
            self.order_id = 0
            self.market_order: list[tuple[int, int]] = []
            self.fill_provenance: tuple[int, int, int] | None = None

        def on_book_update(self, update) -> None:
            self.market_order.append((update.source_id, update.global_market_sequence))
            if update.instrument_id == 7 and not self.order_id:
                self.order_id = self.submit_limit(7, Side.BUY, 101, 1)

        def on_trade(self, trade) -> None:
            self.market_order.append((trade.source_id, trade.global_market_sequence))

        def on_fill(self, fill) -> None:
            self.fill_provenance = (
                fill.trigger_source_id,
                fill.trigger_source_sequence,
                fill.trigger_global_market_sequence,
            )

    summary_path = tmp_path / "multi_run_summary.json"
    strategy = Capture()
    result = backtest.run(
        strategy,
        str(parent_path),
        DateRange(),
        BacktestConfig(order_latency_ns=1, book_depth=2),
        run_summary_path=str(summary_path),
    )

    assert strategy.market_order == [
        (1, 1),
        (2, 2),
        (1, 3),
        (2, 4),
        (1, 5),
        (2, 6),
    ]
    assert strategy.fill_provenance == (1, 2, 3)
    assert result.dataset_id == "two-instrument-fixture"
    assert result.fills_df.iloc[0].trigger_source_id == 1
    assert result.fills_df.iloc[0].trigger_global_market_sequence == 3
    summary = json.loads(summary_path.read_text(encoding="utf-8"))
    audit = summary["source_audit"]
    assert audit["input_format"] == "multi_source_l2_cache"
    assert audit["records_read"] == 6
    assert audit["records_replayed"] == 6
    assert audit["multi_source"]["global_input_groups"] == 6
    assert audit["multi_source"]["global_replay_groups"] == 6
    assert audit["multi_source"]["provenance_digest_fnv1a64_v1"]
    assert audit["multi_source"]["selected_records_conservation"] is True
    assert audit["multi_source"]["full_replay_exact_once"] is True
    assert all(
        source["selected_records_conservation"]
        for source in audit["multi_source"]["sources"]
    )
    assert all(
        source["full_replay_exact_once"] for source in audit["multi_source"]["sources"]
    )
    assert {
        source["timestamp_semantics"] for source in audit["multi_source"]["sources"]
    } == {"exchange"}
    expected_digest = audit["multi_source"]["provenance_digest_fnv1a64_v1"]
    for _ in range(19):
        repeated = Capture()
        backtest.run(
            repeated,
            str(parent_path),
            DateRange(),
            BacktestConfig(order_latency_ns=1, book_depth=2),
            run_summary_path=str(summary_path),
        )
        repeated_summary = json.loads(summary_path.read_text(encoding="utf-8"))
        assert repeated.market_order == strategy.market_order
        assert repeated.fill_provenance == strategy.fill_provenance
        assert (
            repeated_summary["source_audit"]["multi_source"][
                "provenance_digest_fnv1a64_v1"
            ]
            == expected_digest
        )

    backtest.run(
        Capture(),
        str(parent_path),
        DateRange(start_ts_ns=150_000, end_ts_ns=150_000),
        BacktestConfig(order_latency_ns=1, book_depth=2),
        run_summary_path=str(summary_path),
    )
    ranged_audit = json.loads(summary_path.read_text(encoding="utf-8"))["source_audit"][
        "multi_source"
    ]
    assert ranged_audit["selected_records_conservation"] is True
    assert ranged_audit["full_replay_exact_once"] is False
    assert all(
        source["selected_records_conservation"] and not source["full_replay_exact_once"]
        for source in ranged_audit["sources"]
    )

    parent = json.loads(parent_path.read_text(encoding="utf-8"))
    duplicate_priority = json.loads(json.dumps(parent))
    duplicate_priority["sources"][1]["source_priority"] = 10
    parent_path.write_text(json.dumps(duplicate_priority), encoding="utf-8")
    with pytest.raises(RuntimeError, match="duplicate source_priority"):
        backtest.run(Capture(), str(parent_path), DateRange())

    wrong_count = json.loads(json.dumps(parent))
    wrong_count["sources"][0]["record_count"] += 1
    parent_path.write_text(json.dumps(wrong_count), encoding="utf-8")
    with pytest.raises(RuntimeError, match="does not match child L2 manifest"):
        backtest.run(Capture(), str(parent_path), DateRange())

    wrong_hash = json.loads(json.dumps(parent))
    wrong_hash["sources"][0]["sha256"] = "0" * 64
    parent_path.write_text(json.dumps(wrong_hash), encoding="utf-8")
    with pytest.raises(RuntimeError, match="source manifest SHA-256 mismatch"):
        backtest.run(Capture(), str(parent_path), DateRange())


def test_multi_source_rejects_incompatible_timestamp_semantics(
    tmp_path: Path,
) -> None:
    exchange_root = tmp_path / "exchange"
    receive_root = tmp_path / "receive"
    exchange_root.mkdir()
    receive_root.mkdir()
    exchange_manifest = convert_fixture(
        exchange_root,
        "--instrument-id",
        "7",
        "--dataset-id",
        "exchange-child",
    )
    receive_manifest = convert_fixture(
        receive_root,
        "--instrument-id",
        "8",
        "--dataset-id",
        "receive-child",
    )
    parent_path = tmp_path / "incompatible_timestamps.json"
    subprocess.run(
        [
            sys.executable,
            str(ROOT / "scripts" / "create_multi_source_manifest.py"),
            str(parent_path),
            "--dataset-id",
            "incompatible-timestamps",
            "--source",
            f"10:{exchange_manifest}",
            "--source",
            f"20:{receive_manifest}",
        ],
        cwd=ROOT,
        check=True,
        text=True,
        capture_output=True,
    )

    receive_child = json.loads(receive_manifest.read_text(encoding="utf-8"))
    receive_child["timestamp_semantics"] = "receive"
    receive_manifest.write_text(json.dumps(receive_child), encoding="utf-8")
    parent = json.loads(parent_path.read_text(encoding="utf-8"))
    parent["sources"][1]["bytes"] = receive_manifest.stat().st_size
    parent["sources"][1]["sha256"] = hashlib.sha256(
        receive_manifest.read_bytes()
    ).hexdigest()
    parent_path.write_text(json.dumps(parent), encoding="utf-8")

    with pytest.raises(RuntimeError, match="incompatible timestamp_semantics"):
        backtest.run(Strategy(), str(parent_path), DateRange())


def test_multi_source_source_identity_unsigned_boundaries(tmp_path: Path) -> None:
    parent_path = create_multi_source_fixture(tmp_path)
    original = json.loads(parent_path.read_text(encoding="utf-8"))
    maximum = (1 << 32) - 1

    for field in ("source_id", "source_priority"):
        valid_maximum = json.loads(json.dumps(original))
        valid_maximum["sources"][0][field] = maximum
        write_parent_and_run(parent_path, valid_maximum)

        zero = json.loads(json.dumps(original))
        zero["sources"][0][field] = 0
        with pytest.raises(RuntimeError, match="ID and priority must be positive"):
            write_parent_and_run(parent_path, zero)

        for invalid in (-1, maximum + 1):
            out_of_range = json.loads(json.dumps(original))
            out_of_range["sources"][0][field] = invalid
            with pytest.raises(
                RuntimeError,
                match=rf"field '{field}' is outside its supported range",
            ):
                write_parent_and_run(parent_path, out_of_range)


def test_multi_source_unsigned_count_size_and_sequence_boundaries(
    tmp_path: Path,
) -> None:
    parent_path = create_multi_source_fixture(tmp_path)
    original = json.loads(parent_path.read_text(encoding="utf-8"))
    uint64_maximum = (1 << 64) - 1
    fields = (
        (None, "expected_global_group_count", uint64_maximum),
        (0, "bytes", uint64_maximum),
        (0, "record_count", uint64_maximum),
        (0, "group_count", uint64_maximum),
        (0, "min_source_sequence", uint64_maximum),
        (0, "max_source_sequence", uint64_maximum),
    )

    for source_index, field, maximum in fields:

        def with_value(value: int) -> dict:
            candidate = json.loads(json.dumps(original))
            target = (
                candidate
                if source_index is None
                else candidate["sources"][source_index]
            )
            target[field] = value
            return candidate

        with pytest.raises(
            RuntimeError,
            match=rf"field '{field}' is outside its supported range",
        ):
            write_parent_and_run(parent_path, with_value(-1))

        with pytest.raises(RuntimeError):
            write_parent_and_run(parent_path, with_value(0))

        with pytest.raises(RuntimeError) as maximum_error:
            write_parent_and_run(parent_path, with_value(maximum))
        assert "outside its supported range" not in str(maximum_error.value)

        with pytest.raises(RuntimeError):
            write_parent_and_run(parent_path, with_value(maximum + 1))


def test_multi_source_manifest_version_unsigned_boundaries(tmp_path: Path) -> None:
    parent_path = create_multi_source_fixture(tmp_path)
    original = json.loads(parent_path.read_text(encoding="utf-8"))
    maximum = (1 << 16) - 1

    for value in (-1, maximum + 1):
        candidate = json.loads(json.dumps(original))
        candidate["manifest_version"] = value
        with pytest.raises(
            RuntimeError,
            match="field 'manifest_version' is outside its supported range",
        ):
            write_parent_and_run(parent_path, candidate)

    for value in (0, maximum):
        candidate = json.loads(json.dumps(original))
        candidate["manifest_version"] = value
        with pytest.raises(RuntimeError, match="unsupported multi-source"):
            write_parent_and_run(parent_path, candidate)


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
