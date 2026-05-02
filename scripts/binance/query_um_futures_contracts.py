#!/home/liuxiang/dev/pyenv/lx/bin/python

import argparse
import json
import sys
import urllib.error
import urllib.parse
import urllib.request
from decimal import Decimal, InvalidOperation
from pathlib import Path
from typing import Any, Iterable

try:
    import pandas as pd
except ImportError as exc:  # pragma: no cover - utility script
    raise SystemExit("missing dependency: pandas. Install with `pip install pandas`.") from exc


DEFAULT_BASE_URL = "https://fapi.binance.com"
USER_AGENT = "aquila-binance-um-futures-contract-query/1.0"

DATAFRAME_COLUMNS = [
    "exchange",
    "symbol_id",
    "exchange_symbol",
    "base_asset",
    "quote_asset",
    "settle_asset",
    "status",
    "contract_type",
    "price_tick",
    "price_decimal_places",
    "quantity_step",
    "quantity_decimal_places",
    "quantity_min",
    "quantity_max",
    "market_quantity_max",
    "min_notional",
    "contract_multiplier",
    "price_limit_up",
    "price_limit_down",
    "market_price_bound",
]


def normalize_symbol(symbol: str) -> str:
    return symbol.strip().upper().replace("_", "").replace("/", "")


def parse_symbol_inputs(
    symbols: Iterable[str], file_lines: Iterable[str] | None = None
) -> list[str]:
    result: list[str] = []
    seen: set[str] = set()

    def add_symbol(raw_symbol: str) -> None:
        symbol = normalize_symbol(raw_symbol)
        if not symbol or symbol.startswith("#") or symbol in seen:
            return
        seen.add(symbol)
        result.append(symbol)

    for symbol in symbols:
        add_symbol(symbol)

    if file_lines is not None:
        for line in file_lines:
            add_symbol(line)

    return result


def read_symbol_file(path: str) -> list[str]:
    return Path(path).read_text(encoding="utf-8").splitlines()


def decimal_places(value: Any) -> int:
    text = str(value).strip()
    if not text:
        return 0
    try:
        decimal = Decimal(text)
    except InvalidOperation:
        return 0
    exponent = decimal.normalize().as_tuple().exponent
    return max(0, -exponent)


def to_float(value: Any) -> float | None:
    if value is None or value == "":
        return None
    return float(value)


def to_int(value: Any) -> int | None:
    if value is None or value == "":
        return None
    return int(value)


def filter_map(symbol_payload: dict[str, Any]) -> dict[str, dict[str, Any]]:
    filters = symbol_payload.get("filters") or []
    return {
        item.get("filterType", ""): item
        for item in filters
        if isinstance(item, dict) and item.get("filterType")
    }


def filter_float(filter_value: dict[str, Any], key: str) -> float | None:
    return to_float(filter_value.get(key))


def min_notional_value(filters: dict[str, dict[str, Any]]) -> float | None:
    filter_value = filters.get("MIN_NOTIONAL") or filters.get("NOTIONAL") or {}
    return to_float(filter_value.get("notional", filter_value.get("minNotional")))


def price_limit_up(percent_price: dict[str, Any]) -> float | None:
    multiplier_up = to_float(percent_price.get("multiplierUp"))
    if multiplier_up is None:
        return None
    return round(multiplier_up - 1.0, 12)


def price_limit_down(percent_price: dict[str, Any]) -> float | None:
    multiplier_down = to_float(percent_price.get("multiplierDown"))
    if multiplier_down is None:
        return None
    return round(1.0 - multiplier_down, 12)


def symbol_to_row(symbol_id: int, payload: dict[str, Any]) -> dict[str, Any]:
    filters = filter_map(payload)
    price_filter = filters.get("PRICE_FILTER") or {}
    lot_size = filters.get("LOT_SIZE") or {}
    market_lot_size = filters.get("MARKET_LOT_SIZE") or {}
    percent_price = filters.get("PERCENT_PRICE") or filters.get("PERCENT_PRICE_BY_SIDE") or {}
    price_tick = price_filter.get("tickSize")
    quantity_step = lot_size.get("stepSize")
    return {
        "exchange": "binance",
        "symbol_id": symbol_id,
        "exchange_symbol": payload.get("symbol", ""),
        "base_asset": payload.get("baseAsset", ""),
        "quote_asset": payload.get("quoteAsset", ""),
        "settle_asset": payload.get("marginAsset", ""),
        "status": str(payload.get("status", "")).upper(),
        "contract_type": payload.get("contractType", ""),
        "price_tick": to_float(price_tick),
        "price_decimal_places": decimal_places(price_tick),
        "quantity_step": to_float(quantity_step),
        "quantity_decimal_places": decimal_places(quantity_step),
        "quantity_min": filter_float(lot_size, "minQty"),
        "quantity_max": filter_float(lot_size, "maxQty"),
        "market_quantity_max": filter_float(market_lot_size, "maxQty"),
        "min_notional": min_notional_value(filters),
        "contract_multiplier": 1.0,
        "price_limit_up": price_limit_up(percent_price),
        "price_limit_down": price_limit_down(percent_price),
        "market_price_bound": to_float(payload.get("marketTakeBound")),
    }


