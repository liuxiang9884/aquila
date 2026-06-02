#!/home/liuxiang/dev/pyenv/lx/bin/python

import argparse
import csv
import datetime as dt
import json
import sys
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any, Iterable

SCRIPT_ROOT = Path(__file__).resolve().parents[1]
GATE_SCRIPT_DIR = SCRIPT_ROOT / "gate" / "market_data"
BINANCE_SCRIPT_DIR = SCRIPT_ROOT / "binance" / "market_data"
for script_dir in (GATE_SCRIPT_DIR, BINANCE_SCRIPT_DIR):
    if str(script_dir) not in sys.path:
        sys.path.insert(0, str(script_dir))

import query_futures_contracts as gate_contracts
import query_um_futures_contracts as binance_contracts


CATALOG_COLUMNS = [
    "symbol_id",
    "symbol",
    "exchange",
    "exchange_symbol",
    "base_asset",
    "quote_asset",
    "settle_asset",
    "product_type",
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

DEFAULT_OUTPUT_DIR = Path("config/instruments")
DEFAULT_OUTPUT_PREFIX = "usdt_futures_common_gate_binance"
PRODUCT_TYPE = "linear_perpetual"
USER_AGENT = "aquila-common-usdt-perp-catalog-generator/1.0"


def gate_contracts_url(base_url: str = gate_contracts.DEFAULT_BASE_URL) -> str:
    return urllib.parse.urljoin(base_url.rstrip("/") + "/", "futures/usdt/contracts")


def fetch_gate_contracts(
    base_url: str = gate_contracts.DEFAULT_BASE_URL, timeout: float = 8.0
) -> list[dict[str, Any]]:
    request = urllib.request.Request(
        gate_contracts_url(base_url),
        headers={
            "Accept": "application/json",
            "User-Agent": USER_AGENT,
            "X-Gate-Size-Decimal": "1",
        },
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            body = response.read().decode("utf-8")
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"Gate contracts query failed: HTTP {exc.code}: {detail}") from exc
    except urllib.error.URLError as exc:
        raise RuntimeError(f"Gate contracts query failed: {exc}") from exc

    payload = json.loads(body)
    if not isinstance(payload, list):
        raise RuntimeError("Gate contracts query returned non-list payload")
    return [item for item in payload if isinstance(item, dict)]


def fetch_binance_exchange_info(
    base_url: str = binance_contracts.DEFAULT_BASE_URL, timeout: float = 8.0
) -> dict[str, Any]:
    return binance_contracts.fetch_exchange_info(base_url=base_url, timeout=timeout)


def _upper_text(value: Any) -> str:
    return str(value or "").strip().upper()


def binance_internal_symbol(payload: dict[str, Any]) -> str:
    base_asset = _upper_text(payload.get("baseAsset"))
    quote_asset = _upper_text(payload.get("quoteAsset"))
    if base_asset and quote_asset:
        return f"{base_asset}_{quote_asset}"

    symbol = _upper_text(payload.get("symbol"))
    if symbol.endswith("USDT"):
        return f"{symbol[:-4]}_USDT"
    return symbol


def gate_internal_symbol(payload: dict[str, Any]) -> str:
    return _upper_text(payload.get("name"))


def select_binance_usdt_perpetuals(exchange_info: dict[str, Any]) -> list[dict[str, Any]]:
    symbols = exchange_info.get("symbols")
    if not isinstance(symbols, list):
        raise RuntimeError("Binance exchangeInfo payload missing symbols list")
    selected: list[dict[str, Any]] = []
    for item in symbols:
        if not isinstance(item, dict):
            continue
        if _upper_text(item.get("quoteAsset")) != "USDT":
            continue
        if _upper_text(item.get("marginAsset")) != "USDT":
            continue
        if _upper_text(item.get("contractType")) != "PERPETUAL":
            continue
        if _upper_text(item.get("status")) != "TRADING":
            continue
        selected.append(item)
    return selected


def select_gate_usdt_perpetuals(payloads: Iterable[dict[str, Any]]) -> list[dict[str, Any]]:
    selected: list[dict[str, Any]] = []
    for item in payloads:
        symbol = gate_internal_symbol(item)
        if not symbol.endswith("_USDT"):
            continue
        if _upper_text(item.get("status")) != "TRADING":
            continue
        selected.append(item)
    return selected


def _unique_by_symbol(
    payloads: Iterable[dict[str, Any]], symbol_fn
) -> dict[str, dict[str, Any]]:
    by_symbol: dict[str, dict[str, Any]] = {}
    for payload in payloads:
        symbol = symbol_fn(payload)
        if not symbol or symbol in by_symbol:
            continue
        by_symbol[symbol] = payload
    return by_symbol


def _catalog_row(symbol: str, mapped: dict[str, Any]) -> dict[str, Any]:
    return {
        "symbol_id": mapped["symbol_id"],
        "symbol": symbol,
        "exchange": mapped["exchange"],
        "exchange_symbol": mapped["exchange_symbol"],
        "base_asset": mapped["base_asset"],
        "quote_asset": mapped["quote_asset"],
        "settle_asset": mapped["settle_asset"],
        "product_type": PRODUCT_TYPE,
        "status": mapped["status"],
        "contract_type": mapped["contract_type"],
        "price_tick": mapped["price_tick"],
        "price_decimal_places": mapped["price_decimal_places"],
        "quantity_step": mapped["quantity_step"],
        "quantity_decimal_places": mapped["quantity_decimal_places"],
        "min_quantity": mapped["min_quantity"],
        "max_quantity": mapped["max_quantity"],
        "max_market_quantity": mapped["max_market_quantity"],
        "min_notional": mapped["min_notional"],
        "notional_multiplier": mapped["notional_multiplier"],
        "price_limit_up": mapped["price_limit_up"],
        "price_limit_down": mapped["price_limit_down"],
        "market_price_bound": mapped["market_price_bound"],
    }


def build_catalog_rows(
    gate_payloads: Iterable[dict[str, Any]], binance_payloads: Iterable[dict[str, Any]]
) -> list[dict[str, Any]]:
    gate_by_symbol = _unique_by_symbol(
        select_gate_usdt_perpetuals(gate_payloads), gate_internal_symbol
    )
    binance_by_symbol = _unique_by_symbol(
        select_binance_usdt_perpetuals({"symbols": list(binance_payloads)}),
        binance_internal_symbol,
    )
    common_symbols = sorted(set(gate_by_symbol) & set(binance_by_symbol))

    rows: list[dict[str, Any]] = []
    for symbol_id, symbol in enumerate(common_symbols):
        gate_row = gate_contracts.contract_to_row(symbol_id, gate_by_symbol[symbol])
        binance_row = binance_contracts.symbol_to_row(
            symbol_id, binance_by_symbol[symbol]
        )
        rows.append(_catalog_row(symbol, gate_row))
        rows.append(_catalog_row(symbol, binance_row))
    return rows


def write_catalog_csv(
    output_path: Path, rows: Iterable[dict[str, Any]], overwrite: bool = False
) -> None:
    if output_path.exists() and not overwrite:
        raise FileExistsError(f"{output_path} already exists; pass --overwrite to replace it")
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=CATALOG_COLUMNS,
                                lineterminator="\n")
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def default_output_path(output_dir: Path, date_text: str) -> Path:
    return output_dir / f"{DEFAULT_OUTPUT_PREFIX}_{date_text}.csv"


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Generate an Aquila instrument catalog for tradable USDT perpetual "
            "contracts common to Gate and Binance USD-M futures."
        )
    )
    parser.add_argument("--output", type=Path, help="Exact output CSV path.")
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=DEFAULT_OUTPUT_DIR,
        help="Output directory when --output is omitted.",
    )
    parser.add_argument(
        "--date",
        default=dt.date.today().strftime("%Y%m%d"),
        help="Date suffix for the default output file name.",
    )
    parser.add_argument(
        "--overwrite",
        action="store_true",
        help="Allow replacing an existing output file.",
    )
    parser.add_argument(
        "--gate-base-url",
        default=gate_contracts.DEFAULT_BASE_URL,
        help="Gate API v4 base URL.",
    )
    parser.add_argument(
        "--binance-base-url",
        default=binance_contracts.DEFAULT_BASE_URL,
        help="Binance USD-M futures REST base URL.",
    )
    parser.add_argument("--timeout", type=float, default=8.0, help="HTTP timeout seconds.")
    parser.add_argument(
        "--format",
        choices=("summary", "json"),
        default="summary",
        help="Stdout format.",
    )
    return parser.parse_args(argv)


