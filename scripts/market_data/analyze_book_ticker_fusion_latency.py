#!/usr/bin/env python3

import argparse
import csv
import json
import sys
from collections import Counter
from itertools import combinations
from pathlib import Path
from typing import Any, Mapping, Sequence

import numpy as np


SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from analyze_book_ticker_latency import book_ticker_dtype, load_book_tickers  # noqa: E402


SUMMARY_PERCENTILES = (1, 5, 10, 25, 50, 75, 90, 95, 99, 99.9)


def fusion_metadata_dtype() -> np.dtype:
    return np.dtype(
        {
            "names": [
                "source_id",
                "symbol_id",
                "record_id",
                "exchange_ns",
                "event_ns",
                "source_local_ns",
                "fusion_publish_ns",
            ],
            "formats": [
                "<i4",
                "<i4",
                "<i8",
                "<i8",
                "<i8",
                "<i8",
                "<i8",
            ],
            "offsets": [0, 4, 8, 16, 24, 32, 40],
            "itemsize": 48,
        }
    )


def load_fusion_metadata(path: Path) -> np.ndarray:
    file_size = path.stat().st_size
    dtype = fusion_metadata_dtype()
    if file_size % dtype.itemsize != 0:
        raise ValueError(
            f"file size {file_size} is not a multiple of FusionMetadataRecord "
            f"size {dtype.itemsize}"
        )
    return np.fromfile(path, dtype=dtype)


def _percentile_key(percentile: float) -> str:
    if float(percentile).is_integer() and int(percentile) < 10:
        return f"p0{int(percentile)}"
    if float(percentile).is_integer():
        return f"p{int(percentile)}"
    return f"p{str(percentile).replace('.', '_')}"


def _percentile_int(values: np.ndarray, percentile: float) -> int:
    return int(round(float(np.percentile(values, percentile))))


def _latency_summary(values: np.ndarray) -> dict[str, int]:
    if len(values) == 0:
        raise ValueError("no latency values")
    summary = {
        "min": int(values.min()),
        "max": int(values.max()),
        "mean": int(round(float(values.mean()))),
    }
    for percentile in SUMMARY_PERCENTILES:
        summary[_percentile_key(percentile)] = _percentile_int(values, percentile)
    return summary


def _combo_name(labels: Sequence[str]) -> str:
    if all(len(label) == 1 for label in labels):
        return "".join(labels)
    return "+".join(labels)


def _filter_records(
    records: np.ndarray, symbol_id: int | None, exchange: int | None
) -> np.ndarray:
    mask = np.ones(len(records), dtype=bool)
    if symbol_id is not None:
        mask &= records["symbol_id"] == symbol_id
    if exchange is not None:
        mask &= records["exchange"] == exchange
    return records[mask]


def _filter_book_records_for_published(
    records: np.ndarray,
    symbol_id: int | None,
    exchange: int | None,
    id_start: int | None,
    id_end: int | None,
) -> np.ndarray:
    mask = np.ones(len(records), dtype=bool)
    if symbol_id is not None:
        mask &= records["symbol_id"] == symbol_id
    if exchange is not None:
        mask &= records["exchange"] == exchange
    if id_start is not None:
        mask &= records["id"] >= id_start
    if id_end is not None:
        mask &= records["id"] <= id_end
    return records[mask]


def _filter_metadata_records(
    records: np.ndarray,
    symbol_id: int | None,
    id_start: int | None,
    id_end: int | None,
) -> np.ndarray:
    mask = np.ones(len(records), dtype=bool)
    if symbol_id is not None:
        mask &= records["symbol_id"] == symbol_id
    if id_start is not None:
        mask &= records["record_id"] >= id_start
    if id_end is not None:
        mask &= records["record_id"] <= id_end
    return records[mask]


def _latency_units(summary_ns: Mapping[str, int]) -> dict[str, float]:
    return {key: value / 1_000.0 for key, value in summary_ns.items()}


def _latency_ms(summary_ns: Mapping[str, int]) -> dict[str, float]:
    return {key: value / 1_000_000.0 for key, value in summary_ns.items()}


