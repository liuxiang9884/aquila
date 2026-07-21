#!/home/liuxiang/dev/pyenv/lx/bin/python

import csv
import json
import sys
from dataclasses import dataclass
from decimal import Decimal, InvalidOperation
from pathlib import Path

import numpy as np


SCRIPT_DIR = Path(__file__).resolve().parent
MARKET_DATA_SCRIPT_DIR = SCRIPT_DIR.parent / "market_data"
for script_dir in (SCRIPT_DIR, MARKET_DATA_SCRIPT_DIR):
    if str(script_dir) not in sys.path:
        sys.path.insert(0, str(script_dir))

import analyze_order_detail as orders  # noqa: E402
import typed_binary  # noqa: E402


EXECUTION_DETAIL_FIELDS = [
    "run_id",
    "local_order_id",
    "exchange_order_id",
    "exec_id",
    "symbol",
    "symbol_id",
    "order_role",
    "position_id",
    "side",
    "hold_side",
    "trade_scope",
    "exec_price",
    "exec_quantity",
    "exec_value",
    "exec_time_exchange_ns",
    "fast_fill_message_exchange_ns",
    "fast_fill_updated_exchange_ns",
    "fast_fill_local_receive_ns",
    "fast_fill_local_receive_monotonic_ns",
    "place_creation_exchange_ns",
    "ack_local_receive_ns",
    "order_feedback_local_receive_ns",
    "creation_to_exec_ns",
    "fast_fill_after_ack_ns",
    "fast_fill_after_order_feedback_ns",
    "authoritative_filled_quantity",
    "fast_fill_order_quantity",
    "rest_present",
    "fee_coin",
    "actual_fee_quote",
    "exec_pnl",
    "source",
    "warnings",
]


ORDER_FILLABILITY_FIELDS = [
    "run_id",
    "local_order_id",
    "exchange_order_id",
    "symbol",
    "symbol_id",
    "order_role",
    "status",
    "side",
    "order_price",
    "price_tick",
    "slippage_ticks",
    "signal_lag_id",
    "signal_lag_local_ns",
    "signal_lag_age_ns",
    "request_send_local_ns",
    "place_creation_exchange_ns",
    "terminal_event",
    "terminal_exchange_ns",
    "window_duration_ns",
    "signal_marketability",
    "creation_marketability",
    "window_marketability",
    "terminal_marketability",
    "marketability_observation",
    "bbo_records_at_creation",
    "bbo_records_in_window",
    "bbo_records_at_terminal",
    "first_no_cross_after_send_ns",
    "first_no_cross_opposite_price",
    "missing_reason",
]


@dataclass(frozen=True)
class ExecutionAnalysisResult:
    rows: list[dict[str, str]]
    stats: dict[str, int | bool]


def decimal_value(value: str | None) -> Decimal | None:
    if value in (None, ""):
        return None
    try:
        return Decimal(value)
    except InvalidOperation:
        return None


def decimal_text(value: Decimal | None) -> str:
    if value is None:
        return ""
    if value == 0:
        return "0"
    return format(value.normalize(), "f")


def milliseconds_to_nanoseconds(value: str | None) -> str:
    if value in (None, "", "0"):
        return ""
    try:
        return str(int(value) * 1_000_000)
    except ValueError:
        return ""


def nanosecond_delta(lhs: str | None, rhs: str | None) -> str:
    if lhs in (None, "", "0") or rhs in (None, "", "0"):
        return ""
    try:
        return str(int(lhs) - int(rhs))
    except ValueError:
        return ""


def message_from_line(line: str) -> tuple[str, dict[str, str]]:
    marker = "] "
    marker_index = line.find(marker)
    message = line[marker_index + len(marker) :].strip() if marker_index >= 0 else line.strip()
    return orders.parse_message(message)


