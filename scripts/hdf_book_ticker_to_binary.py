#!/home/liuxiang/dev/pyenv/lx/bin/python

import argparse
import csv
import os
import sys
from dataclasses import dataclass, field
from datetime import datetime, timedelta
from pathlib import Path
from typing import Iterable

import h5py
import numpy as np
import pandas as pd

SCRIPT_DIR = Path(__file__).resolve().parent
MARKET_DATA_SCRIPT_DIR = SCRIPT_DIR / "market_data"
if str(MARKET_DATA_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(MARKET_DATA_SCRIPT_DIR))

import typed_binary  # noqa: E402


EXCHANGE_BINANCE = 0
EXCHANGE_GATE = 2
MS_TO_NS = 1_000_000
SUPPORTED_HDF_VERSIONS = {2, 3}

BOOK_TICKER_DTYPE = typed_binary.book_ticker_dtype()


@dataclass(frozen=True)
class HdfSource:
    exchange_name: str
    exchange_id: int
    file_prefix: str
    symbol_id: int

    def path_for_hour(self, input_dir: Path, date_hour: str) -> Path:
        return input_dir / f"{self.file_prefix}_{date_hour}.h5"


@dataclass
class ConvertStats:
    date: str
    output_path: Path
    records_written: int = 0
    records_by_exchange: dict[str, int] = field(default_factory=dict)
    files_read: int = 0
    first_exchange_ns: int | None = None
    last_exchange_ns: int | None = None


def _scalar(value):
    return value.item() if hasattr(value, "item") else value


def _require_fields(dtype, dataset_name: str, fields: Iterable[str]) -> None:
    names = set(dtype.names or [])
    missing = [field_name for field_name in fields if field_name not in names]
    if missing:
        raise ValueError(f"{dataset_name} missing fields: {', '.join(missing)}")


def _open_bbo_dataset(handle, path: Path, expected_date_hour: str):
    if "config" not in handle:
        raise ValueError(f"{path}: missing config dataset")
    config_dataset = handle["config"]
    if config_dataset.shape[0] != 1:
        raise ValueError(f"{path}: config must contain exactly 1 row")
    _require_fields(
        config_dataset.dtype,
        "config",
        [
            "date",
            "price_multiplier",
            "qty_multiplier",
            "hdf_version",
            "bbo",
            "bbo_ns",
        ],
    )
    config = config_dataset[0]
    hdf_version = int(_scalar(config["hdf_version"]))
    if hdf_version not in SUPPORTED_HDF_VERSIONS:
        raise ValueError(f"{path}: unsupported hdf_version {hdf_version}")
    config_date = int(_scalar(config["date"]))
    expected_date = int(expected_date_hour)
    if config_date != expected_date:
        raise ValueError(
            f"{path}: config.date {config_date} != filename date {expected_date}"
        )

    price_multiplier = int(_scalar(config["price_multiplier"]))
    qty_multiplier = int(_scalar(config["qty_multiplier"]))
    if price_multiplier <= 0:
        raise ValueError(f"{path}: price_multiplier must be positive")
    if qty_multiplier <= 0:
        raise ValueError(f"{path}: qty_multiplier must be positive")

    has_bbo_ns = bool(int(_scalar(config["bbo_ns"])))
    has_bbo = bool(int(_scalar(config["bbo"])))
    if has_bbo_ns:
        dataset_name = "bbo_ns"
        time_scale = 1
    elif has_bbo:
        dataset_name = "bbo"
        time_scale = MS_TO_NS
    else:
        raise ValueError(f"{path}: config disables both bbo_ns and bbo")
    if dataset_name not in handle:
        raise ValueError(f"{path}: config enables missing dataset {dataset_name}")

    dataset = handle[dataset_name]
    _require_fields(
        dataset.dtype,
        dataset_name,
        [
            "ask_price",
            "ask_qty",
            "bid_price",
            "bid_qty",
            "localtime",
            "tx_time",
        ],
    )
    return dataset, time_scale, price_multiplier, qty_multiplier


