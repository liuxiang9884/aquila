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
    "trigger_exchange_ns",
    "trigger_local_ns",
    "on_book_ticker_entry_ns",
    "signal_decision_ns",
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
    "order_session_id",
    "owner_thread_cpu",
    "owner_thread_tid",
    "local_ip",
    "local_port",
    "remote_ip",
    "remote_port",
    "send_cpu",
    "ack_cpu",
    "diagnostic_cpu",
    "tcp_info_available",
    "tcp_info_rtt_us",
    "tcp_info_rttvar_us",
    "tcp_info_retrans",
    "tcp_info_total_retrans",
    "tcp_info_unacked",
    "tcp_info_snd_cwnd",
    "ack_rtt_ns",
    "latency_diagnostic_reason",
    "latency_diagnostic_ack_rtt_ns",
    "send_to_first_after_hook_ns",
    "send_to_first_drive_read_ns",
    "drive_read_duration_ns",
    "max_observed_drive_read_duration_ns",
    "latency_diagnostic_inflight_at_send",
    "max_runtime_loop_gap_ns",
    "runtime_loop_iterations_before_ack",
    "order_encode_done_ns",
    "ws_frame_encode_done_ns",
    "write_enqueue_ns",
    "drive_write_enter_ns",
    "write_some_enter_ns",
    "write_some_return_ns",
    "write_complete_ns",
    "write_some_bytes",
    "write_complete_bytes",
    "write_errno",
    "write_eagain",
    "pending_write_count_after",
    "socket_send_queue_available",
    "tcp_sendq_bytes",
    "tcp_notsent_bytes",
    "request_send_local_ns",
    "ack_local_receive_ns",
    "order_finished_local_ns",
    "source_schema",
    "warnings",
]


POSITION_DETAIL_FIELDS = [
    "run_id",
    "position_key",
    "symbol",
    "symbol_id",
    "position_id",
    "position_direction",
    "status",
    "entry_local_order_id",
    "exit_local_order_id",
    "entry_exchange_order_id",
    "exit_exchange_order_id",
    "entry_ns",
    "exit_ns",
    "holding_ns",
    "entry_side",
    "exit_side",
    "entry_raw_price",
    "exit_raw_price",
    "entry_order_price",
    "exit_order_price",
    "entry_price",
    "exit_price",
    "entry_volume",
    "exit_volume",
    "matched_volume",
    "remaining_entry_volume",
    "contract_multiplier",
    "entry_notional",
    "exit_notional",
    "gross_pnl",
    "entry_fee_quote_estimated",
    "exit_fee_quote_estimated",
    "total_fee_quote_estimated",
    "net_pnl",
    "entry_ack_rtt_ns",
    "exit_ack_rtt_ns",
    "entry_fee_source",
    "exit_fee_source",
    "warnings",
]


LATENCY_DETAIL_FIELDS = [
    "run_id",
    "latency_key",
    "local_order_id",
    "exchange_order_id",
    "symbol",
    "symbol_id",
    "position_id",
    "position_direction",
    "order_role",
    "action",
    "side",
    "reduce_only",
    "status",
    "finish_reason",
    "reject_reason",
    "request_sequence",
    "encoded_request_id",
    "trigger_exchange_ns",
    "trigger_local_ns",
    "on_book_ticker_entry_ns",
    "signal_decision_ns",
    "request_send_local_ns",
    "ack_local_receive_ns",
    "response_local_receive_ns",
    "order_finished_local_ns",
    "ack_exchange_ns",
    "response_exchange_ns",
    "accepted_exchange_ns",
    "finish_exchange_ns",
    "ack_rtt_ns",
    "response_rtt_ns",
    "send_to_ack_local_ns",
    "send_to_response_local_ns",
    "send_to_finish_local_ns",
    "ack_to_finish_local_ns",
    "bbo_to_strategy_ns",
    "strategy_to_signal_ns",
    "signal_to_request_send_ns",
    "trigger_to_request_send_ns",
    "ack_exchange_to_local_ns",
    "response_exchange_to_local_ns",
    "exchange_lifecycle_ns",
    "order_session_id",
    "owner_thread_cpu",
    "owner_thread_tid",
    "local_ip",
    "local_port",
    "remote_ip",
    "remote_port",
    "send_cpu",
    "ack_cpu",
    "diagnostic_cpu",
    "tcp_info_available",
    "tcp_info_rtt_us",
    "tcp_info_rttvar_us",
    "tcp_info_retrans",
    "tcp_info_total_retrans",
    "tcp_info_unacked",
    "tcp_info_snd_cwnd",
    "latency_diagnostic_reason",
    "latency_diagnostic_ack_rtt_ns",
    "send_to_first_after_hook_ns",
    "send_to_first_drive_read_ns",
    "drive_read_duration_ns",
    "max_observed_drive_read_duration_ns",
    "latency_diagnostic_inflight_at_send",
    "max_runtime_loop_gap_ns",
    "runtime_loop_iterations_before_ack",
    "order_encode_done_ns",
    "ws_frame_encode_done_ns",
    "write_enqueue_ns",
    "drive_write_enter_ns",
    "write_some_enter_ns",
    "write_some_return_ns",
    "write_complete_ns",
    "write_some_bytes",
    "write_complete_bytes",
    "write_errno",
    "write_eagain",
    "pending_write_count_after",
    "socket_send_queue_available",
    "tcp_sendq_bytes",
    "tcp_notsent_bytes",
    "warnings",
]


