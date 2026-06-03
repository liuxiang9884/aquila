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


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Analyze 4-way BookTicker latency fusion. The global id interval is the "
            "intersection of the four feed id ranges; each combination uses the union "
            "of available ids inside that interval and takes the minimum latency per id."
        )
    )
    parser.add_argument(
        "--input",
        action="append",
        required=True,
        help="BookTicker binary input in LABEL=PATH format; pass exactly 4 times",
    )
    parser.add_argument("--top-n", type=int, default=20, help="top outliers per combination")
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
    if len(args.input) != 4:
        raise ValueError(f"expected exactly 4 --input values, got {len(args.input)}")

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

    records_by_label = _load_inputs(args.input)
    summary = analyze_fusion_latency(
        records_by_label,
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
        write_summary_csv(summary_output, summary)
    if top_output is not None:
        write_top_outliers_csv(top_output, summary)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