def generate_catalog(args: argparse.Namespace) -> tuple[Path, list[dict[str, Any]], dict[str, int]]:
    gate_payloads = fetch_gate_contracts(base_url=args.gate_base_url, timeout=args.timeout)
    binance_info = fetch_binance_exchange_info(
        base_url=args.binance_base_url, timeout=args.timeout
    )
    gate_selected = select_gate_usdt_perpetuals(gate_payloads)
    binance_selected = select_binance_usdt_perpetuals(binance_info)
    rows = build_catalog_rows(gate_selected, binance_selected)
    output_path = args.output or default_output_path(args.output_dir, args.date)
    write_catalog_csv(output_path, rows, overwrite=args.overwrite)
    counts = {
        "gate_trading_usdt_perpetuals": len(gate_selected),
        "binance_trading_usdt_perpetuals": len(binance_selected),
        "common_symbols": len(rows) // 2,
        "catalog_rows": len(rows),
    }
    return output_path, rows, counts


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        output_path, _rows, counts = generate_catalog(args)
    except Exception as exc:  # pragma: no cover - utility script
        print(f"[FAIL] {type(exc).__name__}: {exc}", file=sys.stderr)
        return 1

    summary = {"output": str(output_path), **counts}
    if args.format == "json":
        print(json.dumps(summary, sort_keys=True))
    else:
        print(
            "generated_common_usdt_perp_catalog "
            f"output={output_path} "
            f"gate_trading_usdt_perpetuals={counts['gate_trading_usdt_perpetuals']} "
            f"binance_trading_usdt_perpetuals={counts['binance_trading_usdt_perpetuals']} "
            f"common_symbols={counts['common_symbols']} "
            f"catalog_rows={counts['catalog_rows']}"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