LOG_MESSAGE_RE = re.compile(r"\] (?P<message>lead_lag_|gate_order_|feedback_event).*$")
RAW_MESSAGE_PREFIXES = (
    "lead_lag_strategy_live_open_close_smoke_summary ",
)


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
    if match is not None:
        return line[match.start("message") :].strip()
    stripped = line.strip()
    if stripped.startswith(RAW_MESSAGE_PREFIXES):
        return stripped
    return None


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
                "symbol_id": row.get("symbol_id", ""),
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


def append_unique_text(order: dict[str, str], field: str, value: str | None) -> None:
    if value in (None, ""):
        return
    existing = order.get(field, "")
    if existing == "":
        order[field] = value
    elif value not in existing.split(";"):
        order[field] = existing + ";" + value


def choose_first_nonzero_text(existing: str | None, value: str | None) -> str:
    if existing not in (None, "", "0"):
        return existing
    if value not in (None, "", "0"):
        return value
    return existing or value or ""


def choose_first_present_text(existing: str | None, value: str | None) -> str:
    if existing not in (None, ""):
        return existing
    if value not in (None, ""):
        return value
    return existing or value or ""


def max_int_text(existing: str | None, value: str | None) -> str:
    if value in (None, ""):
        return existing or ""
    try:
        parsed = int(value)
    except ValueError:
        return existing or ""
    if existing in (None, ""):
        return value
    try:
        current = int(existing)
    except ValueError:
        return value
    return str(max(current, parsed))


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
        "trigger_exchange_ns",
        "trigger_local_ns",
        "on_book_ticker_entry_ns",
        "signal_decision_ns",
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
    for key in ("order_session_id", "send_cpu"):
        if fields.get(key) not in (None, ""):
            order[key] = fields[key]
    order.setdefault("symbol", fields.get("contract", ""))
    order.setdefault("side", fields.get("side", ""))
    order.setdefault("reduce_only", fields.get("reduce_only", ""))
    order.setdefault("quantity", fields.get("quantity", ""))
    order.setdefault("order_price", fields.get("price", ""))
    order.setdefault("price_text", fields.get("price", ""))
    order.setdefault("quantity_text", fields.get("quantity", ""))
    order["time_in_force"] = fields.get("tif", "")
    order["request_send_local_ns"] = fields.get("request_send_local_ns", "")


def merge_session_connected(
    session: dict[str, str], fields: dict[str, str]
) -> None:
    for key in (
        "order_session_id",
        "owner_thread_cpu",
        "owner_thread_tid",
        "local_ip",
        "local_port",
        "remote_ip",
        "remote_port",
    ):
        if fields.get(key) not in (None, ""):
            session[key] = fields[key]


def merge_session_snapshot(order: dict[str, str], session: dict[str, str]) -> None:
    for key in (
        "owner_thread_cpu",
        "owner_thread_tid",
        "local_ip",
        "local_port",
        "remote_ip",
        "remote_port",
    ):
        if order.get(key, "") == "" and session.get(key, "") != "":
            order[key] = session[key]


def merge_tcp_info(order: dict[str, str], fields: dict[str, str]) -> None:
    if fields.get("tcp_info_available") == "true":
        order["tcp_info_available"] = "true"
    elif order.get("tcp_info_available", "") == "":
        order["tcp_info_available"] = fields.get("tcp_info_available", "")
    for key in (
        "tcp_info_rtt_us",
        "tcp_info_rttvar_us",
        "tcp_info_retrans",
        "tcp_info_total_retrans",
        "tcp_info_unacked",
        "tcp_info_snd_cwnd",
    ):
        order[key] = max_int_text(order.get(key), fields.get(key))


def merge_ack(order: dict[str, str], fields: dict[str, str]) -> None:
    for key in ("order_session_id", "ack_cpu"):
        if fields.get(key) not in (None, ""):
            order[key] = fields[key]
    merge_tcp_info(order, fields)
    order["ack_local_receive_ns"] = fields.get("local_receive_ns", "")
    order["ack_exchange_ns"] = fields.get("exchange_ns", "")
    order["ack_exchange_to_local_ns"] = fields.get("exchange_to_local_ns", "")
    if fields.get("exchange_order_id") not in (None, "", "0"):
        order["exchange_order_id"] = fields["exchange_order_id"]
    send_ns = fields.get("request_send_local_ns") or order.get("request_send_local_ns")
    ack_ns = fields.get("local_receive_ns")
    if send_ns and ack_ns:
        try:
            order["ack_rtt_ns"] = str(int(ack_ns) - int(send_ns))
        except ValueError:
            pass


def merge_submit_response(order: dict[str, str], fields: dict[str, str]) -> None:
    for key in ("order_session_id", "ack_cpu"):
        if fields.get(key) not in (None, ""):
            order[key] = fields[key]
    merge_tcp_info(order, fields)
    order["exchange_order_id"] = choose_nonzero(
        fields.get("exchange_order_id"), order.get("exchange_order_id")
    )
    order["response_local_receive_ns"] = fields.get("local_receive_ns", "")
    order["response_exchange_ns"] = fields.get("exchange_ns", "")
    order["response_exchange_to_local_ns"] = fields.get("exchange_to_local_ns", "")
    send_ns = order.get("request_send_local_ns")
    response_ns = fields.get("local_receive_ns")
    if send_ns and response_ns:
        try:
            order["response_rtt_ns"] = str(int(response_ns) - int(send_ns))
        except ValueError:
            pass


