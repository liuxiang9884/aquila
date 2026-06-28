#!/usr/bin/env python3

import argparse
import csv
import json
import sys
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any, Iterable, Sequence

import numpy as np
import pandas as pd


SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from analyze_book_ticker_latency import book_ticker_dtype  # noqa: E402


EXCHANGE_IDS = {
    "binance": 0,
    "gate": 2,
}

TARDIS_EXCHANGES = {
    "binance": "binance-futures",
    "gate": "gate-io-futures",
}

US_TO_MS = 1_000
NS_TO_MS = 1_000_000

KEY_DTYPE = np.dtype(
    [
        ("timestamp_ms", "<i8"),
        ("bid_price_units", "<i8"),
        ("bid_volume_units", "<i8"),
        ("ask_price_units", "<i8"),
        ("ask_volume_units", "<i8"),
    ]
)

VALUE_DTYPE = np.dtype(
    [
        ("bid_price_units", "<i8"),
        ("bid_volume_units", "<i8"),
        ("ask_price_units", "<i8"),
        ("ask_volume_units", "<i8"),
    ]
)


@dataclass(frozen=True)
class CatalogEntry:
    symbol_id: int
    symbol: str
    exchange: str
    exchange_symbol: str
    price_tick: float
    quantity_step: float
    tardis_exchange: str


def parse_date_ms(date: str) -> tuple[int, int]:
    start = datetime.strptime(date, "%Y%m%d").replace(tzinfo=timezone.utc)
    end = start + timedelta(days=1)
    return int(start.timestamp() * 1000), int(end.timestamp() * 1000)


def _required_float(row: dict[str, str], field: str, path: Path, row_number: int) -> float:
    value = (row.get(field) or "").strip()
    if not value:
        raise ValueError(f"{path}:{row_number}: empty {field}")
    parsed = float(value)
    if parsed <= 0:
        raise ValueError(f"{path}:{row_number}: {field} must be positive")
    return parsed


def load_catalog(path: Path) -> dict[tuple[str, str], CatalogEntry]:
    entries: dict[tuple[str, str], CatalogEntry] = {}
    with path.open(newline="") as handle:
        reader = csv.DictReader(handle)
        required = {"symbol_id", "symbol", "exchange", "exchange_symbol", "price_tick", "quantity_step"}
        missing = required - set(reader.fieldnames or ())
        if missing:
            raise ValueError(f"{path} missing columns: {', '.join(sorted(missing))}")

        for row_number, row in enumerate(reader, start=2):
            exchange = (row.get("exchange") or "").strip().lower()
            if exchange not in EXCHANGE_IDS:
                continue
            symbol = (row.get("symbol") or "").strip()
            exchange_symbol = (row.get("exchange_symbol") or "").strip()
            if not symbol or not exchange_symbol:
                raise ValueError(f"{path}:{row_number}: empty symbol or exchange_symbol")
            key = (exchange, symbol)
            entry = CatalogEntry(
                symbol_id=int((row.get("symbol_id") or "").strip()),
                symbol=symbol,
                exchange=exchange,
                exchange_symbol=exchange_symbol,
                price_tick=_required_float(row, "price_tick", path, row_number),
                quantity_step=_required_float(row, "quantity_step", path, row_number),
                tardis_exchange=TARDIS_EXCHANGES[exchange],
            )
            existing = entries.get(key)
            if existing is not None and existing != entry:
                raise ValueError(f"{path}:{row_number}: duplicate catalog entry for {key}")
            entries[key] = entry
    return entries


def tardis_book_ticker_path(root: Path, entry: CatalogEntry, date: str) -> Path:
    return (
        root
        / entry.tardis_exchange
        / "book_ticker"
        / date
        / f"{entry.exchange_symbol}-book_ticker-{date}.csv.gz"
    )


def _existing_tardis_path(root: Path, entry: CatalogEntry, date: str) -> Path:
    gz_path = tardis_book_ticker_path(root, entry, date)
    if gz_path.exists():
        return gz_path
    csv_path = gz_path.with_suffix("")
    if csv_path.exists():
        return csv_path
    return gz_path


def _to_units(values: np.ndarray, step: float) -> np.ndarray:
    return np.rint(values.astype(np.float64, copy=False) / step).astype(np.int64)