def fast_fill_row(
    fields: dict[str, str], order: dict[str, str] | None, run_id: str
) -> dict[str, str]:
    local_order_id = fields.get("client_oid", "").removeprefix("a-")
    order = order or {}
    exec_time_ns = milliseconds_to_nanoseconds(fields.get("exec_time_ms"))
    local_receive_ns = fields.get("local_receive_realtime_ns", "")
    warnings = "" if order else "missing_authoritative_order"
    return {
        "run_id": run_id,
        "local_order_id": local_order_id,
        "exchange_order_id": fields.get("order_id", ""),
        "exec_id": fields.get("exec_id", ""),
        "symbol": fields.get("symbol", order.get("symbol", "")),
        "symbol_id": order.get("symbol_id", ""),
        "order_role": order.get("order_role", ""),
        "position_id": order.get("position_id", ""),
        "side": fields.get("side", order.get("side", "")),
        "hold_side": fields.get("hold_side", ""),
        "trade_scope": fields.get("trade_scope", ""),
        "exec_price": fields.get("exec_price", ""),
        "exec_quantity": fields.get("exec_quantity", ""),
        "exec_value": "",
        "exec_time_exchange_ns": exec_time_ns,
        "fast_fill_message_exchange_ns": milliseconds_to_nanoseconds(
            fields.get("exchange_message_time_ms")
        ),
        "fast_fill_updated_exchange_ns": milliseconds_to_nanoseconds(
            fields.get("updated_time_ms")
        ),
        "fast_fill_local_receive_ns": local_receive_ns,
        "fast_fill_local_receive_monotonic_ns": fields.get(
            "local_receive_monotonic_ns", ""
        ),
        "place_creation_exchange_ns": order.get("place_creation_exchange_ns", ""),
        "ack_local_receive_ns": order.get("ack_local_receive_ns", ""),
        "order_feedback_local_receive_ns": order.get("feedback_local_receive_ns", ""),
        "creation_to_exec_ns": nanosecond_delta(
            exec_time_ns, order.get("place_creation_exchange_ns", "")
        ),
        "fast_fill_after_ack_ns": nanosecond_delta(
            local_receive_ns, order.get("ack_local_receive_ns", "")
        ),
        "fast_fill_after_order_feedback_ns": nanosecond_delta(
            local_receive_ns, order.get("feedback_local_receive_ns", "")
        ),
        "authoritative_filled_quantity": order.get(
            "cumulative_filled_quantity", ""
        ),
        "fast_fill_order_quantity": "",
        "rest_present": "false",
        "fee_coin": "",
        "actual_fee_quote": "",
        "exec_pnl": "",
        "source": "fast_fill",
        "warnings": warnings,
    }


def fee_detail(fees: object) -> tuple[str, str]:
    if not isinstance(fees, list):
        return "", ""
    coins: list[str] = []
    total = Decimal(0)
    found = False
    for fee in fees:
        if not isinstance(fee, dict):
            continue
        coin = str(fee.get("feeCoin", ""))
        if coin and coin not in coins:
            coins.append(coin)
        value = decimal_value(str(fee.get("fee", "")))
        if value is not None:
            total += value
            found = True
    return ";".join(coins), decimal_text(total) if found else ""


def merge_rest_fill(
    row: dict[str, str], fill: dict, order: dict[str, str] | None
) -> None:
    order = order or {}
    fee_coin, actual_fee = fee_detail(fill.get("feeDetail"))
    row["rest_present"] = "true"
    row["source"] = "fast_fill+rest" if row.get("source") == "fast_fill" else "rest"
    for target, source in (
        ("exchange_order_id", "orderId"),
        ("exec_id", "execId"),
        ("symbol", "symbol"),
        ("side", "side"),
        ("trade_scope", "tradeScope"),
        ("exec_price", "execPrice"),
        ("exec_quantity", "execQty"),
        ("exec_value", "execValue"),
    ):
        value = str(fill.get(source, ""))
        if row.get(target, "") == "" and value != "":
            row[target] = value
    if row.get("exec_time_exchange_ns", "") == "":
        row["exec_time_exchange_ns"] = milliseconds_to_nanoseconds(
            str(fill.get("createdTime", ""))
        )
    row["fee_coin"] = fee_coin
    row["actual_fee_quote"] = actual_fee
    row["exec_pnl"] = str(fill.get("execPnl", ""))
    if row.get("symbol_id", "") == "":
        row["symbol_id"] = order.get("symbol_id", "")
    if row.get("order_role", "") == "":
        row["order_role"] = order.get("order_role", "")
    if row.get("position_id", "") == "":
        row["position_id"] = order.get("position_id", "")


