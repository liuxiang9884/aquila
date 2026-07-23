#!/home/liuxiang/dev/pyenv/lx/bin/python

import argparse
import csv
import datetime as dt
import json
import math
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import asdict, dataclass
from decimal import Decimal, InvalidOperation
from pathlib import Path
from typing import Any, Iterable

DEFAULT_CATALOG = Path("config/instruments/usdt_future_universe.csv")
DEFAULT_OUTPUT_ROOT = Path("/home/liuxiang/tmp")
DEFAULT_GATE_BASE_URL = "https://api.gateio.ws/api/v4"
DEFAULT_BINANCE_BASE_URL = "https://fapi.binance.com"
USER_AGENT = "aquila-common-usdt-perp-kline-volatility/1.0"
INTERVAL_MS = 60_000
DEFAULT_VOL_WINDOWS = (30, 60)


@dataclass(frozen=True)
class CommonSymbol:
    symbol: str
    gate_symbol: str
    binance_symbol: str
    gate_tick_size: str
    binance_tick_size: str


@dataclass(frozen=True)
class KlineRow:
    exchange: str
    symbol: str
    exchange_symbol: str
    open_time_ms: int
    close_time_ms: int
    open: float
    high: float
    low: float
    close: float
    volume: float
    quote_volume: float
    closed: bool


KLINE_COLUMNS = [
    "exchange",
    "symbol",
    "exchange_symbol",
    "open_time_ms",
    "close_time_ms",
    "open",
    "high",
    "low",
    "close",
    "volume",
    "quote_volume",
    "closed",
]


def exchange_result_columns(windows: Iterable[int]) -> list[str]:
    normalized = sorted({int(window) for window in windows})
    columns = ["exchange", "symbol", "exchange_symbol", "tick_size"]
    columns.extend([f"vol_{window}m_bps" for window in normalized])
    columns.extend([f"quote_volume_{window}m" for window in normalized])
    columns.extend([f"volume_{window}m" for window in normalized])
    columns.extend([f"valid_{window}m" for window in normalized])
    columns.extend(
        [
            "close_count",
            "latest_closed_open_time_ms",
            "reference_price",
        ]
    )
    return columns


EXCHANGE_RESULT_COLUMNS = exchange_result_columns(DEFAULT_VOL_WINDOWS)


def summary_columns(windows: Iterable[int]) -> list[str]:
    normalized = sorted({int(window) for window in windows})
    columns = ["symbol"]
    for exchange in ("gate", "binance"):
        for window in normalized:
            columns.append(f"{exchange}_vol_{window}m_bps")
        for window in normalized:
            columns.append(f"{exchange}_valid_{window}m")
        columns.append(f"{exchange}_close_count")
    largest = max(normalized) if normalized else 0
    columns.extend([f"max_vol_{largest}m_bps", f"min_vol_{largest}m_bps"])
    return columns


def _to_float(value: Any) -> float:
    if value is None or value == "":
        return 0.0
    return float(value)


def _to_int(value: Any) -> int:
    return int(value)


def _now_ms() -> int:
    return int(time.time() * 1000)


def _bool_text(value: bool) -> str:
    return "true" if value else "false"


def normalize_decimal_text(value: Any) -> str:
    text = str(value or "").strip()
    if not text:
        return ""
    try:
        decimal_value = Decimal(text)
    except InvalidOperation:
        return text
    normalized = format(decimal_value.normalize(), "f")
    if "." in normalized:
        normalized = normalized.rstrip("0").rstrip(".")
    return normalized or "0"