def read_hdf_bbo_frame(
    path: Path,
    source: HdfSource,
    source_order: int,
    expected_date_hour: str,
) -> pd.DataFrame:
    with h5py.File(path, "r") as handle:
        dataset, time_scale, price_multiplier, qty_multiplier = _open_bbo_dataset(
            handle, path, expected_date_hour
        )
        frame = pd.DataFrame(dataset[...])

    count = len(frame)
    exchange_ns = frame["tx_time"].astype("int64", copy=False) * time_scale
    if count > 1 and (exchange_ns.diff().iloc[1:] < 0).any():
        raise ValueError(f"{path}: bbo rows are not sorted by tx_time")

    return pd.DataFrame(
        {
            "exchange_ns": exchange_ns.to_numpy(dtype=np.int64, copy=False),
            "exchange_id": np.full(count, source.exchange_id, dtype=np.uint8),
            "source_order": np.full(count, source_order, dtype=np.int32),
            "input_sequence": np.arange(count, dtype=np.int64),
            "symbol_id": np.full(count, source.symbol_id, dtype=np.int32),
            "local_ns": (
                frame["localtime"].astype("int64", copy=False) * time_scale
            ).to_numpy(dtype=np.int64, copy=False),
            "bid_price": (
                frame["bid_price"].astype("float64", copy=False) / price_multiplier
            ).to_numpy(dtype=np.float64, copy=False),
            "bid_volume": (
                frame["bid_qty"].astype("float64", copy=False) / qty_multiplier
            ).to_numpy(dtype=np.float64, copy=False),
            "ask_price": (
                frame["ask_price"].astype("float64", copy=False) / price_multiplier
            ).to_numpy(dtype=np.float64, copy=False),
            "ask_volume": (
                frame["ask_qty"].astype("float64", copy=False) / qty_multiplier
            ).to_numpy(dtype=np.float64, copy=False),
        }
    )


def parse_date(date_text: str) -> datetime:
    try:
        return datetime.strptime(date_text, "%Y%m%d")
    except ValueError as exc:
        raise ValueError("date must use YYYYMMDD format") from exc


def expand_date_range(start_date: str, end_date: str) -> list[str]:
    start = parse_date(start_date)
    end = parse_date(end_date)
    if end < start:
        raise ValueError("end-date must be >= start-date")
    dates = []
    current = start
    while current <= end:
        dates.append(current.strftime("%Y%m%d"))
        current += timedelta(days=1)
    return dates


def hdf_symbol_from_canonical(symbol: str) -> str:
    return f"{symbol.lower()}_swap"


def load_sources_from_catalog(
    catalog_path: Path,
    symbol: str,
    hdf_symbol: str,
) -> list[HdfSource]:
    exchange_ids = {"binance": EXCHANGE_BINANCE, "gate": EXCHANGE_GATE}
    file_prefixes = {
        "binance": f"binance.{hdf_symbol}",
        "gate": f"gateio.{hdf_symbol}",
    }
    rows_by_exchange: dict[str, dict[str, str]] = {}
    with catalog_path.open(newline="") as handle:
        reader = csv.DictReader(handle)
        for row in reader:
            exchange = row.get("exchange", "")
            if row.get("symbol") == symbol and exchange in exchange_ids:
                rows_by_exchange[exchange] = row

    sources: list[HdfSource] = []
    for exchange in ("binance", "gate"):
        row = rows_by_exchange.get(exchange)
        if row is None:
            raise ValueError(f"missing catalog row for {exchange} {symbol}")
        sources.append(
            HdfSource(
                exchange_name=exchange,
                exchange_id=exchange_ids[exchange],
                file_prefix=file_prefixes[exchange],
                symbol_id=int(row["symbol_id"]),
            )
        )
    return sources