def rest_only_row(fill: dict, order: dict[str, str] | None, run_id: str) -> dict[str, str]:
    client_oid = str(fill.get("clientOid", ""))
    order = order or {}
    local_order_id = order.get("local_order_id", "") or client_oid.removeprefix(
        "a-"
    )
    exec_time_ns = milliseconds_to_nanoseconds(str(fill.get("createdTime", "")))
    row = {field: "" for field in EXECUTION_DETAIL_FIELDS}
    row.update(
        {
            "run_id": run_id,
            "local_order_id": local_order_id,
            "exchange_order_id": str(fill.get("orderId", "")),
            "exec_id": str(fill.get("execId", "")),
            "symbol": str(fill.get("symbol", order.get("symbol", ""))),
            "symbol_id": order.get("symbol_id", ""),
            "order_role": order.get("order_role", ""),
            "position_id": order.get("position_id", ""),
            "side": str(fill.get("side", order.get("side", ""))),
            "trade_scope": str(fill.get("tradeScope", "")),
            "exec_price": str(fill.get("execPrice", "")),
            "exec_quantity": str(fill.get("execQty", "")),
            "exec_value": str(fill.get("execValue", "")),
            "exec_time_exchange_ns": exec_time_ns,
            "place_creation_exchange_ns": order.get(
                "place_creation_exchange_ns", ""
            ),
            "ack_local_receive_ns": order.get("ack_local_receive_ns", ""),
            "order_feedback_local_receive_ns": order.get(
                "feedback_local_receive_ns", ""
            ),
            "creation_to_exec_ns": nanosecond_delta(
                exec_time_ns, order.get("place_creation_exchange_ns", "")
            ),
            "authoritative_filled_quantity": order.get(
                "cumulative_filled_quantity", ""
            ),
            "rest_present": "true",
            "source": "rest",
            "warnings": "" if order else "missing_authoritative_order",
        }
    )
    merge_rest_fill(row, fill, order)
    return row