def _book_record_summary(records: np.ndarray) -> dict[str, Any]:
    result: dict[str, Any] = {
        "count": int(len(records)),
    }
    if len(records) == 0:
        result.update(
            {
                "min_id": None,
                "max_id": None,
                "negative_count": 0,
                "zero_count": 0,
                "latency_ns": {},
                "latency_us": {},
                "latency_ms": {},
            }
        )
        return result

    latency_ns = records["local_ns"].astype(np.int64) - records["exchange_ns"].astype(np.int64)
    latency_summary = _latency_summary(latency_ns)
    result.update(
        {
            "min_id": int(records["id"].min()),
            "max_id": int(records["id"].max()),
            "negative_count": int(np.count_nonzero(latency_ns < 0)),
            "zero_count": int(np.count_nonzero(latency_ns == 0)),
            "latency_ns": latency_summary,
            "latency_us": _latency_units(latency_summary),
            "latency_ms": _latency_ms(latency_summary),
        }
    )
    return result


def _metadata_latency_summary(records: np.ndarray) -> dict[str, Any]:
    result: dict[str, Any] = {
        "count": int(len(records)),
    }
    if len(records) == 0:
        result.update(
            {
                "winner_counts": {},
                "winner_ratio": {},
                "winner_counts_by_symbol": {},
                "source_latency_ns": {},
                "fusion_latency_ns": {},
                "fusion_hop_ns": {},
            }
        )
        return result

    source_latency_ns = (
        records["source_local_ns"].astype(np.int64)
        - records["exchange_ns"].astype(np.int64)
    )
    fusion_latency_ns = (
        records["fusion_publish_ns"].astype(np.int64)
        - records["exchange_ns"].astype(np.int64)
    )
    fusion_hop_ns = (
        records["fusion_publish_ns"].astype(np.int64)
        - records["source_local_ns"].astype(np.int64)
    )
    winner_counter = Counter(str(int(source_id)) for source_id in records["source_id"])
    winner_counts = dict(sorted(winner_counter.items(), key=lambda item: int(item[0])))
    winner_ratio = {
        source_id: count / float(len(records))
        for source_id, count in winner_counts.items()
    }

    winner_counts_by_symbol: dict[str, dict[str, int]] = {}
    for symbol_id in sorted(int(value) for value in np.unique(records["symbol_id"])):
        mask = records["symbol_id"] == symbol_id
        symbol_counter = Counter(
            str(int(source_id)) for source_id in records["source_id"][mask]
        )
        winner_counts_by_symbol[str(symbol_id)] = dict(
            sorted(symbol_counter.items(), key=lambda item: int(item[0]))
        )

    source_latency_summary = _latency_summary(source_latency_ns)
    fusion_latency_summary = _latency_summary(fusion_latency_ns)
    fusion_hop_summary = _latency_summary(fusion_hop_ns)
    result.update(
        {
            "winner_counts": winner_counts,
            "winner_ratio": winner_ratio,
            "winner_counts_by_symbol": winner_counts_by_symbol,
            "negative_source_latency_count": int(np.count_nonzero(source_latency_ns < 0)),
            "negative_fusion_latency_count": int(np.count_nonzero(fusion_latency_ns < 0)),
            "negative_fusion_hop_count": int(np.count_nonzero(fusion_hop_ns < 0)),
            "source_latency_ns": source_latency_summary,
            "source_latency_us": _latency_units(source_latency_summary),
            "source_latency_ms": _latency_ms(source_latency_summary),
            "fusion_latency_ns": fusion_latency_summary,
            "fusion_latency_us": _latency_units(fusion_latency_summary),
            "fusion_latency_ms": _latency_ms(fusion_latency_summary),
            "fusion_hop_ns": fusion_hop_summary,
            "fusion_hop_us": _latency_units(fusion_hop_summary),
            "fusion_hop_ms": _latency_ms(fusion_hop_summary),
        }
    )
    return result


