#!/home/liuxiang/dev/pyenv/lx/bin/python

import argparse
import csv
import json
import re
import shutil
import sys
import tomllib
from collections import Counter
from dataclasses import dataclass
from decimal import Decimal, InvalidOperation
from pathlib import Path

import analyze_bitget_execution as bitget_execution
import analyze_order_detail as orders


SIGNAL_DETAIL_FIELDS = [
    "run_id",
    "signal_index",
    "log_time",
    "trigger_exchange",
    "trigger_symbol_id",
    "trigger_exchange_ns",
    "trigger_local_ns",
    "on_book_ticker_entry_ns",
    "signal_decision_ns",
    "lead_exchange_ns",
    "lead_local_ns",
    "signal_lead_id",
    "lead_freshness_ns",
    "lag_exchange_ns",
    "lag_local_ns",
    "signal_lag_id",
    "lag_freshness_ns",
    "symbol",
    "symbol_id",
    "signal_role",
    "action",
    "side",
    "reduce_only",
    "signal_position_id",
    "raw_price",
    "local_order_id",
    "request_sequence",
    "request_send_local_ns",
    "bbo_to_strategy_ns",
    "strategy_to_signal_ns",
    "signal_to_request_send_ns",
    "trigger_to_request_send_ns",
    "exchange_order_id",
    "order_role",
    "order_position_id",
    "position_event",
    "position_direction",
    "order_price",
    "price_tick",
    "slippage_ticks",
    "quantity",
    "status",
    "cumulative_filled_quantity",
    "average_fill_price",
    "exec_slippage_ticks",
    "ack_rtt_ns",
    "order_finished_local_ns",
    "warnings",
]

DEFAULT_SCHEMA_PATH = Path("docs/lead_lag_live_report_csv_schema.md")
DEFAULT_INSTRUMENT_CATALOG_PATH = Path("config/instruments/usdt_futures.csv")
DEFAULT_BITGET_INSTRUMENT_CATALOG_PATH = Path(
    "config/instruments/usdt_future_universe.csv"
)
LOG_TIME_RE = re.compile(
    r"^I(?P<log_time>\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d+)"
)


@dataclass(frozen=True)
class LiveReportResult:
    report_dir: Path
    signal_rows: int
    order_rows: int
    position_rows: int
    latency_rows: int
    execution_rows: int = 0
    fillability_rows: int = 0


def parse_decimal(value: str | None) -> Decimal | None:
    if value in (None, ""):
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


def log_time_from_line(line: str) -> str:
    match = LOG_TIME_RE.search(line)
    if match is None:
        return ""
    return match.group("log_time")


def append_warning(existing: str, warning: str) -> str:
    if existing == "":
        return warning
    parts = existing.split(";")
    if warning in parts:
        return existing
    return existing + ";" + warning


def signal_join_key(fields: dict[str, str]) -> str:
    signal_decision_ns = fields.get("signal_decision_ns", "")
    if signal_decision_ns == "":
        return ""
    return "|".join(
        [
            signal_decision_ns,
            fields.get("symbol_id", ""),
            fields.get("action", ""),
            fields.get("side", ""),
            fields.get("reduce_only", ""),
            fields.get("raw_price", ""),
            fields.get("lead_exchange_ns", ""),
            fields.get("lag_exchange_ns", ""),
        ]
    )


def build_order_index(order_rows: list[dict[str, str]]) -> dict[str, dict[str, str]]:
    result: dict[str, dict[str, str]] = {}
    for order in order_rows:
        key = signal_join_key(order)
        if key == "":
            continue
        result[key] = order
    return result


def submitted_order_count(order_rows: list[dict[str, str]]) -> int:
    return sum(
        1
        for row in order_rows
        if row.get("source_schema", "") != "intent_rejected_v1"
    )


def build_signal_detail_rows(
    log_path: Path, order_rows: list[dict[str, str]], run_id: str
) -> list[dict[str, str]]:
    orders_by_trigger = build_order_index(order_rows)
    rows: list[dict[str, str]] = []
    with log_path.open(encoding="utf-8") as input_file:
        for line in input_file:
            message = orders.message_from_line(line)
            if message is None:
                continue
            tag, fields = orders.parse_message(message)
            if tag != "lead_lag_signal_triggered":
                continue
            order = orders_by_trigger.get(signal_join_key(fields), {})
            warnings = order.get("warnings", "")
            if not order:
                warnings = append_warning(warnings, "missing_order")
            row = {
                "run_id": run_id,
                "signal_index": str(len(rows) + 1),
                "log_time": log_time_from_line(line),
                "trigger_exchange": fields.get("trigger_exchange", ""),
                "trigger_symbol_id": fields.get("trigger_symbol_id", ""),
                "trigger_exchange_ns": fields.get("trigger_exchange_ns", ""),
                "trigger_local_ns": fields.get("trigger_local_ns", ""),
                "on_book_ticker_entry_ns": fields.get(
                    "on_book_ticker_entry_ns", ""
                ),
                "signal_decision_ns": fields.get("signal_decision_ns", ""),
                "lead_exchange_ns": (
                    fields.get("lead_exchange_ns", "")
                    or order.get("lead_exchange_ns", "")
                ),
                "lead_local_ns": (
                    fields.get("lead_local_ns", "")
                    or order.get("lead_local_ns", "")
                ),
                "signal_lead_id": (
                    fields.get("signal_lead_id", "")
                    or order.get("signal_lead_id", "")
                ),
                "lead_freshness_ns": (
                    fields.get("lead_freshness_ns", "")
                    or order.get("lead_freshness_ns", "")
                ),
                "lag_exchange_ns": (
                    fields.get("lag_exchange_ns", "")
                    or order.get("lag_exchange_ns", "")
                ),
                "lag_local_ns": (
                    fields.get("lag_local_ns", "") or order.get("lag_local_ns", "")
                ),
                "signal_lag_id": (
                    fields.get("signal_lag_id", "")
                    or order.get("signal_lag_id", "")
                ),
                "lag_freshness_ns": (
                    fields.get("lag_freshness_ns", "")
                    or order.get("lag_freshness_ns", "")
                ),
                "symbol": fields.get("symbol", ""),
                "symbol_id": fields.get("symbol_id", ""),
                "signal_role": fields.get("role", ""),
                "action": fields.get("action", ""),
                "side": fields.get("side", ""),
                "reduce_only": fields.get("reduce_only", ""),
                "signal_position_id": fields.get("position_id", ""),
                "raw_price": fields.get("raw_price", ""),
                "local_order_id": order.get("local_order_id", ""),
                "request_sequence": order.get("request_sequence", ""),
                "request_send_local_ns": order.get("request_send_local_ns", ""),
                "bbo_to_strategy_ns": orders.ns_delta_text(
                    fields.get("on_book_ticker_entry_ns", ""),
                    fields.get("trigger_local_ns", ""),
                ),
                "strategy_to_signal_ns": orders.ns_delta_text(
                    fields.get("signal_decision_ns", ""),
                    fields.get("on_book_ticker_entry_ns", ""),
                ),
                "signal_to_request_send_ns": orders.ns_delta_text(
                    order.get("request_send_local_ns", ""),
                    fields.get("signal_decision_ns", ""),
                ),
                "trigger_to_request_send_ns": orders.ns_delta_text(
                    order.get("request_send_local_ns", ""),
                    fields.get("trigger_local_ns", ""),
                ),
                "exchange_order_id": order.get("exchange_order_id", ""),
                "order_role": order.get("order_role", ""),
                "order_position_id": order.get("position_id", ""),
                "position_event": order.get("position_event", ""),
                "position_direction": order.get("position_direction", ""),
                "order_price": order.get("order_price", ""),
                "price_tick": order.get("price_tick", ""),
                "slippage_ticks": order.get("slippage_ticks", ""),
                "quantity": order.get("quantity", ""),
                "status": order.get("status", ""),
                "cumulative_filled_quantity": order.get(
                    "cumulative_filled_quantity", ""
                ),
                "average_fill_price": order.get("average_fill_price", ""),
                "exec_slippage_ticks": order.get("exec_slippage_ticks", ""),
                "ack_rtt_ns": order.get("ack_rtt_ns", ""),
                "order_finished_local_ns": order.get("order_finished_local_ns", ""),
                "warnings": warnings,
            }
            rows.append({field: row.get(field, "") for field in SIGNAL_DETAIL_FIELDS})
    return rows