def load_common_symbols(
    catalog_path: Path, requested_symbols: set[str] | None = None
) -> list[CommonSymbol]:
    by_symbol: dict[str, dict[str, str]] = {}
    with catalog_path.open(newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            symbol = str(row.get("symbol", "")).strip().upper()
            exchange = str(row.get("exchange", "")).strip().lower()
            exchange_symbol = str(row.get("exchange_symbol", "")).strip()
            if not symbol or exchange not in ("gate", "binance") or not exchange_symbol:
                continue
            if requested_symbols is not None and symbol not in requested_symbols:
                continue
            entry = by_symbol.setdefault(symbol, {})
            entry[exchange] = exchange_symbol
            entry[f"{exchange}_tick_size"] = normalize_decimal_text(row.get("price_tick", ""))

    result: list[CommonSymbol] = []
    for symbol in sorted(by_symbol):
        entry = by_symbol[symbol]
        gate_symbol = entry.get("gate")
        binance_symbol = entry.get("binance")
        gate_tick_size = entry.get("gate_tick_size")
        binance_tick_size = entry.get("binance_tick_size")
        if gate_symbol and binance_symbol and gate_tick_size and binance_tick_size:
            result.append(
                CommonSymbol(
                    symbol=symbol,
                    gate_symbol=gate_symbol,
                    binance_symbol=binance_symbol,
                    gate_tick_size=gate_tick_size,
                    binance_tick_size=binance_tick_size,
                )
            )
    return result


def parse_symbols_arg(symbols_text: str | None) -> set[str] | None:
    if not symbols_text:
        return None
    result = {
        item.strip().upper()
        for item in symbols_text.split(",")
        if item.strip()
    }
    return result or None


def _json_get(url: str, timeout: float) -> Any:
    request = urllib.request.Request(
        url,
        headers={"Accept": "application/json", "User-Agent": USER_AGENT},
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"HTTP {exc.code} for {url}: {detail}") from exc
    except urllib.error.URLError as exc:
        raise RuntimeError(f"request failed for {url}: {exc}") from exc


def fetch_binance_klines(
    exchange_symbol: str,
    limit: int,
    base_url: str = DEFAULT_BINANCE_BASE_URL,
    timeout: float = 8.0,
) -> list[Any]:
    params = urllib.parse.urlencode(
        {"symbol": exchange_symbol, "interval": "1m", "limit": limit}
    )
    url = f"{base_url.rstrip('/')}/fapi/v1/klines?{params}"
    payload = _json_get(url, timeout)
    if not isinstance(payload, list):
        raise RuntimeError(f"Binance kline query returned non-list payload for {exchange_symbol}")
    return payload


def fetch_gate_klines(
    exchange_symbol: str,
    limit: int,
    base_url: str = DEFAULT_GATE_BASE_URL,
    timeout: float = 8.0,
) -> list[Any]:
    params = urllib.parse.urlencode(
        {"contract": exchange_symbol, "interval": "1m", "limit": limit}
    )
    url = f"{base_url.rstrip('/')}/futures/usdt/candlesticks?{params}"
    payload = _json_get(url, timeout)
    if not isinstance(payload, list):
        raise RuntimeError(f"Gate kline query returned non-list payload for {exchange_symbol}")
    return payload


def parse_binance_kline(
    symbol: str, exchange_symbol: str, payload: list[Any], now_ms: int | None = None
) -> KlineRow:
    timestamp_ms = _now_ms() if now_ms is None else now_ms
    open_time_ms = _to_int(payload[0])
    close_time_ms = _to_int(payload[6])
    return KlineRow(
        exchange="binance",
        symbol=symbol,
        exchange_symbol=exchange_symbol,
        open_time_ms=open_time_ms,
        close_time_ms=close_time_ms,
        open=_to_float(payload[1]),
        high=_to_float(payload[2]),
        low=_to_float(payload[3]),
        close=_to_float(payload[4]),
        volume=_to_float(payload[5]),
        quote_volume=_to_float(payload[7]),
        closed=close_time_ms < timestamp_ms,
    )


def parse_gate_kline(
    symbol: str, exchange_symbol: str, payload: Any, now_ms: int | None = None
) -> KlineRow:
    timestamp_ms = _now_ms() if now_ms is None else now_ms
    if isinstance(payload, dict):
        open_time_ms = _to_int(payload.get("t")) * 1000
        open_price = payload.get("o")
        high = payload.get("h")
        low = payload.get("l")
        close = payload.get("c")
        volume = payload.get("v")
        quote_volume = payload.get("sum", "")
    elif isinstance(payload, list):
        open_time_ms = _to_int(payload[0]) * 1000
        volume = payload[1]
        close = payload[2]
        high = payload[3]
        low = payload[4]
        open_price = payload[5]
        quote_volume = payload[6] if len(payload) > 6 else ""
    else:
        raise RuntimeError(f"unsupported Gate kline payload type: {type(payload).__name__}")

    close_time_ms = open_time_ms + INTERVAL_MS - 1
    return KlineRow(
        exchange="gate",
        symbol=symbol,
        exchange_symbol=exchange_symbol,
        open_time_ms=open_time_ms,
        close_time_ms=close_time_ms,
        open=_to_float(open_price),
        high=_to_float(high),
        low=_to_float(low),
        close=_to_float(close),
        volume=_to_float(volume),
        quote_volume=_to_float(quote_volume),
        closed=close_time_ms < timestamp_ms,
    )


def realized_vol_bps(closes: list[float], window_minutes: int) -> float | None:
    if window_minutes <= 0:
        raise ValueError("window_minutes must be positive")
    if len(closes) < window_minutes + 1:
        return None
    window_closes = closes[-(window_minutes + 1):]
    sum_squared = 0.0
    for previous, current in zip(window_closes, window_closes[1:]):
        if previous <= 0.0 or current <= 0.0:
            return None
        ret = math.log(current / previous)
        sum_squared += ret * ret
    return math.sqrt(sum_squared) * 10000.0


def closed_closes(rows: Iterable[KlineRow]) -> list[float]:
    sorted_rows = sorted(
        (row for row in rows if row.closed),
        key=lambda row: row.open_time_ms,
    )
    return [row.close for row in sorted_rows]


def closed_rows(rows: Iterable[KlineRow]) -> list[KlineRow]:
    return sorted(
        (row for row in rows if row.closed),
        key=lambda row: row.open_time_ms,
    )


def _format_float(value: float | None) -> str:
    if value is None:
        return ""
    return f"{value:.6f}"


def _exchange_symbol(common_symbol: CommonSymbol, exchange: str) -> str:
    if exchange == "gate":
        return common_symbol.gate_symbol
    if exchange == "binance":
        return common_symbol.binance_symbol
    raise ValueError(f"unsupported exchange: {exchange}")


def _tick_size(common_symbol: CommonSymbol, exchange: str) -> str:
    if exchange == "gate":
        return common_symbol.gate_tick_size
    if exchange == "binance":
        return common_symbol.binance_tick_size
    raise ValueError(f"unsupported exchange: {exchange}")


def _latest_sum(rows: list[KlineRow], window: int, field: str) -> float | None:
    if len(rows) < window:
        return None
    selected_rows = rows[-window:]
    if field == "volume":
        return sum(row.volume for row in selected_rows)
    if field == "quote_volume":
        return sum(row.quote_volume for row in selected_rows)
    raise ValueError(f"unsupported sum field: {field}")


def build_summary_rows(
    symbols: Iterable[CommonSymbol],
    gate_rows_by_symbol: dict[str, list[KlineRow]],
    binance_rows_by_symbol: dict[str, list[KlineRow]],
    windows: list[int],
) -> list[dict[str, Any]]:
    normalized_windows = sorted({int(window) for window in windows})
    largest = max(normalized_windows)
    rows: list[dict[str, Any]] = []
    for symbol in symbols:
        gate_closes = closed_closes(gate_rows_by_symbol.get(symbol.symbol, []))
        binance_closes = closed_closes(binance_rows_by_symbol.get(symbol.symbol, []))
        row: dict[str, Any] = {"symbol": symbol.symbol}

        largest_values: list[float] = []
        for exchange, closes in (("gate", gate_closes), ("binance", binance_closes)):
            vol_by_window: dict[int, float | None] = {}
            for window in normalized_windows:
                vol = realized_vol_bps(closes, window)
                vol_by_window[window] = vol
                row[f"{exchange}_vol_{window}m_bps"] = _format_float(vol)
            for window in normalized_windows:
                row[f"{exchange}_valid_{window}m"] = _bool_text(
                    vol_by_window[window] is not None
                )
            row[f"{exchange}_close_count"] = len(closes)
            largest_vol = vol_by_window[largest]
            if largest_vol is not None:
                largest_values.append(largest_vol)

        row[f"max_vol_{largest}m_bps"] = _format_float(
            max(largest_values) if largest_values else None
        )
        row[f"min_vol_{largest}m_bps"] = _format_float(
            min(largest_values) if largest_values else None
        )
        rows.append(row)
    return rows


def build_exchange_result_rows(
    exchange: str,
    symbols: Iterable[CommonSymbol],
    rows_by_symbol: dict[str, list[KlineRow]],
    windows: list[int],
) -> list[dict[str, Any]]:
    normalized_windows = sorted({int(window) for window in windows})
    result_rows: list[dict[str, Any]] = []
    for common_symbol in symbols:
        rows = closed_rows(rows_by_symbol.get(common_symbol.symbol, []))
        closes = [row.close for row in rows]
        latest_row = rows[-1] if rows else None
        row: dict[str, Any] = {
            "exchange": exchange,
            "symbol": common_symbol.symbol,
            "exchange_symbol": _exchange_symbol(common_symbol, exchange),
            "tick_size": _tick_size(common_symbol, exchange),
        }

        vol_by_window: dict[int, float | None] = {}
        for window in normalized_windows:
            vol_by_window[window] = realized_vol_bps(closes, window)
            row[f"vol_{window}m_bps"] = _format_float(vol_by_window[window])
        for window in normalized_windows:
            row[f"quote_volume_{window}m"] = _format_float(
                _latest_sum(rows, window, "quote_volume")
            )
        for window in normalized_windows:
            row[f"volume_{window}m"] = _format_float(
                _latest_sum(rows, window, "volume")
            )
        for window in normalized_windows:
            row[f"valid_{window}m"] = _bool_text(vol_by_window[window] is not None)

        row["close_count"] = len(closes)
        row["latest_closed_open_time_ms"] = (
            latest_row.open_time_ms if latest_row is not None else ""
        )
        reference_price = _format_float(
            latest_row.close if latest_row is not None else None
        )
        row["reference_price"] = reference_price
        result_rows.append(row)
    return result_rows


def write_kline_csv(output_path: Path, rows: Iterable[KlineRow]) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=KLINE_COLUMNS, lineterminator="\n")
        writer.writeheader()
        for row in rows:
            data = asdict(row)
            data["closed"] = _bool_text(row.closed)
            writer.writerow(data)