def analyze_executions(
    feedback_log_path: Path,
    order_rows: list[dict[str, str]],
    *,
    rest_fills_path: Path | None = None,
    run_id: str,
) -> ExecutionAnalysisResult:
    orders_by_id = {row.get("local_order_id", ""): row for row in order_rows}
    orders_by_exchange_id = {
        row.get("exchange_order_id", ""): row
        for row in order_rows
        if row.get("exchange_order_id", "")
    }
    rows_by_exec_id: dict[str, dict[str, str]] = {}
    fast_fill_records = 0
    duplicate_exec_ids = 0
    validation_errors = 0
    subscribed = False
    with feedback_log_path.open(encoding="utf-8") as input_file:
        for line in input_file:
            tag, fields = message_from_line(line)
            if tag == "bitget_fast_fill_subscribe" and fields.get("accepted") == "true":
                subscribed = True
            elif tag == "bitget_fast_fill_validation_error":
                validation_errors += 1
            elif tag == "bitget_fast_fill_raw_update":
                fast_fill_records += 1
                exec_id = fields.get("exec_id", "")
                if exec_id == "":
                    validation_errors += 1
                    continue
                if exec_id in rows_by_exec_id:
                    duplicate_exec_ids += 1
                    continue
                local_order_id = fields.get("client_oid", "").removeprefix("a-")
                rows_by_exec_id[exec_id] = fast_fill_row(
                    fields, orders_by_id.get(local_order_id), run_id
                )

    rest_execution_records = 0
    rest_matched_execution_records = 0
    rest_unmatched_execution_records = 0
    if rest_fills_path is not None:
        payload = json.loads(rest_fills_path.read_text(encoding="utf-8"))
        fills = payload.get("fills", []) if isinstance(payload, dict) else []
        if not isinstance(fills, list):
            raise ValueError("Bitget REST fills JSON field 'fills' must be a list")
        for fill in fills:
            if not isinstance(fill, dict):
                continue
            rest_execution_records += 1
            exec_id = str(fill.get("execId", ""))
            if exec_id == "":
                continue
            local_order_id = str(fill.get("clientOid", "")).removeprefix("a-")
            fill_exchange_order_id = str(fill.get("orderId", ""))
            order = orders_by_id.get(local_order_id)
            if (
                order is not None
                and order.get("exchange_order_id", "")
                and fill_exchange_order_id
                and order["exchange_order_id"] != fill_exchange_order_id
            ):
                order = None
            order = order or orders_by_exchange_id.get(fill_exchange_order_id)
            if order is None:
                rest_unmatched_execution_records += 1
                continue
            rest_matched_execution_records += 1
            local_order_id = order.get("local_order_id", local_order_id)
            row = rows_by_exec_id.get(exec_id)
            if row is None:
                row = rest_only_row(fill, order, run_id)
                rows_by_exec_id[exec_id] = row
            else:
                merge_rest_fill(row, fill, order)

    fast_quantities: dict[str, Decimal] = {}
    fast_order_ids: set[str] = set()
    for row in rows_by_exec_id.values():
        if "fast_fill" not in row.get("source", ""):
            continue
        local_order_id = row.get("local_order_id", "")
        quantity = decimal_value(row.get("exec_quantity")) or Decimal(0)
        fast_quantities[local_order_id] = fast_quantities.get(
            local_order_id, Decimal(0)
        ) + quantity
        fast_order_ids.add(local_order_id)
    for row in rows_by_exec_id.values():
        local_order_id = row.get("local_order_id", "")
        if local_order_id in fast_quantities:
            row["fast_fill_order_quantity"] = decimal_text(
                fast_quantities[local_order_id]
            )

    filled_orders = {
        local_order_id
        for local_order_id, order in orders_by_id.items()
        if (decimal_value(order.get("cumulative_filled_quantity")) or Decimal(0)) > 0
    }
    quantity_mismatch_orders = 0
    for local_order_id in filled_orders & fast_order_ids:
        authoritative = decimal_value(
            orders_by_id[local_order_id].get("cumulative_filled_quantity")
        )
        if authoritative != fast_quantities.get(local_order_id):
            quantity_mismatch_orders += 1

    rows = [
        {field: row.get(field, "") for field in EXECUTION_DETAIL_FIELDS}
        for row in sorted(
            rows_by_exec_id.values(),
            key=lambda row: (
                int(row.get("exec_time_exchange_ns", "") or 0),
                row.get("exec_id", ""),
            ),
        )
    ]
    stats: dict[str, int | bool] = {
        "fast_fill_subscribed": subscribed,
        "fast_fill_records": fast_fill_records,
        "fast_fill_unique_exec_ids": len(
            [row for row in rows if "fast_fill" in row.get("source", "")]
        ),
        "fast_fill_unique_orders": len(fast_order_ids),
        "fast_fill_duplicate_exec_ids": duplicate_exec_ids,
        "fast_fill_validation_errors": validation_errors,
        "authoritative_filled_orders": len(filled_orders),
        "filled_orders_missing_fast_fill": len(filled_orders - fast_order_ids),
        "fast_fill_orders_without_authoritative_fill": len(
            fast_order_ids - filled_orders
        ),
        "quantity_mismatch_orders": quantity_mismatch_orders,
        "rest_execution_records": rest_execution_records,
        "rest_matched_execution_records": rest_matched_execution_records,
        "rest_unmatched_execution_records": rest_unmatched_execution_records,
    }
    return ExecutionAnalysisResult(rows=rows, stats=stats)


