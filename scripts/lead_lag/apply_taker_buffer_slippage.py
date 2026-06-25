#!/usr/bin/env python3

import argparse
import csv
import json
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence


PAIR_HEADER_RE = re.compile(r"^\s*\[\[lead_lag\.pairs\]\]\s*$")
EXECUTE_HEADER_RE = re.compile(r"^\s*\[lead_lag\.pairs\.execute\]\s*$")
SYMBOL_RE = re.compile(r'^(\s*symbol\s*=\s*)"([^"]+)"(\s*)$')
SYMBOL_ID_RE = re.compile(r"^(\s*symbol_id\s*=\s*)(\d+)(\s*)$")
EXCHANGE_RE = re.compile(r'^(\s*(lead|lag)_exchange\s*=\s*)"([^"]+)"(\s*)$')
OPEN_SLIPPAGE_RE = re.compile(r"^(\s*open_slippage_ticks\s*=\s*)[\d_]+(.*)$")
CLOSE_SLIPPAGE_RE = re.compile(r"^(\s*close_slippage_ticks\s*=\s*)[\d_]+(.*)$")
DEPRECATED_SLIPPAGE_RE = re.compile(r"^\s*(open_slippage|close_slippage)\s*=")


@dataclass(frozen=True)
class PairConfig:
    symbol: str
    symbol_id: int
    lead_exchange: str
    lag_exchange: str
    block_start: int
    block_end: int


@dataclass(frozen=True)
class PairSlippageRow:
    symbol: str
    symbol_id: int
    lead_exchange: str
    lag_exchange: str
    entry_buffer_pct: float
    normal_close_buffer_pct: float
    price_tick: float
    reference_price_method: str
    reference_price: float
    max_bid_price: float
    max_ask_price: float
    open_long_slippage_ticks: int
    open_short_slippage_ticks: int
    close_long_slippage_ticks: int
    close_short_slippage_ticks: int
    generated_open_slippage_ticks: int
    generated_close_slippage_ticks: int


@dataclass(frozen=True)
class ApplyResult:
    updated_pair_count: int
    rows: list[PairSlippageRow]


def normalize_exchange(exchange: str) -> str:
    normalized = exchange.strip().lower().replace("_", "").replace("-", "")
    if normalized.startswith("k"):
        normalized = normalized[1:]
    if normalized == "gateio":
        return "gate"
    return normalized


def parse_pairs(config_text: str) -> tuple[list[str], list[PairConfig]]:
    lines = config_text.splitlines(keepends=True)
    starts = [index for index, line in enumerate(lines) if PAIR_HEADER_RE.match(line)]
    pairs: list[PairConfig] = []
    for position, start in enumerate(starts):
        end = starts[position + 1] if position + 1 < len(starts) else len(lines)
        symbol: str | None = None
        symbol_id: int | None = None
        lead_exchange: str | None = None
        lag_exchange: str | None = None
        for line in lines[start:end]:
            if match := SYMBOL_RE.match(line):
                symbol = match.group(2)
                continue
            if match := SYMBOL_ID_RE.match(line):
                symbol_id = int(match.group(2))
                continue
            if match := EXCHANGE_RE.match(line):
                if match.group(2) == "lead":
                    lead_exchange = normalize_exchange(match.group(3))
                else:
                    lag_exchange = normalize_exchange(match.group(3))
        if (
            symbol is None
            or symbol_id is None
            or lead_exchange is None
            or lag_exchange is None
        ):
            raise ValueError(f"incomplete lead_lag pair block starting at line {start + 1}")
        pairs.append(
            PairConfig(
                symbol=symbol,
                symbol_id=symbol_id,
                lead_exchange=lead_exchange,
                lag_exchange=lag_exchange,
                block_start=start,
                block_end=end,
            )
        )
    return lines, pairs