def _fusion_metadata_alignment(
    fusion_records: np.ndarray, metadata_records: np.ndarray
) -> dict[str, int]:
    compared_count = min(len(fusion_records), len(metadata_records))
    mismatch_count = 0
    if compared_count > 0:
        fusion_window = fusion_records[:compared_count]
        metadata_window = metadata_records[:compared_count]
        mismatch = (
            (fusion_window["symbol_id"] != metadata_window["symbol_id"])
            | (fusion_window["id"] != metadata_window["record_id"])
            | (fusion_window["exchange_ns"] != metadata_window["exchange_ns"])
            | (fusion_window["local_ns"] != metadata_window["fusion_publish_ns"])
        )
        mismatch_count = int(np.count_nonzero(mismatch))

    return {
        "fusion_count": int(len(fusion_records)),
        "metadata_count": int(len(metadata_records)),
        "compared_count": int(compared_count),
        "mismatch_count": mismatch_count,
        "fusion_without_metadata_count": max(int(len(fusion_records) - compared_count), 0),
        "metadata_without_fusion_count": max(int(len(metadata_records) - compared_count), 0),
    }


def _source_identity_set(records: np.ndarray) -> set[tuple[int, int, int, int]]:
    return {
        (
            int(record["symbol_id"]),
            int(record["id"]),
            int(record["exchange_ns"]),
            int(record["local_ns"]),
        )
        for record in records
    }


def _source_metadata_alignment(
    source_records_by_id: Mapping[int, np.ndarray], metadata_records: np.ndarray
) -> dict[str, Any]:
    identities_by_source = {
        int(source_id): _source_identity_set(records)
        for source_id, records in source_records_by_id.items()
    }
    missing_by_source: Counter[int] = Counter()
    matched_count = 0
    missing_count = 0
    unknown_source_id_count = 0
    for record in metadata_records:
        source_id = int(record["source_id"])
        identities = identities_by_source.get(source_id)
        if identities is None:
            unknown_source_id_count += 1
            missing_count += 1
            missing_by_source[source_id] += 1
            continue
        key = (
            int(record["symbol_id"]),
            int(record["record_id"]),
            int(record["exchange_ns"]),
            int(record["source_local_ns"]),
        )
        if key in identities:
            matched_count += 1
        else:
            missing_count += 1
            missing_by_source[source_id] += 1

    return {
        "metadata_count": int(len(metadata_records)),
        "matched_count": int(matched_count),
        "missing_count": int(missing_count),
        "unknown_source_id_count": int(unknown_source_id_count),
        "missing_by_source": {
            str(source_id): int(count)
            for source_id, count in sorted(missing_by_source.items())
        },
    }


def _top_metadata_hop_outliers(
    metadata_records: np.ndarray, top_n: int
) -> list[dict[str, Any]]:
    if top_n <= 0 or len(metadata_records) == 0:
        return []
    fusion_hop_ns = (
        metadata_records["fusion_publish_ns"].astype(np.int64)
        - metadata_records["source_local_ns"].astype(np.int64)
    )
    source_latency_ns = (
        metadata_records["source_local_ns"].astype(np.int64)
        - metadata_records["exchange_ns"].astype(np.int64)
    )
    fusion_latency_ns = (
        metadata_records["fusion_publish_ns"].astype(np.int64)
        - metadata_records["exchange_ns"].astype(np.int64)
    )
    limit = min(top_n, len(metadata_records))
    order = np.argsort(fusion_hop_ns)[-limit:][::-1]
    rows: list[dict[str, Any]] = []
    for index in order:
        record = metadata_records[index]
        rows.append(
            {
                "index": int(index),
                "source_id": int(record["source_id"]),
                "symbol_id": int(record["symbol_id"]),
                "id": int(record["record_id"]),
                "exchange_ns": int(record["exchange_ns"]),
                "source_local_ns": int(record["source_local_ns"]),
                "fusion_publish_ns": int(record["fusion_publish_ns"]),
                "source_latency_ns": int(source_latency_ns[index]),
                "fusion_latency_ns": int(fusion_latency_ns[index]),
                "fusion_hop_ns": int(fusion_hop_ns[index]),
                "fusion_hop_us": int(fusion_hop_ns[index]) / 1_000.0,
            }
        )
    return rows


def _feed_id_bounds(label: str, records: np.ndarray) -> dict[str, int]:
    if len(records) == 0:
        raise ValueError(f"feed {label} has no BookTicker records after filters")
    return {
        "min_id": int(records["id"].min()),
        "max_id": int(records["id"].max()),
        "raw_count": int(len(records)),
    }


