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


DEFAULT_BASE_URL = "https://vip-api-uta.bitget.com"
CATEGORY = "USDT-FUTURES"
SETTLE_ASSET = "USDT"
USER_AGENT = "aquila-bitget-uta-futures-contract-query/1.0"

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
    "min_quantity",
    "max_quantity",
    "max_market_quantity",
    "min_notional",
    "notional_multiplier",
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


def internal_symbol(payload: dict[str, Any]) -> str:
    base_asset = str(payload.get("baseCoin", "")).strip().upper()
    quote_asset = str(payload.get("quoteCoin", "")).strip().upper()
    if base_asset and quote_asset:
        return f"{base_asset}_{quote_asset}"

    symbol = normalize_symbol(str(payload.get("symbol", "")))
    if symbol.endswith("USDT"):
        return f"{symbol[:-4]}_USDT"
    return symbol


def select_usdt_perpetuals(payloads: Iterable[dict[str, Any]]) -> list[dict[str, Any]]:
    selected: list[dict[str, Any]] = []
    for item in payloads:
        if str(item.get("category", "")).upper() != CATEGORY:
            continue
        if str(item.get("type", "")).strip().lower() != "perpetual":
            continue
        if str(item.get("status", "")).strip().lower() != "online":
            continue
        selected.append(item)
    return selected


def select_symbols(
    payloads: Iterable[dict[str, Any]],
    symbols: Iterable[str],
    allow_missing: bool = False,
) -> list[dict[str, Any]]:
    requested = [normalize_symbol(symbol) for symbol in symbols]
    by_symbol = {
        normalize_symbol(str(item.get("symbol", ""))): item
        for item in select_usdt_perpetuals(payloads)
    }
    missing = [symbol for symbol in requested if symbol not in by_symbol]
    if missing and not allow_missing:
        raise RuntimeError(f"Bitget symbols not found: {', '.join(missing)}")
    return [by_symbol[symbol] for symbol in requested if symbol in by_symbol]


def instrument_to_row(symbol_id: int, payload: dict[str, Any]) -> dict[str, Any]:
    price_tick = payload.get("priceMultiplier")
    quantity_step = payload.get("quantityMultiplier")
    return {
        "exchange": "bitget",
        "symbol_id": symbol_id,
        "exchange_symbol": payload.get("symbol", ""),
        "base_asset": payload.get("baseCoin", ""),
        "quote_asset": payload.get("quoteCoin", ""),
        "settle_asset": SETTLE_ASSET,
        "status": payload.get("status", ""),
        "contract_type": payload.get("type", ""),
        "price_tick": to_float(price_tick),
        "price_decimal_places": to_int(payload.get("pricePrecision"))
        if payload.get("pricePrecision") not in (None, "")
        else decimal_places(price_tick),
        "quantity_step": to_float(quantity_step),
        "quantity_decimal_places": to_int(payload.get("quantityPrecision"))
        if payload.get("quantityPrecision") not in (None, "")
        else decimal_places(quantity_step),
        "min_quantity": to_float(payload.get("minOrderQty")),
        "max_quantity": to_float(payload.get("maxOrderQty")),
        "max_market_quantity": to_float(payload.get("maxMarketOrderQty")),
        "min_notional": to_float(payload.get("minOrderAmount")),
        "notional_multiplier": 1.0,
        "price_limit_up": to_float(payload.get("buyLimitPriceRatio")),
        "price_limit_down": to_float(payload.get("sellLimitPriceRatio")),
        "market_price_bound": None,
    }


def instruments_to_dataframe(payloads: Iterable[dict[str, Any]]) -> pd.DataFrame:
    rows = [
        instrument_to_row(symbol_id, payload)
        for symbol_id, payload in enumerate(payloads)
    ]
    return pd.DataFrame(rows, columns=DATAFRAME_COLUMNS)


def instruments_url(base_url: str = DEFAULT_BASE_URL) -> str:
    query = urllib.parse.urlencode({"category": CATEGORY})
    return urllib.parse.urljoin(base_url.rstrip("/") + "/", f"api/v3/market/instruments?{query}")


def fetch_instruments(
    base_url: str = DEFAULT_BASE_URL, timeout: float = 8.0
) -> list[dict[str, Any]]:
    request = urllib.request.Request(
        instruments_url(base_url),
        headers={"Accept": "application/json", "User-Agent": USER_AGENT},
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            body = response.read().decode("utf-8")
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"Bitget instruments query failed: HTTP {exc.code}: {detail}") from exc
    except urllib.error.URLError as exc:
        raise RuntimeError(f"Bitget instruments query failed: {exc}") from exc

    payload = json.loads(body)
    if not isinstance(payload, dict) or payload.get("code") != "00000":
        raise RuntimeError(f"Bitget instruments query returned error payload: {payload}")
    data = payload.get("data")
    if not isinstance(data, list):
        raise RuntimeError("Bitget instruments query returned non-list data")
    return [item for item in data if isinstance(item, dict)]


def query_contracts_dataframe(
    symbols: Iterable[str],
    base_url: str = DEFAULT_BASE_URL,
    timeout: float = 8.0,
    allow_missing: bool = False,
) -> pd.DataFrame:
    instruments = fetch_instruments(base_url=base_url, timeout=timeout)
    selected = select_symbols(instruments, symbols, allow_missing=allow_missing)
    return instruments_to_dataframe(selected)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Query Bitget UTA USDT futures contract metadata into a pandas DataFrame."
    )
    parser.add_argument(
        "symbols",
        nargs="*",
        help="Bitget UTA futures symbols, e.g. BTCUSDT ETHUSDT.",
    )
    parser.add_argument(
        "-f",
        "--file",
        help=(
            "Text file with one symbol per line. Empty lines and lines "
            "starting with # are ignored."
        ),
    )
    parser.add_argument(
        "--base-url",
        default=DEFAULT_BASE_URL,
        help="Bitget UTA REST base URL.",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=8.0,
        help="HTTP timeout in seconds.",
    )
    parser.add_argument(
        "--allow-missing",
        action="store_true",
        help="Skip requested symbols that are absent or not online on Bitget.",
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
        frame = query_contracts_dataframe(
            symbols,
            base_url=args.base_url,
            timeout=args.timeout,
            allow_missing=args.allow_missing,
        )
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