def _keys_from_columns(
    *,
    timestamp_ms: np.ndarray,
    bid_price: np.ndarray,
    bid_volume: np.ndarray,
    ask_price: np.ndarray,
    ask_volume: np.ndarray,
    price_tick: float,
    quantity_step: float,
) -> np.ndarray:
    keys = np.empty(len(timestamp_ms), dtype=KEY_DTYPE)
    keys["timestamp_ms"] = timestamp_ms.astype(np.int64, copy=False)
    keys["bid_price_units"] = _to_units(bid_price, price_tick)
    keys["bid_volume_units"] = _to_units(bid_volume, quantity_step)
    keys["ask_price_units"] = _to_units(ask_price, price_tick)
    keys["ask_volume_units"] = _to_units(ask_volume, quantity_step)
    return keys


def _unique_counts(keys: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    if len(keys) == 0:
        return np.zeros(0, dtype=KEY_DTYPE), np.zeros(0, dtype=np.int64)
    unique, counts = np.unique(keys, return_counts=True)
    return unique, counts.astype(np.int64, copy=False)


def _aligned_counts(
    left_keys: np.ndarray,
    right_keys: np.ndarray,
    right_counts: np.ndarray,
) -> np.ndarray:
    aligned = np.zeros(len(left_keys), dtype=np.int64)
    if len(left_keys) == 0 or len(right_keys) == 0:
        return aligned
    indices = np.searchsorted(right_keys, left_keys)
    valid = indices < len(right_keys)
    if np.any(valid):
        valid_positions = np.flatnonzero(valid)
        matched_positions = valid_positions[
            right_keys[indices[valid_positions]] == left_keys[valid_positions]
        ]
        aligned[matched_positions] = right_counts[indices[matched_positions]]
    return aligned


def _diff_unique_counts(
    left_keys: np.ndarray,
    left_counts: np.ndarray,
    right_keys: np.ndarray,
    right_counts: np.ndarray,
) -> tuple[np.ndarray, np.ndarray]:
    diff_counts = left_counts - _aligned_counts(left_keys, right_keys, right_counts)
    positive = diff_counts > 0
    return left_keys[positive], diff_counts[positive]


def _value_keys(keys: np.ndarray) -> np.ndarray:
    values = np.empty(len(keys), dtype=VALUE_DTYPE)
    values["bid_price_units"] = keys["bid_price_units"]
    values["bid_volume_units"] = keys["bid_volume_units"]
    values["ask_price_units"] = keys["ask_price_units"]
    values["ask_volume_units"] = keys["ask_volume_units"]
    return values


def _read_fusion_chunks(
    path: Path,
    *,
    dtype: np.dtype,
    chunk_records: int,
) -> Iterable[np.ndarray]:
    if chunk_records <= 0:
        raise ValueError("chunk_records must be positive")
    file_size = path.stat().st_size
    if file_size % dtype.itemsize != 0:
        raise ValueError(f"{path} size {file_size} is not a multiple of {dtype.itemsize}")
    total_records = file_size // dtype.itemsize
    if total_records == 0:
        return
    records = np.memmap(path, dtype=dtype, mode="r")
    for start in range(0, total_records, chunk_records):
        end = min(start + chunk_records, total_records)
        yield np.asarray(records[start:end])


def load_fusion_symbol_records(
    *,
    fusion_paths: Sequence[Path],
    entry: CatalogEntry,
    date: str,
    chunk_records: int = 1_000_000,
) -> np.ndarray:
    dtype = book_ticker_dtype()
    date_start_ms, date_end_ms = parse_date_ms(date)
    selected_chunks: list[np.ndarray] = []
    exchange_id = EXCHANGE_IDS[entry.exchange]
    for path in fusion_paths:
        for records in _read_fusion_chunks(path, dtype=dtype, chunk_records=chunk_records):
            timestamp_ms = records["exchange_ns"] // NS_TO_MS
            mask = (
                (records["exchange"] == exchange_id)
                & (records["symbol_id"] == entry.symbol_id)
                & (timestamp_ms >= date_start_ms)
                & (timestamp_ms < date_end_ms)
            )
            if np.any(mask):
                selected_chunks.append(records[mask].copy())
    if not selected_chunks:
        return np.zeros(0, dtype=dtype)
    return np.concatenate(selected_chunks)


def fusion_keys_for_window(
    records: np.ndarray,
    *,
    entry: CatalogEntry,
) -> tuple[np.ndarray, int, int]:
    if len(records) == 0:
        raise ValueError(f"no fusion records for {entry.exchange} {entry.symbol}")
    timestamp_ms = records["exchange_ns"] // NS_TO_MS
    window_start_ms = int(timestamp_ms[0])
    window_end_ms = int(timestamp_ms[-1])
    in_window = (timestamp_ms >= window_start_ms) & (timestamp_ms <= window_end_ms)
    records = records[in_window]
    timestamp_ms = timestamp_ms[in_window]
    return (
        _keys_from_columns(
            timestamp_ms=timestamp_ms,
            bid_price=records["bid_price"],
            bid_volume=records["bid_volume"],
            ask_price=records["ask_price"],
            ask_volume=records["ask_volume"],
            price_tick=entry.price_tick,
            quantity_step=entry.quantity_step,
        ),
        window_start_ms,
        window_end_ms,
    )


def load_tardis_keys(
    *,
    path: Path,
    entry: CatalogEntry,
    window_start_ms: int,
    window_end_ms: int,
    chunk_rows: int = 1_000_000,
) -> np.ndarray:
    if not path.exists():
        raise FileNotFoundError(path)
    chunks: list[np.ndarray] = []
    usecols = [
        "timestamp",
        "ask_amount",
        "ask_price",
        "bid_price",
        "bid_amount",
    ]
    for frame in pd.read_csv(path, usecols=usecols, chunksize=chunk_rows):
        timestamp_ms = (frame["timestamp"].to_numpy(dtype=np.int64, copy=False) // US_TO_MS)
        mask = (timestamp_ms >= window_start_ms) & (timestamp_ms <= window_end_ms)
        if not np.any(mask):
            continue
        selected = frame.loc[mask]
        chunks.append(
            _keys_from_columns(
                timestamp_ms=timestamp_ms[mask],
                bid_price=selected["bid_price"].to_numpy(dtype=np.float64, copy=False),
                bid_volume=selected["bid_amount"].to_numpy(dtype=np.float64, copy=False),
                ask_price=selected["ask_price"].to_numpy(dtype=np.float64, copy=False),
                ask_volume=selected["ask_amount"].to_numpy(dtype=np.float64, copy=False),
                price_tick=entry.price_tick,
                quantity_step=entry.quantity_step,
            )
        )
    if not chunks:
        return np.zeros(0, dtype=KEY_DTYPE)
    return np.concatenate(chunks)


def _key_to_dict(key: np.void, count: int) -> dict[str, int]:
    return {
        "timestamp_ms": int(key["timestamp_ms"]),
        "bid_price_units": int(key["bid_price_units"]),
        "bid_volume_units": int(key["bid_volume_units"]),
        "ask_price_units": int(key["ask_price_units"]),
        "ask_volume_units": int(key["ask_volume_units"]),
        "count": int(count),
    }


def _value_tuple(value: np.void) -> tuple[int, int, int, int]:
    return (
        int(value["bid_price_units"]),
        int(value["bid_volume_units"]),
        int(value["ask_price_units"]),
        int(value["ask_volume_units"]),
    )


def _diff_samples(
    left_keys: np.ndarray,
    left_counts: np.ndarray,
    right_keys: np.ndarray,
    right_counts: np.ndarray,
    sample_limit: int,
) -> list[dict[str, int]]:
    samples: list[dict[str, int]] = []
    if sample_limit <= 0 or len(left_keys) == 0:
        return samples

    if len(right_keys) == 0:
        positions = np.arange(min(sample_limit, len(left_keys)))
        return [_key_to_dict(left_keys[index], int(left_counts[index])) for index in positions]

    right_aligned = _aligned_counts(left_keys, right_keys, right_counts)
    diff_counts = left_counts - right_aligned
    for index in np.flatnonzero(diff_counts > 0)[:sample_limit]:
        samples.append(_key_to_dict(left_keys[index], int(diff_counts[index])))
    return samples


def _count_near_matches(
    left_keys: np.ndarray,
    left_counts: np.ndarray,
    right_keys: np.ndarray,
    right_counts: np.ndarray,
    tolerance_ms: int,
) -> int:
    if tolerance_ms <= 0 or len(left_keys) == 0 or len(right_keys) == 0:
        return 0

    left_values = _value_keys(left_keys)
    right_values = _value_keys(right_keys)
    left_order = np.argsort(left_values, kind="mergesort")
    right_order = np.argsort(right_values, kind="mergesort")
    left_values = left_values[left_order]
    right_values = right_values[right_order]
    left_keys = left_keys[left_order]
    right_keys = right_keys[right_order]
    left_counts = left_counts[left_order]
    right_counts = right_counts[right_order]

    matched = 0
    left_index = 0
    right_index = 0
    while left_index < len(left_keys) and right_index < len(right_keys):
        left_value = left_values[left_index]
        right_value = right_values[right_index]
        left_tuple = _value_tuple(left_value)
        right_tuple = _value_tuple(right_value)
        if left_tuple < right_tuple:
            left_index += 1
            continue
        if right_tuple < left_tuple:
            right_index += 1
            continue

        left_end = left_index + 1
        while left_end < len(left_keys) and left_values[left_end] == left_value:
            left_end += 1
        right_end = right_index + 1
        while right_end < len(right_keys) and right_values[right_end] == right_value:
            right_end += 1

        left_timestamps = np.repeat(
            left_keys["timestamp_ms"][left_index:left_end],
            left_counts[left_index:left_end],
        )
        right_timestamps = np.repeat(
            right_keys["timestamp_ms"][right_index:right_end],
            right_counts[right_index:right_end],
        )
        left_timestamps.sort()
        right_timestamps.sort()

        i = 0
        j = 0
        while i < len(left_timestamps) and j < len(right_timestamps):
            left_ts = int(left_timestamps[i])
            right_ts = int(right_timestamps[j])
            if abs(left_ts - right_ts) <= tolerance_ms:
                matched += 1
                i += 1
                j += 1
            elif left_ts < right_ts:
                i += 1
            else:
                j += 1

        left_index = left_end
        right_index = right_end
    return matched


def compare_key_sets(
    fusion_keys: np.ndarray,
    tardis_keys: np.ndarray,
    *,
    sample_limit: int,
    near_ms: int = 0,
) -> dict[str, Any]:
    fusion_unique, fusion_counts = _unique_counts(fusion_keys)
    tardis_unique, tardis_counts = _unique_counts(tardis_keys)

    if len(fusion_unique) and len(tardis_unique):
        tardis_aligned = _aligned_counts(fusion_unique, tardis_unique, tardis_counts)
        matched = int(np.minimum(fusion_counts, tardis_aligned).sum())
    else:
        matched = 0

    fusion_only_records = int(len(fusion_keys) - matched)
    tardis_only_records = int(len(tardis_keys) - matched)
    fusion_diff_keys, fusion_diff_counts = _diff_unique_counts(
        fusion_unique, fusion_counts, tardis_unique, tardis_counts
    )
    tardis_diff_keys, tardis_diff_counts = _diff_unique_counts(
        tardis_unique, tardis_counts, fusion_unique, fusion_counts
    )
    near_matched = _count_near_matches(
        fusion_diff_keys,
        fusion_diff_counts,
        tardis_diff_keys,
        tardis_diff_counts,
        near_ms,
    )
    return {
        "matched_records": matched,
        "fusion_only_records": fusion_only_records,
        "tardis_only_records": tardis_only_records,
        "near_ms": int(near_ms),
        "near_matched_records": near_matched,
        "fusion_only_after_near_records": fusion_only_records - near_matched,
        "tardis_only_after_near_records": tardis_only_records - near_matched,
        "fusion_unique_keys": int(len(fusion_unique)),
        "tardis_unique_keys": int(len(tardis_unique)),
        "fusion_only_samples": _diff_samples(
            fusion_unique, fusion_counts, tardis_unique, tardis_counts, sample_limit
        ),
        "tardis_only_samples": _diff_samples(
            tardis_unique, tardis_counts, fusion_unique, fusion_counts, sample_limit
        ),
    }


def compare_symbol(
    *,
    fusion_paths: Sequence[Path],
    tardis_root: Path,
    catalog_path: Path,
    exchange: str,
    symbol: str,
    date: str,
    near_ms: int = 0,
    sample_limit: int = 20,
    fusion_chunk_records: int = 1_000_000,
    tardis_chunk_rows: int = 1_000_000,
) -> dict[str, Any]:
    exchange = exchange.lower()
    catalog = load_catalog(catalog_path)
    entry = catalog.get((exchange, symbol))
    if entry is None:
        raise ValueError(f"missing catalog entry for {exchange} {symbol}")

    fusion_records = load_fusion_symbol_records(
        fusion_paths=fusion_paths,
        entry=entry,
        date=date,
        chunk_records=fusion_chunk_records,
    )
    fusion_keys, window_start_ms, window_end_ms = fusion_keys_for_window(
        fusion_records,
        entry=entry,
    )
    tardis_path = _existing_tardis_path(tardis_root, entry, date)
    tardis_keys = load_tardis_keys(
        path=tardis_path,
        entry=entry,
        window_start_ms=window_start_ms,
        window_end_ms=window_end_ms,
        chunk_rows=tardis_chunk_rows,
    )
    diff = compare_key_sets(
        fusion_keys, tardis_keys, sample_limit=sample_limit, near_ms=near_ms
    )
    return {
        "exchange": exchange,
        "tardis_exchange": entry.tardis_exchange,
        "symbol": symbol,
        "exchange_symbol": entry.exchange_symbol,
        "symbol_id": entry.symbol_id,
        "date": date,
        "fusion_paths": [str(path) for path in fusion_paths],
        "tardis_path": str(tardis_path),
        "window_start_ms": window_start_ms,
        "window_end_ms": window_end_ms,
        "fusion_records": int(len(fusion_keys)),
        "tardis_records": int(len(tardis_keys)),
        **diff,
    }


def _parse_symbols(values: Sequence[str]) -> list[str]:
    symbols: list[str] = []
    for value in values:
        symbols.extend(part.strip() for part in value.split(",") if part.strip())
    return symbols


def write_summary_csv(path: Path, summaries: Sequence[dict[str, Any]]) -> None:
    fieldnames = [
        "exchange",
        "symbol",
        "exchange_symbol",
        "symbol_id",
        "date",
        "window_start_ms",
        "window_end_ms",
        "fusion_records",
        "tardis_records",
        "matched_records",
        "fusion_only_records",
        "tardis_only_records",
        "near_ms",
        "near_matched_records",
        "fusion_only_after_near_records",
        "tardis_only_after_near_records",
        "fusion_unique_keys",
        "tardis_unique_keys",
        "tardis_path",
    ]
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for summary in summaries:
            writer.writerow({field: summary.get(field, "") for field in fieldnames})


def write_samples_csv(path: Path, rows: Sequence[dict[str, Any]]) -> None:
    fieldnames = [
        "exchange",
        "symbol",
        "side",
        "timestamp_ms",
        "bid_price_units",
        "bid_volume_units",
        "ask_price_units",
        "ask_volume_units",
        "count",
    ]
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow({field: row.get(field, "") for field in fieldnames})


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Compare canonical fusion BookTicker binary data with Tardis book_ticker CSV."
    )
    parser.add_argument("--fusion", action="append", required=True, type=Path)
    parser.add_argument("--tardis-root", required=True, type=Path)
    parser.add_argument("--instrument-catalog", required=True, type=Path)
    parser.add_argument("--exchange", required=True, choices=sorted(EXCHANGE_IDS))
    parser.add_argument("--symbol", action="append", required=True)
    parser.add_argument("--date", required=True, help="UTC date, YYYYMMDD")
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--summary-json", type=Path)
    parser.add_argument("--summary-csv", type=Path)
    parser.add_argument("--sample-limit", type=int, default=20)
    parser.add_argument(
        "--near-ms",
        type=int,
        default=0,
        help="classify strict unmatched rows with same price/qty within this ms tolerance",
    )
    parser.add_argument("--fusion-chunk-records", type=int, default=1_000_000)
    parser.add_argument("--tardis-chunk-rows", type=int, default=1_000_000)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    summaries = []
    all_samples = []
    for symbol in _parse_symbols(args.symbol):
        summary = compare_symbol(
            fusion_paths=args.fusion,
            tardis_root=args.tardis_root,
            catalog_path=args.instrument_catalog,
            exchange=args.exchange,
            symbol=symbol,
            date=args.date,
            near_ms=args.near_ms,
            sample_limit=args.sample_limit,
            fusion_chunk_records=args.fusion_chunk_records,
            tardis_chunk_rows=args.tardis_chunk_rows,
        )
        summaries.append(summary)
        for side in ("fusion_only", "tardis_only"):
            for row in summary[f"{side}_samples"]:
                all_samples.append(
                    {
                        "exchange": summary["exchange"],
                        "symbol": summary["symbol"],
                        "side": side,
                        **row,
                    }
                )

    if args.output_dir is not None:
        args.output_dir.mkdir(parents=True, exist_ok=True)
        if args.summary_json is None:
            args.summary_json = args.output_dir / f"{args.exchange}_summary.json"
        if args.summary_csv is None:
            args.summary_csv = args.output_dir / f"{args.exchange}_summary.csv"
        samples_path = args.output_dir / f"{args.exchange}_missing_samples.csv"
        write_samples_csv(samples_path, all_samples)

    text = json.dumps(summaries, indent=2, sort_keys=True)
    if args.summary_json is not None:
        args.summary_json.parent.mkdir(parents=True, exist_ok=True)
        args.summary_json.write_text(text + "\n")
    else:
        print(text)
    if args.summary_csv is not None:
        args.summary_csv.parent.mkdir(parents=True, exist_ok=True)
        write_summary_csv(args.summary_csv, summaries)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