def merge_latency_diagnostic(order: dict[str, str], fields: dict[str, str]) -> None:
    if fields.get("order_session_id") not in (None, ""):
        order["order_session_id"] = fields["order_session_id"]
    append_unique_text(order, "diagnostic_cpu", fields.get("diagnostic_cpu"))
    merge_tcp_info(order, fields)
    append_unique_text(order, "latency_diagnostic_reason", fields.get("reason"))
    order["latency_diagnostic_ack_rtt_ns"] = max_int_text(
        order.get("latency_diagnostic_ack_rtt_ns"), fields.get("ack_rtt_ns")
    )
    order["send_to_first_after_hook_ns"] = choose_first_nonzero_text(
        order.get("send_to_first_after_hook_ns"),
        fields.get("send_to_first_after_hook_ns"),
    )
    order["send_to_first_drive_read_ns"] = choose_first_nonzero_text(
        order.get("send_to_first_drive_read_ns"),
        fields.get("send_to_first_drive_read_ns"),
    )
    order["drive_read_duration_ns"] = max_int_text(
        order.get("drive_read_duration_ns"), fields.get("drive_read_duration_ns")
    )
    order["max_observed_drive_read_duration_ns"] = max_int_text(
        order.get("max_observed_drive_read_duration_ns"),
        fields.get("max_observed_drive_read_duration_ns"),
    )
    order["latency_diagnostic_inflight_at_send"] = choose_first_nonzero_text(
        order.get("latency_diagnostic_inflight_at_send"),
        fields.get("inflight_at_send"),
    )
    for key in (
        "owner_thread_tid",
        "order_encode_done_ns",
        "ws_frame_encode_done_ns",
        "write_enqueue_ns",
        "drive_write_enter_ns",
        "write_some_enter_ns",
        "write_some_return_ns",
        "write_complete_ns",
        "write_some_bytes",
        "write_complete_bytes",
        "write_errno",
        "pending_write_count_after",
        "tcp_sendq_bytes",
        "tcp_notsent_bytes",
    ):
        order[key] = choose_first_present_text(order.get(key), fields.get(key))
    order["max_runtime_loop_gap_ns"] = max_int_text(
        order.get("max_runtime_loop_gap_ns"), fields.get("max_runtime_loop_gap_ns")
    )
    order["runtime_loop_iterations_before_ack"] = max_int_text(
        order.get("runtime_loop_iterations_before_ack"),
        fields.get("runtime_loop_iterations_before_ack"),
    )
    for key in ("write_eagain", "socket_send_queue_available"):
        if fields.get(key) == "true":
            order[key] = "true"
        elif order.get(key, "") == "":
            order[key] = fields.get(key, "")
    if order.get("request_send_local_ns", "") == "":
        order["request_send_local_ns"] = fields.get("request_send_local_ns", "")
    if (
        order.get("ack_local_receive_ns", "") == ""
        and fields.get("ack_local_receive_ns") not in (None, "", "0")
    ):
        order["ack_local_receive_ns"] = fields["ack_local_receive_ns"]
    if (
        order.get("ack_exchange_ns", "") == ""
        and fields.get("ack_exchange_ns") not in (None, "", "0")
    ):
        order["ack_exchange_ns"] = fields["ack_exchange_ns"]
    if order.get("ack_rtt_ns", "") == "" and fields.get("ack_rtt_ns") not in (
        None,
        "",
        "0",
    ):
        order["ack_rtt_ns"] = fields["ack_rtt_ns"]


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
    kind = fields.get("kind", "")
    if kind in ("kFilled", "kCancelled", "kRejected"):
        order["finish_exchange_ns"] = fields.get(
            "exchange_update_ns", order.get("finish_exchange_ns", "")
        )
    elif kind == "kAccepted":
        order["accepted_exchange_ns"] = fields.get(
            "exchange_update_ns", order.get("accepted_exchange_ns", "")
        )
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
        "response_local_receive_ns",
        "ack_exchange_ns",
        "response_exchange_ns",
        "accepted_exchange_ns",
        "finish_exchange_ns",
        "ack_rtt_ns",
        "response_rtt_ns",
        "ack_exchange_to_local_ns",
        "response_exchange_to_local_ns",
        "exchange_lifecycle_ns",
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


def mark_open_close_smoke_position(
    orders: dict[str, dict[str, str]], run: str, fields: dict[str, str]
) -> None:
    if fields.get("completed") != "true":
        return
    open_local_order_id = fields.get("open_local_order_id", "")
    close_local_order_id = fields.get("close_local_order_id", "")
    if open_local_order_id in ("", "0") or close_local_order_id in ("", "0"):
        return
    open_order = orders.setdefault(
        open_local_order_id, {"run_id": run, "warnings": ""}
    )
    close_order = orders.setdefault(
        close_local_order_id, {"run_id": run, "warnings": ""}
    )
    open_order.setdefault("local_order_id", open_local_order_id)
    close_order.setdefault("local_order_id", close_local_order_id)

    open_side = open_order.get("side", "")
    close_side = close_order.get("side", "")
    if open_side == "kSell" or close_side == "kBuy":
        direction = "kShort"
        open_action = "kOpenShort"
        close_action = "kCloseShort"
    else:
        direction = "kLong"
        open_action = "kOpenLong"
        close_action = "kCloseLong"

    position_id = open_local_order_id
    for order, role, action, event in (
        (open_order, "entry", open_action, "kEntrySubmit"),
        (close_order, "exit", close_action, "kExitSubmit"),
    ):
        if order.get("order_role", "") == "":
            order["order_role"] = role
        if order.get("position_id", "") == "":
            order["position_id"] = position_id
        if order.get("position_direction", "") == "":
            order["position_direction"] = direction
        if order.get("entry_local_order_id", "") == "":
            order["entry_local_order_id"] = open_local_order_id
        if order.get("action", "") == "":
            order["action"] = action
        if order.get("position_event", "") == "":
            order["position_event"] = event
        if order.get("source_schema", "") == "":
            order["source_schema"] = "smoke_summary_v1"