def write_execution_detail_csv(rows: list[dict[str, str]], output_path: Path) -> None:
    with output_path.open("w", newline="", encoding="utf-8") as output_file:
        writer = csv.DictWriter(
            output_file, fieldnames=EXECUTION_DETAIL_FIELDS, lineterminator="\n"
        )
        writer.writeheader()
        writer.writerows(rows)


@dataclass
class BookTickerSegment:
    path: Path
    records: int
    first_local_ns: int
    last_local_ns: int
    data: np.memmap | None = None


class BookTickerStore:
    def __init__(self, manifest_path: Path):
        self.dtype = typed_binary.book_ticker_dtype()
        self.segments: list[BookTickerSegment] = []
        with manifest_path.open(encoding="utf-8") as input_file:
            for line in input_file:
                if not line.strip():
                    continue
                entry = json.loads(line)
                path = Path(entry["file"])
                if not path.is_absolute():
                    path = manifest_path.parent / path
                records = int(entry.get("records", 0))
                if records <= 0:
                    continue
                self.segments.append(
                    BookTickerSegment(
                        path=path,
                        records=records,
                        first_local_ns=int(entry.get("first_local_ns", 0)),
                        last_local_ns=int(entry.get("last_local_ns", 0)),
                    )
                )

    def segment_data(self, segment: BookTickerSegment) -> np.memmap:
        if segment.data is None:
            typed_binary.validate_header(
                typed_binary.read_header(segment.path),
                "book_ticker",
                segment.path,
            )
            available_records = max(
                0,
                (segment.path.stat().st_size - typed_binary.HEADER_SIZE)
                // self.dtype.itemsize,
            )
            if available_records < segment.records:
                raise ValueError(
                    f"book ticker segment is truncated: {segment.path} "
                    f"manifest={segment.records} available={available_records}"
                )
            segment.data = np.memmap(
                segment.path,
                dtype=self.dtype,
                mode="r",
                offset=typed_binary.HEADER_SIZE,
                shape=(segment.records,),
            )
        return segment.data

    def records_near(self, local_ns: int, span_ns: int) -> np.ndarray:
        arrays: list[np.ndarray] = []
        lower = local_ns - span_ns
        upper = local_ns + span_ns
        for segment in self.segments:
            if upper < segment.first_local_ns or lower > segment.last_local_ns:
                continue
            data = self.segment_data(segment)
            local_values = data["local_ns"]
            begin = int(np.searchsorted(local_values, lower, side="left"))
            end = int(np.searchsorted(local_values, upper, side="right"))
            if end > begin:
                arrays.append(np.array(data[begin:end]))
        if arrays:
            return np.concatenate(arrays)
        return np.empty(0, dtype=self.dtype)


def is_crossing(record: np.void, side: str, order_price: float) -> bool:
    normalized_side = side.lower().removeprefix("k")
    if normalized_side == "buy":
        return float(record["ask_price"]) <= order_price
    if normalized_side == "sell":
        return float(record["bid_price"]) >= order_price
    return False


def classify_marketability(
    records: np.ndarray, side: str, order_price: float
) -> str:
    if len(records) == 0:
        return "missing"
    states = [is_crossing(record, side, order_price) for record in records]
    if all(states):
        return "all_cross"
    if any(states):
        return "mixed"
    return "no_cross"


def latest_record_as_of(records: np.ndarray, exchange_ns: int) -> np.ndarray:
    eligible_indexes = np.flatnonzero(records["exchange_ns"] <= exchange_ns)
    if len(eligible_indexes) == 0:
        return np.empty(0, dtype=records.dtype)
    latest_index = max(
        eligible_indexes,
        key=lambda index: (
            int(records[index]["exchange_ns"]),
            int(records[index]["local_ns"]),
            int(records[index]["id"]),
        ),
    )
    return records[latest_index : latest_index + 1]


def marketability_observation(classification: str) -> str:
    if classification == "all_cross":
        return "marketable_observed"
    if classification == "no_cross":
        return "not_marketable_observed"
    return "indeterminate"


