#!/home/liuxiang/dev/pyenv/lx/bin/python

import argparse
import csv
import sys
from collections import defaultdict, deque
from dataclasses import dataclass
from decimal import Decimal, InvalidOperation, getcontext
from pathlib import Path
from typing import Iterable


getcontext().prec = 28

DEFAULT_OPEN_NOTIONAL = Decimal("1000")
DEFAULT_FEE_RATE = Decimal("0.0002")
DEFAULT_TICK_SIZE = Decimal("0")
DEFAULT_SLIPPAGE_TICKS = 0

OPEN_ACTIONS = {
    "kOpenLong": "long",
    "kOpenShort": "short",
}

CLOSE_ACTIONS = {
    "kCloseLong": "long",
    "kStoplossLong": "long",
    "kCloseShort": "short",
    "kStoplossShort": "short",
}


@dataclass(frozen=True)
class SignalRow:
    ticker_id: str
    symbol_id: str
    exchange_ns: str
    local_ns: str
    action: str
    side: str
    price: Decimal
    reduce_only: str


@dataclass(frozen=True)
class OpenPosition:
    ticker_id: str
    symbol_id: str
    exchange_ns: str
    local_ns: str
    direction: str
    action: str
    side: str
    price: Decimal
    quantity: Decimal
    open_notional: Decimal
    open_fee: Decimal


@dataclass(frozen=True)
class ClosedTrade:
    symbol_id: str
    direction: str
    open_ticker_id: str
    close_ticker_id: str
    open_exchange_ns: str
    close_exchange_ns: str
    open_local_ns: str
    close_local_ns: str
    open_action: str
    close_action: str
    open_side: str
    close_side: str
    open_price: Decimal
    close_price: Decimal
    quantity: Decimal
    open_notional: Decimal
    close_notional: Decimal
    gross_pnl: Decimal
    fees: Decimal
    net_pnl: Decimal


@dataclass(frozen=True)
class PnlSummary:
    signals: int
    open_signals: int
    close_signals: int
    closed_trades: int
    open_positions: int
    gross_pnl: Decimal
    fees: Decimal
    net_pnl: Decimal


@dataclass(frozen=True)
class PnlResult:
    summary: PnlSummary
    trades: list[ClosedTrade]


def parse_decimal(value: str, field: str) -> Decimal:
    try:
        parsed = Decimal(value)
    except InvalidOperation as exc:
        raise ValueError(f"{field} must be a decimal: {value!r}") from exc
    if parsed <= 0:
        raise ValueError(f"{field} must be positive: {value!r}")
    return parsed


def parse_non_negative_decimal(value: str, field: str) -> Decimal:
    try:
        parsed = Decimal(value)
    except InvalidOperation as exc:
        raise ValueError(f"{field} must be a decimal: {value!r}") from exc
    if parsed < 0:
        raise ValueError(f"{field} must be non-negative: {value!r}")
    return parsed


def parse_signal_row(raw: dict[str, str], row_number: int) -> SignalRow:
    required = [
        "ticker_id",
        "symbol_id",
        "exchange_ns",
        "local_ns",
        "action",
        "side",
        "price",
        "reduce_only",
    ]
    for field in required:
        if field not in raw or raw[field] == "":
            raise ValueError(f"row {row_number}: missing {field}")
    return SignalRow(
        ticker_id=raw["ticker_id"],
        symbol_id=raw["symbol_id"],
        exchange_ns=raw["exchange_ns"],
        local_ns=raw["local_ns"],
        action=raw["action"],
        side=raw["side"],
        price=parse_decimal(raw["price"], f"row {row_number} price"),
        reduce_only=raw["reduce_only"],
    )


def read_signal_csv(path: Path) -> list[SignalRow]:
    with path.open(newline="") as input_file:
        reader = csv.DictReader(input_file)
        return [parse_signal_row(row, index) for index, row in enumerate(reader, 2)]


def normalize_rows(rows: Iterable[dict[str, str] | SignalRow]) -> list[SignalRow]:
    normalized = []
    for index, row in enumerate(rows, 2):
        if isinstance(row, SignalRow):
            normalized.append(row)
        else:
            normalized.append(parse_signal_row(row, index))
    return normalized