def enrich_order(
    order: dict[str, str],
    pair_config: dict[str, str],
    instrument: dict[str, str],
) -> None:
    symbol = order.get("symbol", "")
    if order.get("text_order_id", "") == "" and order.get("local_order_id", ""):
        order["text_order_id"] = "t-" + order["local_order_id"]
    if order.get("order_role", "") == "":
        order["order_role"] = order_role_for(
            order.get("action", ""), order.get("reduce_only", "")
        )
    if order.get("price_tick", "") == "":
        order["price_tick"] = instrument.get("price_tick", "")
    if order.get("symbol_id", "") == "":
        order["symbol_id"] = instrument.get("symbol_id", "")
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
    order_sessions: dict[str, dict[str, str]] = {}
    run = run_id or log_path.parent.name

    with log_path.open(encoding="utf-8") as input_file:
        for line in input_file:
            message = message_from_line(line)
            if message is None:
                continue
            tag, fields = parse_message(message)
            if tag == "gate_order_session_connected":
                order_session_id = fields.get("order_session_id", "")
                if order_session_id != "":
                    session = order_sessions.setdefault(order_session_id, {})
                    merge_session_connected(session, fields)
            elif tag == "lead_lag_order_submitted":
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
            elif tag == "gate_order_response":
                local_order_id = fields.get("local_order_id", "")
                if local_order_id == "":
                    continue
                order = orders.setdefault(local_order_id, {"run_id": run, "warnings": ""})
                if fields.get("kind") == "kAck":
                    merge_ack(order, fields)
                else:
                    merge_submit_response(order, fields)
            elif tag == "gate_order_ack_latency_diagnostic":
                local_order_id = fields.get("local_order_id", "")
                if local_order_id == "":
                    continue
                order = orders.setdefault(local_order_id, {"run_id": run, "warnings": ""})
                merge_latency_diagnostic(order, fields)
            elif tag == "lead_lag_order_feedback":
                local_order_id = fields.get("local_order_id", "")
                if local_order_id == "":
                    continue
                order = orders.setdefault(local_order_id, {"run_id": run, "warnings": ""})
                merge_feedback(order, fields)
            elif tag == "feedback_event" and fields.get("publish_ok") == "true":
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
            elif tag == "lead_lag_strategy_live_open_close_smoke_summary":
                mark_open_close_smoke_position(orders, run, fields)

    rows: list[dict[str, str]] = []
    for local_order_id in sorted(orders, key=lambda value: int(value)):
        order = orders[local_order_id]
        order_session_id = order.get("order_session_id", "")
        if order_session_id != "":
            merge_session_snapshot(
                order, order_sessions.get(order_session_id, {})
            )
        symbol = order.get("symbol", "")
        enrich_order(order, pair_configs.get(symbol, {}), instruments.get(symbol, {}))
        rows.append(dict(order))
    return AnalysisResult(rows=rows)


def write_order_detail_csv(rows: list[dict[str, str]], output_path: Path) -> None:
    with output_path.open("w", newline="", encoding="utf-8") as output_file:
        writer = csv.DictWriter(
            output_file,
            fieldnames=ORDER_DETAIL_FIELDS,
            extrasaction="ignore",
            lineterminator="\n",
        )
        writer.writeheader()
        writer.writerows(rows)


def order_fill_price(order: dict[str, str]) -> Decimal | None:
    average_fill = parse_decimal(order.get("average_fill_price"))
    if average_fill is not None and average_fill > 0:
        return average_fill
    last_fill = parse_decimal(order.get("last_fill_price"))
    if last_fill is not None and last_fill > 0:
        return last_fill
    return None


def order_filled_quantity(order: dict[str, str]) -> Decimal:
    return parse_decimal(order.get("cumulative_filled_quantity")) or Decimal(0)


def order_finished_ns(order: dict[str, str]) -> str:
    return order.get("order_finished_local_ns", "")


def order_sort_ns(order: dict[str, str]) -> str:
    return (
        order.get("order_finished_local_ns")
        or order.get("ack_local_receive_ns")
        or order.get("request_send_local_ns")
        or ""
    )


def int_delta_text(lhs: str, rhs: str) -> str:
    if lhs == "" or rhs == "":
        return ""
    try:
        return str(int(lhs) - int(rhs))
    except ValueError:
        return ""


def decimal_ratio(
    amount: Decimal | None, numerator: Decimal, denominator: Decimal
) -> Decimal | None:
    if amount is None:
        return None
    if denominator == 0:
        return None
    return amount * numerator / denominator


def notional_for(
    price: Decimal | None, quantity: Decimal, multiplier: Decimal | None
) -> Decimal | None:
    if price is None or multiplier is None:
        return None
    return price * quantity * multiplier


def position_group_key(order: dict[str, str]) -> tuple[str, str, str]:
    return (
        order.get("run_id", ""),
        order.get("symbol_id", ""),
        order.get("position_id", ""),
    )