def write_signal_detail_csv(rows: list[dict[str, str]], output_path: Path) -> None:
    with output_path.open("w", newline="", encoding="utf-8") as output_file:
        writer = csv.DictWriter(
            output_file, fieldnames=SIGNAL_DETAIL_FIELDS, lineterminator="\n"
        )
        writer.writeheader()
        writer.writerows(rows)


def count_positive(rows: list[dict[str, str]], field: str) -> int:
    count = 0
    for row in rows:
        value = parse_decimal(row.get(field))
        if value is not None and value > 0:
            count += 1
    return count


def sum_decimal(rows: list[dict[str, str]], field: str) -> Decimal:
    total = Decimal(0)
    for row in rows:
        value = parse_decimal(row.get(field))
        if value is not None:
            total += value
    return total


def ns_values(rows: list[dict[str, str]], field: str) -> list[int]:
    values: list[int] = []
    for row in rows:
        text = row.get(field, "")
        if text == "":
            continue
        try:
            values.append(int(text))
        except ValueError:
            continue
    return values


def int_value(text: str | None) -> int | None:
    if text in (None, ""):
        return None
    try:
        return int(text)
    except ValueError:
        return None


def ns_delta_value(lhs: str | None, rhs: str | None) -> int | None:
    lhs_value = int_value(lhs)
    rhs_value = int_value(rhs)
    if lhs_value is None or rhs_value is None:
        return None
    return lhs_value - rhs_value


def non_negative_ns_delta_value(lhs: str | None, rhs: str | None) -> int | None:
    value = ns_delta_value(lhs, rhs)
    if value is None or value < 0:
        return None
    return value


def positive_decimal_rows(
    rows: list[dict[str, str]], field: str
) -> list[dict[str, str]]:
    result: list[dict[str, str]] = []
    for row in rows:
        value = parse_decimal(row.get(field))
        if value is not None and value > 0:
            result.append(row)
    return result


def decimal_values(rows: list[dict[str, str]], field: str) -> list[Decimal]:
    values: list[Decimal] = []
    for row in rows:
        value = parse_decimal(row.get(field))
        if value is not None:
            values.append(value)
    return values


def decimal_percentile_nearest(
    values: list[Decimal], numerator: int, denominator: int
) -> Decimal:
    if not values:
        return Decimal(0)
    ordered = sorted(values)
    index = (len(ordered) * numerator + denominator - 1) // denominator - 1
    index = max(0, min(index, len(ordered) - 1))
    return ordered[index]


def format_decimal_rounded(value: Decimal | None, places: str = "0.001") -> str:
    if value is None:
        return ""
    return format_decimal(value.quantize(Decimal(places)))


def format_ms(ns: int | Decimal) -> str:
    value = Decimal(ns) / Decimal("1000000")
    return format_decimal(value.quantize(Decimal("0.001")))


def format_optional_ms(value: str | None) -> str:
    if value in (None, ""):
        return ""
    try:
        return format_ms(int(value))
    except ValueError:
        return ""


def percentile_nearest(values: list[int], numerator: int, denominator: int) -> int:
    if not values:
        return 0
    ordered = sorted(values)
    index = (len(ordered) * numerator + denominator - 1) // denominator - 1
    index = max(0, min(index, len(ordered) - 1))
    return ordered[index]


def format_percent(wins: int, total: int) -> str:
    if total <= 0:
        return ""
    value = (Decimal(wins) * Decimal("100")) / Decimal(total)
    return f"{value.quantize(Decimal('0.01'))}%"


def markdown_table(headers: list[str], rows: list[list[str]]) -> list[str]:
    if not rows:
        return []
    lines = [
        "| " + " | ".join(headers) + " |",
        "| " + " | ".join("---" for _ in headers) + " |",
    ]
    for row in rows:
        lines.append("| " + " | ".join(row) + " |")
    return lines


def format_int_like(value: object) -> str:
    if value in (None, ""):
        return ""
    if isinstance(value, bool):
        return str(int(value))
    if isinstance(value, int):
        return str(value)
    if isinstance(value, float):
        decimal_value = Decimal(str(value))
        if decimal_value == decimal_value.to_integral_value():
            return str(int(decimal_value))
        return format_decimal(decimal_value)
    if isinstance(value, str):
        decimal_value = parse_decimal(value)
        if decimal_value is None:
            return value
        if decimal_value == decimal_value.to_integral_value():
            return str(int(decimal_value))
        return format_decimal(decimal_value)
    return str(value)


def load_toml_dict(path: Path) -> dict:
    with path.open("rb") as input_file:
        value = tomllib.load(input_file)
    return value if isinstance(value, dict) else {}


def resolve_lead_lag_config(config_path: Path) -> dict:
    if not config_path.exists():
        return {}
    config = load_toml_dict(config_path)
    lead_lag = config.get("lead_lag")
    if isinstance(lead_lag, dict) and isinstance(lead_lag.get("pairs"), list):
        return config

    strategy = config.get("strategy")
    if not isinstance(strategy, dict):
        return config
    nested_text = strategy.get("config")
    if not isinstance(nested_text, str) or not nested_text:
        return config

    nested_path = Path(nested_text)
    candidates = [nested_path] if nested_path.is_absolute() else [
        config_path.parent / nested_path,
        nested_path,
    ]
    for candidate in candidates:
        if candidate.exists():
            return load_toml_dict(candidate)
    return config


