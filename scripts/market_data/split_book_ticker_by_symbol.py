#!/usr/bin/env python3

import argparse
import csv
import json
import sys
from pathlib import Path
from typing import BinaryIO, NamedTuple, Sequence

import numpy as np


SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from analyze_book_ticker_latency import book_ticker_dtype  # noqa: E402


class SplitSummary(NamedTuple):
    output_dir: Path
    files_processed: int
    total_records_read: int
    trailing_bytes_ignored: int
    unknown_symbol_id_records: int
    records_written_by_symbol: dict[str, int]


def load_symbol_id_map(catalog_path: Path) -> dict[int, str]:
    id_to_symbol: dict[int, str] = {}
    with catalog_path.open(newline="") as handle:
        reader = csv.DictReader(handle)
        if "symbol_id" not in (reader.fieldnames or ()):
            raise ValueError(f"{catalog_path} does not contain symbol_id column")
        if "symbol" not in (reader.fieldnames or ()):
            raise ValueError(f"{catalog_path} does not contain symbol column")
        for row_number, row in enumerate(reader, start=2):
            symbol_id_text = (row.get("symbol_id") or "").strip()
            symbol = (row.get("symbol") or "").strip()
            if not symbol_id_text or not symbol:
                raise ValueError(f"{catalog_path}:{row_number}: empty symbol_id or symbol")
            symbol_id = int(symbol_id_text)
            existing = id_to_symbol.get(symbol_id)
            if existing is not None and existing != symbol:
                raise ValueError(
                    f"{catalog_path}:{row_number}: symbol_id {symbol_id} maps to both "
                    f"{existing} and {symbol}"
                )
            id_to_symbol[symbol_id] = symbol
    return id_to_symbol


def _normalize_symbols(symbols: set[str] | None) -> set[str] | None:
    if symbols is None:
        return None
    normalized = {symbol.strip() for symbol in symbols if symbol.strip()}
    return normalized or None


def _target_symbol_ids(
    id_to_symbol: dict[int, str], symbols: set[str] | None
) -> dict[int, str]:
    symbols = _normalize_symbols(symbols)
    if symbols is None:
        return dict(id_to_symbol)

    known_symbols = set(id_to_symbol.values())
    missing = sorted(symbols - known_symbols)
    if missing:
        raise ValueError(f"symbols are not present in catalog: {', '.join(missing)}")
    return {
        symbol_id: symbol
        for symbol_id, symbol in id_to_symbol.items()
        if symbol in symbols
    }


def _prepare_output_dir(
    output_root: Path, run_id: str, target_symbols: set[str] | None
) -> Path:
    run_path = Path(run_id)
    if run_path.name != run_id:
        raise ValueError("run_id must be a single path component")

    output_dir = output_root / run_id
    output_dir.mkdir(parents=True, exist_ok=True)

    if target_symbols is None:
        stale_paths = list(output_dir.glob("*.bin"))
    else:
        stale_paths = [output_dir / f"{symbol}.bin" for symbol in target_symbols]
    for path in stale_paths:
        if path.exists():
            path.unlink()
    return output_dir


def _open_output(
    handles: dict[str, BinaryIO], output_dir: Path, symbol: str
) -> BinaryIO:
    handle = handles.get(symbol)
    if handle is None:
        handle = (output_dir / f"{symbol}.bin").open("ab")
        handles[symbol] = handle
    return handle


def _complete_input_size(
    path: Path, record_size: int, allow_trailing_bytes: bool
) -> tuple[int, int]:
    file_size = path.stat().st_size
    trailing = file_size % record_size
    if trailing and not allow_trailing_bytes:
        raise ValueError(
            f"{path} size {file_size} is not a multiple of BookTicker size {record_size}"
        )
    return file_size - trailing, trailing


def _read_chunks(
    path: Path,
    *,
    dtype: np.dtype,
    chunk_records: int,
    allow_trailing_bytes: bool,
):
    if chunk_records <= 0:
        raise ValueError("chunk_records must be positive")

    complete_size, trailing = _complete_input_size(
        path, dtype.itemsize, allow_trailing_bytes
    )
    bytes_per_chunk = chunk_records * dtype.itemsize
    with path.open("rb") as handle:
        remaining = complete_size
        while remaining > 0:
            to_read = min(bytes_per_chunk, remaining)
            data = handle.read(to_read)
            if len(data) != to_read:
                raise ValueError(
                    f"{path} changed while reading: expected {to_read} bytes, got {len(data)}"
                )
            remaining -= to_read
            yield np.frombuffer(data, dtype=dtype), 0
    if trailing:
        yield np.zeros(0, dtype=dtype), trailing


