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


DEFAULT_BASE_URL = "https://api.gateio.ws/api/v4"
SETTLE = "usdt"
USER_AGENT = "aquila-gate-futures-contract-query/1.0"

DATAFRAME_COLUMNS = [
    "symbol_id",
    "symbol",
    "settle",
    "status",
    "contract_type",
    "price_tick",
    "price_decimal_places",
    "mark_price_tick",
    "mark_price_decimal_places",
    "contract_multiplier",
    "order_size_min",
    "order_size_max",
    "enable_decimal",
    "market_order_size_max",
    "market_order_slip_ratio",
    "order_price_deviate",
    "orders_limit",
    "maker_fee_rate",
    "taker_fee_rate",
    "leverage_min",
    "leverage_max",
    "risk_limit_base",
    "risk_limit_step",
    "risk_limit_max",
    "maintenance_rate",
    "in_delisting",
    "config_change_time",
]


def normalize_symbol(symbol: str) -> str:
    return symbol.strip().upper()


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


def contract_to_row(symbol_id: int, payload: dict[str, Any]) -> dict[str, Any]:
    price_tick = payload.get("order_price_round")
    mark_price_tick = payload.get("mark_price_round")
    return {
        "symbol_id": symbol_id,
        "symbol": payload.get("name", ""),
        "settle": SETTLE,
        "status": payload.get("status", ""),
        "contract_type": payload.get("type", ""),
        "price_tick": to_float(price_tick),
        "price_decimal_places": decimal_places(price_tick),
        "mark_price_tick": to_float(mark_price_tick),
        "mark_price_decimal_places": decimal_places(mark_price_tick),
        "contract_multiplier": to_float(payload.get("quanto_multiplier")),
        "order_size_min": to_int(payload.get("order_size_min")),
        "order_size_max": to_int(payload.get("order_size_max")),
        "enable_decimal": bool(payload.get("enable_decimal", False)),
        "market_order_size_max": to_float(payload.get("market_order_size_max")),
        "market_order_slip_ratio": to_float(payload.get("market_order_slip_ratio")),
        "order_price_deviate": to_float(payload.get("order_price_deviate")),
        "orders_limit": to_int(payload.get("orders_limit")),
        "maker_fee_rate": to_float(payload.get("maker_fee_rate")),
        "taker_fee_rate": to_float(payload.get("taker_fee_rate")),
        "leverage_min": to_float(payload.get("leverage_min")),
        "leverage_max": to_float(payload.get("leverage_max")),
        "risk_limit_base": to_float(payload.get("risk_limit_base")),
        "risk_limit_step": to_float(payload.get("risk_limit_step")),
        "risk_limit_max": to_float(payload.get("risk_limit_max")),
        "maintenance_rate": to_float(payload.get("maintenance_rate")),
        "in_delisting": bool(payload.get("in_delisting", False)),
        "config_change_time": to_int(payload.get("config_change_time")),
    }


def contracts_to_dataframe(payloads: Iterable[dict[str, Any]]) -> pd.DataFrame:
    rows = [contract_to_row(symbol_id, payload) for symbol_id, payload in enumerate(payloads)]
    return pd.DataFrame(rows, columns=DATAFRAME_COLUMNS)


def contract_url(symbol: str, base_url: str = DEFAULT_BASE_URL) -> str:
    encoded_symbol = urllib.parse.quote(normalize_symbol(symbol), safe="")
    return f"{base_url.rstrip('/')}/futures/{SETTLE}/contracts/{encoded_symbol}"


def fetch_contract(symbol: str, base_url: str = DEFAULT_BASE_URL, timeout: float = 8.0) -> dict[str, Any]:
    request = urllib.request.Request(
        contract_url(symbol, base_url),
        headers={"Accept": "application/json", "User-Agent": USER_AGENT},
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            body = response.read().decode("utf-8")
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"Gate REST query failed for {symbol}: HTTP {exc.code}: {detail}") from exc
    except urllib.error.URLError as exc:
        raise RuntimeError(f"Gate REST query failed for {symbol}: {exc}") from exc

    payload = json.loads(body)
    if isinstance(payload, dict) and payload.get("label"):
        raise RuntimeError(f"Gate REST query failed for {symbol}: {payload}")
    if not isinstance(payload, dict):
        raise RuntimeError(f"Gate REST query returned non-object payload for {symbol}")
    return payload


def query_contracts_dataframe(
    symbols: Iterable[str], base_url: str = DEFAULT_BASE_URL, timeout: float = 8.0
) -> pd.DataFrame:
    payloads = [fetch_contract(symbol, base_url=base_url, timeout=timeout) for symbol in symbols]
    return contracts_to_dataframe(payloads)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Query Gate USDT futures contract metadata into a pandas DataFrame."
    )
    parser.add_argument(
        "symbols",
        nargs="*",
        help="Gate futures contract symbols, e.g. BTC_USDT ETH_USDT.",
    )
    parser.add_argument(
        "-f",
        "--file",
        help="Text file with one symbol per line. Empty lines and lines starting with # are ignored.",
    )
    parser.add_argument(
        "--base-url",
        default=DEFAULT_BASE_URL,
        help="Gate API v4 base URL.",
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