def load_pair_freshness_rows(config_path: Path) -> list[dict[str, str]]:
    config = resolve_lead_lag_config(config_path)
    lead_lag = config.get("lead_lag")
    if not isinstance(lead_lag, dict):
        return []
    pairs = lead_lag.get("pairs")
    if not isinstance(pairs, list):
        return []

    rows: list[dict[str, str]] = []
    for pair in pairs:
        if not isinstance(pair, dict):
            continue
        rows.append(
            {
                "symbol": str(pair.get("symbol", "")),
                "symbol_id": format_int_like(pair.get("symbol_id")),
                "lead_exchange": str(pair.get("lead_exchange", "")),
                "lag_exchange": str(pair.get("lag_exchange", "")),
                "max_lead_freshness_ms": format_int_like(
                    pair.get("max_lead_freshness_ms")
                ),
                "max_lag_freshness_ms": format_int_like(
                    pair.get("max_lag_freshness_ms")
                ),
            }
        )
    return rows


def latency_rows_by_descending_ns(
    rows: list[dict[str, str]], field: str
) -> list[dict[str, str]]:
    def row_value(row: dict[str, str]) -> int:
        try:
            return int(row.get(field, ""))
        except ValueError:
            return -1

    return sorted(
        [row for row in rows if row_value(row) >= 0],
        key=row_value,
        reverse=True,
    )


@dataclass(frozen=True)
class AckSplitSample:
    local_order_id: str
    total_ns: int | None
    upstream_ns: int | None
    exchange_process_ns: int | None
    downstream_ns: int | None


def build_ack_split_samples(
    latency_rows: list[dict[str, str]]
) -> list[AckSplitSample]:
    samples: list[AckSplitSample] = []
    for row in latency_rows:
        total_ns = int_value(row.get("ack_rtt_ns"))
        if total_ns is None:
            total_ns = non_negative_ns_delta_value(
                row.get("ack_local_receive_ns"), row.get("request_send_local_ns")
            )
        samples.append(
            AckSplitSample(
                local_order_id=row.get("local_order_id", ""),
                total_ns=total_ns,
                upstream_ns=non_negative_ns_delta_value(
                    row.get("ack_exchange_request_ingress_ns"),
                    row.get("request_send_local_ns"),
                ),
                exchange_process_ns=int_value(row.get("ack_exchange_process_ns")),
                downstream_ns=non_negative_ns_delta_value(
                    row.get("ack_local_receive_ns"),
                    row.get("ack_exchange_response_egress_ns"),
                ),
            )
        )
    return samples


def ns_stat_table_row(label: str, values: list[int]) -> list[str]:
    if not values:
        return [label, "0", "", "", "", "", "", "", "0", "0", "0"]
    return [
        label,
        str(len(values)),
        format_ms(min(values)),
        format_ms(percentile_nearest(values, 1, 2)),
        format_ms(sum(values) // len(values)),
        format_ms(percentile_nearest(values, 95, 100)),
        format_ms(percentile_nearest(values, 99, 100)),
        format_ms(max(values)),
        str(sum(1 for value in values if value > 1_000_000)),
        str(sum(1 for value in values if value > 5_000_000)),
        str(sum(1 for value in values if value > 10_000_000)),
    ]


def dominant_ack_stage(sample: AckSplitSample) -> str:
    candidates = [
        ("上行", sample.upstream_ns),
        ("Gate in->out", sample.exchange_process_ns),
        ("下行", sample.downstream_ns),
    ]
    present = [(label, value) for label, value in candidates if value is not None]
    if not present:
        return ""
    return max(present, key=lambda item: item[1])[0]


def raw_position_gross_pnl(row: dict[str, str]) -> Decimal | None:
    entry_raw = parse_decimal(row.get("entry_raw_price"))
    exit_raw = parse_decimal(row.get("exit_raw_price"))
    matched_volume = parse_decimal(row.get("matched_volume"))
    multiplier = parse_decimal(row.get("contract_multiplier"))
    if (
        entry_raw is None
        or exit_raw is None
        or matched_volume is None
        or multiplier is None
    ):
        return None
    direction = row.get("position_direction", "")
    if direction == "kLong":
        return (exit_raw - entry_raw) * matched_volume * multiplier
    if direction == "kShort":
        return (entry_raw - exit_raw) * matched_volume * multiplier
    return None


def slippage_summary_row(label: str, rows: list[dict[str, str]]) -> list[str]:
    values = decimal_values(rows, "exec_slippage_ticks")
    improvements = decimal_values(rows, "limit_improvement_ticks")
    if not values:
        return [label, "0", "", "", "", "", "0", "0", ""]
    avg = sum(values) / Decimal(len(values))
    avg_improvement = (
        sum(improvements) / Decimal(len(improvements)) if improvements else None
    )
    return [
        label,
        str(len(values)),
        format_decimal_rounded(avg),
        format_decimal_rounded(decimal_percentile_nearest(values, 1, 2)),
        format_decimal_rounded(min(values)),
        format_decimal_rounded(max(values)),
        str(sum(1 for value in values if value > 0)),
        str(sum(1 for value in values if value < 0)),
        format_decimal_rounded(avg_improvement),
    ]


def load_guard_summary(path: Path | None) -> dict | None:
    if path is None:
        return None
    text = path.read_text(encoding="utf-8")
    try:
        value = json.loads(text)
    except json.JSONDecodeError:
        decoder = json.JSONDecoder()
        candidates: list[dict] = []
        index = 0
        while True:
            start = text.find("{", index)
            if start < 0:
                break
            try:
                value, end = decoder.raw_decode(text[start:])
            except json.JSONDecodeError:
                index = start + 1
                continue
            if isinstance(value, dict):
                candidates.append(value)
            index = start + max(end, 1)
        for candidate in reversed(candidates):
            if any(key in candidate for key in ("affinity", "result", "strategy")):
                return candidate
        return candidates[-1] if candidates else None
    return value if isinstance(value, dict) else None


def copy_runtime_configs(report_dir: Path, guard_summary: dict | None) -> None:
    if guard_summary is None:
        return
    affinity = guard_summary.get("affinity")
    if not isinstance(affinity, dict):
        return
    runtime_dir = report_dir / "runtime_configs"
    copies: list[tuple[str, str]] = []
    profile = affinity.get("profile")
    if isinstance(profile, str) and profile:
        copies.append(("profile", profile))
    generated = affinity.get("generated_configs", {})
    if isinstance(generated, dict):
        for name, path in generated.items():
            if isinstance(name, str) and isinstance(path, str) and path:
                copies.append((name, path))
    for name, source_text in copies:
        source = Path(source_text)
        if not source.exists() or not source.is_file():
            continue
        runtime_dir.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, runtime_dir / f"{name}__{source.name}")


def append_affinity_summary(lines: list[str], guard_summary: dict | None) -> None:
    if guard_summary is None:
        return
    affinity = guard_summary.get("affinity")
    if not isinstance(affinity, dict):
        return
    lines += [
        "",
        "## Runtime Profile",
        "",
        f"- affinity profile: `{affinity.get('profile_name', '')}`",
        f"- affinity split: `{str(affinity.get('affinity_split', False)).lower()}`",
        f"- affinity output dir: `{affinity.get('output_dir', '')}`",
    ]
    core_path = affinity.get("core_path", {})
    if isinstance(core_path, dict):
        for key in (
            "gate_market_data_cpu",
            "binance_market_data_cpu",
            "strategy_order_owner_cpu",
            "gate_order_feedback_cpu",
            "log_backend_cpu",
        ):
            if key in core_path:
                lines.append(f"- {key}: `{core_path[key]}`")
    generated = affinity.get("generated_configs", {})
    if isinstance(generated, dict):
        for name, path in sorted(generated.items()):
            lines.append(f"- generated {name} config: `{path}`")


