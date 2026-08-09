#!/usr/bin/env python3
"""Build a strict flat CMF multi-source manifest from L2 child manifests."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
from typing import Any


def positive_integer(text: str) -> int:
    value = int(text)
    if value <= 0:
        raise argparse.ArgumentTypeError("value must be positive")
    return value


def source_argument(text: str) -> tuple[int, Path]:
    priority_text, separator, path_text = text.partition(":")
    if not separator or not path_text:
        raise argparse.ArgumentTypeError("source must be PRIORITY:PATH")
    return positive_integer(priority_text), Path(path_text).resolve()


def child_entry(
    source_id: int, priority: int, path: Path, output_root: Path
) -> dict[str, Any]:
    try:
        relative = path.relative_to(output_root)
    except ValueError as error:
        raise ValueError(
            f"child manifest must be inside {output_root}: {path}"
        ) from error
    child = json.loads(path.read_text(encoding="utf-8"))
    if child.get("format") != "cmf-l2-parquet-cache-v1":
        raise ValueError(f"unsupported child manifest format: {path}")
    partitions = child.get("partitions")
    if not isinstance(partitions, list) or not partitions:
        raise ValueError(f"child manifest has no partitions: {path}")
    rows = child["conversion_stats"]["rows"]
    return {
        "_timestamp_semantics": child["timestamp_semantics"],
        "source_id": source_id,
        "source_priority": priority,
        "format": "cmf-l2-parquet-cache-v1",
        "path": relative.as_posix(),
        "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
        "bytes": path.stat().st_size,
        "record_count": rows,
        "group_count": rows,
        "min_event_ts_ns": partitions[0]["min_event_ts_ns"],
        "max_event_ts_ns": partitions[-1]["max_event_ts_ns"],
        "min_source_sequence": partitions[0]["min_merged_sequence"],
        "max_source_sequence": partitions[-1]["max_merged_sequence"],
        "instrument_ids": [child["instrument_id"]],
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--dataset-id", required=True)
    parser.add_argument(
        "--source",
        action="append",
        type=source_argument,
        required=True,
        help="unique source priority and child path as PRIORITY:PATH",
    )
    arguments = parser.parse_args()
    if len(arguments.source) < 2:
        parser.error("at least two --source values are required")
    output = arguments.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    priorities = [priority for priority, _ in arguments.source]
    if len(priorities) != len(set(priorities)):
        parser.error("source priorities must be unique")
    sources = [
        child_entry(index, priority, path, output.parent)
        for index, (priority, path) in enumerate(arguments.source, start=1)
    ]
    instruments = [source["instrument_ids"][0] for source in sources]
    if len(instruments) != len(set(instruments)):
        parser.error("child instrument ownership must be disjoint")
    timestamp_semantics = {source["_timestamp_semantics"] for source in sources}
    if len(timestamp_semantics) != 1:
        parser.error("child timestamp semantics must be identical")
    for source in sources:
        del source["_timestamp_semantics"]
    manifest = {
        "format": "cmf-multi-source-v1",
        "manifest_version": 1,
        "dataset_id": arguments.dataset_id,
        "sources": sources,
        "expected_global_group_count": sum(source["group_count"] for source in sources),
    }
    temporary = output.with_name(output.name + ".tmp")
    temporary.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    os.replace(temporary, output)
    print(output)


if __name__ == "__main__":
    main()