def position_sort_key(key: tuple[str, str, str]) -> tuple[str, int, int]:
    run_id, symbol_id, position_id = key
    try:
        symbol_sort = int(symbol_id)
    except ValueError:
        symbol_sort = -1
    try:
        position_sort = int(position_id)
    except ValueError:
        position_sort = -1
    return run_id, symbol_sort, position_sort


def order_sort_key(order: dict[str, str]) -> tuple[int, int]:
    ns = order_sort_ns(order)
    local_order_id = order.get("local_order_id", "")
    try:
        ns_sort = int(ns)
    except ValueError:
        ns_sort = 0
    try:
        order_sort = int(local_order_id)
    except ValueError:
        order_sort = 0
    return ns_sort, order_sort


def append_row_warning(row: dict[str, str], warning: str) -> None:
    append_warning(row, warning)


def build_closed_position_row(
    entry: dict[str, str],
    exit_order: dict[str, str],
    matched_volume: Decimal,
    remaining_entry_volume: Decimal,
    entry_fee: Decimal | None,
    exit_fee: Decimal | None,
    status: str,
    warnings: list[str],
) -> dict[str, str]:
    entry_price = order_fill_price(entry)
    exit_price = order_fill_price(exit_order)
    multiplier = parse_decimal(entry.get("contract_multiplier")) or parse_decimal(
        exit_order.get("contract_multiplier")
    )
    direction = entry.get("position_direction") or exit_order.get("position_direction", "")
    gross_pnl: Decimal | None = None
    if entry_price is not None and exit_price is not None and multiplier is not None:
        if direction == "kLong":
            gross_pnl = (exit_price - entry_price) * matched_volume * multiplier
        elif direction == "kShort":
            gross_pnl = (entry_price - exit_price) * matched_volume * multiplier
    total_fee: Decimal | None = None
    if entry_fee is not None or exit_fee is not None:
        total_fee = (entry_fee or Decimal(0)) + (exit_fee or Decimal(0))
    net_pnl = None
    if gross_pnl is not None:
        net_pnl = gross_pnl - (total_fee or Decimal(0))
    entry_ns = order_finished_ns(entry)
    exit_ns = order_finished_ns(exit_order)
    row = {
        "run_id": entry.get("run_id", exit_order.get("run_id", "")),
        "position_key": (
            f"{entry.get('run_id', exit_order.get('run_id', ''))}:"
            f"{entry.get('symbol_id', exit_order.get('symbol_id', ''))}:"
            f"{entry.get('position_id', exit_order.get('position_id', ''))}:"
            f"{exit_order.get('local_order_id', '')}"
        ),
        "symbol": entry.get("symbol") or exit_order.get("symbol", ""),
        "symbol_id": entry.get("symbol_id") or exit_order.get("symbol_id", ""),
        "position_id": entry.get("position_id") or exit_order.get("position_id", ""),
        "position_direction": direction,
        "status": status,
        "entry_local_order_id": entry.get("local_order_id", ""),
        "exit_local_order_id": exit_order.get("local_order_id", ""),
        "entry_exchange_order_id": entry.get("exchange_order_id", ""),
        "exit_exchange_order_id": exit_order.get("exchange_order_id", ""),
        "entry_ns": entry_ns,
        "exit_ns": exit_ns,
        "holding_ns": int_delta_text(exit_ns, entry_ns),
        "entry_side": entry.get("side", ""),
        "exit_side": exit_order.get("side", ""),
        "entry_raw_price": entry.get("raw_price", ""),
        "exit_raw_price": exit_order.get("raw_price", ""),
        "entry_order_price": entry.get("order_price", ""),
        "exit_order_price": exit_order.get("order_price", ""),
        "entry_price": format_decimal(entry_price),
        "exit_price": format_decimal(exit_price),
        "entry_volume": format_decimal(matched_volume),
        "exit_volume": format_decimal(order_filled_quantity(exit_order)),
        "matched_volume": format_decimal(matched_volume),
        "remaining_entry_volume": format_decimal(remaining_entry_volume),
        "contract_multiplier": format_decimal(multiplier),
        "entry_notional": format_decimal(
            notional_for(entry_price, matched_volume, multiplier)
        ),
        "exit_notional": format_decimal(
            notional_for(exit_price, matched_volume, multiplier)
        ),
        "gross_pnl": format_decimal(gross_pnl),
        "entry_fee_quote_estimated": format_decimal(entry_fee),
        "exit_fee_quote_estimated": format_decimal(exit_fee),
        "total_fee_quote_estimated": format_decimal(total_fee),
        "net_pnl": format_decimal(net_pnl),
        "entry_ack_rtt_ns": entry.get("ack_rtt_ns", ""),
        "exit_ack_rtt_ns": exit_order.get("ack_rtt_ns", ""),
        "entry_fee_source": entry.get("fee_source", ""),
        "exit_fee_source": exit_order.get("fee_source", ""),
        "warnings": "",
    }
    for warning in warnings:
        append_row_warning(row, warning)
    return row