def split_book_ticker_files_by_symbol(
    *,
    input_paths: Sequence[Path],
    catalog_path: Path,
    output_root: Path,
    run_id: str,
    symbols: set[str] | None = None,
    chunk_records: int = 1_000_000,
    allow_trailing_bytes: bool = False,
) -> SplitSummary:
    if not input_paths:
        raise ValueError("input_paths must not be empty")

    dtype = book_ticker_dtype()
    id_to_symbol = load_symbol_id_map(catalog_path)
    normalized_symbols = _normalize_symbols(symbols)
    target_ids = _target_symbol_ids(id_to_symbol, normalized_symbols)
    output_dir = _prepare_output_dir(output_root, run_id, normalized_symbols)

    handles: dict[str, BinaryIO] = {}
    records_written_by_symbol: dict[str, int] = {}
    total_records_read = 0
    trailing_bytes_ignored = 0
    unknown_symbol_id_records = 0

    try:
        for input_path in input_paths:
            for records, trailing in _read_chunks(
                input_path,
                dtype=dtype,
                chunk_records=chunk_records,
                allow_trailing_bytes=allow_trailing_bytes,
            ):
                trailing_bytes_ignored += trailing
                if len(records) == 0:
                    continue
                total_records_read += int(len(records))

                unique_symbol_ids = np.unique(records["symbol_id"])
                for raw_symbol_id in unique_symbol_ids:
                    symbol_id = int(raw_symbol_id)
                    mask = records["symbol_id"] == symbol_id
                    row_count = int(np.count_nonzero(mask))
                    symbol = target_ids.get(symbol_id)
                    if symbol is None:
                        if symbol_id not in id_to_symbol:
                            unknown_symbol_id_records += row_count
                        continue

                    selected = records[mask]
                    _open_output(handles, output_dir, symbol)
                    selected.tofile(handles[symbol])
                    records_written_by_symbol[symbol] = (
                        records_written_by_symbol.get(symbol, 0) + len(selected)
                    )
    finally:
        for handle in handles.values():
            handle.close()

    return SplitSummary(
        output_dir=output_dir,
        files_processed=len(input_paths),
        total_records_read=total_records_read,
        trailing_bytes_ignored=trailing_bytes_ignored,
        unknown_symbol_id_records=unknown_symbol_id_records,
        records_written_by_symbol=dict(sorted(records_written_by_symbol.items())),
    )


def _parse_symbol_args(values: Sequence[str] | None) -> set[str] | None:
    if not values:
        return None
    symbols: set[str] = set()
    for value in values:
        symbols.update(part.strip() for part in value.split(",") if part.strip())
    return symbols or None


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Split raw BookTicker recorder binary files into per-symbol files."
    )
    parser.add_argument(
        "--input",
        action="append",
        required=True,
        type=Path,
        help="BookTicker binary input path; pass multiple times for consecutive files",
    )
    parser.add_argument(
        "--instrument-catalog",
        required=True,
        type=Path,
        help="instrument catalog CSV containing symbol_id and symbol columns",
    )
    parser.add_argument("--run-id", required=True, help="output folder name")
    parser.add_argument(
        "--output-root",
        type=Path,
        default=Path("/home/liuxiang/tmp/book_ticker_symbol_splits"),
        help="parent directory for the run-id output folder",
    )
    parser.add_argument(
        "--symbol",
        action="append",
        help="symbol to export; can be repeated or comma-separated; omit to split all",
    )
    parser.add_argument(
        "--chunk-records",
        type=int,
        default=1_000_000,
        help="records to process per read chunk",
    )
    parser.add_argument(
        "--allow-trailing-bytes",
        action="store_true",
        help="ignore a live-growing file tail smaller than one BookTicker record",
    )
    parser.add_argument(
        "--json-output",
        type=Path,
        help="optional path for JSON summary",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    summary = split_book_ticker_files_by_symbol(
        input_paths=args.input,
        catalog_path=args.instrument_catalog,
        output_root=args.output_root,
        run_id=args.run_id,
        symbols=_parse_symbol_args(args.symbol),
        chunk_records=args.chunk_records,
        allow_trailing_bytes=args.allow_trailing_bytes,
    )
    summary_json = json.dumps(
        {
            "output_dir": str(summary.output_dir),
            "files_processed": summary.files_processed,
            "total_records_read": summary.total_records_read,
            "trailing_bytes_ignored": summary.trailing_bytes_ignored,
            "unknown_symbol_id_records": summary.unknown_symbol_id_records,
            "records_written_by_symbol": summary.records_written_by_symbol,
        },
        indent=2,
        sort_keys=True,
    )
    if args.json_output is not None:
        args.json_output.write_text(summary_json + "\n")
    print(summary_json)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
