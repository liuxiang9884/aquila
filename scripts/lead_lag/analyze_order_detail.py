#!/home/liuxiang/dev/pyenv/lx/bin/python

import argparse
import csv
import re
import sys
import tomllib
from dataclasses import dataclass
from decimal import Decimal, InvalidOperation, getcontext
from pathlib import Path


getcontext().prec = 34


ORDER_DETAIL_FIELDS = [
    "run_id",
    "local_order_id",
    "text_order_id",
    "request_sequence",
    "encoded_request_id",
    "exchange_order_id",
    "symbol",
    "symbol_id",
    "trigger_ticker_id",
    "trigger_exchange",
    "trigger_symbol_id",
    "signal_role",
    "order_role",
    "position_id",
    "position_event",
    "position_direction",
    "entry_local_order_id",
    "action",
    "side",
    "reduce_only",
    "time_in_force",
    "status",
    "finish_reason",
    "reject_reason",
    "raw_price",
    "order_price",
    "price_text",
    "price_tick",
    "slippage_ticks",
    "order_offset_ticks",
    "quantity",
    "quantity_text",
    "cumulative_filled_quantity",
    "left_quantity",
    "cancelled_quantity",
    "fill_rate",
    "average_fill_price",
    "last_fill_price",
    "contract_multiplier",
    "filled_notional",
    "fill_role",
    "exec_slippage_price",
    "exec_slippage_ticks",
    "exec_slippage_bps",
    "exec_slippage_quote",
    "limit_improvement_ticks",
    "fee_rate_config",
    "fee_quote_estimated",
    "fee_source",
    "ack_rtt_ns",
    "request_send_local_ns",
    "ack_local_receive_ns",
    "order_finished_local_ns",
    "source_schema",
    "warnings",
]


LOG_MESSAGE_RE = re.compile(r"\] (?P<message>lead_lag_|gate_order_).*$")


@dataclass(frozen=True)
class AnalysisResult:
    rows: list[dict[str, str]]


def parse_decimal(value: str | None) -> Decimal | None:
    if value is None or value == "":
        return None
    try:
        return Decimal(value)
    except InvalidOperation:
        return None


def format_decimal(value: Decimal | None) -> str:
    if value is None:
        return ""
    if value == 0:
        return "0"
    text = format(value.normalize(), "f")
    if "." in text:
        text = text.rstrip("0").rstrip(".")
    return text


def message_from_line(line: str) -> str | None:
    match = LOG_MESSAGE_RE.search(line)
    if match is None:
        return None
    return line[match.start("message") :].strip()


def parse_message(message: str) -> tuple[str, dict[str, str]]:
    parts = message.split()
    if not parts:
        return "", {}
    tag = parts[0]
    fields: dict[str, str] = {}
    for token in parts[1:]:
        if "=" not in token:
            continue
        key, value = token.split("=", 1)
        fields[key] = value
    return tag, fields


def load_toml(path: Path) -> dict:
    with path.open("rb") as input_file:
        return tomllib.load(input_file)


def resolve_strategy_config(path: Path | None) -> Path | None:
    if path is None:
        return None
    data = load_toml(path)
    if "lead_lag" in data:
        return path
    nested = data.get("strategy", {}).get("config")
    if not isinstance(nested, str) or nested == "":
        return path
    nested_path = Path(nested)
    if not nested_path.is_absolute():
        nested_path = path.parent / nested_path
        if not nested_path.exists():
            nested_path = Path.cwd() / nested
    return nested_path


def load_pair_config(path: Path | None) -> dict[str, dict[str, str]]:
    strategy_path = resolve_strategy_config(path)
    if strategy_path is None or not strategy_path.exists():
        return {}
    data = load_toml(strategy_path)
    lead_lag = data.get("lead_lag", {})
    result: dict[str, dict[str, str]] = {}
    for pair in lead_lag.get("pairs", []):
        symbol = pair.get("symbol")
        if not isinstance(symbol, str):
            continue
        execute = pair.get("execute", {})
        result[symbol] = {
            "fee_rate_config": str(pair.get("lag_taker_fee", "")),
            "open_slippage": str(execute.get("open_slippage", "")),
            "close_slippage": str(execute.get("close_slippage", "")),
        }
    return result