def close_position(
    open_position: OpenPosition,
    close_signal: SignalRow,
    close_price: Decimal,
    fee_rate: Decimal,
) -> ClosedTrade:
    close_notional = open_position.quantity * close_price
    if open_position.direction == "long":
        gross_pnl = (close_price - open_position.price) * open_position.quantity
    else:
        gross_pnl = (open_position.price - close_price) * open_position.quantity
    close_fee = close_notional * fee_rate
    fees = open_position.open_fee + close_fee
    net_pnl = gross_pnl - fees
    return ClosedTrade(
        symbol_id=open_position.symbol_id,
        direction=open_position.direction,
        open_ticker_id=open_position.ticker_id,
        close_ticker_id=close_signal.ticker_id,
        open_exchange_ns=open_position.exchange_ns,
        close_exchange_ns=close_signal.exchange_ns,
        open_local_ns=open_position.local_ns,
        close_local_ns=close_signal.local_ns,
        open_action=open_position.action,
        close_action=close_signal.action,
        open_side=open_position.side,
        close_side=close_signal.side,
        open_price=open_position.price,
        close_price=close_price,
        quantity=open_position.quantity,
        open_notional=open_position.open_notional,
        close_notional=close_notional,
        gross_pnl=gross_pnl,
        fees=fees,
        net_pnl=net_pnl,
    )


def calculate_pnl(
    rows: Iterable[dict[str, str] | SignalRow],
    *,
    open_notional: Decimal,
    fee_rate: Decimal,
    tick_size: Decimal = DEFAULT_TICK_SIZE,
    slippage_ticks: int = DEFAULT_SLIPPAGE_TICKS,
) -> PnlResult:
    if open_notional <= 0:
        raise ValueError("open_notional must be positive")
    if fee_rate < 0:
        raise ValueError("fee_rate must be non-negative")
    if tick_size < 0:
        raise ValueError("tick_size must be non-negative")
    if slippage_ticks < 0:
        raise ValueError("slippage_ticks must be non-negative")

    signals = normalize_rows(rows)
    positions: dict[tuple[str, str], deque[OpenPosition]] = defaultdict(deque)
    trades: list[ClosedTrade] = []
    open_signal_count = 0
    close_signal_count = 0

    for signal in signals:
        if signal.action in OPEN_ACTIONS:
            direction = OPEN_ACTIONS[signal.action]
            open_price = slippage_price(signal, tick_size, slippage_ticks)
            positions[(signal.symbol_id, direction)].append(
                OpenPosition(
                    ticker_id=signal.ticker_id,
                    symbol_id=signal.symbol_id,
                    exchange_ns=signal.exchange_ns,
                    local_ns=signal.local_ns,
                    direction=direction,
                    action=signal.action,
                    side=signal.side,
                    price=open_price,
                    quantity=open_notional / open_price,
                    open_notional=open_notional,
                    open_fee=open_notional * fee_rate,
                )
            )
            open_signal_count += 1
            continue

        if signal.action in CLOSE_ACTIONS:
            direction = CLOSE_ACTIONS[signal.action]
            key = (signal.symbol_id, direction)
            if not positions[key]:
                raise ValueError(
                    f"close without open: ticker_id={signal.ticker_id} "
                    f"action={signal.action} symbol_id={signal.symbol_id}"
                )
            close_price = slippage_price(signal, tick_size, slippage_ticks)
            trades.append(
                close_position(positions[key].popleft(), signal, close_price, fee_rate)
            )
            close_signal_count += 1
            continue

        raise ValueError(f"unsupported action: {signal.action}")

    open_positions = sum(len(queue) for queue in positions.values())
    gross_pnl = sum((trade.gross_pnl for trade in trades), Decimal("0"))
    fees = sum((trade.fees for trade in trades), Decimal("0"))
    net_pnl = gross_pnl - fees
    return PnlResult(
        summary=PnlSummary(
            signals=len(signals),
            open_signals=open_signal_count,
            close_signals=close_signal_count,
            closed_trades=len(trades),
            open_positions=open_positions,
            gross_pnl=gross_pnl,
            fees=fees,
            net_pnl=net_pnl,
        ),
        trades=trades,
    )


def slippage_price(
    signal: SignalRow, tick_size: Decimal, slippage_ticks: int
) -> Decimal:
    slippage = tick_size * Decimal(slippage_ticks)
    if signal.side == "kBuy":
        price = signal.price + slippage
    elif signal.side == "kSell":
        price = signal.price - slippage
    else:
        raise ValueError(f"unsupported side: {signal.side}")
    if price <= 0:
        raise ValueError(
            f"slipped price must be positive: ticker_id={signal.ticker_id} "
            f"side={signal.side} price={signal.price} slippage={slippage}"
        )
    return price


