#!/home/liuxiang/dev/pyenv/lx/bin/python

from __future__ import annotations

import argparse
import csv
import json
from collections import defaultdict, deque
from decimal import Decimal, InvalidOperation
from pathlib import Path
from typing import Iterable

GUARD_REQUIRED_FIELDS = {"symbol_id", "signal_lag_id", "action", "would_block"}
ORDER_REQUIRED_FIELDS = {
    "symbol_id",
    "signal_lag_id",
    "action",
    "order_role",
    "status",
}
POSITION_REQUIRED_FIELDS = {"symbol_id", "position_id", "status", "gross_pnl", "net_pnl"}


def read_csv_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def truthy(text: str) -> bool:
    return text.strip().lower() == "true"


def decimal_or_zero(text: str | None) -> Decimal:
    if text is None or text == "":
        return Decimal(0)
    try:
        return Decimal(text)
    except InvalidOperation:
        return Decimal(0)


def decimal_to_json(value: Decimal) -> str:
    return format(value.normalize(), "f") if value != 0 else "0"


def ratio_to_json(numerator: int, denominator: int) -> str:
    if denominator == 0:
        return "0"
    return decimal_to_json(Decimal(numerator) / Decimal(denominator))


def missing_fields(rows: list[dict[str, str]], required: set[str]) -> list[str]:
    if not rows:
        return []
    available = set(rows[0].keys())
    return sorted(required - available)


def audit_key(row: dict[str, str]) -> tuple[str, str, str]:
    return (
        row.get("symbol_id", ""),
        row.get("signal_lag_id", ""),
        row.get("action", ""),
    )


def entry_order_key(row: dict[str, str]) -> tuple[str, str, str]:
    return (
        row.get("symbol_id", ""),
        row.get("signal_lag_id", ""),
        row.get("action", ""),
    )


def position_key(row: dict[str, str]) -> tuple[str, str]:
    return row.get("symbol_id", ""), row.get("position_id", "")


def initial_group() -> dict[str, object]:
    return {
        "open_signal_count": 0,
        "order_count": 0,
        "submitted": 0,
        "filled": 0,
        "partially_filled": 0,
        "cancelled": 0,
        "zero_fill_cancelled": 0,
        "position_count": 0,
        "closed_positions": 0,
        "open_positions": 0,
        "partial_closed_positions": 0,
        "missing_entry_positions": 0,
        "over_closed_positions": 0,
        "unknown_position_status": 0,
        "gross_pnl": Decimal(0),
        "net_pnl": Decimal(0),
    }


def complete_group(group: dict[str, object]) -> dict[str, object]:
    result = dict(group)
    order_count = int(result["order_count"])
    zero_fill_cancelled = int(result["zero_fill_cancelled"])
    result["zero_fill_cancel_rate"] = ratio_to_json(zero_fill_cancelled, order_count)
    result["gross_pnl"] = decimal_to_json(result["gross_pnl"])  # type: ignore[arg-type]
    result["net_pnl"] = decimal_to_json(result["net_pnl"])  # type: ignore[arg-type]
    return result


def complete_breakdown(
    breakdown: dict[str, dict[str, int]]
) -> dict[str, dict[str, object]]:
    result: dict[str, dict[str, object]] = {}
    for key, value in sorted(breakdown.items()):
        open_count = value["open_signal_count"]
        block_count = value["would_block_count"]
        result[key] = {
            "open_signal_count": open_count,
            "would_block_count": block_count,
            "block_rate": ratio_to_json(block_count, open_count),
        }
    return result


def count_order(group: dict[str, object], order: dict[str, str]) -> None:
    group["order_count"] = int(group["order_count"]) + 1
    group["submitted"] = int(group["submitted"]) + 1
    status = order.get("status", "")
    filled_quantity = decimal_or_zero(order.get("cumulative_filled_quantity"))
    if status == "kFilled":
        group["filled"] = int(group["filled"]) + 1
    elif status == "kPartiallyCancelled" or (
        status == "kCancelled" and filled_quantity > 0
    ):
        group["partially_filled"] = int(group["partially_filled"]) + 1
    elif status == "kCancelled":
        group["cancelled"] = int(group["cancelled"]) + 1
        if filled_quantity == 0:
            group["zero_fill_cancelled"] = int(group["zero_fill_cancelled"]) + 1