def _iter_param_objects(data: object) -> Iterable[dict]:
    if isinstance(data, list):
        for item in data:
            if not isinstance(item, dict):
                raise ValueError("params JSON array must contain objects")
            yield item
        return
    if isinstance(data, dict):
        pairs = data.get("pairs")
        if isinstance(pairs, list):
            for item in pairs:
                if not isinstance(item, dict):
                    raise ValueError("params JSON pairs array must contain objects")
                yield item
            return
        yield data
        return
    raise ValueError("params JSON must be an object or array")


def load_params(params_json: Sequence[Path]) -> dict[tuple[int, str, str], dict]:
    result: dict[tuple[int, str, str], dict] = {}
    for path in params_json:
        data = json.loads(path.read_text(encoding="utf-8"))
        for params in _iter_param_objects(data):
            if "slippage" not in params:
                raise ValueError(f"{path} is missing slippage output")
            symbol_id = int(params["symbol_id"])
            lead_exchange = normalize_exchange(str(params["lead_exchange"]))
            lag_exchange = normalize_exchange(str(params["lag_exchange"]))
            result[(symbol_id, lead_exchange, lag_exchange)] = params
    return result


def build_rows(
    pairs: Sequence[PairConfig], params_by_pair: dict[tuple[int, str, str], dict]
) -> list[PairSlippageRow]:
    rows: list[PairSlippageRow] = []
    for pair in pairs:
        params = params_by_pair.get(
            (pair.symbol_id, pair.lead_exchange, pair.lag_exchange)
        )
        if params is None:
            raise ValueError(
                "missing taker buffer slippage params for "
                f"symbol={pair.symbol} symbol_id={pair.symbol_id} "
                f"lead_exchange={pair.lead_exchange} lag_exchange={pair.lag_exchange}"
            )
        taker = params["taker_buffer"]
        slippage = params["slippage"]
        rows.append(
            PairSlippageRow(
                symbol=pair.symbol,
                symbol_id=pair.symbol_id,
                lead_exchange=pair.lead_exchange,
                lag_exchange=pair.lag_exchange,
                entry_buffer_pct=float(taker["entry_fixed_pct"]),
                normal_close_buffer_pct=float(taker["normal_close_fixed_pct"]),
                price_tick=float(slippage["price_tick"]),
                reference_price_method=str(slippage["reference_price_method"]),
                reference_price=float(slippage["reference_price"]),
                max_bid_price=float(slippage["max_bid_price"]),
                max_ask_price=float(slippage["max_ask_price"]),
                open_long_slippage_ticks=int(
                    slippage["open_long_slippage_ticks"]
                ),
                open_short_slippage_ticks=int(
                    slippage["open_short_slippage_ticks"]
                ),
                close_long_slippage_ticks=int(
                    slippage["close_long_slippage_ticks"]
                ),
                close_short_slippage_ticks=int(
                    slippage["close_short_slippage_ticks"]
                ),
                generated_open_slippage_ticks=int(slippage["open_slippage_ticks"]),
                generated_close_slippage_ticks=int(
                    slippage["close_slippage_ticks"]
                ),
            )
        )
    return rows


def render_updated_config(
    lines: Sequence[str], pairs: Sequence[PairConfig], rows: Sequence[PairSlippageRow]
) -> str:
    updated = list(lines)
    for pair, row in reversed(list(zip(pairs, rows))):
        execute_start: int | None = None
        open_replaced = False
        close_replaced = False
        for index in range(pair.block_start, pair.block_end):
            line = updated[index]
            if DEPRECATED_SLIPPAGE_RE.match(line):
                raise ValueError(
                    "deprecated slippage field in input config; use "
                    "open_slippage_ticks and close_slippage_ticks"
                )
            if EXECUTE_HEADER_RE.match(line):
                execute_start = index
                continue
            if OPEN_SLIPPAGE_RE.match(line):
                updated[index] = OPEN_SLIPPAGE_RE.sub(
                    rf"\g<1>{row.generated_open_slippage_ticks}\g<2>", line
                )
                open_replaced = True
                continue
            if CLOSE_SLIPPAGE_RE.match(line):
                updated[index] = CLOSE_SLIPPAGE_RE.sub(
                    rf"\g<1>{row.generated_close_slippage_ticks}\g<2>", line
                )
                close_replaced = True
        if execute_start is None:
            raise ValueError(f"pair symbol={pair.symbol} has no execute table")
        insert_at = execute_start + 1
        missing_lines = []
        if not open_replaced:
            missing_lines.append(
                f"open_slippage_ticks = {row.generated_open_slippage_ticks}\n"
            )
        if not close_replaced:
            missing_lines.append(
                f"close_slippage_ticks = {row.generated_close_slippage_ticks}\n"
            )
        if missing_lines:
            updated[insert_at:insert_at] = missing_lines
    return "".join(updated)