def _min_latency_by_id(
    records: np.ndarray, start_id: int, end_id: int
) -> tuple[dict[int, int], dict[str, int]]:
    mask = (records["id"] >= start_id) & (records["id"] <= end_id)
    window_records = records[mask]
    latency_ns = (
        window_records["local_ns"].astype(np.int64)
        - window_records["exchange_ns"].astype(np.int64)
    )

    latency_by_id: dict[int, int] = {}
    for record_id, latency in zip(window_records["id"], latency_ns):
        key = int(record_id)
        value = int(latency)
        current = latency_by_id.get(key)
        if current is None or value < current:
            latency_by_id[key] = value

    stats = {
        "window_record_count": int(len(window_records)),
        "unique_id_count": int(len(latency_by_id)),
        "duplicate_id_count": int(len(window_records) - len(latency_by_id)),
        "negative_record_count": int(np.count_nonzero(latency_ns < 0)),
        "zero_record_count": int(np.count_nonzero(latency_ns == 0)),
    }
    return latency_by_id, stats


def _top_outliers(
    combo_name: str,
    labels: Sequence[str],
    combined_latency_by_id: Mapping[int, int],
    best_source_by_id: Mapping[int, str],
    feed_latency_by_label: Mapping[str, Mapping[int, int]],
    top_n: int,
) -> list[dict[str, Any]]:
    if top_n <= 0 or not combined_latency_by_id:
        return []

    ids = np.fromiter(combined_latency_by_id.keys(), dtype=np.int64)
    latencies = np.fromiter(combined_latency_by_id.values(), dtype=np.int64)
    limit = min(top_n, len(latencies))
    order = np.argsort(latencies)[-limit:][::-1]

    rows: list[dict[str, Any]] = []
    for index in order:
        record_id = int(ids[index])
        latency_ns = int(latencies[index])
        available_latency_ns = {
            label: int(feed_latency_by_label[label][record_id])
            for label in labels
            if record_id in feed_latency_by_label[label]
        }
        rows.append(
            {
                "combo": combo_name,
                "id": record_id,
                "latency_ns": latency_ns,
                "latency_us": latency_ns / 1_000.0,
                "latency_ms": latency_ns / 1_000_000.0,
                "best_source": best_source_by_id[record_id],
                "available_latency_ns": available_latency_ns,
            }
        )
    return rows


def _combine_latency_maps(
    combo_labels: Sequence[str],
    feed_latency_by_label: Mapping[str, Mapping[int, int]],
    interval_id_count: int,
    top_n: int,
) -> dict[str, Any]:
    combined_latency_by_id: dict[int, int] = {}
    best_source_by_id: dict[int, str] = {}
    for label in combo_labels:
        for record_id, latency_ns in feed_latency_by_label[label].items():
            current = combined_latency_by_id.get(record_id)
            if current is None or latency_ns < current:
                combined_latency_by_id[record_id] = latency_ns
                best_source_by_id[record_id] = label

    combo = _combo_name(combo_labels)
    observed_id_count = len(combined_latency_by_id)
    missing_id_count = max(interval_id_count - observed_id_count, 0)
    latencies = np.fromiter(combined_latency_by_id.values(), dtype=np.int64)
    best_source_counts = dict(sorted(Counter(best_source_by_id.values()).items()))

    result: dict[str, Any] = {
        "labels": list(combo_labels),
        "size": len(combo_labels),
        "observed_id_count": int(observed_id_count),
        "missing_id_count": int(missing_id_count),
        "coverage_ratio": (
            float(observed_id_count) / float(interval_id_count) if interval_id_count > 0 else 0.0
        ),
        "negative_count": int(np.count_nonzero(latencies < 0)),
        "zero_count": int(np.count_nonzero(latencies == 0)),
        "best_source_counts": best_source_counts,
        "latency_ns": _latency_summary(latencies) if observed_id_count else {},
        "top_outliers": _top_outliers(
            combo,
            combo_labels,
            combined_latency_by_id,
            best_source_by_id,
            feed_latency_by_label,
            top_n,
        ),
    }
    result["latency_us"] = {
        key: value / 1_000.0 for key, value in result["latency_ns"].items()
    }
    result["latency_ms"] = {
        key: value / 1_000_000.0 for key, value in result["latency_ns"].items()
    }
    return result