def count_position(group: dict[str, object], position: dict[str, str]) -> None:
    group["position_count"] = int(group["position_count"]) + 1
    status = position.get("status", "")
    if status == "closed":
        group["closed_positions"] = int(group["closed_positions"]) + 1
    elif status == "open":
        group["open_positions"] = int(group["open_positions"]) + 1
    elif status == "partial_closed":
        group["partial_closed_positions"] = (
            int(group["partial_closed_positions"]) + 1
        )
    elif status == "missing_entry":
        group["missing_entry_positions"] = int(group["missing_entry_positions"]) + 1
    elif status == "over_closed":
        group["over_closed_positions"] = int(group["over_closed_positions"]) + 1
    else:
        group["unknown_position_status"] = int(group["unknown_position_status"]) + 1
    group["gross_pnl"] = group["gross_pnl"] + decimal_or_zero(  # type: ignore[operator]
        position.get("gross_pnl")
    )
    group["net_pnl"] = group["net_pnl"] + decimal_or_zero(  # type: ignore[operator]
        position.get("net_pnl")
    )


def build_order_index(
    order_rows: Iterable[dict[str, str]],
) -> tuple[dict[tuple[str, str, str], deque[dict[str, str]]], list[dict[str, str]]]:
    index: dict[tuple[str, str, str], deque[dict[str, str]]] = defaultdict(deque)
    entry_orders: list[dict[str, str]] = []
    for row in order_rows:
        if row.get("order_role", "") != "entry":
            continue
        entry_orders.append(row)
        index[entry_order_key(row)].append(row)
    return index, entry_orders


def build_position_index(
    position_rows: Iterable[dict[str, str]],
) -> dict[tuple[str, str], list[dict[str, str]]]:
    index: dict[tuple[str, str], list[dict[str, str]]] = defaultdict(list)
    for row in position_rows:
        index[position_key(row)].append(row)
    return index


def summarize_guard_audit(
    guard_audit: Path, order_detail: Path, position: Path | None
) -> dict[str, object]:
    audit_rows = read_csv_rows(guard_audit)
    order_rows = read_csv_rows(order_detail)
    position_rows = read_csv_rows(position) if position is not None else []
    warnings = []

    guard_missing = missing_fields(audit_rows, GUARD_REQUIRED_FIELDS)
    if guard_missing:
        warnings.append(
            "guard_audit missing required fields: " + ", ".join(guard_missing)
        )
    order_missing = missing_fields(order_rows, ORDER_REQUIRED_FIELDS)
    if order_missing:
        warnings.append(
            "order_detail missing required fields: " + ", ".join(order_missing)
        )
    position_missing = missing_fields(position_rows, POSITION_REQUIRED_FIELDS)
    if position_missing:
        warnings.append(
            "position missing required fields: " + ", ".join(position_missing)
        )

    order_index, entry_orders = build_order_index(order_rows)
    position_index = build_position_index(position_rows)
    groups = {"blocked": initial_group(), "allowed": initial_group()}
    unmatched_audit_rows = 0
    matched_order_ids: set[int] = set()
    matched_position_keys: set[tuple[str, str, int]] = set()
    by_symbol: dict[str, dict[str, int]] = defaultdict(
        lambda: {"open_signal_count": 0, "would_block_count": 0}
    )
    by_action: dict[str, dict[str, int]] = defaultdict(
        lambda: {"open_signal_count": 0, "would_block_count": 0}
    )

    for audit_row in audit_rows:
        blocked = truthy(audit_row.get("would_block", ""))
        group = groups["blocked" if blocked else "allowed"]
        group["open_signal_count"] = int(group["open_signal_count"]) + 1

        symbol = audit_row.get("symbol", "")
        action = audit_row.get("action", "")
        by_symbol[symbol]["open_signal_count"] += 1
        by_action[action]["open_signal_count"] += 1
        if blocked:
            by_symbol[symbol]["would_block_count"] += 1
            by_action[action]["would_block_count"] += 1

        key = audit_key(audit_row)
        if "" in key:
            unmatched_audit_rows += 1
            continue
        order_queue = order_index.get(key)
        if not order_queue:
            unmatched_audit_rows += 1
            continue

        order = order_queue.popleft()
        matched_order_ids.add(id(order))
        count_order(group, order)

        for index, position_row in enumerate(
            position_index.get(position_key(order), [])
        ):
            match_key = (*position_key(position_row), index)
            if match_key in matched_position_keys:
                continue
            matched_position_keys.add(match_key)
            count_position(group, position_row)

    unmatched_order_rows = sum(
        1 for row in entry_orders if id(row) not in matched_order_ids
    )
    open_signal_count = len(audit_rows)
    would_block_count = sum(
        1 for row in audit_rows if truthy(row.get("would_block", ""))
    )

    return {
        "totals": {
            "open_signal_count": open_signal_count,
            "would_block_count": would_block_count,
            "block_rate": (
                str(Decimal(would_block_count) / Decimal(open_signal_count))
                if open_signal_count
                else "0"
            ),
            "unmatched_audit_rows": unmatched_audit_rows,
            "unmatched_order_rows": unmatched_order_rows,
        },
        "by_symbol": complete_breakdown(by_symbol),
        "by_action": complete_breakdown(by_action),
        "groups": {
            "blocked": complete_group(groups["blocked"]),
            "allowed": complete_group(groups["allowed"]),
        },
        "warnings": warnings,
    }


