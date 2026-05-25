#!/home/liuxiang/dev/pyenv/lx/bin/python

import argparse
import csv
import re
import shutil
import sys
from collections import Counter
from dataclasses import dataclass
from decimal import Decimal, InvalidOperation
from pathlib import Path

import analyze_order_detail as orders


SIGNAL_DETAIL_FIELDS = [
    "run_id",
    "signal_index",
    "log_time",
    "trigger_ticker_id",
    "trigger_exchange",
    "trigger_symbol_id",
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


def build_order_index(order_rows: list[dict[str, str]]) -> dict[str, dict[str, str]]:
    result: dict[str, dict[str, str]] = {}
    for order in order_rows:
        trigger_ticker_id = order.get("trigger_ticker_id", "")
        if trigger_ticker_id == "":
            continue
        result[trigger_ticker_id] = order
    return result


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
            trigger_ticker_id = fields.get("trigger_ticker_id", "")
            order = orders_by_trigger.get(trigger_ticker_id, {})
            warnings = order.get("warnings", "")
            if not order:
                warnings = append_warning(warnings, "missing_order")
            row = {
                "run_id": run_id,
                "signal_index": str(len(rows) + 1),
                "log_time": log_time_from_line(line),
                "trigger_ticker_id": trigger_ticker_id,
                "trigger_exchange": fields.get("trigger_exchange", ""),
                "trigger_symbol_id": fields.get("trigger_symbol_id", ""),
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
        writer = csv.DictWriter(output_file, fieldnames=SIGNAL_DETAIL_FIELDS)
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


def format_ms(ns: int | Decimal) -> str:
    value = Decimal(ns) / Decimal("1000000")
    return format_decimal(value.quantize(Decimal("0.001")))


def percentile_nearest(values: list[int], numerator: int, denominator: int) -> int:
    if not values:
        return 0
    ordered = sorted(values)
    index = (len(ordered) * numerator + denominator - 1) // denominator - 1
    index = max(0, min(index, len(ordered) - 1))
    return ordered[index]


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
) -> None:
    status_counts = Counter(row.get("status", "") or "unknown" for row in order_rows)
    symbol_counts = Counter(row.get("symbol", "") or "unknown" for row in signal_rows)
    ack_values = ns_values(latency_rows, "ack_rtt_ns")
    finish_values = ns_values(latency_rows, "send_to_finish_local_ns")
    gross_pnl = sum_decimal(position_rows, "gross_pnl")
    net_pnl = sum_decimal(position_rows, "net_pnl")
    any_fill_count = count_positive(order_rows, "cumulative_filled_quantity")
    finished_count = sum(1 for row in order_rows if row.get("order_finished_local_ns", ""))
    ack_count = sum(
        1
        for row in order_rows
        if row.get("ack_local_receive_ns", "") or row.get("ack_rtt_ns", "")
    )
    send_count = sum(1 for row in order_rows if row.get("request_sequence", ""))

    lines = [
        "# LeadLag Live Run Report",
        "",
        "## 基本信息",
        "",
        f"- run_id: `{run_id}`",
        f"- 策略配置: `{config_path}`" if config_path is not None else "- 策略配置: ``",
        f"- 源日志: `{log_path}`",
    ]
    if guard_stdout_path is not None:
        lines.append(f"- guard stdout: `{guard_stdout_path}`")
    if signal_rows:
        lines.append(f"- 首个 signal 时间: `{signal_rows[0].get('log_time', '')}`")
        lines.append(f"- 最后 signal 时间: `{signal_rows[-1].get('log_time', '')}`")
    lines += [
        "",
        "## 同目录 CSV",
        "",
        f"- `signal.csv`: {len(signal_rows)} 条 signal，并关联对应 order",
        f"- `order_detail.csv`: {len(order_rows)} 条 order 明细",
        f"- `position.csv`: {len(position_rows)} 条 position 明细",
        f"- `latency.csv`: {len(latency_rows)} 条 order latency 明细",
        "- 字段参考: `lead_lag_live_report_csv_schema.md`",
        "",
        "## Signal 和 Order",
        "",
        f"- signal: `{len(signal_rows)}`",
        f"- submitted order: `{len(order_rows)}`",
        f"- Gate send ok: `{send_count}`",
        f"- ack: `{ack_count}`",
        f"- order finished: `{finished_count}`",
        f"- 有成交 order: `{any_fill_count}`",
        "",
    ]
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
    lines += [
        "## PnL",
        "",
        f"- gross PnL: `{format_decimal(gross_pnl)}`",
        f"- net PnL: `{format_decimal(net_pnl)}`",
        "",
    ]
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
    if finish_values:
        avg_finish = sum(finish_values) // len(finish_values)
        lines += [
            f"- send-to-finish min: `{format_ms(min(finish_values))} ms`",
            f"- send-to-finish median: `{format_ms(percentile_nearest(finish_values, 1, 2))} ms`",
            f"- send-to-finish avg: `{format_ms(avg_finish)} ms`",
            f"- send-to-finish p95: `{format_ms(percentile_nearest(finish_values, 95, 100))} ms`",
            f"- send-to-finish max: `{format_ms(max(finish_values))} ms`",
        ]
    lines.append("")
    output_path.write_text("\n".join(lines), encoding="utf-8")


def generate_live_report(
    *,
    log_path: Path,
    config_path: Path | None,
    instrument_catalog_path: Path | None,
    run_id: str,
    output_root: Path,
    schema_path: Path,
    guard_stdout_path: Path | None = None,
) -> LiveReportResult:
    report_dir = output_root / run_id
    report_dir.mkdir(parents=True, exist_ok=False)

    order_result = orders.analyze_order_detail(
        log_path,
        config_path=config_path,
        instrument_catalog_path=instrument_catalog_path,
        run_id=run_id,
    )
    order_rows = order_result.rows
    position_rows = orders.build_position_detail_rows(order_rows)
    latency_rows = orders.build_latency_detail_rows(order_rows)
    signal_rows = build_signal_detail_rows(log_path, order_rows, run_id)

    write_signal_detail_csv(signal_rows, report_dir / "signal.csv")
    orders.write_order_detail_csv(order_rows, report_dir / "order_detail.csv")
    orders.write_position_detail_csv(position_rows, report_dir / "position.csv")
    orders.write_latency_detail_csv(latency_rows, report_dir / "latency.csv")
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
    )
    return LiveReportResult(
        report_dir=report_dir,
        signal_rows=len(signal_rows),
        order_rows=len(order_rows),
        position_rows=len(position_rows),
        latency_rows=len(latency_rows),
    )


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Generate a LeadLag live report")
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--log", required=True, type=Path)
    parser.add_argument("--config", type=Path)
    parser.add_argument(
        "--instrument-catalog",
        type=Path,
        default=DEFAULT_INSTRUMENT_CATALOG_PATH,
    )
    parser.add_argument("--guard-stdout", type=Path)
    parser.add_argument("--output-root", type=Path, default=Path("reports"))
    parser.add_argument("--schema", type=Path, default=DEFAULT_SCHEMA_PATH)
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    result = generate_live_report(
        log_path=args.log,
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
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