def _attach_p99_improvement(combinations_by_name: dict[str, dict[str, Any]]) -> None:
    for combo_name, combo_summary in combinations_by_name.items():
        labels = combo_summary["labels"]
        if len(labels) == 1 or not combo_summary["latency_ns"]:
            combo_summary["p99_improvement_vs_best_member_ns"] = None
            combo_summary["p99_improvement_vs_best_member_pct"] = None
            continue

        member_p99 = [
            combinations_by_name[label]["latency_ns"]["p99"]
            for label in labels
            if combinations_by_name[label]["latency_ns"]
        ]
        if not member_p99:
            combo_summary["p99_improvement_vs_best_member_ns"] = None
            combo_summary["p99_improvement_vs_best_member_pct"] = None
            continue

        best_member_p99 = min(member_p99)
        combo_p99 = combo_summary["latency_ns"]["p99"]
        improvement_ns = best_member_p99 - combo_p99
        combo_summary["p99_improvement_vs_best_member_ns"] = int(improvement_ns)
        combo_summary["p99_improvement_vs_best_member_pct"] = (
            float(improvement_ns) / float(best_member_p99) * 100.0
            if best_member_p99 != 0
            else None
        )


def analyze_fusion_latency(
    records_by_label: Mapping[str, np.ndarray],
    top_n: int = 20,
    symbol_id: int | None = None,
    exchange: int | None = None,
    id_start: int | None = None,
    id_end: int | None = None,
) -> dict[str, Any]:
    if len(records_by_label) != 4:
        raise ValueError(f"expected exactly 4 feeds, got {len(records_by_label)}")

    labels = list(records_by_label.keys())
    filtered_records_by_label = {
        label: _filter_records(records, symbol_id, exchange)
        for label, records in records_by_label.items()
    }
    feed_bounds = {
        label: _feed_id_bounds(label, records)
        for label, records in filtered_records_by_label.items()
    }

    auto_start_id = max(bounds["min_id"] for bounds in feed_bounds.values())
    auto_end_id = min(bounds["max_id"] for bounds in feed_bounds.values())
    start_id = auto_start_id if id_start is None else max(auto_start_id, id_start)
    end_id = auto_end_id if id_end is None else min(auto_end_id, id_end)
    if start_id > end_id:
        raise ValueError(
            f"no common id interval: auto=[{auto_start_id}, {auto_end_id}], "
            f"requested=[{id_start}, {id_end}]"
        )

    interval_id_count = int(end_id - start_id + 1)
    feed_latency_by_label: dict[str, dict[int, int]] = {}
    feeds_summary: dict[str, dict[str, Any]] = {}
    for label in labels:
        latency_by_id, id_stats = _min_latency_by_id(
            filtered_records_by_label[label], start_id, end_id
        )
        feed_latency_by_label[label] = latency_by_id
        feeds_summary[label] = {
            **feed_bounds[label],
            **id_stats,
        }

    combinations_by_name: dict[str, dict[str, Any]] = {}
    for size in range(1, len(labels) + 1):
        for combo_labels in combinations(labels, size):
            combo_name = _combo_name(combo_labels)
            combinations_by_name[combo_name] = _combine_latency_maps(
                combo_labels,
                feed_latency_by_label,
                interval_id_count,
                top_n,
            )
    _attach_p99_improvement(combinations_by_name)

    return {
        "input_labels": labels,
        "filters": {
            "symbol_id": symbol_id,
            "exchange": exchange,
            "id_start": id_start,
            "id_end": id_end,
        },
        "global_id_interval": {
            "start_id": int(start_id),
            "end_id": int(end_id),
            "id_count": interval_id_count,
        },
        "feeds": feeds_summary,
        "combinations": combinations_by_name,
    }