def write_summary_json(summary: dict[str, object], output_path: Path) -> None:
    output_path.write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def write_summary_markdown(summary: dict[str, object], output_path: Path) -> None:
    totals = summary["totals"]  # type: ignore[index]
    groups = summary["groups"]  # type: ignore[index]
    by_symbol = summary["by_symbol"]  # type: ignore[index]
    by_action = summary["by_action"]  # type: ignore[index]
    warnings = summary["warnings"]  # type: ignore[index]
    lines = [
        "# Lag Vol Guard Audit Summary",
        "",
        f"- Open signals: {totals['open_signal_count']}",
        f"- Would block: {totals['would_block_count']}",
        f"- Block rate: {totals['block_rate']}",
        f"- Unmatched audit rows: {totals['unmatched_audit_rows']}",
        f"- Unmatched order rows: {totals['unmatched_order_rows']}",
        "",
        "| Group | Signals | Orders | Filled | Partial | Cancelled | Zero-fill Cancelled | Zero-fill Rate | Positions | Closed | Partial Closed | Open | Missing Entry | Over Closed | Gross PnL | Net PnL |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for name in ("blocked", "allowed"):
        group = groups[name]
        lines.append(
            f"| {name} | {group['open_signal_count']} | {group['order_count']} | "
            f"{group['filled']} | {group['partially_filled']} | "
            f"{group['cancelled']} | {group['zero_fill_cancelled']} | "
            f"{group['zero_fill_cancel_rate']} | {group['position_count']} | "
            f"{group['closed_positions']} | {group['partial_closed_positions']} | "
            f"{group['open_positions']} | {group['missing_entry_positions']} | "
            f"{group['over_closed_positions']} | {group['gross_pnl']} | "
            f"{group['net_pnl']} |"
        )
    lines.extend(["", "## By Symbol", ""])
    lines.extend(render_breakdown_table(by_symbol))
    lines.extend(["", "## By Action", ""])
    lines.extend(render_breakdown_table(by_action))
    lines.extend(["", "## Warnings", ""])
    if warnings:
        lines.extend(f"- {warning}" for warning in warnings)
    else:
        lines.append("- none")
    output_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def render_breakdown_table(breakdown: dict[str, dict[str, object]]) -> list[str]:
    lines = [
        "| Key | Open Signals | Would Block | Block Rate |",
        "| --- | ---: | ---: | ---: |",
    ]
    for key, value in sorted(breakdown.items()):
        lines.append(
            f"| {key} | {value['open_signal_count']} | "
            f"{value['would_block_count']} | {value['block_rate']} |"
        )
    return lines


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Summarize LeadLag lag_vol_guard replay audit results"
    )
    parser.add_argument("--guard-audit", required=True, type=Path)
    parser.add_argument("--order-detail", required=True, type=Path)
    parser.add_argument("--position", type=Path)
    parser.add_argument("--summary-json", type=Path)
    parser.add_argument("--summary-md", type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    summary = summarize_guard_audit(
        args.guard_audit, args.order_detail, args.position
    )
    if args.summary_json is not None:
        write_summary_json(summary, args.summary_json)
    else:
        print(json.dumps(summary, indent=2, sort_keys=True))
    if args.summary_md is not None:
        write_summary_markdown(summary, args.summary_md)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