def build_open_position_row(
    entry: dict[str, str],
    remaining_entry_volume: Decimal,
    entry_fee: Decimal | None,
    warnings: list[str],
) -> dict[str, str]:
    entry_price = order_fill_price(entry)
    multiplier = parse_decimal(entry.get("contract_multiplier"))
    entry_ns = order_finished_ns(entry)
    row = {
        "run_id": entry.get("run_id", ""),
        "position_key": (
            f"{entry.get('run_id', '')}:{entry.get('symbol_id', '')}:"
            f"{entry.get('position_id', '')}:open"
        ),
        "symbol": entry.get("symbol", ""),
        "symbol_id": entry.get("symbol_id", ""),
        "position_id": entry.get("position_id", ""),
        "position_direction": entry.get("position_direction", ""),
        "status": "open",
        "entry_local_order_id": entry.get("local_order_id", ""),
        "exit_local_order_id": "",
        "entry_exchange_order_id": entry.get("exchange_order_id", ""),
        "exit_exchange_order_id": "",
        "entry_ns": entry_ns,
        "exit_ns": "",
        "holding_ns": "",
        "entry_side": entry.get("side", ""),
        "exit_side": "",
        "entry_raw_price": entry.get("raw_price", ""),
        "exit_raw_price": "",
        "entry_order_price": entry.get("order_price", ""),
        "exit_order_price": "",
        "entry_price": format_decimal(entry_price),
        "exit_price": "",
        "entry_volume": format_decimal(remaining_entry_volume),
        "exit_volume": "",
        "matched_volume": "",
        "remaining_entry_volume": format_decimal(remaining_entry_volume),
        "contract_multiplier": format_decimal(multiplier),
        "entry_notional": format_decimal(
            notional_for(entry_price, remaining_entry_volume, multiplier)
        ),
        "exit_notional": "",
        "gross_pnl": "",
        "entry_fee_quote_estimated": format_decimal(entry_fee),
        "exit_fee_quote_estimated": "",
        "total_fee_quote_estimated": format_decimal(entry_fee),
        "net_pnl": "",
        "entry_ack_rtt_ns": entry.get("ack_rtt_ns", ""),
        "exit_ack_rtt_ns": "",
        "entry_fee_source": entry.get("fee_source", ""),
        "exit_fee_source": "",
        "warnings": "",
    }
    for warning in warnings:
        append_row_warning(row, warning)
    return row


def build_missing_entry_position_row(exit_order: dict[str, str]) -> dict[str, str]:
    exit_price = order_fill_price(exit_order)
    exit_volume = order_filled_quantity(exit_order)
    multiplier = parse_decimal(exit_order.get("contract_multiplier"))
    exit_fee = parse_decimal(exit_order.get("fee_quote_estimated"))
    exit_ns = order_finished_ns(exit_order)
    row = {
        "run_id": exit_order.get("run_id", ""),
        "position_key": (
            f"{exit_order.get('run_id', '')}:{exit_order.get('symbol_id', '')}:"
            f"{exit_order.get('position_id', '')}:{exit_order.get('local_order_id', '')}"
        ),
        "symbol": exit_order.get("symbol", ""),
        "symbol_id": exit_order.get("symbol_id", ""),
        "position_id": exit_order.get("position_id", ""),
        "position_direction": exit_order.get("position_direction", ""),
        "status": "missing_entry",
        "entry_local_order_id": exit_order.get("entry_local_order_id", ""),
        "exit_local_order_id": exit_order.get("local_order_id", ""),
        "entry_exchange_order_id": "",
        "exit_exchange_order_id": exit_order.get("exchange_order_id", ""),
        "entry_ns": "",
        "exit_ns": exit_ns,
        "holding_ns": "",
        "entry_side": "",
        "exit_side": exit_order.get("side", ""),
        "entry_raw_price": "",
        "exit_raw_price": exit_order.get("raw_price", ""),
        "entry_order_price": "",
        "exit_order_price": exit_order.get("order_price", ""),
        "entry_price": "",
        "exit_price": format_decimal(exit_price),
        "entry_volume": "",
        "exit_volume": format_decimal(exit_volume),
        "matched_volume": "",
        "remaining_entry_volume": "",
        "contract_multiplier": format_decimal(multiplier),
        "entry_notional": "",
        "exit_notional": format_decimal(
            notional_for(exit_price, exit_volume, multiplier)
        ),
        "gross_pnl": "",
        "entry_fee_quote_estimated": "",
        "exit_fee_quote_estimated": format_decimal(exit_fee),
        "total_fee_quote_estimated": format_decimal(exit_fee),
        "net_pnl": "",
        "entry_ack_rtt_ns": "",
        "exit_ack_rtt_ns": exit_order.get("ack_rtt_ns", ""),
        "entry_fee_source": "",
        "exit_fee_source": exit_order.get("fee_source", ""),
        "warnings": "missing_entry_order",
    }
    return row