def analyze_published_fusion_latency(
    source_records_by_id: Mapping[int, np.ndarray],
    fusion_records: np.ndarray,
    metadata_records: np.ndarray,
    top_n: int = 20,
    symbol_id: int | None = None,
    exchange: int | None = None,
    id_start: int | None = None,
    id_end: int | None = None,
) -> dict[str, Any]:
    if not source_records_by_id:
        raise ValueError("expected at least one source BookTicker input")

    normalized_sources = {
        int(source_id): _filter_book_records_for_published(
            records, symbol_id, exchange, id_start, id_end
        )
        for source_id, records in source_records_by_id.items()
    }
    filtered_fusion_records = _filter_book_records_for_published(
        fusion_records, symbol_id, exchange, id_start, id_end
    )
    filtered_metadata_records = _filter_metadata_records(
        metadata_records, symbol_id, id_start, id_end
    )

    source_ids = sorted(normalized_sources)
    sources_summary = {
        str(source_id): _book_record_summary(normalized_sources[source_id])
        for source_id in source_ids
    }

    return {
        "mode": "published_fusion",
        "source_ids": source_ids,
        "filters": {
            "symbol_id": symbol_id,
            "exchange": exchange,
            "id_start": id_start,
            "id_end": id_end,
        },
        "sources": sources_summary,
        "fusion": _book_record_summary(filtered_fusion_records),
        "metadata": _metadata_latency_summary(filtered_metadata_records),
        "fusion_metadata_alignment": _fusion_metadata_alignment(
            filtered_fusion_records, filtered_metadata_records
        ),
        "source_metadata_alignment": _source_metadata_alignment(
            normalized_sources, filtered_metadata_records
        ),
        "top_fusion_hop_outliers": _top_metadata_hop_outliers(
            filtered_metadata_records, top_n
        ),
    }


def _parse_input_spec(spec: str) -> tuple[str, Path]:
    if "=" not in spec:
        raise argparse.ArgumentTypeError(
            f"input must use LABEL=PATH format, got {spec!r}"
        )
    label, path_text = spec.split("=", 1)
    label = label.strip()
    path_text = path_text.strip()
    if not label:
        raise argparse.ArgumentTypeError(f"input label is empty in {spec!r}")
    if not path_text:
        raise argparse.ArgumentTypeError(f"input path is empty in {spec!r}")
    return label, Path(path_text)


def _load_inputs(input_specs: Sequence[str]) -> dict[str, np.ndarray]:
    records_by_label: dict[str, np.ndarray] = {}
    for spec in input_specs:
        label, path = _parse_input_spec(spec)
        if label in records_by_label:
            raise ValueError(f"duplicate input label {label!r}")
        records_by_label[label] = load_book_tickers(path)
    return records_by_label


def _parse_source_bin_spec(spec: str) -> tuple[int, Path]:
    if "=" not in spec:
        raise argparse.ArgumentTypeError(
            f"source bin must use SOURCE_ID=PATH format, got {spec!r}"
        )
    source_id_text, path_text = spec.split("=", 1)
    source_id_text = source_id_text.strip()
    path_text = path_text.strip()
    if not source_id_text:
        raise argparse.ArgumentTypeError(f"source id is empty in {spec!r}")
    if not path_text:
        raise argparse.ArgumentTypeError(f"source path is empty in {spec!r}")
    try:
        source_id = int(source_id_text)
    except ValueError as error:
        raise argparse.ArgumentTypeError(
            f"source id must be an integer in {spec!r}"
        ) from error
    return source_id, Path(path_text)


def _load_source_bins(source_bin_specs: Sequence[str]) -> dict[int, np.ndarray]:
    records_by_source_id: dict[int, np.ndarray] = {}
    for spec in source_bin_specs:
        source_id, path = _parse_source_bin_spec(spec)
        if source_id in records_by_source_id:
            raise ValueError(f"duplicate source id {source_id}")
        records_by_source_id[source_id] = load_book_tickers(path)
    return records_by_source_id