def symbols_to_dataframe(symbol_payloads: Iterable[dict[str, Any]]) -> pd.DataFrame:
    rows = [symbol_to_row(symbol_id, payload) for symbol_id, payload in enumerate(symbol_payloads)]
    return pd.DataFrame(rows, columns=DATAFRAME_COLUMNS)


def exchange_info_url(base_url: str = DEFAULT_BASE_URL) -> str:
    return urllib.parse.urljoin(base_url.rstrip("/") + "/", "fapi/v1/exchangeInfo")


def fetch_exchange_info(base_url: str = DEFAULT_BASE_URL, timeout: float = 8.0) -> dict[str, Any]:
    request = urllib.request.Request(
        exchange_info_url(base_url),
        headers={"Accept": "application/json", "User-Agent": USER_AGENT},
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            body = response.read().decode("utf-8")
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"Binance exchangeInfo query failed: HTTP {exc.code}: {detail}") from exc
    except urllib.error.URLError as exc:
        raise RuntimeError(f"Binance exchangeInfo query failed: {exc}") from exc

    payload = json.loads(body)
    if not isinstance(payload, dict) or not isinstance(payload.get("symbols"), list):
        raise RuntimeError("Binance exchangeInfo returned unexpected payload")
    return payload


def select_symbols(exchange_info: dict[str, Any], symbols: Iterable[str]) -> list[dict[str, Any]]:
    requested = list(symbols)
    by_symbol = {
        normalize_symbol(item.get("symbol", "")): item
        for item in exchange_info.get("symbols", [])
        if isinstance(item, dict)
    }
    missing = [symbol for symbol in requested if symbol not in by_symbol]
    if missing:
        raise RuntimeError(f"Binance symbols not found: {', '.join(missing)}")
    return [by_symbol[symbol] for symbol in requested]


def query_contracts_dataframe(
    symbols: Iterable[str], base_url: str = DEFAULT_BASE_URL, timeout: float = 8.0
) -> pd.DataFrame:
    exchange_info = fetch_exchange_info(base_url=base_url, timeout=timeout)
    return symbols_to_dataframe(select_symbols(exchange_info, symbols))


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Query Binance USD-M futures contract metadata into a pandas DataFrame."
    )
    parser.add_argument(
        "symbols",
        nargs="*",
        help="Binance USD-M futures symbols, e.g. BTCUSDT ETHUSDT.",
    )
    parser.add_argument(
        "-f",
        "--file",
        help="Text file with one symbol per line. Empty lines and lines starting with # are ignored.",
    )
    parser.add_argument(
        "--base-url",
        default=DEFAULT_BASE_URL,
        help="Binance USD-M futures REST base URL.",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=8.0,
        help="HTTP timeout in seconds.",
    )
    parser.add_argument(
        "--format",
        choices=("table", "csv", "json"),
        default="table",
        help="Output format.",
    )
    parser.add_argument(
        "--output-csv",
        help="Optional CSV output path.",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    file_lines = read_symbol_file(args.file) if args.file else None
    symbols = parse_symbol_inputs(args.symbols, file_lines=file_lines)
    if not symbols:
        print("no symbols provided", file=sys.stderr)
        return 2

    try:
        frame = query_contracts_dataframe(symbols, base_url=args.base_url, timeout=args.timeout)
    except Exception as exc:  # pragma: no cover - utility script
        print(f"[FAIL] {type(exc).__name__}: {exc}", file=sys.stderr)
        return 1

    if args.output_csv:
        frame.to_csv(args.output_csv, index=False)

    if args.format == "csv":
        print(frame.to_csv(index=False), end="")
    elif args.format == "json":
        print(frame.to_json(orient="records"))
    else:
        print(frame.to_string(index=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
