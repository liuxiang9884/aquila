#!/home/liuxiang/dev/pyenv/lx/bin/python

import argparse
import csv
import json
import sys
from dataclasses import dataclass
from decimal import Decimal, InvalidOperation
from pathlib import Path
from typing import Any


DEFAULT_KEY_FIELDS = [
    "symbol_id",
    "exchange",
    "role",
    "exchange_ns",
    "event_ns",
]

DEFAULT_COMPARE_FIELDS = [
    "action",
    "side",
    "raw_price",
    "reduce_only",
    "exchange_ns",
    "local_ns",
    "event_ns",
    "lead_exchange_ns",
    "lead_raw_bid",
    "lead_raw_ask",
    "lead_drifted_event_ns",
    "lead_drifted_bid",
    "lead_drifted_ask",
    "lag_exchange_ns",
    "lag_bid",
    "lag_ask",
    "drift_mean",
    "drift_ready",
    "drift_deviation",
    "up_entry",
    "down_entry",
    "up_exit",
    "down_exit",
    "lag_spread_mean",
    "lead_noise",
    "lag_noise",
    "active_group_count",
    "group_id",
    "position_direction",
    "trailing_price",
]

DEFAULT_NUMERIC_FIELDS = {
    "symbol_id",
    "exchange_ns",
    "local_ns",
    "event_ns",
    "raw_price",
    "lead_exchange_ns",
    "lead_raw_bid",
    "lead_raw_ask",
    "lead_drifted_event_ns",
    "lead_drifted_bid",
    "lead_drifted_ask",
    "lag_exchange_ns",
    "lag_bid",
    "lag_ask",
    "drift_mean",
    "drift_deviation",
    "up_entry",
    "down_entry",
    "up_exit",
    "down_exit",
    "lag_spread_mean",
    "lead_noise",
    "lag_noise",
    "active_group_count",
    "group_id",
    "trailing_price",
}

DEFAULT_NUMERIC_TOLERANCE = Decimal("0.00000001")
DEFAULT_SAMPLE_LIMIT = 10

ROW_EXCERPT_FIELDS = [
    "symbol_id",
    "exchange",
    "role",
    "exchange_ns",
    "event_ns",
    "action",
    "side",
    "raw_price",
    "reduce_only",
    "lead_drifted_event_ns",
    "lag_exchange_ns",
    "position_direction",
]


@dataclass(frozen=True)
class IndexedSignal:
    row_number: int
    values: dict[str, str]
    key_values: tuple[str, ...]
    occurrence: int

    def lookup_key(self) -> tuple[str, ...]:
        return (*self.key_values, str(self.occurrence))


def parse_field_list(text: str) -> list[str]:
    fields = [field.strip() for field in text.split(",") if field.strip()]
    if not fields:
        raise ValueError("field list must not be empty")
    return fields


def read_signal_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as input_file:
        reader = csv.DictReader(input_file)
        if reader.fieldnames is None:
            raise ValueError(f"missing CSV header: {path}")
        return [dict(row) for row in reader]


def index_rows(
    rows: list[dict[str, str]],
    key_fields: list[str],
) -> dict[tuple[str, ...], IndexedSignal]:
    seen: dict[tuple[str, ...], int] = {}
    indexed: dict[tuple[str, ...], IndexedSignal] = {}
    for offset, row in enumerate(rows, 2):
        missing = [field for field in key_fields if field not in row]
        if missing:
            raise ValueError(
                f"row {offset} missing key fields: {','.join(missing)}"
            )
        key_values = tuple(row[field] for field in key_fields)
        occurrence = seen.get(key_values, 0) + 1
        seen[key_values] = occurrence
        signal = IndexedSignal(
            row_number=offset,
            values=row,
            key_values=key_values,
            occurrence=occurrence,
        )
        indexed[signal.lookup_key()] = signal
    return indexed


def key_dict(signal: IndexedSignal, key_fields: list[str]) -> dict[str, Any]:
    result = {
        field: value for field, value in zip(key_fields, signal.key_values)
    }
    result["_occurrence"] = signal.occurrence
    return result


def row_excerpt(signal: IndexedSignal) -> dict[str, str]:
    return {
        field: signal.values[field]
        for field in ROW_EXCERPT_FIELDS
        if field in signal.values
    }


def row_sample(signal: IndexedSignal, key_fields: list[str]) -> dict[str, Any]:
    return {
        "row_number": signal.row_number,
        "key": key_dict(signal, key_fields),
        "row": row_excerpt(signal),
    }