def append_guard_result(lines: list[str], guard_summary: dict | None) -> None:
    if guard_summary is None:
        return
    lines += ["", "## Live 安全结果", ""]
    for key in (
        "result",
        "exit_code",
        "runtime_exit_code",
        "emergency_handoff",
        "final_flat",
    ):
        if key in guard_summary:
            lines.append(f"- {key}: `{guard_summary[key]}`")
    quiescence = guard_summary.get("quiescence")
    if isinstance(quiescence, dict):
        for key in ("ok", "reason"):
            if key in quiescence:
                lines.append(f"- quiescence {key}: `{quiescence[key]}`")


def append_run_definition(lines: list[str], run_definition: dict | None) -> None:
    if run_definition is None:
        return
    lines += ["", "## Run Definition", ""]
    for key in ("commit", "duration_sec", "limiter", "order_fanout"):
        if key in run_definition:
            lines.append(f"- {key}: `{run_definition[key]}`")
    market_fanout = run_definition.get("market_fanout")
    if isinstance(market_fanout, dict) and "bitget" in market_fanout:
        lines.append(
            f"- Bitget fusion: `{market_fanout['bitget']}` "
            f"(HA=`{market_fanout.get('bitget_ha', '')}`, "
            f"HS=`{market_fanout.get('bitget_hs', '')}`)"
        )


def append_latency_summary(
    lines: list[str], label: str, values: list[int]
) -> None:
    if not values:
        lines.append(f"- {label}: 无可用数据")
        return
    lines.append(
        f"- {label}: samples=`{len(values)}`, "
        f"median=`{format_ms(percentile_nearest(values, 1, 2))} ms`, "
        f"p95=`{format_ms(percentile_nearest(values, 95, 100))} ms`, "
        f"p99=`{format_ms(percentile_nearest(values, 99, 100))} ms`, "
        f"max=`{format_ms(max(values))} ms`"
    )