def load_instrument_catalog(path: Path | None) -> dict[str, dict[str, str]]:
    if path is None or not path.exists():
        return {}
    result: dict[str, dict[str, str]] = {}
    with path.open(newline="", encoding="utf-8") as input_file:
        for row in csv.DictReader(input_file):
            if row.get("exchange") != "gate":
                continue
            symbol = row.get("symbol", "")
            if symbol == "":
                continue
            result[symbol] = {
                "contract_multiplier": row.get("notional_multiplier", ""),
                "price_tick": row.get("price_tick", ""),
            }
    return result


def append_warning(order: dict[str, str], warning: str) -> None:
    existing = order.get("warnings", "")
    if existing == "":
        order["warnings"] = warning
    elif warning not in existing.split(";"):
        order["warnings"] = existing + ";" + warning


def order_role_for(action: str, reduce_only: str) -> str:
    if reduce_only == "true" or action in {
        "kCloseLong",
        "kCloseShort",
        "kStoplossLong",
        "kStoplossShort",
    }:
        return "exit"
    if action in {"kOpenLong", "kOpenShort"}:
        return "entry"
    return ""


def choose_nonzero(*values: str | None) -> str:
    for value in values:
        if value not in (None, "", "0"):
            return value
    for value in values:
        if value not in (None, ""):
            return value
    return ""


def merge_submitted(order: dict[str, str], fields: dict[str, str]) -> None:
    copy_fields = {
        "local_order_id",
        "trigger_ticker_id",
        "trigger_exchange",
        "trigger_symbol_id",
        "symbol",
        "symbol_id",
        "signal_role",
        "order_role",
        "position_id",
        "position_event",
        "position_direction",
        "entry_local_order_id",
        "action",
        "side",
        "reduce_only",
        "quantity",
        "quantity_text",
        "raw_price",
        "order_price",
        "price_text",
        "slippage_ticks",
        "price_tick",
        "target_open_notional",
        "estimated_notional",
    }
    for key in copy_fields:
        if key in fields:
            order[key] = fields[key]
    order["source_schema"] = "submitted_v1"


def merge_send(order: dict[str, str], fields: dict[str, str]) -> None:
    order["local_order_id"] = fields.get("local_order_id", order.get("local_order_id", ""))
    order["request_sequence"] = fields.get("request_sequence", "")
    order["encoded_request_id"] = fields.get("encoded_request_id", "")
    order.setdefault("symbol", fields.get("contract", ""))
    order.setdefault("side", fields.get("side", ""))
    order.setdefault("reduce_only", fields.get("reduce_only", ""))
    order.setdefault("quantity", fields.get("quantity", ""))
    order.setdefault("order_price", fields.get("price", ""))
    order.setdefault("price_text", fields.get("price", ""))
    order.setdefault("quantity_text", fields.get("quantity", ""))
    order["time_in_force"] = fields.get("tif", "")
    order["request_send_local_ns"] = fields.get("request_send_local_ns", "")


def merge_ack(order: dict[str, str], fields: dict[str, str]) -> None:
    order["ack_local_receive_ns"] = fields.get("local_receive_ns", "")
    if fields.get("exchange_order_id") not in (None, "", "0"):
        order["exchange_order_id"] = fields["exchange_order_id"]
    send_ns = fields.get("request_send_local_ns") or order.get("request_send_local_ns")
    ack_ns = fields.get("local_receive_ns")
    if send_ns and ack_ns:
        try:
            order["ack_rtt_ns"] = str(int(ack_ns) - int(send_ns))
        except ValueError:
            pass


def merge_feedback(order: dict[str, str], fields: dict[str, str]) -> None:
    order["exchange_order_id"] = choose_nonzero(
        fields.get("exchange_order_id"), order.get("exchange_order_id")
    )
    order["cumulative_filled_quantity"] = fields.get(
        "cumulative_filled_quantity", order.get("cumulative_filled_quantity", "")
    )
    order["left_quantity"] = fields.get("left_quantity", order.get("left_quantity", ""))
    order["cancelled_quantity"] = fields.get(
        "cancelled_quantity", order.get("cancelled_quantity", "")
    )
    order["last_fill_price"] = fields.get("fill_price", order.get("last_fill_price", ""))
    order["fill_role"] = fields.get("role", order.get("fill_role", ""))
    order["finish_reason"] = fields.get(
        "finish_reason", order.get("finish_reason", "")
    )
    order["reject_reason"] = fields.get("reject_reason", order.get("reject_reason", ""))
    if "kind" in fields and "status" not in order:
        order["status"] = fields["kind"]


