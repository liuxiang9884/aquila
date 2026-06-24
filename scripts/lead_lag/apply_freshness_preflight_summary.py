#!/usr/bin/env python3

import argparse
import csv
import json
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence


PAIR_HEADER_RE = re.compile(r"^\s*\[\[lead_lag\.pairs\]\]\s*$")
SYMBOL_RE = re.compile(r'^(\s*symbol\s*=\s*)"([^"]+)"(\s*)$')
SYMBOL_ID_RE = re.compile(r"^(\s*symbol_id\s*=\s*)(\d+)(\s*)$")
EXCHANGE_RE = re.compile(r'^(\s*(lead|lag)_exchange\s*=\s*)"([^"]+)"(\s*)$')
LAG_FRESHNESS_RE = re.compile(r"^(\s*max_lag_freshness_ms\s*=\s*)\d+(\s*)$")


@dataclass(frozen=True)
class PairConfig:
    symbol: str
    symbol_id: int
    lead_exchange: str
    lag_exchange: str
    block_start: int
    block_end: int


@dataclass(frozen=True)
class PairFreshnessRow:
    symbol: str
    symbol_id: int
    lead_exchange: str
    lag_exchange: str
    lead_sample_count: int
    lag_sample_count: int
    lead_mean_ms: float
    lag_mean_ms: float
    lag_p50_ms: float
    lag_p95_ms: float
    generated_lag_freshness_ms: int


@dataclass(frozen=True)
class ApplyResult:
    updated_pair_count: int
    rows: list[PairFreshnessRow]


def normalize_exchange(exchange: str) -> str:
    normalized = exchange.strip().lower().replace("_", "").replace("-", "")
    if normalized.startswith("k"):
        normalized = normalized[1:]
    if normalized == "gateio":
        return "gate"
    return normalized


def lag_freshness_from_p50_ms(p50_ms: float) -> int:
    if p50_ms <= 20.0:
        return 20
    if p50_ms <= 50.0:
        return 50
    if p50_ms <= 100.0:
        return 100
    return 200


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


def load_summary(summary_json: Path) -> dict[tuple[int, str], dict]:
    data = json.loads(summary_json.read_text(encoding="utf-8"))
    groups = data.get("groups", [])
    if not isinstance(groups, list):
        raise ValueError("summary JSON must contain a groups array")
    result: dict[tuple[int, str], dict] = {}
    for group in groups:
        symbol_id = int(group["symbol_id"])
        exchange = normalize_exchange(str(group["exchange"]))
        result[(symbol_id, exchange)] = group
    return result


def build_rows(
    pairs: Sequence[PairConfig], summary: dict[tuple[int, str], dict]
) -> list[PairFreshnessRow]:
    rows: list[PairFreshnessRow] = []
    for pair in pairs:
        lead = summary.get((pair.symbol_id, pair.lead_exchange))
        lag = summary.get((pair.symbol_id, pair.lag_exchange))
        if lead is None or lag is None:
            raise ValueError(
                "missing freshness summary for "
                f"symbol={pair.symbol} symbol_id={pair.symbol_id} "
                f"lead_exchange={pair.lead_exchange} lag_exchange={pair.lag_exchange}"
            )
        lag_p50_ms = float(lag["p50_ms"])
        rows.append(
            PairFreshnessRow(
                symbol=pair.symbol,
                symbol_id=pair.symbol_id,
                lead_exchange=pair.lead_exchange,
                lag_exchange=pair.lag_exchange,
                lead_sample_count=int(lead["sample_count"]),
                lag_sample_count=int(lag["sample_count"]),
                lead_mean_ms=float(lead["mean_ms"]),
                lag_mean_ms=float(lag["mean_ms"]),
                lag_p50_ms=lag_p50_ms,
                lag_p95_ms=float(lag["p95_ms"]),
                generated_lag_freshness_ms=lag_freshness_from_p50_ms(lag_p50_ms),
            )
        )
    return rows


def render_updated_config(
    lines: Sequence[str], pairs: Sequence[PairConfig], rows: Sequence[PairFreshnessRow]
) -> str:
    updated = list(lines)
    for pair, row in reversed(list(zip(pairs, rows))):
        replacement = row.generated_lag_freshness_ms
        replaced = False
        for index in range(pair.block_start, pair.block_end):
            line = updated[index]
            if LAG_FRESHNESS_RE.match(line):
                updated[index] = LAG_FRESHNESS_RE.sub(
                    rf"\g<1>{replacement}\g<2>", line
                )
                replaced = True
                break
        if not replaced:
            insert_at = pair.block_start + 1
            updated.insert(insert_at, f"max_lag_freshness_ms = {replacement}\n")
    return "".join(updated)


def write_csv(path: Path, rows: Sequence[PairFreshnessRow]) -> None:
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(
            handle,
            fieldnames=[
                "symbol",
                "symbol_id",
                "lead_exchange",
                "lag_exchange",
                "lead_sample_count",
                "lag_sample_count",
                "lead_mean_ms",
                "lag_mean_ms",
                "lag_p50_ms",
                "lag_p95_ms",
                "generated_lag_freshness_ms",
            ],
        )
        writer.writeheader()
        for row in rows:
            writer.writerow(
                {
                    "symbol": row.symbol,
                    "symbol_id": row.symbol_id,
                    "lead_exchange": row.lead_exchange,
                    "lag_exchange": row.lag_exchange,
                    "lead_sample_count": row.lead_sample_count,
                    "lag_sample_count": row.lag_sample_count,
                    "lead_mean_ms": f"{row.lead_mean_ms:.6f}",
                    "lag_mean_ms": f"{row.lag_mean_ms:.6f}",
                    "lag_p50_ms": f"{row.lag_p50_ms:.6f}",
                    "lag_p95_ms": f"{row.lag_p95_ms:.6f}",
                    "generated_lag_freshness_ms": row.generated_lag_freshness_ms,
                }
            )


def apply_summary_to_config(
    *,
    summary_json: Path,
    config_in: Path,
    config_out: Path,
    csv_out: Path | None,
) -> ApplyResult:
    lines, pairs = parse_pairs(config_in.read_text(encoding="utf-8"))
    if not pairs:
        raise ValueError("input config has no [[lead_lag.pairs]] blocks")
    rows = build_rows(pairs, load_summary(summary_json))
    config_out.write_text(
        render_updated_config(lines, pairs, rows),
        encoding="utf-8",
    )
    if csv_out is not None:
        write_csv(csv_out, rows)
    return ApplyResult(updated_pair_count=len(rows), rows=rows)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Apply LeadLag preflight freshness summary to strategy TOML."
    )
    parser.add_argument("--summary-json", type=Path, required=True)
    parser.add_argument("--config-in", type=Path, required=True)
    parser.add_argument("--config-out", type=Path, required=True)
    parser.add_argument("--csv-out", type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    result = apply_summary_to_config(
        summary_json=args.summary_json,
        config_in=args.config_in,
        config_out=args.config_out,
        csv_out=args.csv_out,
    )
    print(
        "apply_freshness_preflight_summary "
        f"result=ok updated_pairs={result.updated_pair_count} "
        f"config_out={args.config_out}"
    )
    if args.csv_out is not None:
        print(f"csv_out={args.csv_out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