def write_summary_csv(output_path: Path, rows: list[dict[str, Any]]) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    fieldnames = list(rows[0].keys()) if rows else summary_columns([30, 60])
    with output_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames, lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)


def write_exchange_result_csv(output_path: Path, rows: list[dict[str, Any]]) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    fieldnames = list(rows[0].keys()) if rows else EXCHANGE_RESULT_COLUMNS
    with output_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames, lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)


def write_metadata_json(output_path: Path, metadata: dict[str, Any]) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(
        json.dumps(metadata, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def make_run_dir(output_root: Path, run_id: str | None) -> Path:
    timestamp = dt.datetime.now(dt.UTC).strftime("%Y%m%d_%H%M%S")
    actual_run_id = run_id or f"common_usdt_perp_klines_{timestamp}"
    return output_root / actual_run_id


def query_symbol_klines(
    common_symbol: CommonSymbol,
    limit: int,
    timeout: float,
    gate_base_url: str,
    binance_base_url: str,
) -> tuple[list[KlineRow], list[KlineRow]]:
    now_ms = _now_ms()
    gate_payloads = fetch_gate_klines(
        common_symbol.gate_symbol,
        limit=limit,
        base_url=gate_base_url,
        timeout=timeout,
    )
    gate_rows = [
        parse_gate_kline(common_symbol.symbol, common_symbol.gate_symbol, payload, now_ms)
        for payload in gate_payloads
    ]
    binance_payloads = fetch_binance_klines(
        common_symbol.binance_symbol,
        limit=limit,
        base_url=binance_base_url,
        timeout=timeout,
    )
    binance_rows = [
        parse_binance_kline(
            common_symbol.symbol, common_symbol.binance_symbol, payload, now_ms
        )
        for payload in binance_payloads
    ]
    return gate_rows, binance_rows


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Fetch Gate and Binance 1m klines for common USDT perpetual symbols "
            "and compute close-to-close realized volatility."
        )
    )
    parser.add_argument("--catalog", type=Path, default=DEFAULT_CATALOG)
    parser.add_argument("--symbols", help="Comma-separated internal symbols, e.g. BTC_USDT,ETH_USDT.")
    parser.add_argument("--lookback-minutes", type=int, default=120)
    parser.add_argument(
        "--vol-window",
        type=int,
        action="append",
        default=None,
        help="Volatility window in minutes. Can be repeated. Defaults to 30 and 60.",
    )
    parser.add_argument("--output-root", type=Path, default=DEFAULT_OUTPUT_ROOT)
    parser.add_argument("--run-id", help="Output directory name under --output-root.")
    parser.add_argument("--request-sleep-sec", type=float, default=0.1)
    parser.add_argument("--timeout", type=float, default=8.0)
    parser.add_argument("--gate-base-url", default=DEFAULT_GATE_BASE_URL)
    parser.add_argument("--binance-base-url", default=DEFAULT_BINANCE_BASE_URL)
    parser.add_argument("--max-symbols", type=int, default=0)
    return parser.parse_args(argv)