def merge_finished(order: dict[str, str], fields: dict[str, str]) -> None:
    for key in (
        "symbol_id",
        "symbol",
        "status",
        "reduce_only",
        "quantity",
        "cumulative_filled_quantity",
        "average_fill_price",
        "last_fill_price",
        "request_send_local_ns",
        "ack_local_receive_ns",
        "ack_rtt_ns",
        "position_id",
        "position_direction",
        "order_role",
        "entry_local_order_id",
        "order_finished_local_ns",
    ):
        if key in fields:
            order[key] = fields[key]
    order["exchange_order_id"] = choose_nonzero(
        fields.get("exchange_order_id"), order.get("exchange_order_id")
    )


def enrich_order(order: dict[str, str], pair_config: dict[str, str], instrument: dict[str, str]) -> None:
    symbol = order.get("symbol", "")
    if order.get("text_order_id", "") == "" and order.get("local_order_id", ""):
        order["text_order_id"] = "t-" + order["local_order_id"]
    if order.get("order_role", "") == "":
        order["order_role"] = order_role_for(
            order.get("action", ""), order.get("reduce_only", "")
        )
    if order.get("price_tick", "") == "":
        order["price_tick"] = instrument.get("price_tick", "")
    order["contract_multiplier"] = instrument.get("contract_multiplier", "")
    if order.get("fee_rate_config", "") == "":
        order["fee_rate_config"] = pair_config.get("fee_rate_config", "")

    raw_price = parse_decimal(order.get("raw_price"))
    order_price = parse_decimal(order.get("order_price"))
    price_tick = parse_decimal(order.get("price_tick"))
    average_fill = parse_decimal(order.get("average_fill_price"))
    last_fill = parse_decimal(order.get("last_fill_price"))
    fill_price = average_fill if average_fill is not None and average_fill > 0 else last_fill
    quantity = parse_decimal(order.get("quantity"))
    filled_quantity = parse_decimal(order.get("cumulative_filled_quantity"))
    multiplier = parse_decimal(order.get("contract_multiplier"))
    fee_rate = parse_decimal(order.get("fee_rate_config"))
    side = order.get("side", "")

    if quantity is not None and quantity != 0 and filled_quantity is not None:
        order["fill_rate"] = format_decimal(filled_quantity / quantity)

    if raw_price is not None and order_price is not None and price_tick not in (None, Decimal(0)):
        if side == "kBuy":
            order["order_offset_ticks"] = format_decimal((order_price - raw_price) / price_tick)
        elif side == "kSell":
            order["order_offset_ticks"] = format_decimal((raw_price - order_price) / price_tick)

    if (
        fill_price is not None
        and fill_price > 0
        and raw_price is not None
        and price_tick not in (None, Decimal(0))
    ):
        if side == "kBuy":
            slippage_price = fill_price - raw_price
        elif side == "kSell":
            slippage_price = raw_price - fill_price
        else:
            slippage_price = None
        if slippage_price is not None:
            order["exec_slippage_price"] = format_decimal(slippage_price)
            order["exec_slippage_ticks"] = format_decimal(slippage_price / price_tick)
            if raw_price != 0:
                order["exec_slippage_bps"] = format_decimal(
                    slippage_price / raw_price * Decimal("10000")
                )
            if filled_quantity is not None and multiplier is not None:
                order["exec_slippage_quote"] = format_decimal(
                    slippage_price * filled_quantity * multiplier
                )

    if (
        fill_price is not None
        and fill_price > 0
        and order_price is not None
        and price_tick not in (None, Decimal(0))
    ):
        if side == "kBuy":
            order["limit_improvement_ticks"] = format_decimal(
                (order_price - fill_price) / price_tick
            )
        elif side == "kSell":
            order["limit_improvement_ticks"] = format_decimal(
                (fill_price - order_price) / price_tick
            )

    if (
        fill_price is not None
        and filled_quantity is not None
        and multiplier is not None
        and fill_price > 0
    ):
        filled_notional = fill_price * filled_quantity * multiplier
        order["filled_notional"] = format_decimal(filled_notional)
        if fee_rate is not None:
            order["fee_quote_estimated"] = format_decimal(filled_notional * fee_rate)
            order["fee_source"] = "config_estimated"
    elif order.get("fee_source", "") == "":
        order["fee_source"] = ""

    if order.get("exchange_order_id", "") in ("", "0") and order.get("status", ""):
        append_warning(order, "missing_exchange_order_id")
    if order.get("source_schema", "") == "":
        order["source_schema"] = "unknown"
        append_warning(order, "missing_submitted_log")
    if symbol == "":
        append_warning(order, "missing_symbol")