def format_float(value: float) -> str:
    return format(value, ".15g")


def analyze_fillability(
    order_rows: list[dict[str, str]],
    execution_rows: list[dict[str, str]],
    manifest_path: Path,
) -> list[dict[str, str]]:
    store = BookTickerStore(manifest_path)
    executions_by_order: dict[str, list[dict[str, str]]] = {}
    for execution in execution_rows:
        executions_by_order.setdefault(execution.get("local_order_id", ""), []).append(
            execution
        )
    result: list[dict[str, str]] = []
    for order in order_rows:
        if order.get("source_schema") != "submitted_v1":
            continue
        local_order_id = order.get("local_order_id", "")
        if local_order_id == "":
            continue
        order_price_value = decimal_value(order.get("order_price"))
        symbol_id_text = order.get("symbol_id", "")
        place_creation_ns = order.get("place_creation_exchange_ns", "")
        if order_price_value is None or symbol_id_text == "" or place_creation_ns == "":
            row = {field: "" for field in ORDER_FILLABILITY_FIELDS}
            row.update(
                {
                    "run_id": order.get("run_id", ""),
                    "local_order_id": local_order_id,
                    "exchange_order_id": order.get("exchange_order_id", ""),
                    "symbol": order.get("symbol", ""),
                    "symbol_id": symbol_id_text,
                    "order_role": order.get("order_role", ""),
                    "status": order.get("status", ""),
                    "side": order.get("side", ""),
                    "order_price": order.get("order_price", ""),
                    "missing_reason": "missing_order_price_symbol_or_creation_time",
                }
            )
            result.append(row)
            continue

        executions = executions_by_order.get(local_order_id, [])
        execution_times = [
            int(execution["exec_time_exchange_ns"])
            for execution in executions
            if execution.get("exec_time_exchange_ns", "")
        ]
        if order.get("status") == "kFilled":
            if execution_times:
                terminal_event = "exec"
                terminal_ns = min(execution_times)
            else:
                terminal_event = "filled_feedback"
                terminal_text = (
                    order.get("feedback_updated_exchange_ns", "")
                    or order.get("finish_exchange_ns", "")
                )
                terminal_ns = int(terminal_text) if terminal_text else 0
        else:
            terminal_event = "cancel"
            terminal_text = (
                order.get("feedback_updated_exchange_ns", "")
                or order.get("finish_exchange_ns", "")
            )
            terminal_ns = int(terminal_text) if terminal_text else 0

        anchor_text = (
            order.get("feedback_local_receive_ns", "")
            or order.get("ack_local_receive_ns", "")
            or order.get("request_send_local_ns", "")
        )
        anchor_ns = int(anchor_text) if anchor_text else 0
        symbol_id = int(symbol_id_text)
        lifecycle_records = (
            store.records_near(anchor_ns, 50_000_000)
            if anchor_ns
            else np.empty(0, dtype=store.dtype)
        )
        lifecycle_records = lifecycle_records[
            (lifecycle_records["symbol_id"] == symbol_id)
            & (lifecycle_records["local_ns"] <= anchor_ns)
        ]
        creation_ns = int(place_creation_ns)
        creation_records = latest_record_as_of(lifecycle_records, creation_ns)
        terminal_records = (
            latest_record_as_of(lifecycle_records, terminal_ns)
            if terminal_ns
            else np.empty(0, dtype=store.dtype)
        )
        if terminal_ns and len(creation_records) > 0:
            window_updates = lifecycle_records[
                (lifecycle_records["exchange_ns"] > creation_ns)
                & (lifecycle_records["exchange_ns"] <= terminal_ns)
            ]
            window_records = np.concatenate((creation_records, window_updates))
        else:
            window_records = np.empty(0, dtype=store.dtype)

        side = order.get("side", "")
        order_price = float(order_price_value)
        creation_class = classify_marketability(
            creation_records, side, order_price
        )
        window_class = classify_marketability(window_records, side, order_price)
        terminal_class = classify_marketability(
            terminal_records, side, order_price
        )

        signal_class = "missing"
        signal_lag_id = order.get("signal_lag_id", "")
        lag_local_text = order.get("lag_local_ns", "")
        if signal_lag_id and lag_local_text:
            signal_records = store.records_near(int(lag_local_text), 5_000_000)
            signal_records = signal_records[
                (signal_records["symbol_id"] == symbol_id)
                & (signal_records["id"] == int(signal_lag_id))
            ]
            signal_class = classify_marketability(signal_records, side, order_price)

        first_no_cross_delta = ""
        first_no_cross_price = ""
        request_send_text = order.get("request_send_local_ns", "")
        if request_send_text:
            request_send_ns = int(request_send_text)
            send_records = store.records_near(request_send_ns, 20_000_000)
            send_records = send_records[
                (send_records["symbol_id"] == symbol_id)
                & (send_records["local_ns"] >= request_send_ns)
            ]
            for record in send_records:
                if is_crossing(record, side, order_price):
                    continue
                first_no_cross_delta = str(
                    int(record["local_ns"]) - request_send_ns
                )
                opposite_price = (
                    float(record["ask_price"])
                    if side.lower().removeprefix("k") == "buy"
                    else float(record["bid_price"])
                )
                first_no_cross_price = format_float(opposite_price)
                break

        observation_class = (
            terminal_class
            if terminal_event in ("exec", "filled_feedback")
            else window_class
        )
        if len(lifecycle_records) == 0:
            missing_reason = "missing_bbo_near_local_terminal"
        elif len(creation_records) == 0:
            missing_reason = "missing_creation_bbo"
        elif len(terminal_records) == 0:
            missing_reason = "missing_terminal_bbo"
        elif len(window_records) == 0:
            missing_reason = "missing_bbo_exchange_window"
        else:
            missing_reason = ""

        row = {
            "run_id": order.get("run_id", ""),
            "local_order_id": local_order_id,
            "exchange_order_id": order.get("exchange_order_id", ""),
            "symbol": order.get("symbol", ""),
            "symbol_id": symbol_id_text,
            "order_role": order.get("order_role", ""),
            "status": order.get("status", ""),
            "side": side,
            "order_price": order.get("order_price", ""),
            "price_tick": order.get("price_tick", ""),
            "slippage_ticks": order.get("slippage_ticks", ""),
            "signal_lag_id": signal_lag_id,
            "signal_lag_local_ns": lag_local_text,
            "signal_lag_age_ns": order.get("lag_freshness_ns", ""),
            "request_send_local_ns": request_send_text,
            "place_creation_exchange_ns": place_creation_ns,
            "terminal_event": terminal_event,
            "terminal_exchange_ns": str(terminal_ns) if terminal_ns else "",
            "window_duration_ns": str(terminal_ns - creation_ns)
            if terminal_ns
            else "",
            "signal_marketability": signal_class,
            "creation_marketability": creation_class,
            "window_marketability": window_class,
            "terminal_marketability": terminal_class,
            "marketability_observation": marketability_observation(
                observation_class
            ),
            "bbo_records_at_creation": str(len(creation_records)),
            "bbo_records_in_window": str(len(window_records)),
            "bbo_records_at_terminal": str(len(terminal_records)),
            "first_no_cross_after_send_ns": first_no_cross_delta,
            "first_no_cross_opposite_price": first_no_cross_price,
            "missing_reason": missing_reason
            if terminal_ns
            else "missing_terminal_exchange_time",
        }
        result.append(
            {field: row.get(field, "") for field in ORDER_FILLABILITY_FIELDS}
        )
    return result


def write_order_fillability_csv(
    rows: list[dict[str, str]], output_path: Path
) -> None:
    with output_path.open("w", newline="", encoding="utf-8") as output_file:
        writer = csv.DictWriter(
            output_file, fieldnames=ORDER_FILLABILITY_FIELDS, lineterminator="\n"
        )
        writer.writeheader()
        writer.writerows(rows)