def write_markdown_report(
    *,
    output_path: Path,
    run_id: str,
    log_path: Path,
    config_path: Path | None,
    guard_stdout_path: Path | None,
    signal_rows: list[dict[str, str]],
    order_rows: list[dict[str, str]],
    position_rows: list[dict[str, str]],
    latency_rows: list[dict[str, str]],
    exchange: str = "gate",
    execution_rows: list[dict[str, str]] | None = None,
    execution_stats: dict[str, int | bool] | None = None,
    fillability_rows: list[dict[str, str]] | None = None,
    rest_fills_path: Path | None = None,
    book_ticker_manifest_path: Path | None = None,
    run_definition: dict | None = None,
    guard_summary: dict | None = None,
    pair_freshness_rows: list[dict[str, str]] | None = None,
) -> None:
    execution_rows = execution_rows or []
    execution_stats = execution_stats or {}
    fillability_rows = fillability_rows or []
    status_counts = Counter(row.get("status", "") or "unknown" for row in order_rows)
    symbol_counts = Counter(row.get("symbol", "") or "unknown" for row in signal_rows)
    ack_values = ns_values(latency_rows, "ack_rtt_ns")
    finish_values = ns_values(latency_rows, "send_to_finish_local_ns")
    exchange_lifecycle_values = ns_values(latency_rows, "exchange_lifecycle_ns")
    gate_ack_process_values = ns_values(latency_rows, "ack_exchange_process_ns")
    gross_pnl = sum_decimal(position_rows, "gross_pnl")
    net_pnl = sum_decimal(position_rows, "net_pnl")
    raw_position_rows: list[tuple[dict[str, str], Decimal, Decimal]] = []
    raw_gross_pnl = Decimal(0)
    raw_net_pnl = Decimal(0)
    for row in position_rows:
        raw_gross = raw_position_gross_pnl(row)
        if raw_gross is None:
            continue
        fee = parse_decimal(row.get("total_fee_quote_estimated")) or Decimal(0)
        raw_net = raw_gross - fee
        raw_gross_pnl += raw_gross
        raw_net_pnl += raw_net
        raw_position_rows.append((row, raw_gross, raw_net))
    actual_win_rows = [
        row
        for row in position_rows
        if parse_decimal(row.get("matched_volume")) not in (None, Decimal(0))
        and parse_decimal(row.get("net_pnl")) is not None
    ]
    actual_wins = sum(
        1
        for row in actual_win_rows
        if (parse_decimal(row.get("net_pnl")) or Decimal(0)) > 0
    )
    raw_wins = sum(1 for _, _, raw_net in raw_position_rows if raw_net > 0)
    any_fill_count = count_positive(order_rows, "cumulative_filled_quantity")
    filled_order_rows = positive_decimal_rows(order_rows, "cumulative_filled_quantity")
    finished_count = sum(1 for row in order_rows if row.get("order_finished_local_ns", ""))
    ack_count = sum(
        1
        for row in order_rows
        if row.get("ack_local_receive_ns", "") or row.get("ack_rtt_ns", "")
    )
    send_count = sum(1 for row in order_rows if row.get("request_sequence", ""))
    submitted_entry_rows = [
        row
        for row in order_rows
        if row.get("source_schema") == "submitted_v1"
        and row.get("order_role") == "entry"
    ]
    filled_entry_rows = positive_decimal_rows(
        submitted_entry_rows, "cumulative_filled_quantity"
    )
    reject_counts = Counter(
        row.get("reject_reason", "") or "unknown"
        for row in order_rows
        if row.get("source_schema") == "intent_rejected_v1"
    )

    lines = [
        "# LeadLag Live Run Report",
        "",
        "## 基本信息",
        "",
        f"- run_id: `{run_id}`",
        f"- exchange: `{exchange}`",
        f"- 策略配置: `{config_path}`" if config_path is not None else "- 策略配置: ``",
        f"- 源日志: `{log_path}`",
    ]
    if guard_stdout_path is not None:
        lines.append(f"- guard stdout: `{guard_stdout_path}`")
    if signal_rows:
        lines.append(f"- 首个 signal 时间: `{signal_rows[0].get('log_time', '')}`")
        lines.append(f"- 最后 signal 时间: `{signal_rows[-1].get('log_time', '')}`")
    append_affinity_summary(lines, guard_summary)
    append_guard_result(lines, guard_summary)
    append_run_definition(lines, run_definition)
    if pair_freshness_rows:
        lines += [
            "",
            "## Pair Freshness 参数",
            "",
            "单位为 `ms`，来自策略配置的 `max_lead_freshness_ms` / `max_lag_freshness_ms`。",
            "",
        ]
        lines += markdown_table(
            [
                "symbol",
                "symbol_id",
                "lead_exchange",
                "lag_exchange",
                "max_lead_freshness_ms",
                "max_lag_freshness_ms",
            ],
            [
                [
                    row.get("symbol", ""),
                    row.get("symbol_id", ""),
                    row.get("lead_exchange", ""),
                    row.get("lag_exchange", ""),
                    row.get("max_lead_freshness_ms", ""),
                    row.get("max_lag_freshness_ms", ""),
                ]
                for row in pair_freshness_rows
            ],
        )
    lines += [
        "",
        "## 同目录 CSV",
        "",
        f"- `signal.csv`: {len(signal_rows)} 条 signal，并关联对应 order",
        f"- `order_detail.csv`: {len(order_rows)} 条 order 明细",
        f"- `position.csv`: {len(position_rows)} 条 position 明细",
        f"- `latency.csv`: {len(latency_rows)} 条 order latency 明细",
        *(
            [
                f"- `execution_detail.csv`: {len(execution_rows)} 条逐笔成交明细",
                f"- `order_fillability.csv`: {len(fillability_rows)} 条 IOC BBO fillability 明细",
            ]
            if exchange == "bitget"
            else []
        ),
        "- 字段参考: `lead_lag_live_report_csv_schema.md`",
        "",
        "## Signal 和 Order",
        "",
        f"- signal: `{len(signal_rows)}`",
        f"- submitted order: `{submitted_order_count(order_rows)}`",
        f"- order session send: `{send_count}`",
        f"- ack: `{ack_count}`",
        f"- order finished: `{finished_count}`",
        f"- 有成交 order: `{any_fill_count}`",
        "",
    ]
    lines += [
        f"- entry submitted: `{len(submitted_entry_rows)}`",
        f"- entry any-fill: `{len(filled_entry_rows)}`",
        f"- entry any-fill rate: `{format_percent(len(filled_entry_rows), len(submitted_entry_rows))}`",
        "",
    ]
    if reject_counts:
        lines += ["", "### 未下单 signal 原因", ""]
        lines += markdown_table(
            ["reason", "count"],
            [[reason, str(count)] for reason, count in sorted(reject_counts.items())],
        )
        lines.append("")
    lines += markdown_table(
        ["symbol", "signals"],
        [[symbol, str(count)] for symbol, count in sorted(symbol_counts.items())],
    )
    if symbol_counts:
        lines.append("")
    lines += markdown_table(
        ["status", "count"],
        [[status, str(count)] for status, count in sorted(status_counts.items())],
    )
    if status_counts:
        lines.append("")
    if exchange == "bitget":
        submitted_by_symbol = Counter(
            row.get("symbol", "") or "unknown" for row in submitted_entry_rows
        )
        filled_by_symbol = Counter(
            row.get("symbol", "") or "unknown" for row in filled_entry_rows
        )
        lines += ["### Entry fill funnel（按 symbol）", ""]
        lines += markdown_table(
            ["symbol", "submitted", "any_fill", "fill_rate"],
            [
                [
                    symbol,
                    str(submitted_by_symbol[symbol]),
                    str(filled_by_symbol[symbol]),
                    format_percent(
                        filled_by_symbol[symbol], submitted_by_symbol[symbol]
                    ),
                ]
                for symbol in sorted(submitted_by_symbol)
            ],
        )
        if submitted_by_symbol:
            lines.append("")
        cancel_reason_counts = Counter(
            row.get("cancel_reason", "") or "unknown"
            for row in order_rows
            if row.get("status") in ("kCancelled", "kPartiallyCancelled")
        )
        if cancel_reason_counts:
            lines += ["### Bitget cancel reason", ""]
            lines += markdown_table(
                ["reason", "orders"],
                [
                    [reason, str(count)]
                    for reason, count in sorted(cancel_reason_counts.items())
                ],
            )
            lines.append("")
    if filled_order_rows:
        entry_filled_rows = [
            row for row in filled_order_rows if row.get("order_role", "") == "entry"
        ]
        exit_filled_rows = [
            row for row in filled_order_rows if row.get("order_role", "") == "exit"
        ]
        lines += [
            "### 滑点分析",
            "",
            "`exec_slippage_ticks` 以 raw price 为基准，正数表示成交比 raw 更差，负数表示成交优于 raw；`limit_improvement_ticks` 表示相对 aggressive limit 的改善 tick。",
            "",
        ]
        lines += markdown_table(
            [
                "scope",
                "filled_orders",
                "avg_exec_slippage_ticks",
                "median",
                "min",
                "max",
                "adverse",
                "favorable",
                "avg_limit_improvement_ticks",
            ],
            [
                slippage_summary_row("all filled", filled_order_rows),
                slippage_summary_row("entry", entry_filled_rows),
                slippage_summary_row("exit", exit_filled_rows),
            ],
        )
        lines.append("")
    if exchange == "bitget":
        lines += ["## Fast-fill 成交对账", ""]
        if rest_fills_path is not None:
            lines.append(f"- REST fills: `{rest_fills_path}`")
        for label, key in (
            ("fast-fill subscribed", "fast_fill_subscribed"),
            ("fast-fill records", "fast_fill_records"),
            ("fast-fill unique execIds", "fast_fill_unique_exec_ids"),
            ("fast-fill unique orders", "fast_fill_unique_orders"),
            ("fast-fill duplicate execIds", "fast_fill_duplicate_exec_ids"),
            ("fast-fill validation errors", "fast_fill_validation_errors"),
            ("authoritative filled orders", "authoritative_filled_orders"),
            ("filled orders missing fast-fill", "filled_orders_missing_fast_fill"),
            ("quantity mismatch orders", "quantity_mismatch_orders"),
            ("REST matched executions", "rest_matched_execution_records"),
            ("REST unmatched executions", "rest_unmatched_execution_records"),
        ):
            value = execution_stats.get(key, "unavailable")
            rendered_value = str(value).lower() if isinstance(value, bool) else value
            lines.append(f"- {label}: `{rendered_value}`")
        lines += [
            "",
            "Fast-fill 只用于逐笔成交与到达时序诊断；订单终态、累计成交量和恢复语义仍以 authoritative `order` channel 为准。跨交易所/本机时钟的差值不解释为单向网络延迟。",
            "",
        ]
        append_latency_summary(
            lines,
            "creation->exec（交易所时钟）",
            ns_values(execution_rows, "creation_to_exec_ns"),
        )
        append_latency_summary(
            lines,
            "fast-fill after Ack（本机 realtime）",
            ns_values(execution_rows, "fast_fill_after_ack_ns"),
        )
        append_latency_summary(
            lines,
            "fast-fill after order feedback（本机 realtime）",
            ns_values(execution_rows, "fast_fill_after_order_feedback_ns"),
        )

        lines += ["", "## IOC BBO Fillability", ""]
        if book_ticker_manifest_path is None:
            lines.append("- 未提供 `book_ticker_manifest`，本节不可用。")
        else:
            lines.append(f"- book ticker manifest: `{book_ticker_manifest_path}`")
            lines.append("")
            observation_counts = Counter(
                row.get("marketability_observation", "") or "missing"
                for row in fillability_rows
            )
            lines += markdown_table(
                ["observation", "orders"],
                [
                    [observation, str(count)]
                    for observation, count in sorted(observation_counts.items())
                ],
            )
            outcome_counts = Counter(
                (
                    row.get("terminal_event", "") or "unknown",
                    row.get("marketability_observation", "") or "missing",
                )
                for row in fillability_rows
            )
            lines += ["", "### 终态 × marketability", ""]
            lines += markdown_table(
                ["terminal_event", "observation", "orders"],
                [
                    [terminal_event, observation, str(count)]
                    for (terminal_event, observation), count in sorted(
                        outcome_counts.items()
                    )
                ],
            )
            missing_counts = Counter(
                row.get("missing_reason", "")
                for row in fillability_rows
                if row.get("missing_reason", "")
            )
            lines += [
                "",
                f"- complete BBO windows: `{len(fillability_rows) - sum(missing_counts.values())}`",
                f"- incomplete BBO windows: `{sum(missing_counts.values())}`",
            ]
            if missing_counts:
                lines += markdown_table(
                    ["missing_reason", "orders"],
                    [
                        [reason, str(count)]
                        for reason, count in sorted(missing_counts.items())
                    ],
                )
            lines += [
                "",
                "`marketable_observed` / `not_marketable_observed` 只描述已归档 BBO 窗口内是否观察到 IOC limit crossing，不证明交易所撮合队列位置，也不把缺失 BBO 判为未成交原因。",
                "",
            ]

    lines += [
        "## PnL",
        "",
        f"- {'order feedback gross PnL' if exchange == 'bitget' else 'gross PnL'}: `{format_decimal(gross_pnl)}`",
        f"- {'configured-fee net PnL estimate' if exchange == 'bitget' else 'net PnL'}: `{format_decimal(net_pnl)}`",
        "",
    ]
    if exchange == "bitget":
        rest_execution_rows = [
            row for row in execution_rows if row.get("rest_present") == "true"
        ]
        if rest_execution_rows:
            rest_exec_pnl = sum_decimal(rest_execution_rows, "exec_pnl")
            rest_fee = sum_decimal(rest_execution_rows, "actual_fee_quote")
            rest_net = rest_exec_pnl - rest_fee
            lines += [
                "### Bitget REST 实际 PnL",
                "",
                "以 `/api/v3/trade/fills` 返回的 `execPnl` 与 `feeDetail` 为准；配置费率 PnL 仅保留作估算对照。",
                "",
                f"- REST executions: `{len(rest_execution_rows)}`",
                f"- REST execPnl: `{format_decimal(rest_exec_pnl)} USDT`",
                f"- REST actual fee: `{format_decimal(rest_fee)} USDT`",
                f"- REST net PnL: `{format_decimal(rest_net)} USDT`",
                "",
            ]
        else:
            lines += [
                "### Bitget REST 实际 PnL",
                "",
                "- 未提供或未匹配 REST fills；实际手续费与账户实际 PnL 不可用，不能把配置费率估算值当作实际结果。",
                "",
            ]
    if raw_position_rows or actual_win_rows:
        comparison_heading = (
            "### Order feedback PnL 与 Raw PnL"
            if exchange == "bitget"
            else "### Raw PnL 和胜率"
        )
        comparison_description = (
            "Order feedback PnL 使用 feedback average fill price，fee 使用配置费率估算；Raw PnL 使用 entry / exit 的 `raw_price`。两者都不是 Bitget REST 实际手续费 PnL。"
            if exchange == "bitget"
            else "Raw PnL 使用 entry / exit 的 `raw_price` 计算，fee 仍使用 report CSV 中的配置费率估算值；胜率按 net PnL > 0 计算。"
        )
        actual_prefix = "feedback-price" if exchange == "bitget" else "actual"
        actual_net_label = (
            "feedback-price configured-fee net PnL"
            if exchange == "bitget"
            else "actual net PnL"
        )
        actual_win_label = (
            "feedback-price configured-fee win rate"
            if exchange == "bitget"
            else "actual win rate"
        )
        lines += [
            comparison_heading,
            "",
            comparison_description,
            "",
            f"- {actual_prefix} gross PnL: `{format_decimal(gross_pnl)}`",
            f"- {actual_net_label}: `{format_decimal(net_pnl)}`",
            f"- {actual_win_label}: `{format_percent(actual_wins, len(actual_win_rows))}` "
            f"({actual_wins}/{len(actual_win_rows)})",
            f"- raw gross PnL: `{format_decimal(raw_gross_pnl)}`",
            f"- raw net PnL: `{format_decimal(raw_net_pnl)}`",
            f"- raw win rate: `{format_percent(raw_wins, len(raw_position_rows))}` "
            f"({raw_wins}/{len(raw_position_rows)})",
            "",
        ]
        lines += markdown_table(
            [
                "symbol",
                "direction",
                "matched",
                f"{actual_prefix}_gross",
                "raw_gross",
                f"{actual_prefix}_net"
                if exchange == "gate"
                else f"{actual_prefix}_configured_fee_net",
                "raw_net",
                "actual_minus_raw_gross",
            ],
            [
                [
                    row.get("symbol", ""),
                    row.get("position_direction", ""),
                    row.get("matched_volume", ""),
                    row.get("gross_pnl", ""),
                    format_decimal(raw_gross),
                    row.get("net_pnl", ""),
                    format_decimal(raw_net),
                    format_decimal(
                        (parse_decimal(row.get("gross_pnl")) or Decimal(0))
                        - raw_gross
                    ),
                ]
                for row, raw_gross, raw_net in raw_position_rows
            ],
        )
        lines.append("")
    lines += markdown_table(
        ["symbol", "direction", "matched", "gross_pnl", "net_pnl"],
        [
            [
                row.get("symbol", ""),
                row.get("position_direction", ""),
                row.get("matched_volume", ""),
                row.get("gross_pnl", ""),
                row.get("net_pnl", ""),
            ]
            for row in position_rows
        ],
    )
    if position_rows:
        lines.append("")
        holding_by_position: dict[tuple[str, str], int] = {}
        for row in position_rows:
            holding_ns = int_value(row.get("holding_ns"))
            if holding_ns is None:
                continue
            key = (row.get("symbol_id", ""), row.get("position_id", ""))
            holding_by_position[key] = max(
                holding_by_position.get(key, 0), holding_ns
            )
        exit_counts = Counter(
            (row.get("symbol_id", ""), row.get("position_id", ""))
            for row in order_rows
            if row.get("source_schema") == "submitted_v1"
            and row.get("order_role") == "exit"
        )
        retry_positions = sum(1 for count in exit_counts.values() if count > 1)
        lines += ["## Close retry 与持仓时间", ""]
        lines.append(f"- positions with submitted exits: `{len(exit_counts)}`")
        lines.append(f"- positions requiring close retry: `{retry_positions}`")
        append_latency_summary(
            lines, "final holding time", list(holding_by_position.values())
        )
        lines.append("")
    lines += ["## 延迟", ""]
    if ack_values:
        avg_ack = sum(ack_values) // len(ack_values)
        lines += [
            "`ack_rtt_ns` 是本地下单发送到收到 Ack 的 RTT，不是收到 Filled / Cancelled 终态回报的时间。",
            "",
            f"- ack RTT min: `{format_ms(min(ack_values))} ms`",
            f"- ack RTT median: `{format_ms(percentile_nearest(ack_values, 1, 2))} ms`",
            f"- ack RTT avg: `{format_ms(avg_ack)} ms`",
            f"- ack RTT p95: `{format_ms(percentile_nearest(ack_values, 95, 100))} ms`",
            f"- ack RTT max: `{format_ms(max(ack_values))} ms`",
        ]
    else:
        lines.append("- ack RTT: 无可用数据")
    if exchange == "bitget":
        append_latency_summary(
            lines,
            "send->write complete（本机时钟）",
            ns_values(latency_rows, "send_to_write_complete_local_ns"),
        )
        append_latency_summary(
            lines,
            "write complete->Ack（monotonic）",
            ns_values(latency_rows, "write_complete_to_ack_local_ns"),
        )
        append_latency_summary(
            lines,
            "creation->terminal（交易所时钟）",
            ns_values(latency_rows, "bitget_creation_to_terminal_ns"),
        )
    if exchange == "gate" and gate_ack_process_values:
        avg_gate_ack_process = sum(gate_ack_process_values) // len(
            gate_ack_process_values
        )
        lines += [
            "- Gate Ack process min: "
            f"`{format_ms(min(gate_ack_process_values))} ms`",
            "- Gate Ack process median: "
            f"`{format_ms(percentile_nearest(gate_ack_process_values, 1, 2))} ms`",
            "- Gate Ack process avg: "
            f"`{format_ms(avg_gate_ack_process)} ms`",
            "- Gate Ack process p95: "
            f"`{format_ms(percentile_nearest(gate_ack_process_values, 95, 100))} ms`",
            "- Gate Ack process max: "
            f"`{format_ms(max(gate_ack_process_values))} ms`",
        ]
    ack_split_samples = build_ack_split_samples(latency_rows)
    if exchange == "gate" and ack_split_samples:
        total_values = [
            sample.total_ns for sample in ack_split_samples if sample.total_ns is not None
        ]
        upstream_values = [
            sample.upstream_ns
            for sample in ack_split_samples
            if sample.upstream_ns is not None
        ]
        exchange_process_split_values = [
            sample.exchange_process_ns
            for sample in ack_split_samples
            if sample.exchange_process_ns is not None
        ]
        downstream_values = [
            sample.downstream_ns
            for sample in ack_split_samples
            if sample.downstream_ns is not None
        ]
        lines += [
            "",
            "### Ack RTT 三段拆解",
            "",
            "Ack RTT 拆为上行 `request_send_local_ns -> ack_exchange_request_ingress_ns`、Gate `x_in -> x_out`、下行 `ack_exchange_response_egress_ns -> ack_local_receive_ns`。上行和下行跨本机 / Gate 时钟，只用于同机时钟同步后的诊断。",
            "",
        ]
        lines += markdown_table(
            [
                "stage",
                "samples",
                "min_ms",
                "median_ms",
                "avg_ms",
                "p95_ms",
                "p99_ms",
                "max_ms",
                ">1ms",
                ">5ms",
                ">10ms",
            ],
            [
                ns_stat_table_row("Ack RTT total", total_values),
                ns_stat_table_row("上行 send->Gate x_in", upstream_values),
                ns_stat_table_row(
                    "Gate x_in->x_out", exchange_process_split_values
                ),
                ns_stat_table_row("下行 Gate x_out->local", downstream_values),
            ],
        )
        tail_samples = [
            sample
            for sample in ack_split_samples
            if sample.total_ns is not None and sample.total_ns > 5_000_000
        ]
        if tail_samples:
            dominant_counts = Counter(
                dominant_ack_stage(sample) or "unknown" for sample in tail_samples
            )
            lines += [
                "",
                "- `>5ms` Ack tail dominant stage: "
                + ", ".join(
                    f"{stage}={count}"
                    for stage, count in sorted(dominant_counts.items())
                ),
            ]
            lines += markdown_table(
                [
                    "local_order_id",
                    "ack_rtt_ms",
                    "upstream_ms",
                    "gate_in_to_out_ms",
                    "downstream_ms",
                    "dominant_stage",
                ],
                [
                    [
                        sample.local_order_id,
                        format_ms(sample.total_ns or 0),
                        format_ms(sample.upstream_ns)
                        if sample.upstream_ns is not None
                        else "",
                        format_ms(sample.exchange_process_ns)
                        if sample.exchange_process_ns is not None
                        else "",
                        format_ms(sample.downstream_ns)
                        if sample.downstream_ns is not None
                        else "",
                        dominant_ack_stage(sample),
                    ]
                    for sample in sorted(
                        tail_samples,
                        key=lambda sample: sample.total_ns or 0,
                        reverse=True,
                    )[:10]
                ],
            )
    diagnostic_rows = [
        row for row in latency_rows if row.get("latency_diagnostic_reason", "")
    ]
    if diagnostic_rows:
        lines.append(f"- latency diagnostic outliers: `{len(diagnostic_rows)}`")
        lines += markdown_table(
            [
                "local_order_id",
                "reason",
                "ack_rtt_ms",
                "send_to_first_drive_read_ms",
                "drive_read_duration_ms",
            ],
            [
                [
                    row.get("local_order_id", ""),
                    row.get("latency_diagnostic_reason", ""),
                    format_optional_ms(row.get("latency_diagnostic_ack_rtt_ns")),
                    format_optional_ms(row.get("send_to_first_drive_read_ns")),
                    format_optional_ms(row.get("drive_read_duration_ns")),
                ]
                for row in diagnostic_rows[:10]
            ],
        )
    if finish_values:
        avg_finish = sum(finish_values) // len(finish_values)
        lines += [
            f"- send-to-finish min: `{format_ms(min(finish_values))} ms`",
            f"- send-to-finish median: `{format_ms(percentile_nearest(finish_values, 1, 2))} ms`",
            f"- send-to-finish avg: `{format_ms(avg_finish)} ms`",
            f"- send-to-finish p95: `{format_ms(percentile_nearest(finish_values, 95, 100))} ms`",
            f"- send-to-finish max: `{format_ms(max(finish_values))} ms`",
        ]
    else:
        lines.append("- send-to-finish: 无可用本地终态时间")
    if exchange == "gate" and exchange_lifecycle_values:
        avg_exchange_lifecycle = sum(exchange_lifecycle_values) // len(
            exchange_lifecycle_values
        )
        lines += [
            "",
            "`exchange_lifecycle_ns` 是 Gate exchange timestamp 中 Ack 到订单终态 update 的差值；它不混用本地时钟，但仍受 Gate timestamp 字段语义限制，只作交易所侧 lifecycle 诊断。",
            "",
            f"- exchange Ack-to-finish min: `{format_ms(min(exchange_lifecycle_values))} ms`",
            f"- exchange Ack-to-finish median: `{format_ms(percentile_nearest(exchange_lifecycle_values, 1, 2))} ms`",
            f"- exchange Ack-to-finish avg: `{format_ms(avg_exchange_lifecycle)} ms`",
            f"- exchange Ack-to-finish p95: `{format_ms(percentile_nearest(exchange_lifecycle_values, 95, 100))} ms`",
            f"- exchange Ack-to-finish max: `{format_ms(max(exchange_lifecycle_values))} ms`",
            "",
        ]
        lines += markdown_table(
            [
                "local_order_id",
                "symbol",
                "status",
                "finish_reason",
                "exchange_ack_to_finish_ms",
                "ack_to_finish_local_ms",
                "send_to_finish_ms",
            ],
            [
                [
                    row.get("local_order_id", ""),
                    row.get("symbol", ""),
                    row.get("status", ""),
                    row.get("finish_reason", ""),
                    format_optional_ms(row.get("exchange_lifecycle_ns")),
                    format_optional_ms(row.get("ack_to_finish_local_ns")),
                    format_optional_ms(row.get("send_to_finish_local_ns")),
                ]
                for row in latency_rows_by_descending_ns(
                    latency_rows, "exchange_lifecycle_ns"
                )[:5]
            ],
        )
    elif exchange == "gate":
        lines.append("- exchange Ack-to-finish: 无可用交易所终态时间")
    lines.append("")
    output_path.write_text("\n".join(lines), encoding="utf-8")