def analyze_order_detail(
    log_path: Path,
    *,
    config_path: Path | None = None,
    instrument_catalog_path: Path | None = None,
    run_id: str | None = None,
) -> AnalysisResult:
    pair_configs = load_pair_config(config_path)
    instruments = load_instrument_catalog(instrument_catalog_path)
    orders: dict[str, dict[str, str]] = {}
    run = run_id or log_path.parent.name

    with log_path.open(encoding="utf-8") as input_file:
        for line in input_file:
            message = message_from_line(line)
            if message is None:
                continue
            tag, fields = parse_message(message)
            if tag == "lead_lag_order_submitted":
                local_order_id = fields.get("local_order_id", "")
                if local_order_id == "":
                    continue
                order = orders.setdefault(local_order_id, {"run_id": run, "warnings": ""})
                merge_submitted(order, fields)
            elif tag == "gate_order_send_ok" and fields.get("type") == "place":
                local_order_id = fields.get("local_order_id", "")
                if local_order_id == "":
                    continue
                order = orders.setdefault(local_order_id, {"run_id": run, "warnings": ""})
                merge_send(order, fields)
            elif tag == "gate_order_response" and fields.get("kind") == "kAck":
                local_order_id = fields.get("local_order_id", "")
                if local_order_id == "":
                    continue
                order = orders.setdefault(local_order_id, {"run_id": run, "warnings": ""})
                merge_ack(order, fields)
            elif tag == "lead_lag_order_feedback":
                local_order_id = fields.get("local_order_id", "")
                if local_order_id == "":
                    continue
                order = orders.setdefault(local_order_id, {"run_id": run, "warnings": ""})
                merge_feedback(order, fields)
            elif tag == "lead_lag_order_finished":
                local_order_id = fields.get("local_order_id", "")
                if local_order_id == "":
                    continue
                order = orders.setdefault(local_order_id, {"run_id": run, "warnings": ""})
                merge_finished(order, fields)

    rows: list[dict[str, str]] = []
    for local_order_id in sorted(orders, key=lambda value: int(value)):
        order = orders[local_order_id]
        symbol = order.get("symbol", "")
        enrich_order(order, pair_configs.get(symbol, {}), instruments.get(symbol, {}))
        rows.append({field: order.get(field, "") for field in ORDER_DETAIL_FIELDS})
    return AnalysisResult(rows=rows)


def write_order_detail_csv(rows: list[dict[str, str]], output_path: Path) -> None:
    with output_path.open("w", newline="", encoding="utf-8") as output_file:
        writer = csv.DictWriter(output_file, fieldnames=ORDER_DETAIL_FIELDS)
        writer.writeheader()
        writer.writerows(rows)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build LeadLag order_detail.csv")
    parser.add_argument("--log", required=True, type=Path)
    parser.add_argument("--config", type=Path)
    parser.add_argument(
        "--instrument-catalog",
        type=Path,
        default=Path("config/instruments/usdt_futures.csv"),
    )
    parser.add_argument("--run-id")
    parser.add_argument("--output", required=True, type=Path)
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    result = analyze_order_detail(
        args.log,
        config_path=args.config,
        instrument_catalog_path=args.instrument_catalog,
        run_id=args.run_id,
    )
    write_order_detail_csv(result.rows, args.output)
    print(f"wrote {len(result.rows)} order rows to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