def build_position_detail_rows(order_rows: list[dict[str, str]]) -> list[dict[str, str]]:
    groups: dict[tuple[str, str, str], dict[str, list[dict[str, str]]]] = {}
    for order in order_rows:
        position_id = order.get("position_id", "")
        if position_id in ("", "0"):
            continue
        role = order.get("order_role", "")
        if role not in {"entry", "exit"}:
            continue
        if order_filled_quantity(order) <= 0:
            continue
        bucket = groups.setdefault(position_group_key(order), {"entry": [], "exit": []})
        bucket[role].append(order)

    rows: list[dict[str, str]] = []
    for key in sorted(groups, key=position_sort_key):
        entries = sorted(groups[key]["entry"], key=order_sort_key)
        exits = sorted(groups[key]["exit"], key=order_sort_key)
        if not entries:
            for exit_order in exits:
                rows.append(build_missing_entry_position_row(exit_order))
            continue
        entry = entries[0]
        entry_volume = order_filled_quantity(entry)
        remaining_entry_volume = entry_volume
        entry_fee_total = parse_decimal(entry.get("fee_quote_estimated"))
        common_warnings: list[str] = []
        if len(entries) > 1:
            common_warnings.append("multiple_entry_orders")

        for exit_order in exits:
            exit_volume = order_filled_quantity(exit_order)
            matched_volume = min(exit_volume, remaining_entry_volume)
            row_warnings = list(common_warnings)
            status = "closed" if remaining_entry_volume == 0 else "partial_closed"
            if matched_volume <= 0:
                row_warnings.append("exit_volume_exceeds_entry")
                status = "over_closed"
            elif exit_volume > remaining_entry_volume:
                row_warnings.append("exit_volume_exceeds_entry")
            entry_fee = decimal_ratio(entry_fee_total, matched_volume, entry_volume)
            exit_fee = parse_decimal(exit_order.get("fee_quote_estimated"))
            remaining_entry_volume -= min(exit_volume, remaining_entry_volume)
            if matched_volume > 0:
                status = "closed" if remaining_entry_volume == 0 else "partial_closed"
            rows.append(
                build_closed_position_row(
                    entry,
                    exit_order,
                    matched_volume,
                    remaining_entry_volume,
                    entry_fee,
                    exit_fee,
                    status,
                    row_warnings,
                )
            )

        if remaining_entry_volume > 0:
            entry_fee = decimal_ratio(
                entry_fee_total, remaining_entry_volume, entry_volume
            )
            rows.append(
                build_open_position_row(
                    entry,
                    remaining_entry_volume,
                    entry_fee,
                    common_warnings,
                )
            )
    return [
        {field: row.get(field, "") for field in POSITION_DETAIL_FIELDS}
        for row in rows
    ]


def write_position_detail_csv(rows: list[dict[str, str]], output_path: Path) -> None:
    with output_path.open("w", newline="", encoding="utf-8") as output_file:
        writer = csv.DictWriter(
            output_file, fieldnames=POSITION_DETAIL_FIELDS, lineterminator="\n"
        )
        writer.writeheader()
        writer.writerows(rows)


def ns_delta_text(lhs: str, rhs: str) -> str:
    return int_delta_text(lhs, rhs)


def nonzero_ns_delta_text(lhs: str, rhs: str) -> str:
    if lhs in ("", "0") or rhs in ("", "0"):
        return ""
    return ns_delta_text(lhs, rhs)


def exchange_lifecycle_ns(order: dict[str, str]) -> str:
    return nonzero_ns_delta_text(
        order.get("finish_exchange_ns", ""), order.get("ack_exchange_ns", "")
    )


