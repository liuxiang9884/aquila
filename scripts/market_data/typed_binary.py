#!/usr/bin/env python3

import struct
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO, Iterator

import numpy as np


FORMAT_NAME = "aquila.market_data.binary"
MAGIC_BYTES = b"AQMD"
VERSION = 1
HEADER_SIZE = 16
FLAGS = 0
BOOK_TICKER_FEED_TYPE = 1
TRADE_FEED_TYPE = 2

_MAGIC_INT = int.from_bytes(MAGIC_BYTES, "little")
_HEADER_STRUCT = struct.Struct("<IHHHHI")


@dataclass(frozen=True)
class MarketDataBinaryHeader:
    magic: int
    version: int
    header_size: int
    feed_type: int
    record_size: int
    flags: int

    @property
    def feed(self) -> str:
        return feed_name_from_type(self.feed_type)


def book_ticker_dtype() -> np.dtype:
    return np.dtype(
        {
            "names": [
                "id",
                "symbol_id",
                "exchange",
                "exchange_ns",
                "event_ns",
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
                "<i8",
                "<f8",
                "<f8",
                "<f8",
                "<f8",
            ],
            "offsets": [0, 8, 12, 16, 24, 32, 40, 48, 56, 64],
            "itemsize": 72,
        }
    )


def trade_dtype() -> np.dtype:
    return np.dtype(
        {
            "names": [
                "id",
                "symbol_id",
                "exchange",
                "side",
                "reserved",
                "exchange_ns",
                "event_ns",
                "local_ns",
                "price",
                "volume",
                "batch_index",
                "batch_count",
            ],
            "formats": [
                "<i8",
                "<i4",
                "u1",
                "u1",
                "<u2",
                "<i8",
                "<i8",
                "<i8",
                "<f8",
                "<f8",
                "<u4",
                "<u4",
            ],
            "offsets": [0, 8, 12, 13, 14, 16, 24, 32, 40, 48, 56, 60],
            "itemsize": 64,
        }
    )


def _feed_name(feed: str) -> str:
    name = str(feed).strip().lower()
    if name in ("book_ticker", "trade"):
        return name
    raise ValueError(f"unsupported market data feed: {feed}")


def dtype_for_feed(feed: str) -> np.dtype:
    name = _feed_name(feed)
    if name == "book_ticker":
        return book_ticker_dtype()
    if name == "trade":
        return trade_dtype()
    raise ValueError(f"unsupported market data feed: {feed}")


def feed_type_for_name(feed: str) -> int:
    name = _feed_name(feed)
    if name == "book_ticker":
        return BOOK_TICKER_FEED_TYPE
    if name == "trade":
        return TRADE_FEED_TYPE
    raise ValueError(f"unsupported market data feed: {feed}")


def feed_name_from_type(feed_type: int) -> str:
    if feed_type == BOOK_TICKER_FEED_TYPE:
        return "book_ticker"
    if feed_type == TRADE_FEED_TYPE:
        return "trade"
    return "unknown"


def make_header(feed: str) -> MarketDataBinaryHeader:
    return MarketDataBinaryHeader(
        magic=_MAGIC_INT,
        version=VERSION,
        header_size=HEADER_SIZE,
        feed_type=feed_type_for_name(feed),
        record_size=dtype_for_feed(feed).itemsize,
        flags=FLAGS,
    )


def _source_text(source_name: str | Path) -> str:
    return str(source_name)


def validate_header(
    header: MarketDataBinaryHeader, feed: str, source_name: str | Path
) -> None:
    source = _source_text(source_name)
    if header.magic != _MAGIC_INT:
        raise ValueError(
            f"binary data file '{source}' has invalid market data magic "
            f"0x{header.magic:08x}"
        )
    if header.version != VERSION:
        raise ValueError(
            f"binary data file '{source}' has unsupported market data version "
            f"{header.version}"
        )
    if header.header_size != HEADER_SIZE:
        raise ValueError(
            f"binary data file '{source}' has unsupported market data header size "
            f"{header.header_size}"
        )
    if header.flags != FLAGS:
        raise ValueError(
            f"binary data file '{source}' has unsupported market data flags "
            f"0x{header.flags:08x}"
        )

    expected_feed_type = feed_type_for_name(feed)
    if header.feed_type != expected_feed_type:
        raise ValueError(
            f"binary data file '{source}' feed mismatch: "
            f"header={feed_name_from_type(header.feed_type)}, expected={_feed_name(feed)}"
        )

    expected_record_size = dtype_for_feed(feed).itemsize
    if header.record_size != expected_record_size:
        raise ValueError(
            f"binary data file '{source}' {_feed_name(feed)} record size mismatch: "
            f"header={header.record_size}, expected={expected_record_size}"
        )


