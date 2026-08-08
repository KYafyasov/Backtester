#!/usr/bin/env python3
"""Convert chronological top-25 CSV snapshots/trades to Parquet + L2 cache."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import shutil
import struct
import tempfile
import time
from dataclasses import dataclass, field
from datetime import UTC, datetime
from pathlib import Path
from typing import Any, Iterator, TextIO

import pyarrow as pa
import pyarrow.parquet as pq


SCHEMA_VERSION = 1
CACHE_MAGIC = b"CMFL2C01"
CACHE_HEADER = struct.Struct("<8sHHqqqqQ")
RECORD_HEADER = struct.Struct("<BbHqQQ")
TRADE_PAYLOAD = struct.Struct("<qq")


class ConversionError(RuntimeError):
    """Input or generated-data contract violation."""


@dataclass(frozen=True)
class Config:
    input_dir: Path
    output_dir: Path
    dataset_id: str
    instrument_id: int
    symbol: str | None
    source_provider: str | None
    venue: str | None
    timestamp_unit: str
    timestamp_semantics: str
    timezone: str
    price_scale: int
    tick_size_ticks: int
    contract_multiplier: int
    trade_side_semantics: str
    same_timestamp_policy: str
    depth: int
    batch_rows: int
    allow_unverified_metadata: bool


@dataclass
class SourceRow:
    kind: str
    source_row: int
    event_ts_ns: int
    side: int = 0
    price_ticks: int = 0
    quantity: int = 0
    bids: list[tuple[int, int]] = field(default_factory=list)
    asks: list[tuple[int, int]] = field(default_factory=list)


def exact_scaled_integer(text: str, scale: int, context: str) -> int:
    value = text.strip()
    if not value:
        raise ConversionError(f"{context}: empty numeric value")
    negative = value.startswith("-")
    if value[:1] in {"+", "-"}:
        value = value[1:]
    whole, dot, fraction = value.partition(".")
    if (
        not whole
        or not whole.isdigit()
        or (dot and (not fraction or not fraction.isdigit()))
    ):
        raise ConversionError(f"{context}: invalid decimal {text!r}")
    numerator = int(whole + fraction)
    if negative:
        numerator = -numerator
    denominator = 10 ** len(fraction)
    scaled, remainder = divmod(numerator * scale, denominator)
    if remainder:
        raise ConversionError(f"{context}: {text!r} is not exactly representable")
    if not -(2**63) <= scaled < 2**63:
        raise ConversionError(f"{context}: int64 overflow")
    return scaled


def positive_quantity(text: str, context: str) -> int:
    value = exact_scaled_integer(text, 1, context)
    if value <= 0:
        raise ConversionError(f"{context}: quantity must be positive")
    return value


def timestamp_ns(text: str, unit: str, context: str) -> int:
    try:
        value = int(text)
    except ValueError as error:
        raise ConversionError(f"{context}: timestamp must be an integer") from error
    factors = {"s": 1_000_000_000, "ms": 1_000_000, "us": 1_000, "ns": 1}
    result = value * factors[unit]
    if not -(2**63) <= result < 2**63:
        raise ConversionError(f"{context}: nanosecond timestamp overflow")
    return result


def expected_lob_header(depth: int) -> list[str]:
    columns = ["", "local_timestamp"]
    for index in range(depth):
        for side in ("asks", "bids"):
            columns.extend((f"{side}[{index}].price", f"{side}[{index}].amount"))
    return columns


def open_rows(path: Path, expected: list[str]) -> tuple[TextIO, csv.DictReader[str]]:
    stream = path.open("r", encoding="utf-8", newline="")
    reader = csv.DictReader(stream)
    if reader.fieldnames != expected:
        stream.close()
        raise ConversionError(
            f"{path}: header mismatch\nexpected={expected!r}\nactual={reader.fieldnames!r}"
        )
    return stream, reader


def snapshot_rows(config: Config) -> Iterator[SourceRow]:
    path = config.input_dir / "lob.csv"
    stream, reader = open_rows(path, expected_lob_header(config.depth))
    previous_source = -1
    previous_ts = -(2**63)
    try:
        for physical_row, row in enumerate(reader, 2):
            context = f"{path}:{physical_row}"
            try:
                source = int(row[""])
            except ValueError as error:
                raise ConversionError(f"{context}: invalid source row") from error
            event_ts = timestamp_ns(
                row["local_timestamp"], config.timestamp_unit, context
            )
            if source <= previous_source or event_ts < previous_ts:
                raise ConversionError(f"{context}: source row or timestamp regression")
            bids: list[tuple[int, int]] = []
            asks: list[tuple[int, int]] = []
            for side, target in (("bids", bids), ("asks", asks)):
                for index in range(config.depth):
                    price = exact_scaled_integer(
                        row[f"{side}[{index}].price"],
                        config.price_scale,
                        f"{context}:{side}[{index}].price",
                    )
                    quantity = positive_quantity(
                        row[f"{side}[{index}].amount"],
                        f"{context}:{side}[{index}].amount",
                    )
                    if price <= 0 or price % config.tick_size_ticks:
                        raise ConversionError(
                            f"{context}: invalid or tick-misaligned {side} price"
                        )
                    target.append((price, quantity))
            if any(a[0] <= b[0] for a, b in zip(bids, bids[1:])):
                raise ConversionError(f"{context}: bids must be strictly descending")
            if any(a[0] >= b[0] for a, b in zip(asks, asks[1:])):
                raise ConversionError(f"{context}: asks must be strictly ascending")
            if bids[0][0] >= asks[0][0]:
                raise ConversionError(f"{context}: locked or crossed snapshot")
            yield SourceRow("snapshot", source, event_ts, bids=bids, asks=asks)
            previous_source, previous_ts = source, event_ts
    finally:
        stream.close()


def trade_rows(config: Config) -> Iterator[SourceRow]:
    path = config.input_dir / "trades.csv"
    stream, reader = open_rows(path, ["", "local_timestamp", "side", "price", "amount"])
    previous_source = -1
    previous_ts = -(2**63)
    try:
        for physical_row, row in enumerate(reader, 2):
            context = f"{path}:{physical_row}"
            try:
                source = int(row[""])
            except ValueError as error:
                raise ConversionError(f"{context}: invalid source row") from error
            event_ts = timestamp_ns(
                row["local_timestamp"], config.timestamp_unit, context
            )
            if source <= previous_source or event_ts < previous_ts:
                raise ConversionError(f"{context}: source row or timestamp regression")
            side_token = row["side"].strip().lower()
            if side_token not in {"buy", "sell"}:
                raise ConversionError(
                    f"{context}: unsupported trade side {side_token!r}"
                )
            price = exact_scaled_integer(
                row["price"], config.price_scale, f"{context}:price"
            )
            quantity = positive_quantity(row["amount"], f"{context}:amount")
            if price <= 0 or price % config.tick_size_ticks:
                raise ConversionError(
                    f"{context}: invalid or tick-misaligned trade price"
                )
            yield SourceRow(
                "trade",
                source,
                event_ts,
                1 if side_token == "buy" else -1,
                price,
                quantity,
            )
            previous_source, previous_ts = source, event_ts
    finally:
        stream.close()


def take(iterator: Iterator[SourceRow]) -> SourceRow | None:
    try:
        return next(iterator)
    except StopIteration:
        return None


def merged_rows(config: Config) -> Iterator[tuple[int, SourceRow]]:
    snapshots = snapshot_rows(config)
    trades = trade_rows(config)
    snapshot = take(snapshots)
    trade = take(trades)
    sequence = 0
    snapshot_priority = 0 if config.same_timestamp_policy == "snapshot_first" else 1
    while snapshot is not None or trade is not None:
        if snapshot is None:
            chosen = trade
            trade = take(trades)
        elif trade is None:
            chosen = snapshot
            snapshot = take(snapshots)
        else:
            snapshot_key = (
                snapshot.event_ts_ns,
                snapshot_priority,
                snapshot.source_row,
            )
            trade_key = (trade.event_ts_ns, 1 - snapshot_priority, trade.source_row)
            if snapshot_key <= trade_key:
                chosen = snapshot
                snapshot = take(snapshots)
            else:
                chosen = trade
                trade = take(trades)
        assert chosen is not None
        sequence += 1
        if sequence >= 2**64:
            raise ConversionError("merged sequence overflow")
        yield sequence, chosen


def snapshot_schema(depth: int) -> pa.Schema:
    fields = [
        pa.field("schema_version", pa.uint16(), False),
        pa.field("instrument_id", pa.int64(), False),
        pa.field("event_ts_ns", pa.int64(), False),
        pa.field("receive_ts_ns", pa.int64(), True),
        pa.field("merged_sequence", pa.uint64(), False),
        pa.field("source_row", pa.uint64(), False),
        pa.field("depth", pa.uint8(), False),
    ]
    for side in ("bid", "ask"):
        for index in range(depth):
            fields.append(
                pa.field(f"{side}_price_ticks_{index:02d}", pa.int64(), False)
            )
            fields.append(pa.field(f"{side}_quantity_{index:02d}", pa.int64(), False))
    return pa.schema(fields)


TRADE_SCHEMA = pa.schema(
    [
        pa.field("schema_version", pa.uint16(), False),
        pa.field("instrument_id", pa.int64(), False),
        pa.field("event_ts_ns", pa.int64(), False),
        pa.field("receive_ts_ns", pa.int64(), True),
        pa.field("merged_sequence", pa.uint64(), False),
        pa.field("source_row", pa.uint64(), False),
        pa.field("side", pa.int8(), False),
        pa.field("price_ticks", pa.int64(), False),
        pa.field("quantity", pa.int64(), False),
    ]
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(4 * 1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


class PartitionWriter:
    def __init__(self, config: Config, root: Path, date: str):
        self.config = config
        self.date = date
        self.directory = (
            root
            / f"schema_version={SCHEMA_VERSION}"
            / f"instrument_id={config.instrument_id}"
            / f"date={date}"
        )
        self.directory.mkdir(parents=True)
        self.snapshot_path = self.directory / "book_snapshots.parquet"
        self.trade_path = self.directory / "trades.parquet"
        self.cache_path = self.directory / "replay.l2cache"
        self.snapshot_writer = pq.ParquetWriter(
            self.snapshot_path,
            snapshot_schema(config.depth),
            compression="zstd",
            write_statistics=True,
        )
        self.trade_writer = pq.ParquetWriter(
            self.trade_path,
            TRADE_SCHEMA,
            compression="zstd",
            write_statistics=True,
        )
        self.cache = self.cache_path.open("w+b")
        self.cache.write(
            CACHE_HEADER.pack(
                CACHE_MAGIC,
                1,
                config.depth,
                config.instrument_id,
                config.price_scale,
                config.tick_size_ticks,
                config.contract_multiplier,
                0,
            )
        )
        self.snapshot_rows: list[dict[str, Any]] = []
        self.trade_rows: list[dict[str, Any]] = []
        self.snapshot_count = 0
        self.trade_count = 0
        self.record_count = 0
        self.min_ts: int | None = None
        self.max_ts: int | None = None
        self.min_sequence: int | None = None
        self.max_sequence: int | None = None

    def append(self, sequence: int, row: SourceRow) -> None:
        base: dict[str, Any] = {
            "schema_version": SCHEMA_VERSION,
            "instrument_id": self.config.instrument_id,
            "event_ts_ns": row.event_ts_ns,
            "receive_ts_ns": (
                row.event_ts_ns
                if self.config.timestamp_semantics in {"receive", "local_receive"}
                else None
            ),
            "merged_sequence": sequence,
            "source_row": row.source_row,
        }
        if row.kind == "snapshot":
            values = base | {"depth": self.config.depth}
            for side, levels in (("bid", row.bids), ("ask", row.asks)):
                for index, (price, quantity) in enumerate(levels):
                    values[f"{side}_price_ticks_{index:02d}"] = price
                    values[f"{side}_quantity_{index:02d}"] = quantity
            self.snapshot_rows.append(values)
            self.cache.write(
                RECORD_HEADER.pack(1, 0, 0, row.event_ts_ns, sequence, row.source_row)
            )
            for bid, ask in zip(row.bids, row.asks):
                self.cache.write(struct.pack("<qqqq", bid[0], bid[1], ask[0], ask[1]))
            self.snapshot_count += 1
        else:
            self.trade_rows.append(
                base
                | {
                    "side": row.side,
                    "price_ticks": row.price_ticks,
                    "quantity": row.quantity,
                }
            )
            self.cache.write(
                RECORD_HEADER.pack(
                    2,
                    row.side,
                    0,
                    row.event_ts_ns,
                    sequence,
                    row.source_row,
                )
            )
            self.cache.write(TRADE_PAYLOAD.pack(row.price_ticks, row.quantity))
            self.trade_count += 1
        self.record_count += 1
        self.min_ts = (
            row.event_ts_ns
            if self.min_ts is None
            else min(self.min_ts, row.event_ts_ns)
        )
        self.max_ts = (
            row.event_ts_ns
            if self.max_ts is None
            else max(self.max_ts, row.event_ts_ns)
        )
        self.min_sequence = sequence if self.min_sequence is None else self.min_sequence
        self.max_sequence = sequence
        if len(self.snapshot_rows) >= self.config.batch_rows:
            self._flush_snapshots()
        if len(self.trade_rows) >= self.config.batch_rows:
            self._flush_trades()

    def _flush_snapshots(self) -> None:
        if self.snapshot_rows:
            self.snapshot_writer.write_table(
                pa.Table.from_pylist(
                    self.snapshot_rows, schema=snapshot_schema(self.config.depth)
                )
            )
            self.snapshot_rows.clear()

    def _flush_trades(self) -> None:
        if self.trade_rows:
            self.trade_writer.write_table(
                pa.Table.from_pylist(self.trade_rows, schema=TRADE_SCHEMA)
            )
            self.trade_rows.clear()

    def close(self, root: Path) -> dict[str, Any]:
        self._flush_snapshots()
        self._flush_trades()
        self.snapshot_writer.close()
        self.trade_writer.close()
        self.cache.seek(CACHE_HEADER.size - 8)
        self.cache.write(struct.pack("<Q", self.record_count))
        self.cache.close()
        # Reopen every generated file and validate schema/count/header.
        snapshot_file = pq.ParquetFile(self.snapshot_path)
        trade_file = pq.ParquetFile(self.trade_path)
        if snapshot_file.schema_arrow != snapshot_schema(self.config.depth):
            raise ConversionError(f"{self.snapshot_path}: generated schema mismatch")
        if trade_file.schema_arrow != TRADE_SCHEMA:
            raise ConversionError(f"{self.trade_path}: generated schema mismatch")
        if snapshot_file.metadata.num_rows != self.snapshot_count:
            raise ConversionError(f"{self.snapshot_path}: generated row-count mismatch")
        if trade_file.metadata.num_rows != self.trade_count:
            raise ConversionError(f"{self.trade_path}: generated row-count mismatch")
        with self.cache_path.open("rb") as stream:
            header = CACHE_HEADER.unpack(stream.read(CACHE_HEADER.size))
        if header[0] != CACHE_MAGIC or header[-1] != self.record_count:
            raise ConversionError(f"{self.cache_path}: generated cache header mismatch")

        def relative(path: Path) -> str:
            return path.relative_to(root).as_posix()

        return {
            "date": self.date,
            "min_event_ts_ns": self.min_ts,
            "max_event_ts_ns": self.max_ts,
            "min_merged_sequence": self.min_sequence,
            "max_merged_sequence": self.max_sequence,
            "snapshot_rows": self.snapshot_count,
            "trade_rows": self.trade_count,
            "book_parquet": relative(self.snapshot_path),
            "book_parquet_sha256": sha256(self.snapshot_path),
            "trade_parquet": relative(self.trade_path),
            "trade_parquet_sha256": sha256(self.trade_path),
            "replay_cache": relative(self.cache_path),
            "replay_cache_bytes": self.cache_path.stat().st_size,
            "replay_cache_sha256": sha256(self.cache_path),
        }


def utc_date(timestamp: int) -> str:
    seconds, nanoseconds = divmod(timestamp, 1_000_000_000)
    del nanoseconds
    try:
        return datetime.fromtimestamp(seconds, UTC).date().isoformat()
    except (OverflowError, OSError, ValueError) as error:
        raise ConversionError(
            f"timestamp {timestamp} cannot be partitioned as UTC"
        ) from error


def metadata_is_verified(config: Config) -> bool:
    return bool(
        config.symbol
        and config.source_provider
        and config.venue
        and config.timestamp_semantics != "unknown"
        and config.trade_side_semantics != "unknown"
    )


def validate_config(config: Config) -> None:
    if (
        config.instrument_id <= 0
        or config.price_scale <= 0
        or config.tick_size_ticks <= 0
    ):
        raise ConversionError(
            "instrument id, price scale, and tick size must be positive"
        )
    if config.contract_multiplier <= 0 or config.depth <= 0 or config.depth > 255:
        raise ConversionError(
            "multiplier and depth must be positive; depth must be <= 255"
        )
    if config.batch_rows <= 0:
        raise ConversionError("batch rows must be positive")
    if config.timezone != "UTC":
        raise ConversionError("version 1 supports UTC partitioning only")
    if not metadata_is_verified(config) and not config.allow_unverified_metadata:
        raise ConversionError(
            "unverified provenance/semantics require --allow-unverified-metadata"
        )
    if config.output_dir.exists():
        raise ConversionError(f"output already exists: {config.output_dir}")


def convert(config: Config) -> Path:
    validate_config(config)
    started = time.perf_counter()
    raw_files = [config.input_dir / "lob.csv", config.input_dir / "trades.csv"]
    for path in raw_files:
        if not path.is_file():
            raise ConversionError(f"missing source file: {path}")
    config.output_dir.parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(
        tempfile.mkdtemp(
            prefix=f".{config.output_dir.name}.tmp-",
            dir=config.output_dir.parent,
        )
    )
    partitions: list[dict[str, Any]] = []
    writer: PartitionWriter | None = None
    try:
        for sequence, row in merged_rows(config):
            date = utc_date(row.event_ts_ns)
            if writer is None or writer.date != date:
                if writer is not None:
                    partitions.append(writer.close(temporary))
                writer = PartitionWriter(config, temporary, date)
            writer.append(sequence, row)
        if writer is not None:
            partitions.append(writer.close(temporary))
        if not partitions:
            raise ConversionError("source dataset is empty")
        total_rows = sum(
            partition["snapshot_rows"] + partition["trade_rows"]
            for partition in partitions
        )
        total_snapshots = sum(partition["snapshot_rows"] for partition in partitions)
        total_trades = sum(partition["trade_rows"] for partition in partitions)
        output_bytes = sum(
            path.stat().st_size for path in temporary.rglob("*") if path.is_file()
        )
        elapsed_seconds = time.perf_counter() - started
        manifest = {
            "format": "cmf-l2-parquet-cache-v1",
            "manifest_version": 1,
            "data_schema_version": SCHEMA_VERSION,
            "dataset_id": config.dataset_id,
            "verified_metadata": metadata_is_verified(config),
            "source_provider": config.source_provider,
            "venue": config.venue,
            "symbol": config.symbol,
            "instrument_id": config.instrument_id,
            "timestamp_unit": config.timestamp_unit,
            "timestamp_semantics": config.timestamp_semantics,
            "timezone": config.timezone,
            "price_scale": config.price_scale,
            "tick_size_ticks": config.tick_size_ticks,
            "contract_multiplier": config.contract_multiplier,
            "book_depth": config.depth,
            "trade_side_semantics": config.trade_side_semantics,
            "same_timestamp_policy": config.same_timestamp_policy,
            "converter_version": "convert_l2_csv.py/v1",
            "created_at": datetime.now(UTC).isoformat(),
            "source_files": [
                {
                    "path": raw_files[0].name,
                    "bytes": raw_files[0].stat().st_size,
                    "rows": total_snapshots,
                    "sha256": sha256(raw_files[0]),
                },
                {
                    "path": raw_files[1].name,
                    "bytes": raw_files[1].stat().st_size,
                    "rows": total_trades,
                    "sha256": sha256(raw_files[1]),
                },
            ],
            "partitions": partitions,
            "conversion_stats": {
                "rows": total_rows,
                "elapsed_seconds": elapsed_seconds,
                "rows_per_second": total_rows / elapsed_seconds,
                "input_bytes": sum(path.stat().st_size for path in raw_files),
                "output_bytes_before_manifest": output_bytes,
            },
        }
        manifest_path = temporary / "manifest.json"
        manifest_path.write_text(
            json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
        )
        os.replace(temporary, config.output_dir)
        return config.output_dir / "manifest.json"
    except BaseException:
        shutil.rmtree(temporary, ignore_errors=True)
        raise


def parse_args() -> Config:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input_dir", type=Path)
    parser.add_argument("output_dir", type=Path)
    parser.add_argument("--dataset-id", required=True)
    parser.add_argument("--instrument-id", required=True, type=int)
    parser.add_argument("--symbol")
    parser.add_argument("--source-provider")
    parser.add_argument("--venue")
    parser.add_argument(
        "--timestamp-unit", choices=("s", "ms", "us", "ns"), required=True
    )
    parser.add_argument(
        "--timestamp-semantics",
        choices=("exchange", "receive", "local_receive", "unknown"),
        required=True,
    )
    parser.add_argument("--timezone", default="UTC")
    parser.add_argument("--price-scale", required=True, type=int)
    parser.add_argument("--tick-size-ticks", required=True, type=int)
    parser.add_argument("--contract-multiplier", required=True, type=int)
    parser.add_argument(
        "--trade-side-semantics",
        choices=("aggressor", "maker", "unknown"),
        required=True,
    )
    parser.add_argument(
        "--same-timestamp-policy",
        choices=("snapshot_first", "trade_first"),
        required=True,
    )
    parser.add_argument("--depth", default=25, type=int)
    parser.add_argument("--batch-rows", default=65_536, type=int)
    parser.add_argument("--allow-unverified-metadata", action="store_true")
    args = parser.parse_args()
    return Config(**vars(args))


if __name__ == "__main__":
    try:
        print(convert(parse_args()))
    except ConversionError as error:
        raise SystemExit(f"conversion failed: {error}") from error