def build_latency_detail_rows(order_rows: list[dict[str, str]]) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    for order in order_rows:
        local_order_id = order.get("local_order_id", "")
        if local_order_id == "":
            continue
        request_send_ns = order.get("request_send_local_ns", "")
        ack_receive_ns = order.get("ack_local_receive_ns", "")
        response_receive_ns = order.get("response_local_receive_ns", "")
        finished_ns = order_finished_ns(order)
        row = {
            "run_id": order.get("run_id", ""),
            "latency_key": f"{order.get('run_id', '')}:{local_order_id}",
            "local_order_id": local_order_id,
            "exchange_order_id": order.get("exchange_order_id", ""),
            "symbol": order.get("symbol", ""),
            "symbol_id": order.get("symbol_id", ""),
            "position_id": order.get("position_id", ""),
            "position_direction": order.get("position_direction", ""),
            "order_role": order.get("order_role", ""),
            "action": order.get("action", ""),
            "side": order.get("side", ""),
            "reduce_only": order.get("reduce_only", ""),
            "status": order.get("status", ""),
            "finish_reason": order.get("finish_reason", ""),
            "reject_reason": order.get("reject_reason", ""),
            "request_sequence": order.get("request_sequence", ""),
            "encoded_request_id": order.get("encoded_request_id", ""),
            "trigger_exchange_ns": order.get("trigger_exchange_ns", ""),
            "trigger_local_ns": order.get("trigger_local_ns", ""),
            "on_book_ticker_entry_ns": order.get("on_book_ticker_entry_ns", ""),
            "signal_decision_ns": order.get("signal_decision_ns", ""),
            "request_send_local_ns": request_send_ns,
            "ack_local_receive_ns": ack_receive_ns,
            "response_local_receive_ns": response_receive_ns,
            "order_finished_local_ns": finished_ns,
            "ack_exchange_ns": order.get("ack_exchange_ns", ""),
            "response_exchange_ns": order.get("response_exchange_ns", ""),
            "accepted_exchange_ns": order.get("accepted_exchange_ns", ""),
            "finish_exchange_ns": order.get("finish_exchange_ns", ""),
            "ack_rtt_ns": order.get("ack_rtt_ns", ""),
            "response_rtt_ns": order.get("response_rtt_ns", ""),
            "send_to_ack_local_ns": ns_delta_text(ack_receive_ns, request_send_ns),
            "send_to_response_local_ns": ns_delta_text(
                response_receive_ns, request_send_ns
            ),
            "send_to_finish_local_ns": ns_delta_text(finished_ns, request_send_ns),
            "ack_to_finish_local_ns": ns_delta_text(finished_ns, ack_receive_ns),
            "bbo_to_strategy_ns": ns_delta_text(
                order.get("on_book_ticker_entry_ns", ""),
                order.get("trigger_local_ns", ""),
            ),
            "strategy_to_signal_ns": ns_delta_text(
                order.get("signal_decision_ns", ""),
                order.get("on_book_ticker_entry_ns", ""),
            ),
            "signal_to_request_send_ns": ns_delta_text(
                request_send_ns, order.get("signal_decision_ns", "")
            ),
            "trigger_to_request_send_ns": ns_delta_text(
                request_send_ns, order.get("trigger_local_ns", "")
            ),
            "ack_exchange_to_local_ns": order.get("ack_exchange_to_local_ns", ""),
            "response_exchange_to_local_ns": order.get(
                "response_exchange_to_local_ns", ""
            ),
            "exchange_lifecycle_ns": exchange_lifecycle_ns(order),
            "order_session_id": order.get("order_session_id", ""),
            "owner_thread_cpu": order.get("owner_thread_cpu", ""),
            "owner_thread_tid": order.get("owner_thread_tid", ""),
            "local_ip": order.get("local_ip", ""),
            "local_port": order.get("local_port", ""),
            "remote_ip": order.get("remote_ip", ""),
            "remote_port": order.get("remote_port", ""),
            "send_cpu": order.get("send_cpu", ""),
            "ack_cpu": order.get("ack_cpu", ""),
            "diagnostic_cpu": order.get("diagnostic_cpu", ""),
            "tcp_info_available": order.get("tcp_info_available", ""),
            "tcp_info_rtt_us": order.get("tcp_info_rtt_us", ""),
            "tcp_info_rttvar_us": order.get("tcp_info_rttvar_us", ""),
            "tcp_info_retrans": order.get("tcp_info_retrans", ""),
            "tcp_info_total_retrans": order.get("tcp_info_total_retrans", ""),
            "tcp_info_unacked": order.get("tcp_info_unacked", ""),
            "tcp_info_snd_cwnd": order.get("tcp_info_snd_cwnd", ""),
            "latency_diagnostic_reason": order.get("latency_diagnostic_reason", ""),
            "latency_diagnostic_ack_rtt_ns": order.get(
                "latency_diagnostic_ack_rtt_ns", ""
            ),
            "send_to_first_after_hook_ns": order.get(
                "send_to_first_after_hook_ns", ""
            ),
            "send_to_first_drive_read_ns": order.get(
                "send_to_first_drive_read_ns", ""
            ),
            "drive_read_duration_ns": order.get("drive_read_duration_ns", ""),
            "max_observed_drive_read_duration_ns": order.get(
                "max_observed_drive_read_duration_ns", ""
            ),
            "latency_diagnostic_inflight_at_send": order.get(
                "latency_diagnostic_inflight_at_send", ""
            ),
            "max_runtime_loop_gap_ns": order.get("max_runtime_loop_gap_ns", ""),
            "runtime_loop_iterations_before_ack": order.get(
                "runtime_loop_iterations_before_ack", ""
            ),
            "order_encode_done_ns": order.get("order_encode_done_ns", ""),
            "ws_frame_encode_done_ns": order.get("ws_frame_encode_done_ns", ""),
            "write_enqueue_ns": order.get("write_enqueue_ns", ""),
            "drive_write_enter_ns": order.get("drive_write_enter_ns", ""),
            "write_some_enter_ns": order.get("write_some_enter_ns", ""),
            "write_some_return_ns": order.get("write_some_return_ns", ""),
            "write_complete_ns": order.get("write_complete_ns", ""),
            "write_some_bytes": order.get("write_some_bytes", ""),
            "write_complete_bytes": order.get("write_complete_bytes", ""),
            "write_errno": order.get("write_errno", ""),
            "write_eagain": order.get("write_eagain", ""),
            "pending_write_count_after": order.get("pending_write_count_after", ""),
            "socket_send_queue_available": order.get(
                "socket_send_queue_available", ""
            ),
            "tcp_sendq_bytes": order.get("tcp_sendq_bytes", ""),
            "tcp_notsent_bytes": order.get("tcp_notsent_bytes", ""),
            "warnings": "",
        }
        if row["ack_rtt_ns"] == "":
            row["ack_rtt_ns"] = row["send_to_ack_local_ns"]
        if request_send_ns == "":
            append_row_warning(row, "missing_request_send_local_ns")
        if ack_receive_ns == "":
            append_row_warning(row, "missing_ack_local_receive_ns")
        rows.append(row)
    return [
        {field: row.get(field, "") for field in LATENCY_DETAIL_FIELDS}
        for row in sorted(rows, key=order_sort_key)
    ]


def write_latency_detail_csv(rows: list[dict[str, str]], output_path: Path) -> None:
    with output_path.open("w", newline="", encoding="utf-8") as output_file:
        writer = csv.DictWriter(
            output_file, fieldnames=LATENCY_DETAIL_FIELDS, lineterminator="\n"
        )
        writer.writeheader()
        writer.writerows(rows)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build LeadLag order, position, and latency CSVs"
    )
    parser.add_argument("--log", required=True, type=Path)
    parser.add_argument("--config", type=Path)
    parser.add_argument(
        "--instrument-catalog",
        type=Path,
        default=Path("config/instruments/usdt_futures.csv"),
    )
    parser.add_argument("--run-id")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--positions-output", type=Path)
    parser.add_argument("--latency-output", type=Path)
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
    if args.positions_output is not None:
        position_rows = build_position_detail_rows(result.rows)
        write_position_detail_csv(position_rows, args.positions_output)
        print(f"wrote {len(position_rows)} position rows to {args.positions_output}")
    if args.latency_output is not None:
        latency_rows = build_latency_detail_rows(result.rows)
        write_latency_detail_csv(latency_rows, args.latency_output)
        print(f"wrote {len(latency_rows)} latency rows to {args.latency_output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