def run(args: argparse.Namespace) -> Path:
    windows = sorted(set(args.vol_window or DEFAULT_VOL_WINDOWS))
    limit = max(args.lookback_minutes + 1, max(windows) + 1)
    requested_symbols = parse_symbols_arg(args.symbols)
    symbols = load_common_symbols(args.catalog, requested_symbols=requested_symbols)
    if args.max_symbols > 0:
        symbols = symbols[: args.max_symbols]
    if not symbols:
        raise RuntimeError("no common symbols selected")

    run_dir = make_run_dir(args.output_root, args.run_id)
    gate_rows_by_symbol: dict[str, list[KlineRow]] = {}
    binance_rows_by_symbol: dict[str, list[KlineRow]] = {}
    all_gate_rows: list[KlineRow] = []
    all_binance_rows: list[KlineRow] = []
    failures: list[dict[str, str]] = []

    for index, common_symbol in enumerate(symbols):
        try:
            gate_rows, binance_rows = query_symbol_klines(
                common_symbol,
                limit=limit,
                timeout=args.timeout,
                gate_base_url=args.gate_base_url,
                binance_base_url=args.binance_base_url,
            )
        except Exception as exc:
            failures.append({"symbol": common_symbol.symbol, "error": str(exc)})
            continue

        gate_rows_by_symbol[common_symbol.symbol] = gate_rows
        binance_rows_by_symbol[common_symbol.symbol] = binance_rows
        all_gate_rows.extend(gate_rows)
        all_binance_rows.extend(binance_rows)
        if args.request_sleep_sec > 0 and index + 1 < len(symbols):
            time.sleep(args.request_sleep_sec)

    summary_rows = build_summary_rows(
        symbols,
        gate_rows_by_symbol=gate_rows_by_symbol,
        binance_rows_by_symbol=binance_rows_by_symbol,
        windows=windows,
    )
    gate_result_rows = build_exchange_result_rows(
        "gate",
        symbols,
        gate_rows_by_symbol,
        windows,
    )
    binance_result_rows = build_exchange_result_rows(
        "binance",
        symbols,
        binance_rows_by_symbol,
        windows,
    )

    write_kline_csv(run_dir / "gate_1m_klines.csv", all_gate_rows)
    write_kline_csv(run_dir / "binance_1m_klines.csv", all_binance_rows)
    write_summary_csv(run_dir / "volatility_summary.csv", summary_rows)
    write_exchange_result_csv(run_dir / "gate_volatility_liquidity.csv", gate_result_rows)
    write_exchange_result_csv(
        run_dir / "binance_volatility_liquidity.csv",
        binance_result_rows,
    )
    write_metadata_json(
        run_dir / "run_metadata.json",
        {
            "catalog": str(args.catalog),
            "symbols_requested": sorted(requested_symbols) if requested_symbols else None,
            "symbols_selected": [symbol.symbol for symbol in symbols],
            "lookback_minutes": args.lookback_minutes,
            "fetch_limit": limit,
            "vol_windows": windows,
            "gate_rows": len(all_gate_rows),
            "binance_rows": len(all_binance_rows),
            "gate_result_rows": len(gate_result_rows),
            "binance_result_rows": len(binance_result_rows),
            "gate_result_csv": str(run_dir / "gate_volatility_liquidity.csv"),
            "binance_result_csv": str(run_dir / "binance_volatility_liquidity.csv"),
            "failures": failures,
        },
    )
    return run_dir


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        run_dir = run(args)
    except Exception as exc:  # pragma: no cover - utility script
        print(f"[FAIL] {type(exc).__name__}: {exc}", file=sys.stderr)
        return 1

    print(f"common_usdt_perp_kline_volatility output_dir={run_dir}")
    print(f"gate_klines={run_dir / 'gate_1m_klines.csv'}")
    print(f"binance_klines={run_dir / 'binance_1m_klines.csv'}")
    print(f"summary={run_dir / 'volatility_summary.csv'}")
    print(f"gate_result={run_dir / 'gate_volatility_liquidity.csv'}")
    print(f"binance_result={run_dir / 'binance_volatility_liquidity.csv'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