def decimal_or_none(value: str) -> Decimal | None:
    if value == "":
        return None
    try:
        return Decimal(value)
    except InvalidOperation:
        return None


def compare_field(
    field: str,
    live_value: str | None,
    replay_value: str | None,
    numeric_fields: set[str],
    numeric_tolerance: Decimal,
) -> dict[str, str] | None:
    if live_value == replay_value:
        return None

    if field in numeric_fields and live_value is not None and replay_value is not None:
        live_decimal = decimal_or_none(live_value)
        replay_decimal = decimal_or_none(replay_value)
        if live_decimal is not None and replay_decimal is not None:
            difference = abs(live_decimal - replay_decimal)
            if difference <= numeric_tolerance:
                return None
            return {
                "field": field,
                "live": live_value,
                "replay": replay_value,
                "absolute_difference": str(difference),
            }

    return {
        "field": field,
        "live": "" if live_value is None else live_value,
        "replay": "" if replay_value is None else replay_value,
    }


def compare_rows(
    live: IndexedSignal,
    replay: IndexedSignal,
    compare_fields: list[str],
    numeric_fields: set[str],
    numeric_tolerance: Decimal,
    key_fields: list[str],
) -> dict[str, Any] | None:
    differences = []
    for field in compare_fields:
        if field not in live.values and field not in replay.values:
            continue
        difference = compare_field(
            field,
            live.values.get(field),
            replay.values.get(field),
            numeric_fields,
            numeric_tolerance,
        )
        if difference is not None:
            differences.append(difference)

    if not differences:
        return None

    return {
        "key": key_dict(live, key_fields),
        "live_row_number": live.row_number,
        "replay_row_number": replay.row_number,
        "differences": differences,
        "live_row": row_excerpt(live),
        "replay_row": row_excerpt(replay),
    }


def compare_csv_files(
    live_csv: Path,
    replay_csv: Path,
    *,
    key_fields: list[str] | None = None,
    compare_fields: list[str] | None = None,
    numeric_fields: set[str] | None = None,
    numeric_tolerance: Decimal = DEFAULT_NUMERIC_TOLERANCE,
    sample_limit: int = DEFAULT_SAMPLE_LIMIT,
) -> dict[str, Any]:
    key_fields = list(DEFAULT_KEY_FIELDS if key_fields is None else key_fields)
    compare_fields = list(
        DEFAULT_COMPARE_FIELDS if compare_fields is None else compare_fields
    )
    numeric_fields = set(
        DEFAULT_NUMERIC_FIELDS if numeric_fields is None else numeric_fields
    )

    live_rows = read_signal_csv(live_csv)
    replay_rows = read_signal_csv(replay_csv)
    live_index = index_rows(live_rows, key_fields)
    replay_index = index_rows(replay_rows, key_fields)
    live_keys = set(live_index)
    replay_keys = set(replay_index)
    matched_keys = sorted(live_keys & replay_keys)
    live_only_keys = sorted(live_keys - replay_keys)
    replay_only_keys = sorted(replay_keys - live_keys)

    mismatches = []
    for lookup_key in matched_keys:
        mismatch = compare_rows(
            live_index[lookup_key],
            replay_index[lookup_key],
            compare_fields,
            numeric_fields,
            numeric_tolerance,
            key_fields,
        )
        if mismatch is not None:
            mismatches.append(mismatch)

    live_only = [
        row_sample(live_index[lookup_key], key_fields)
        for lookup_key in live_only_keys[:sample_limit]
    ]
    replay_only = [
        row_sample(replay_index[lookup_key], key_fields)
        for lookup_key in replay_only_keys[:sample_limit]
    ]

    return {
        "live_csv": str(live_csv),
        "replay_csv": str(replay_csv),
        "key_fields": key_fields,
        "compare_fields": compare_fields,
        "numeric_tolerance": str(numeric_tolerance),
        "sample_limit": sample_limit,
        "counts": {
            "live_signals": len(live_rows),
            "replay_signals": len(replay_rows),
            "matched": len(matched_keys),
            "live_only": len(live_only_keys),
            "replay_only": len(replay_only_keys),
            "mismatched": len(mismatches),
        },
        "live_only": live_only,
        "replay_only": replay_only,
        "mismatches": mismatches[:sample_limit],
        "mismatches_total": len(mismatches),
    }