def generate_live_report(
    *,
    log_path: Path,
    additional_log_paths: list[Path] | None = None,
    exchange: str = "gate",
    feedback_log_path: Path | None = None,
    rest_fills_path: Path | None = None,
    book_ticker_manifest_path: Path | None = None,
    run_definition_path: Path | None = None,
    config_path: Path | None,
    instrument_catalog_path: Path | None,
    run_id: str,
    output_root: Path,
    schema_path: Path,
    guard_stdout_path: Path | None = None,
) -> LiveReportResult:
    report_dir = output_root / run_id
    report_dir.mkdir(parents=True, exist_ok=False)

    if instrument_catalog_path is None:
        instrument_catalog_path = (
            DEFAULT_BITGET_INSTRUMENT_CATALOG_PATH
            if exchange == "bitget"
            else DEFAULT_INSTRUMENT_CATALOG_PATH
        )
    order_result = orders.analyze_order_detail(
        log_path,
        additional_log_paths=additional_log_paths,
        config_path=config_path,
        instrument_catalog_path=instrument_catalog_path,
        exchange=exchange,
        run_id=run_id,
    )
    order_rows = order_result.rows
    position_rows = orders.build_position_detail_rows(order_rows)
    latency_rows = orders.build_latency_detail_rows(order_rows)
    signal_rows = build_signal_detail_rows(log_path, order_rows, run_id)
    execution_rows: list[dict[str, str]] = []
    execution_stats: dict[str, int | bool] = {}
    fillability_rows: list[dict[str, str]] = []
    if exchange == "bitget":
        if feedback_log_path is not None:
            execution_result = bitget_execution.analyze_executions(
                feedback_log_path,
                order_rows,
                rest_fills_path=rest_fills_path,
                run_id=run_id,
            )
            execution_rows = execution_result.rows
            execution_stats = execution_result.stats
        if book_ticker_manifest_path is not None:
            fillability_rows = bitget_execution.analyze_fillability(
                order_rows, execution_rows, book_ticker_manifest_path
            )
    guard_summary = load_guard_summary(guard_stdout_path)
    run_definition = None
    if run_definition_path is not None:
        loaded_run_definition = json.loads(
            run_definition_path.read_text(encoding="utf-8")
        )
        if not isinstance(loaded_run_definition, dict):
            raise ValueError("run definition JSON must be an object")
        run_definition = loaded_run_definition
    pair_freshness_rows = (
        load_pair_freshness_rows(config_path) if config_path is not None else []
    )
    copy_runtime_configs(report_dir, guard_summary)
    if run_definition_path is not None:
        shutil.copyfile(run_definition_path, report_dir / "run_definition.json")

    write_signal_detail_csv(signal_rows, report_dir / "signal.csv")
    orders.write_order_detail_csv(order_rows, report_dir / "order_detail.csv")
    orders.write_position_detail_csv(position_rows, report_dir / "position.csv")
    orders.write_latency_detail_csv(latency_rows, report_dir / "latency.csv")
    if exchange == "bitget":
        bitget_execution.write_execution_detail_csv(
            execution_rows, report_dir / "execution_detail.csv"
        )
        bitget_execution.write_order_fillability_csv(
            fillability_rows, report_dir / "order_fillability.csv"
        )
    shutil.copyfile(schema_path, report_dir / "lead_lag_live_report_csv_schema.md")
    write_markdown_report(
        output_path=report_dir / "report.md",
        run_id=run_id,
        log_path=log_path,
        config_path=config_path,
        guard_stdout_path=guard_stdout_path,
        signal_rows=signal_rows,
        order_rows=order_rows,
        position_rows=position_rows,
        latency_rows=latency_rows,
        exchange=exchange,
        execution_rows=execution_rows,
        execution_stats=execution_stats,
        fillability_rows=fillability_rows,
        rest_fills_path=rest_fills_path,
        book_ticker_manifest_path=book_ticker_manifest_path,
        run_definition=run_definition,
        guard_summary=guard_summary,
        pair_freshness_rows=pair_freshness_rows,
    )
    return LiveReportResult(
        report_dir=report_dir,
        signal_rows=len(signal_rows),
        order_rows=len(order_rows),
        position_rows=len(position_rows),
        latency_rows=len(latency_rows),
        execution_rows=len(execution_rows),
        fillability_rows=len(fillability_rows),
    )


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Generate a LeadLag live report")
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--log", required=True, type=Path)
    parser.add_argument("--additional-log", action="append", type=Path, default=[])
    parser.add_argument("--exchange", choices=("gate", "bitget"), default="gate")
    parser.add_argument("--feedback-log", type=Path)
    parser.add_argument("--rest-fills", type=Path)
    parser.add_argument("--book-ticker-manifest", type=Path)
    parser.add_argument("--run-definition", type=Path)
    parser.add_argument("--config", type=Path)
    parser.add_argument(
        "--instrument-catalog",
        type=Path,
        default=None,
    )
    parser.add_argument("--guard-stdout", type=Path)
    parser.add_argument("--output-root", type=Path, default=Path("reports"))
    parser.add_argument("--schema", type=Path, default=DEFAULT_SCHEMA_PATH)
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    result = generate_live_report(
        log_path=args.log,
        additional_log_paths=args.additional_log,
        exchange=args.exchange,
        feedback_log_path=args.feedback_log,
        rest_fills_path=args.rest_fills,
        book_ticker_manifest_path=args.book_ticker_manifest,
        run_definition_path=args.run_definition,
        config_path=args.config,
        instrument_catalog_path=args.instrument_catalog,
        run_id=args.run_id,
        output_root=args.output_root,
        schema_path=args.schema,
        guard_stdout_path=args.guard_stdout,
    )
    print(
        "wrote report "
        f"{result.report_dir} "
        f"signals={result.signal_rows} "
        f"orders={result.order_rows} "
        f"positions={result.position_rows} "
        f"latency={result.latency_rows}"
        f" executions={result.execution_rows}"
        f" fillability={result.fillability_rows}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