def _read_exact(stream: BinaryIO, size: int) -> bytes:
    chunks: list[bytes] = []
    remaining = size
    while remaining > 0:
        chunk = stream.read(remaining)
        if not chunk:
            break
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def _parse_header(data: bytes, source_name: str | Path) -> MarketDataBinaryHeader:
    if len(data) != HEADER_SIZE:
        source = _source_text(source_name)
        raise ValueError(f"binary data file '{source}' is missing market data header")
    return MarketDataBinaryHeader(*_HEADER_STRUCT.unpack(data))


def read_header(path: Path) -> MarketDataBinaryHeader:
    with Path(path).open("rb") as handle:
        return _parse_header(handle.read(HEADER_SIZE), path)


def write_header(handle: BinaryIO, feed: str) -> MarketDataBinaryHeader:
    header = make_header(feed)
    handle.write(
        _HEADER_STRUCT.pack(
            header.magic,
            header.version,
            header.header_size,
            header.feed_type,
            header.record_size,
            header.flags,
        )
    )
    return header


def _checked_record_count_from_header(
    path: Path, feed: str, header: MarketDataBinaryHeader
) -> int:
    validate_header(header, feed, path)
    file_size = path.stat().st_size
    if file_size < header.header_size:
        raise ValueError(
            f"binary data file '{path}' size {file_size} is smaller than "
            f"market data header {header.header_size}"
        )
    payload_bytes = file_size - header.header_size
    if header.record_size == 0 or payload_bytes % header.record_size != 0:
        raise ValueError(
            f"binary data file '{path}' payload size {payload_bytes} is not a "
            f"multiple of {_feed_name(feed)} record size {header.record_size}"
        )
    return payload_bytes // header.record_size


def checked_record_count(path: Path, feed: str) -> int:
    path = Path(path)
    return _checked_record_count_from_header(path, feed, read_header(path))


def load_records(path: Path, feed: str) -> np.ndarray:
    path = Path(path)
    header = read_header(path)
    record_count = _checked_record_count_from_header(path, feed, header)
    dtype = dtype_for_feed(feed)
    with path.open("rb") as handle:
        handle.seek(header.header_size)
        return np.fromfile(handle, dtype=dtype, count=record_count)


def memmap_records(path: Path, feed: str) -> np.memmap:
    path = Path(path)
    header = read_header(path)
    record_count = _checked_record_count_from_header(path, feed, header)
    return np.memmap(
        path,
        dtype=dtype_for_feed(feed),
        mode="r",
        offset=header.header_size,
        shape=(record_count,),
    )


def iter_record_chunks(
    path: Path, feed: str, chunk_records: int
) -> Iterator[np.ndarray]:
    if chunk_records <= 0:
        raise ValueError("chunk_records must be positive")

    path = Path(path)
    header = read_header(path)
    record_count = _checked_record_count_from_header(path, feed, header)
    dtype = dtype_for_feed(feed)
    remaining = record_count
    with path.open("rb") as handle:
        handle.seek(header.header_size)
        while remaining > 0:
            to_read = min(chunk_records, remaining)
            records = np.fromfile(handle, dtype=dtype, count=to_read)
            if len(records) != to_read:
                raise ValueError(
                    f"{path} changed while reading: expected {to_read} records, "
                    f"got {len(records)}"
                )
            remaining -= to_read
            yield records


def iter_record_chunks_from_stream(
    stream: BinaryIO, feed: str, chunk_records: int, source_name: str | Path
) -> Iterator[np.ndarray]:
    if chunk_records <= 0:
        raise ValueError("chunk_records must be positive")

    header = _parse_header(_read_exact(stream, HEADER_SIZE), source_name)
    validate_header(header, feed, source_name)
    dtype = dtype_for_feed(feed)
    chunk_bytes = chunk_records * header.record_size
    pending = b""

    while True:
        data = stream.read(chunk_bytes)
        if not data:
            break
        data = pending + data
        complete_size = (len(data) // header.record_size) * header.record_size
        if complete_size:
            yield np.frombuffer(data[:complete_size], dtype=dtype).copy()
        pending = data[complete_size:]

    if pending:
        raise ValueError(
            f"binary data stream '{source_name}' has {len(pending)} trailing "
            f"bytes after {_feed_name(feed)} records"
        )


def write_records(path: Path, feed: str, records: np.ndarray) -> None:
    path = Path(path)
    if path.parent != Path("."):
        path.parent.mkdir(parents=True, exist_ok=True)
    dtype = dtype_for_feed(feed)
    typed_records = np.asarray(records, dtype=dtype)
    with path.open("wb") as handle:
        write_header(handle, feed)
        typed_records.tofile(handle)