def format_markdown(summary: dict[str, Any]) -> str:
    counts = summary["counts"]
    lines = [
        "# LeadLag Signal Compare Summary",
        "",
        f"- live_csv: `{summary['live_csv']}`",
        f"- replay_csv: `{summary['replay_csv']}`",
        f"- key_fields: `{','.join(summary['key_fields'])}`",
        f"- compare_fields: `{','.join(summary['compare_fields'])}`",
        f"- numeric_tolerance: `{summary['numeric_tolerance']}`",
        "",
        "## Counts",
        "",
        "| metric | count |",
        "| --- | ---: |",
    ]
    for name in [
        "live_signals",
        "replay_signals",
        "matched",
        "live_only",
        "replay_only",
        "mismatched",
    ]:
        lines.append(f"| {name} | {counts[name]} |")

    lines.extend(
        [
            "",
            "## Notes",
            "",
            "- `live_only` means a live signal key was not produced during replay.",
            "- `replay_only` means replay produced a signal key not seen in live output.",
            "- With live `latest` readers and recorder `drain` readers, replay-only rows can appear because replay sees intermediate ticks that live sampling skipped.",
            "",
        ]
    )

    append_samples(lines, "Live Only Samples", summary["live_only"])
    append_samples(lines, "Replay Only Samples", summary["replay_only"])
    append_mismatches(lines, summary["mismatches"], summary["mismatches_total"])
    return "\n".join(lines) + "\n"


def append_samples(lines: list[str], title: str, samples: list[dict[str, Any]]) -> None:
    lines.extend([f"## {title}", ""])
    if not samples:
        lines.extend(["None.", ""])
        return
    for sample in samples:
        key = json.dumps(sample["key"], sort_keys=True, ensure_ascii=False)
        row = json.dumps(sample["row"], sort_keys=True, ensure_ascii=False)
        lines.append(f"- row={sample['row_number']} key=`{key}` row=`{row}`")
    lines.append("")


def append_mismatches(
    lines: list[str],
    mismatches: list[dict[str, Any]],
    mismatches_total: int,
) -> None:
    lines.extend(["## Mismatch Samples", ""])
    if not mismatches:
        lines.extend(["None.", ""])
        return
    lines.append(f"Showing {len(mismatches)} of {mismatches_total}.")
    lines.append("")
    for mismatch in mismatches:
        key = json.dumps(mismatch["key"], sort_keys=True, ensure_ascii=False)
        lines.append(
            f"- key=`{key}` live_row={mismatch['live_row_number']} "
            f"replay_row={mismatch['replay_row_number']}"
        )
        for difference in mismatch["differences"]:
            extra = ""
            if "absolute_difference" in difference:
                extra = f" abs_diff={difference['absolute_difference']}"
            lines.append(
                f"  - {difference['field']}: live=`{difference['live']}` "
                f"replay=`{difference['replay']}`{extra}"
            )
    lines.append("")


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Compare lead_lag live and replay signal CSV files."
    )
    parser.add_argument("live_csv", type=Path)
    parser.add_argument("replay_csv", type=Path)
    parser.add_argument(
        "--key-fields",
        default=",".join(DEFAULT_KEY_FIELDS),
        help="Comma-separated fields used as the signal identity.",
    )
    parser.add_argument(
        "--compare-fields",
        default=",".join(DEFAULT_COMPARE_FIELDS),
        help="Comma-separated fields compared for matched signal keys.",
    )
    parser.add_argument(
        "--numeric-tolerance",
        default=str(DEFAULT_NUMERIC_TOLERANCE),
        help="Decimal tolerance for numeric compare fields.",
    )
    parser.add_argument(
        "--sample-limit",
        type=int,
        default=DEFAULT_SAMPLE_LIMIT,
        help="Maximum samples to include per difference category.",
    )
    parser.add_argument("--json-output", type=Path)
    parser.add_argument("--markdown-output", type=Path)
    parser.add_argument(
        "--fail-on-difference",
        action="store_true",
        help="Return 1 when any live-only, replay-only, or mismatched rows exist.",
    )
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    summary = compare_csv_files(
        args.live_csv,
        args.replay_csv,
        key_fields=parse_field_list(args.key_fields),
        compare_fields=parse_field_list(args.compare_fields),
        numeric_tolerance=Decimal(args.numeric_tolerance),
        sample_limit=args.sample_limit,
    )

    if args.json_output is not None:
        args.json_output.write_text(
            json.dumps(summary, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
    markdown = format_markdown(summary)
    if args.markdown_output is not None:
        args.markdown_output.write_text(markdown, encoding="utf-8")
    if args.json_output is None and args.markdown_output is None:
        print(markdown, end="")

    counts = summary["counts"]
    has_difference = (
        counts["live_only"] > 0
        or counts["replay_only"] > 0
        or counts["mismatched"] > 0
    )
    return 1 if args.fail_on_difference and has_difference else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