def decimal_text(value: Decimal) -> str:
    return format(value.quantize(Decimal("0.00000001")), "f")


def write_trades_csv(path: Path, trades: list[ClosedTrade]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fields = [
        "symbol_id",
        "direction",
        "open_ticker_id",
        "close_ticker_id",
        "open_exchange_ns",
        "close_exchange_ns",
        "open_action",
        "close_action",
        "open_price",
        "close_price",
        "quantity",
        "open_notional",
        "close_notional",
        "gross_pnl",
        "fees",
        "net_pnl",
    ]
    with path.open("w", newline="") as output_file:
        writer = csv.DictWriter(output_file, fieldnames=fields)
        writer.writeheader()
        for trade in trades:
            writer.writerow(
                {
                    "symbol_id": trade.symbol_id,
                    "direction": trade.direction,
                    "open_ticker_id": trade.open_ticker_id,
                    "close_ticker_id": trade.close_ticker_id,
                    "open_exchange_ns": trade.open_exchange_ns,
                    "close_exchange_ns": trade.close_exchange_ns,
                    "open_action": trade.open_action,
                    "close_action": trade.close_action,
                    "open_price": decimal_text(trade.open_price),
                    "close_price": decimal_text(trade.close_price),
                    "quantity": decimal_text(trade.quantity),
                    "open_notional": decimal_text(trade.open_notional),
                    "close_notional": decimal_text(trade.close_notional),
                    "gross_pnl": decimal_text(trade.gross_pnl),
                    "fees": decimal_text(trade.fees),
                    "net_pnl": decimal_text(trade.net_pnl),
                }
            )


def print_summary(
    result: PnlResult,
    open_notional: Decimal,
    fee_rate: Decimal,
    tick_size: Decimal,
    slippage_ticks: int,
) -> None:
    summary = result.summary
    print(f"signals={summary.signals}")
    print(f"open_signals={summary.open_signals}")
    print(f"close_signals={summary.close_signals}")
    print(f"closed_trades={summary.closed_trades}")
    print(f"open_positions={summary.open_positions}")
    print(f"open_notional={decimal_text(open_notional)}")
    print(f"fee_rate={decimal_text(fee_rate)}")
    print(f"tick_size={decimal_text(tick_size)}")
    print(f"slippage_ticks={slippage_ticks}")
    print(f"gross_pnl={decimal_text(summary.gross_pnl)}")
    print(f"fees={decimal_text(summary.fees)}")
    print(f"net_pnl={decimal_text(summary.net_pnl)}")


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Calculate gross and net PnL from lead_lag_replay signal CSV. "
            "Each open signal uses a fixed USDT notional."
        )
    )
    parser.add_argument("signals_csv", type=Path, help="Signal CSV from lead_lag_replay.")
    parser.add_argument(
        "--open-notional",
        default=str(DEFAULT_OPEN_NOTIONAL),
        help=f"USDT notional per open signal. Default: {DEFAULT_OPEN_NOTIONAL}",
    )
    parser.add_argument(
        "--fee-rate",
        default=str(DEFAULT_FEE_RATE),
        help=f"Fee rate per fill, charged on notional. Default: {DEFAULT_FEE_RATE}",
    )
    parser.add_argument(
        "--tick-size",
        default=str(DEFAULT_TICK_SIZE),
        help=(
            "Price tick size used for slippage. Buy fills add ticks, "
            "sell fills subtract ticks. Default: 0"
        ),
    )
    parser.add_argument(
        "--slippage-ticks",
        type=int,
        default=DEFAULT_SLIPPAGE_TICKS,
        help="Adverse slippage ticks per fill. Default: 0",
    )
    parser.add_argument(
        "--trades-output",
        type=Path,
        help="Optional output CSV path for per-trade PnL rows.",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        open_notional = parse_decimal(args.open_notional, "open-notional")
        fee_rate = parse_non_negative_decimal(args.fee_rate, "fee-rate")
        tick_size = parse_non_negative_decimal(args.tick_size, "tick-size")
        signals = read_signal_csv(args.signals_csv)
        result = calculate_pnl(
            signals,
            open_notional=open_notional,
            fee_rate=fee_rate,
            tick_size=tick_size,
            slippage_ticks=args.slippage_ticks,
        )
        if args.trades_output is not None:
            write_trades_csv(args.trades_output, result.trades)
        print_summary(
            result,
            open_notional,
            fee_rate,
            tick_size,
            args.slippage_ticks,
        )
    except (OSError, ValueError) as exc:
        print(str(exc), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
