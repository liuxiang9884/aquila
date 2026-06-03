#!/usr/bin/env python3

import argparse
import csv
import json
from pathlib import Path
from typing import Any

import numpy as np


EXCHANGE_NAMES = {
    0: "kBinance",
    1: "kOkx",
    2: "kGate",
    3: "kBybit",
    4: "kBitget",
    5: "kCoinbase",
}


def book_ticker_dtype() -> np.dtype:
    return np.dtype(
        {
            "names": [
                "id",
                "symbol_id",
                "exchange",
                "exchange_ns",
                "local_ns",
                "bid_price",
                "bid_volume",
                "ask_price",
                "ask_volume",
            ],
            "formats": [
                "<i8",
                "<i4",
                "u1",
                "<i8",
                "<i8",
                "<f8",
                "<f8",
                "<f8",
                "<f8",
            ],
            "offsets": [0, 8, 12, 16, 24, 32, 40, 48, 56],
            "itemsize": 64,
        }
    )


def load_book_tickers(path: Path) -> np.ndarray:
    file_size = path.stat().st_size
    dtype = book_ticker_dtype()
    if file_size % dtype.itemsize != 0:
        raise ValueError(
            f"file size {file_size} is not a multiple of BookTicker size {dtype.itemsize}"
        )
    return np.fromfile(path, dtype=dtype)


def _percentile_int(values: np.ndarray, percentile: float) -> int:
    return int(round(float(np.percentile(values, percentile))))


def _latency_summary(values: np.ndarray) -> dict[str, int]:
    return {
        "min": int(values.min()),
        "p01": _percentile_int(values, 1),
        "p05": _percentile_int(values, 5),
        "p10": _percentile_int(values, 10),
        "p25": _percentile_int(values, 25),
        "p50": _percentile_int(values, 50),
        "p75": _percentile_int(values, 75),
        "p90": _percentile_int(values, 90),
        "p95": _percentile_int(values, 95),
        "p99": _percentile_int(values, 99),
        "max": int(values.max()),
        "mean": int(round(float(values.mean()))),
    }


def _top_outliers(records: np.ndarray, latency_ns: np.ndarray, top_n: int) -> list[dict[str, Any]]:
    if top_n <= 0:
        return []
    limit = min(top_n, len(records))
    order = np.argsort(latency_ns)[-limit:][::-1]
    rows: list[dict[str, Any]] = []
    for index in order:
        record = records[index]
        rows.append(
            {
                "index": int(index),
                "id": int(record["id"]),
                "symbol_id": int(record["symbol_id"]),
                "exchange": EXCHANGE_NAMES.get(int(record["exchange"]), str(int(record["exchange"]))),
                "exchange_ns": int(record["exchange_ns"]),
                "local_ns": int(record["local_ns"]),
                "latency_ns": int(latency_ns[index]),
                "latency_us": float(latency_ns[index]) / 1_000.0,
                "bid_price": float(record["bid_price"]),
                "ask_price": float(record["ask_price"]),
            }
        )
    return rows


def summarize_latency(records: np.ndarray, top_n: int = 20) -> dict[str, Any]:
    if len(records) == 0:
        raise ValueError("no BookTicker records")

    latency_ns = records["local_ns"].astype(np.int64) - records["exchange_ns"].astype(np.int64)
    summary: dict[str, Any] = {
        "count": int(len(records)),
        "first_exchange_ns": int(records["exchange_ns"][0]),
        "last_exchange_ns": int(records["exchange_ns"][-1]),
        "first_local_ns": int(records["local_ns"][0]),
        "last_local_ns": int(records["local_ns"][-1]),
        "negative_count": int(np.count_nonzero(latency_ns < 0)),
        "zero_count": int(np.count_nonzero(latency_ns == 0)),
        "latency_ns": _latency_summary(latency_ns),
        "latency_us": {
            key: value / 1_000.0 for key, value in _latency_summary(latency_ns).items()
        },
        "latency_ms": {
            key: value / 1_000_000.0
            for key, value in _latency_summary(latency_ns).items()
        },
        "by_exchange": {},
        "top_outliers": _top_outliers(records, latency_ns, top_n),
    }

    for exchange_value in sorted(int(value) for value in np.unique(records["exchange"])):
        mask = records["exchange"] == exchange_value
        exchange_latency_ns = latency_ns[mask]
        exchange_name = EXCHANGE_NAMES.get(exchange_value, str(exchange_value))
        summary["by_exchange"][exchange_name] = {
            "count": int(np.count_nonzero(mask)),
            "negative_count": int(np.count_nonzero(exchange_latency_ns < 0)),
            "latency_ns": _latency_summary(exchange_latency_ns),
        }

    return summary


def write_top_outliers_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    fieldnames = [
        "index",
        "id",
        "symbol_id",
        "exchange",
        "exchange_ns",
        "local_ns",
        "latency_ns",
        "latency_us",
        "bid_price",
        "ask_price",
    ]
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Analyze BookTicker exchange_ns -> local_ns latency from binary records."
    )
    parser.add_argument("--input", required=True, type=Path, help="BookTicker binary file")
    parser.add_argument("--top-n", type=int, default=20, help="number of largest latency rows")
    parser.add_argument("--json-output", type=Path, help="optional JSON summary output path")
    parser.add_argument("--top-output", type=Path, help="optional CSV top outliers output path")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    records = load_book_tickers(args.input)
    summary = summarize_latency(records, top_n=args.top_n)
    text = json.dumps(summary, indent=2, sort_keys=True)
    if args.json_output is not None:
        args.json_output.write_text(text + "\n")
    else:
        print(text)
    if args.top_output is not None:
        write_top_outliers_csv(args.top_output, summary["top_outliers"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
