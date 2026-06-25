#!/usr/bin/env python3

import argparse
import json
import math
import subprocess
import sys
import textwrap
from pathlib import Path
from typing import Iterator, Sequence

import numpy as np


SCRIPT_DIR = Path(__file__).resolve().parent
MARKET_DATA_SCRIPT_DIR = SCRIPT_DIR.parent / "market_data"
if str(MARKET_DATA_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(MARKET_DATA_SCRIPT_DIR))

from analyze_book_ticker_latency import book_ticker_dtype  # noqa: E402


EXCHANGE_IDS = {
    "binance": 0,
    "okx": 1,
    "gate": 2,
    "gateio": 2,
    "bybit": 3,
    "bitget": 4,
    "coinbase": 5,
}

SPREAD_PERCENTILES = (50.0, 95.0, 99.0, 100.0)


def _normalize_exchange(exchange: str) -> str:
    normalized = exchange.strip().lower()
    if normalized.startswith("k"):
        normalized = normalized[1:]
    return normalized


def _exchange_id(exchange: str) -> int:
    normalized = _normalize_exchange(exchange)
    if normalized not in EXCHANGE_IDS:
        raise ValueError(f"unsupported exchange: {exchange}")
    return EXCHANGE_IDS[normalized]


def _iter_raw_chunks(path: Path, *, dtype: np.dtype, chunk_records: int):
    chunk_bytes = chunk_records * dtype.itemsize
    with path.open("rb") as handle:
        pending = b""
        while True:
            data = handle.read(chunk_bytes)
            if not data:
                break
            data = pending + data
            complete_size = (len(data) // dtype.itemsize) * dtype.itemsize
            if complete_size:
                yield np.frombuffer(data[:complete_size], dtype=dtype).copy()
            pending = data[complete_size:]
        if pending:
            raise ValueError(
                f"{path} has {len(pending)} trailing bytes after BookTicker records"
            )


def _iter_zstd_chunks(path: Path, *, dtype: np.dtype, chunk_records: int):
    chunk_bytes = chunk_records * dtype.itemsize
    process = subprocess.Popen(
        ["zstd", "-dc", str(path)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert process.stdout is not None
    pending = b""
    try:
        while True:
            data = process.stdout.read(chunk_bytes)
            if not data:
                break
            data = pending + data
            complete_size = (len(data) // dtype.itemsize) * dtype.itemsize
            if complete_size:
                yield np.frombuffer(data[:complete_size], dtype=dtype).copy()
            pending = data[complete_size:]
    finally:
        process.stdout.close()
    stderr = process.stderr.read().decode("utf-8", errors="replace")
    return_code = process.wait()
    if return_code != 0:
        raise ValueError(f"zstd failed for {path}: {stderr.strip()}")
    if pending:
        raise ValueError(
            f"{path} has {len(pending)} trailing bytes after BookTicker records"
        )


def iter_book_ticker_chunks(
    path: Path, *, dtype: np.dtype, chunk_records: int
) -> Iterator[np.ndarray]:
    if chunk_records <= 0:
        raise ValueError("chunk_records must be positive")
    if path.name.endswith(".zst"):
        yield from _iter_zstd_chunks(path, dtype=dtype, chunk_records=chunk_records)
    else:
        yield from _iter_raw_chunks(path, dtype=dtype, chunk_records=chunk_records)


def _collect_records(
    input_paths: Sequence[Path],
    *,
    symbol_id: int,
    exchange_id: int,
    chunk_records: int,
) -> np.ndarray:
    dtype = book_ticker_dtype()
    chunks: list[np.ndarray] = []
    for path in input_paths:
        for records in iter_book_ticker_chunks(path, dtype=dtype, chunk_records=chunk_records):
            mask = (records["symbol_id"] == symbol_id) & (
                records["exchange"] == exchange_id
            )
            if np.any(mask):
                chunks.append(records[mask].copy())
    if not chunks:
        return np.zeros(0, dtype=dtype)
    return np.concatenate(chunks)


def _freshness_summary(records: np.ndarray, label: str) -> dict:
    if len(records) == 0:
        raise ValueError(f"no {label} BookTicker records")
    latency_ns = records["local_ns"].astype(np.int64) - records["exchange_ns"].astype(
        np.int64
    )
    negative_count = int(np.count_nonzero(latency_ns < 0))
    non_negative = latency_ns[latency_ns >= 0]
    if len(non_negative) == 0:
        raise ValueError(f"no non-negative {label} BookTicker latency samples")
    latency_ms = non_negative.astype(np.float64) / 1_000_000.0
    mean_ms = float(np.mean(latency_ms))
    std_ms = float(np.std(latency_ms))
    return {
        "sample_count": int(len(non_negative)),
        "negative_latency_count": negative_count,
        "mean_ms": mean_ms,
        "std_ms": std_ms,
        "threshold_ms": int(math.ceil(mean_ms + 3.0 * std_ms)),
    }


def _lag_bbo_spread_summary(records: np.ndarray, percentile: float) -> dict:
    if len(records) == 0:
        raise ValueError("no lag BookTicker records")
    bid = records["bid_price"].astype(np.float64)
    ask = records["ask_price"].astype(np.float64)
    mid = (bid + ask) / 2.0
    latency_ns = records["local_ns"].astype(np.int64) - records["exchange_ns"].astype(
        np.int64
    )
    mask = (
        (latency_ns >= 0)
        & (bid > 0.0)
        & (ask > 0.0)
        & (ask >= bid)
        & (mid > 0.0)
    )
    if not np.any(mask):
        raise ValueError("no valid lag BBO spread samples")
    spread_pct = (ask[mask] - bid[mask]) / mid[mask]
    percentiles = {
        f"p{value:g}": float(np.percentile(spread_pct, value))
        for value in SPREAD_PERCENTILES
    }
    selected = float(np.percentile(spread_pct, percentile))
    return {
        "sample_count": int(len(spread_pct)),
        "method": f"lag_bbo_spread_pct_p{percentile:g}",
        "percentile": float(percentile),
        "value": selected,
        "spread_percentiles": percentiles,
        "candidate_percentiles": {
            "p95": percentiles["p95"],
            "p99": percentiles["p99"],
        },
        "max": float(np.max(spread_pct)),
        "mean": float(np.mean(spread_pct)),
    }


def _lag_bbo_slippage_summary(
    records: np.ndarray,
    *,
    entry_buffer_pct: float,
    normal_close_buffer_pct: float,
    price_tick: float,
) -> dict:
    if not math.isfinite(price_tick) or price_tick <= 0.0:
        raise ValueError("lag_price_tick must be positive")
    bid = records["bid_price"].astype(np.float64)
    ask = records["ask_price"].astype(np.float64)
    latency_ns = records["local_ns"].astype(np.int64) - records["exchange_ns"].astype(
        np.int64
    )
    mask = (
        (latency_ns >= 0)
        & (bid > 0.0)
        & (ask > 0.0)
        & (ask >= bid)
    )
    if not np.any(mask):
        raise ValueError("no valid lag BBO price samples")
    max_bid_price = float(np.max(bid[mask]))
    max_ask_price = float(np.max(ask[mask]))

    def ticks_for(price: float, buffer_pct: float) -> int:
        if buffer_pct <= 0.0:
            return 0
        return int(math.ceil(price * buffer_pct / price_tick))

    open_long_slippage_ticks = ticks_for(max_ask_price, entry_buffer_pct)
    open_short_slippage_ticks = ticks_for(max_bid_price, entry_buffer_pct)
    close_long_slippage_ticks = ticks_for(max_bid_price, normal_close_buffer_pct)
    close_short_slippage_ticks = ticks_for(max_ask_price, normal_close_buffer_pct)
    return {
        "price_tick": float(price_tick),
        "reference_price_method": "lag_bbo_max",
        "reference_price": max(max_bid_price, max_ask_price),
        "max_bid_price": max_bid_price,
        "max_ask_price": max_ask_price,
        "entry_buffer_pct": float(entry_buffer_pct),
        "normal_close_buffer_pct": float(normal_close_buffer_pct),
        "open_long_slippage_ticks": open_long_slippage_ticks,
        "open_short_slippage_ticks": open_short_slippage_ticks,
        "close_long_slippage_ticks": close_long_slippage_ticks,
        "close_short_slippage_ticks": close_short_slippage_ticks,
        "open_slippage_ticks": max(
            open_long_slippage_ticks, open_short_slippage_ticks
        ),
        "close_slippage_ticks": max(
            close_long_slippage_ticks, close_short_slippage_ticks
        ),
    }


def generate_params(
    *,
    input_paths: Sequence[Path],
    symbol_id: int,
    lead_exchange: str,
    lag_exchange: str,
    buffer_percentile: float,
    lag_price_tick: float | None = None,
    chunk_records: int = 1_000_000,
) -> dict:
    if not input_paths:
        raise ValueError("input_paths must not be empty")
    if buffer_percentile < 0.0 or buffer_percentile > 100.0:
        raise ValueError("buffer_percentile must be between 0 and 100")

    lead_records = _collect_records(
        input_paths,
        symbol_id=symbol_id,
        exchange_id=_exchange_id(lead_exchange),
        chunk_records=chunk_records,
    )
    lag_records = _collect_records(
        input_paths,
        symbol_id=symbol_id,
        exchange_id=_exchange_id(lag_exchange),
        chunk_records=chunk_records,
    )

    lead_freshness = _freshness_summary(lead_records, "lead")
    lag_freshness = _freshness_summary(lag_records, "lag")
    spread = _lag_bbo_spread_summary(lag_records, buffer_percentile)

    params = {
        "symbol_id": int(symbol_id),
        "lead_exchange": _normalize_exchange(lead_exchange),
        "lag_exchange": _normalize_exchange(lag_exchange),
        "taker_buffer": {
            "entry_fixed_pct": spread["value"],
            "normal_close_fixed_pct": spread["value"],
            "source": "generated",
            "sample_count": spread["sample_count"],
            "method": spread["method"],
            "selected_percentile": spread["percentile"],
            "spread_percentiles": spread["spread_percentiles"],
            "candidate_percentiles": spread["candidate_percentiles"],
            "max": spread["max"],
            "mean": spread["mean"],
        },
        "freshness": {
            "lead_threshold_ms": lead_freshness["threshold_ms"],
            "lag_threshold_ms": lag_freshness["threshold_ms"],
            "lead_sample_count": lead_freshness["sample_count"],
            "lag_sample_count": lag_freshness["sample_count"],
            "lead_negative_latency_count": lead_freshness["negative_latency_count"],
            "lag_negative_latency_count": lag_freshness["negative_latency_count"],
            "lead_mean_ms": lead_freshness["mean_ms"],
            "lead_std_ms": lead_freshness["std_ms"],
            "lag_mean_ms": lag_freshness["mean_ms"],
            "lag_std_ms": lag_freshness["std_ms"],
        },
    }
    if lag_price_tick is not None:
        params["slippage"] = _lag_bbo_slippage_summary(
            lag_records,
            entry_buffer_pct=spread["value"],
            normal_close_buffer_pct=spread["value"],
            price_tick=lag_price_tick,
        )
    return params


def render_toml_patch(params: dict) -> str:
    taker = params["taker_buffer"]
    blocks = []
    if "slippage" in params:
        slippage = params["slippage"]
        blocks.append(
            textwrap.dedent(
                f"""
                [lead_lag.pairs.execute]
                open_slippage_ticks = {slippage["open_slippage_ticks"]}
                close_slippage_ticks = {slippage["close_slippage_ticks"]}
                """
            ).strip()
        )
    blocks.append(
        textwrap.dedent(
            f"""
            [lead_lag.pairs.execute.taker_buffer]
            mode = "shadow"
            entry_fixed_pct = {taker["entry_fixed_pct"]:.12g}
            normal_close_fixed_pct = {taker["normal_close_fixed_pct"]:.12g}
            exclude_from_cost_model = false
            source = "generated"
            """
        ).strip()
    )
    return (
        "\n\n".join(blocks)
        + "\n"
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Generate fixed LeadLag taker buffer config "
            "from startup or historical BookTicker binaries."
        )
    )
    parser.add_argument(
        "--input",
        action="append",
        required=True,
        type=Path,
        help="BookTicker .bin or .bin.zst input; pass multiple times for adjacent files",
    )
    parser.add_argument("--symbol-id", required=True, type=int)
    parser.add_argument("--lead-exchange", required=True)
    parser.add_argument("--lag-exchange", required=True)
    parser.add_argument("--buffer-percentile", required=True, type=float)
    parser.add_argument(
        "--lag-price-tick",
        type=float,
        help="Lag exchange price tick; when set, generate open/close slippage ticks",
    )
    parser.add_argument("--chunk-records", type=int, default=1_000_000)
    parser.add_argument("--json-output", type=Path)
    parser.add_argument("--toml-output", type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    params = generate_params(
        input_paths=args.input,
        symbol_id=args.symbol_id,
        lead_exchange=args.lead_exchange,
        lag_exchange=args.lag_exchange,
        buffer_percentile=args.buffer_percentile,
        lag_price_tick=args.lag_price_tick,
        chunk_records=args.chunk_records,
    )
    json_text = json.dumps(params, indent=2, sort_keys=True)
    if args.json_output is not None:
        args.json_output.write_text(json_text + "\n", encoding="utf-8")
    else:
        print(json_text)
    if args.toml_output is not None:
        args.toml_output.write_text(render_toml_patch(params), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