def write_csv(path: Path, rows: Sequence[PairSlippageRow]) -> None:
    fieldnames = [
        "symbol",
        "symbol_id",
        "lead_exchange",
        "lag_exchange",
        "entry_buffer_pct",
        "normal_close_buffer_pct",
        "price_tick",
        "reference_price_method",
        "reference_price",
        "max_bid_price",
        "max_ask_price",
        "open_long_slippage_ticks",
        "open_short_slippage_ticks",
        "close_long_slippage_ticks",
        "close_short_slippage_ticks",
        "generated_open_slippage_ticks",
        "generated_close_slippage_ticks",
    ]
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow(
                {
                    "symbol": row.symbol,
                    "symbol_id": row.symbol_id,
                    "lead_exchange": row.lead_exchange,
                    "lag_exchange": row.lag_exchange,
                    "entry_buffer_pct": f"{row.entry_buffer_pct:.12g}",
                    "normal_close_buffer_pct": f"{row.normal_close_buffer_pct:.12g}",
                    "price_tick": f"{row.price_tick:.12g}",
                    "reference_price_method": row.reference_price_method,
                    "reference_price": f"{row.reference_price:.12g}",
                    "max_bid_price": f"{row.max_bid_price:.12g}",
                    "max_ask_price": f"{row.max_ask_price:.12g}",
                    "open_long_slippage_ticks": row.open_long_slippage_ticks,
                    "open_short_slippage_ticks": row.open_short_slippage_ticks,
                    "close_long_slippage_ticks": row.close_long_slippage_ticks,
                    "close_short_slippage_ticks": row.close_short_slippage_ticks,
                    "generated_open_slippage_ticks": (
                        row.generated_open_slippage_ticks
                    ),
                    "generated_close_slippage_ticks": (
                        row.generated_close_slippage_ticks
                    ),
                }
            )


def apply_slippage_to_config(
    *,
    params_json: Sequence[Path],
    config_in: Path,
    config_out: Path,
    csv_out: Path | None,
) -> ApplyResult:
    if not params_json:
        raise ValueError("params_json must not be empty")
    lines, pairs = parse_pairs(config_in.read_text(encoding="utf-8"))
    if not pairs:
        raise ValueError("input config has no [[lead_lag.pairs]] blocks")
    rows = build_rows(pairs, load_params(params_json))
    config_out.write_text(
        render_updated_config(lines, pairs, rows),
        encoding="utf-8",
    )
    if csv_out is not None:
        write_csv(csv_out, rows)
    return ApplyResult(updated_pair_count=len(rows), rows=rows)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Apply generated LeadLag taker buffer slippage ticks to strategy TOML."
    )
    parser.add_argument(
        "--params-json",
        type=Path,
        action="append",
        required=True,
        help="JSON output from generate_preflight_config_params.py; repeat per pair",
    )
    parser.add_argument("--config-in", type=Path, required=True)
    parser.add_argument("--config-out", type=Path, required=True)
    parser.add_argument("--csv-out", type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    result = apply_slippage_to_config(
        params_json=args.params_json,
        config_in=args.config_in,
        config_out=args.config_out,
        csv_out=args.csv_out,
    )
    print(
        "apply_taker_buffer_slippage "
        f"result=ok updated_pairs={result.updated_pair_count} "
        f"config_out={args.config_out}"
    )
    if args.csv_out is not None:
        print(f"csv_out={args.csv_out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