def convert_date(
    *,
    input_dir: Path,
    output_path: Path,
    sources: list[HdfSource],
    date: str,
    hours: Iterable[int] = range(24),
) -> ConvertStats:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    temp_path = output_path.with_name(
        f"{output_path.name}.tmp.{date}.{os.getpid()}"
    )
    stats = ConvertStats(
        date=date,
        output_path=output_path,
        records_by_exchange={source.exchange_name: 0 for source in sources},
    )
    daily_frames: list[pd.DataFrame] = []

    try:
        source_order = 0
        for hour in hours:
            date_hour = f"{date}{hour:02d}"
            for source in sources:
                path = source.path_for_hour(input_dir, date_hour)
                if not path.exists():
                    raise FileNotFoundError(path)
                frame = read_hdf_bbo_frame(path, source, source_order, date_hour)
                source_order += 1
                stats.files_read += 1
                stats.records_by_exchange[source.exchange_name] += len(frame)
                if not frame.empty:
                    daily_frames.append(frame)

        if daily_frames:
            merged = pd.concat(daily_frames, ignore_index=True)
            merged = merged.sort_values(
                by=["exchange_ns", "exchange_id", "source_order", "input_sequence"],
                kind="mergesort",
                ignore_index=True,
            )
        else:
            merged = pd.DataFrame(
                columns=[
                    "exchange_ns",
                    "exchange_id",
                    "source_order",
                    "input_sequence",
                    "symbol_id",
                    "local_ns",
                    "bid_price",
                    "bid_volume",
                    "ask_price",
                    "ask_volume",
                ]
            )

        stats.records_written = len(merged)
        if stats.records_written > 0:
            stats.first_exchange_ns = int(merged["exchange_ns"].iloc[0])
            stats.last_exchange_ns = int(merged["exchange_ns"].iloc[-1])

        output_records = np.zeros(stats.records_written, dtype=BOOK_TICKER_DTYPE)
        output_records["id"] = np.arange(stats.records_written, dtype=np.int64)
        output_records["symbol_id"] = merged["symbol_id"].to_numpy(
            dtype=np.int32, copy=False
        )
        output_records["exchange"] = merged["exchange_id"].to_numpy(
            dtype=np.uint8, copy=False
        )
        output_records["exchange_ns"] = merged["exchange_ns"].to_numpy(
            dtype=np.int64, copy=False
        )
        output_records["event_ns"] = merged["exchange_ns"].to_numpy(
            dtype=np.int64, copy=False
        )
        output_records["local_ns"] = merged["local_ns"].to_numpy(
            dtype=np.int64, copy=False
        )
        output_records["bid_price"] = merged["bid_price"].to_numpy(
            dtype=np.float64, copy=False
        )
        output_records["bid_volume"] = merged["bid_volume"].to_numpy(
            dtype=np.float64, copy=False
        )
        output_records["ask_price"] = merged["ask_price"].to_numpy(
            dtype=np.float64, copy=False
        )
        output_records["ask_volume"] = merged["ask_volume"].to_numpy(
            dtype=np.float64, copy=False
        )

        with temp_path.open("wb") as output:
            typed_binary.write_header(output, "book_ticker")
            output_records.tofile(output)

        expected_size = (
            typed_binary.HEADER_SIZE
            + stats.records_written * typed_binary.book_ticker_dtype().itemsize
        )
        actual_size = temp_path.stat().st_size
        if actual_size != expected_size:
            raise ValueError(
                f"{temp_path}: expected {expected_size} bytes, got {actual_size}"
            )
        temp_path.replace(output_path)
        return stats
    except Exception:
        temp_path.unlink(missing_ok=True)
        raise


def default_input_dir() -> Path:
    return Path.home() / "download"


def default_output_dir(symbol: str) -> Path:
    return Path.home() / "tardis" / "merged_book_ticker_hdf" / symbol


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Convert xex_mars HDF BBO files to Aquila BookTicker binary."
    )
    parser.add_argument("--input-dir", type=Path, default=default_input_dir())
    parser.add_argument(
        "--instrument-catalog",
        type=Path,
        default=Path("config/instruments/usdt_future_universe.csv"),
    )
    parser.add_argument("--symbol", default="ORDI_USDT")
    parser.add_argument("--hdf-symbol", default=None)
    parser.add_argument("--start-date", required=True)
    parser.add_argument("--end-date", required=True)
    parser.add_argument("--output-dir", type=Path, default=None)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    symbol = args.symbol
    hdf_symbol = args.hdf_symbol or hdf_symbol_from_canonical(symbol)
    output_dir = args.output_dir or default_output_dir(symbol)
    sources = load_sources_from_catalog(
        args.instrument_catalog,
        symbol=symbol,
        hdf_symbol=hdf_symbol,
    )

    for date in expand_date_range(args.start_date, args.end_date):
        stats = convert_date(
            input_dir=args.input_dir,
            output_path=output_dir / f"{date}.bin",
            sources=sources,
            date=date,
        )
        by_exchange = " ".join(
            f"{exchange}_records={count}"
            for exchange, count in stats.records_by_exchange.items()
        )
        file_size_bytes = (
            typed_binary.HEADER_SIZE
            + stats.records_written * typed_binary.book_ticker_dtype().itemsize
        )
        print(
            f"converted date={date} output={stats.output_path} "
            f"records={stats.records_written} {by_exchange} files={stats.files_read} "
            f"first_exchange_ns={stats.first_exchange_ns or 0} "
            f"last_exchange_ns={stats.last_exchange_ns or 0} "
            f"file_size_bytes={file_size_bytes}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