def _summary_csv_rows(summary: Mapping[str, Any]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for combo_name, combo_summary in summary["combinations"].items():
        row: dict[str, Any] = {
            "combo": combo_name,
            "size": combo_summary["size"],
            "labels": "+".join(combo_summary["labels"]),
            "observed_id_count": combo_summary["observed_id_count"],
            "missing_id_count": combo_summary["missing_id_count"],
            "coverage_ratio": combo_summary["coverage_ratio"],
            "negative_count": combo_summary["negative_count"],
            "zero_count": combo_summary["zero_count"],
            "p99_improvement_vs_best_member_ns": combo_summary[
                "p99_improvement_vs_best_member_ns"
            ],
            "p99_improvement_vs_best_member_pct": combo_summary[
                "p99_improvement_vs_best_member_pct"
            ],
            "best_source_counts": json.dumps(
                combo_summary["best_source_counts"], sort_keys=True
            ),
        }
        for key in ["min", "p50", "p90", "p95", "p99", "p99_9", "max", "mean"]:
            row[f"{key}_ns"] = combo_summary["latency_ns"].get(key, "")
            row[f"{key}_us"] = combo_summary["latency_us"].get(key, "")
            row[f"{key}_ms"] = combo_summary["latency_ms"].get(key, "")
        rows.append(row)
    return rows


def write_summary_csv(path: Path, summary: Mapping[str, Any]) -> None:
    rows = _summary_csv_rows(summary)
    if not rows:
        return
    fieldnames = list(rows[0].keys())
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def _top_outlier_csv_rows(summary: Mapping[str, Any]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for combo_name, combo_summary in summary["combinations"].items():
        for row in combo_summary["top_outliers"]:
            rows.append(
                {
                    "combo": combo_name,
                    "size": combo_summary["size"],
                    "id": row["id"],
                    "latency_ns": row["latency_ns"],
                    "latency_us": row["latency_us"],
                    "latency_ms": row["latency_ms"],
                    "best_source": row["best_source"],
                    "available_latency_ns": json.dumps(
                        row["available_latency_ns"], sort_keys=True
                    ),
                }
            )
    return rows


def write_top_outliers_csv(path: Path, summary: Mapping[str, Any]) -> None:
    rows = _top_outlier_csv_rows(summary)
    fieldnames = [
        "combo",
        "size",
        "id",
        "latency_ns",
        "latency_us",
        "latency_ms",
        "best_source",
        "available_latency_ns",
    ]
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def _published_summary_csv_rows(summary: Mapping[str, Any]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []

    def append_latency_row(
        row_type: str,
        source_id: str,
        count: int,
        latency_summary: Mapping[str, int],
        extra: Mapping[str, Any] | None = None,
    ) -> None:
        row: dict[str, Any] = {
            "type": row_type,
            "source_id": source_id,
            "count": count,
        }
        if extra is not None:
            row.update(extra)
        for key in ["min", "p50", "p90", "p95", "p99", "p99_9", "max", "mean"]:
            row[f"{key}_ns"] = latency_summary.get(key, "")
            row[f"{key}_us"] = (
                latency_summary[key] / 1_000.0 if key in latency_summary else ""
            )
        rows.append(row)

    for source_id, source_summary in summary["sources"].items():
        append_latency_row(
            "source",
            source_id,
            source_summary["count"],
            source_summary["latency_ns"],
            {
                "negative_count": source_summary["negative_count"],
                "zero_count": source_summary["zero_count"],
            },
        )

    fusion_summary = summary["fusion"]
    append_latency_row(
        "fusion",
        "",
        fusion_summary["count"],
        fusion_summary["latency_ns"],
        {
            "negative_count": fusion_summary["negative_count"],
            "zero_count": fusion_summary["zero_count"],
        },
    )

    metadata_summary = summary["metadata"]
    append_latency_row(
        "metadata_source",
        "",
        metadata_summary["count"],
        metadata_summary["source_latency_ns"],
    )
    append_latency_row(
        "metadata_fusion",
        "",
        metadata_summary["count"],
        metadata_summary["fusion_latency_ns"],
    )
    append_latency_row(
        "metadata_hop",
        "",
        metadata_summary["count"],
        metadata_summary["fusion_hop_ns"],
        {
            "winner_counts": json.dumps(
                metadata_summary["winner_counts"], sort_keys=True
            )
        },
    )
    return rows


def write_published_summary_csv(path: Path, summary: Mapping[str, Any]) -> None:
    rows = _published_summary_csv_rows(summary)
    if not rows:
        return
    fieldnames = list(rows[0].keys())
    for row in rows:
        for key in row:
            if key not in fieldnames:
                fieldnames.append(key)
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def write_published_top_outliers_csv(path: Path, summary: Mapping[str, Any]) -> None:
    fieldnames = [
        "index",
        "source_id",
        "symbol_id",
        "id",
        "exchange_ns",
        "source_local_ns",
        "fusion_publish_ns",
        "source_latency_ns",
        "fusion_latency_ns",
        "fusion_hop_ns",
        "fusion_hop_us",
    ]
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for row in summary["top_fusion_hop_outliers"]:
            writer.writerow(row)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Analyze BookTicker fusion latency. Legacy mode uses four --input "
            "LABEL=PATH feeds to simulate feed combinations. Published mode uses "
            "--source-bin SOURCE_ID=PATH, --fusion-bin, and --metadata-bin to analyze "
            "the actual gate_book_ticker_fusion output."
        )
    )
    parser.add_argument(
        "--input",
        action="append",
        default=[],
        help="legacy BookTicker binary input in LABEL=PATH format; pass exactly 4 times",
    )
    parser.add_argument(
        "--source-bin",
        action="append",
        default=[],
        help="published fusion source BookTicker bin in SOURCE_ID=PATH format; pass N times",
    )
    parser.add_argument(
        "--fusion-bin",
        type=Path,
        help="published fusion canonical BookTicker bin",
    )
    parser.add_argument(
        "--metadata-bin",
        type=Path,
        help="published fusion sidecar metadata bin",
    )
    parser.add_argument(
        "--top-n",
        type=int,
        default=20,
        help="top outliers per legacy combination or published fusion hop",
    )
    parser.add_argument("--symbol-id", type=int, help="optional symbol_id filter")
    parser.add_argument("--exchange", type=int, help="optional exchange enum filter")
    parser.add_argument("--id-start", type=int, help="optional lower id bound")
    parser.add_argument("--id-end", type=int, help="optional upper id bound")
    parser.add_argument("--json-output", type=Path, help="optional JSON summary path")
    parser.add_argument("--summary-output", type=Path, help="optional CSV summary path")
    parser.add_argument("--top-output", type=Path, help="optional CSV top outliers path")
    parser.add_argument(
        "--output-dir",
        type=Path,
        help="write default JSON, summary CSV, and top outlier CSV into this directory",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    legacy_mode = bool(args.input)
    published_mode = bool(args.source_bin) or args.fusion_bin is not None or args.metadata_bin is not None
    if legacy_mode and published_mode:
        raise ValueError("use either legacy --input mode or published fusion mode, not both")
    if not legacy_mode and not published_mode:
        raise ValueError("expected either --input values or published fusion inputs")
    if legacy_mode and len(args.input) != 4:
        raise ValueError(f"expected exactly 4 --input values, got {len(args.input)}")
    if published_mode:
        if not args.source_bin:
            raise ValueError("published fusion mode requires at least one --source-bin")
        if args.fusion_bin is None:
            raise ValueError("published fusion mode requires --fusion-bin")
        if args.metadata_bin is None:
            raise ValueError("published fusion mode requires --metadata-bin")

    output_dir = args.output_dir
    if output_dir is not None:
        output_dir.mkdir(parents=True, exist_ok=True)

    json_output = args.json_output
    summary_output = args.summary_output
    top_output = args.top_output
    if output_dir is not None:
        json_output = json_output or output_dir / "book_ticker_fusion_latency_summary.json"
        summary_output = summary_output or output_dir / "book_ticker_fusion_latency_summary.csv"
        top_output = top_output or output_dir / "book_ticker_fusion_latency_top_outliers.csv"

    if legacy_mode:
        records_by_label = _load_inputs(args.input)
        summary = analyze_fusion_latency(
            records_by_label,
            top_n=args.top_n,
            symbol_id=args.symbol_id,
            exchange=args.exchange,
            id_start=args.id_start,
            id_end=args.id_end,
        )
    else:
        records_by_source_id = _load_source_bins(args.source_bin)
        summary = analyze_published_fusion_latency(
            records_by_source_id,
            load_book_tickers(args.fusion_bin),
            load_fusion_metadata(args.metadata_bin),
            top_n=args.top_n,
            symbol_id=args.symbol_id,
            exchange=args.exchange,
            id_start=args.id_start,
            id_end=args.id_end,
        )

    text = json.dumps(summary, indent=2, sort_keys=True)
    if json_output is not None:
        json_output.write_text(text + "\n")
    else:
        print(text)
    if summary_output is not None:
        if legacy_mode:
            write_summary_csv(summary_output, summary)
        else:
            write_published_summary_csv(summary_output, summary)
    if top_output is not None:
        if legacy_mode:
            write_top_outliers_csv(top_output, summary)
        else:
            write_published_top_outliers_csv(top_output, summary)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
