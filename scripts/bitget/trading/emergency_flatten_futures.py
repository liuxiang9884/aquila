#!/home/liuxiang/dev/pyenv/lx/bin/python

import argparse
import json
import math
import sys
import time
from dataclasses import dataclass
from decimal import Decimal, InvalidOperation
from pathlib import Path
from typing import Any, Callable, Iterable


if __package__:
    from bitget.account import query_bitget_account as account  # noqa: E402
    from bitget.trading.place_futures_order import (  # noqa: E402
        ApiRequest,
        SignedBitgetTradingClient,
        build_place_order_request,
        normalize_symbol,
    )
else:
    BITGET_ACCOUNT_SCRIPT_DIR = Path(__file__).resolve().parents[1] / "account"
    BITGET_TRADING_SCRIPT_DIR = Path(__file__).resolve().parent
    for script_dir in (BITGET_ACCOUNT_SCRIPT_DIR, BITGET_TRADING_SCRIPT_DIR):
        if str(script_dir) not in sys.path:
            sys.path.insert(0, str(script_dir))
    import query_bitget_account as account  # noqa: E402
    from place_futures_order import (  # noqa: E402
        ApiRequest,
        SignedBitgetTradingClient,
        build_place_order_request,
        normalize_symbol,
    )


EXIT_OK = 0
EXIT_NOT_FLAT = 2
EXIT_SCOPE_REFUSED = 3
EXIT_REST_FAILED = 4

DEFAULT_POLL_TIMEOUT_SEC = 30.0
DEFAULT_POLL_INTERVAL_SEC = 0.5
DEFAULT_MAX_POSITION_COUNT = 8
OPEN_ORDER_PAGE_SIZE = 100
MAX_OPEN_ORDER_PAGES = 5
CANCEL_MIN_INTERVAL_SEC = 0.2
CLOSE_MIN_INTERVAL_SEC = 0.1


class ScopeRefused(ValueError):
    pass


class RestFailure(RuntimeError):
    pass


@dataclass(frozen=True)
class FlattenConfig:
    category: str
    scope: str
    symbols: list[str]
    confirm_dedicated_account: bool
    dry_run: bool
    poll_timeout_sec: float = DEFAULT_POLL_TIMEOUT_SEC
    poll_interval_sec: float = DEFAULT_POLL_INTERVAL_SEC
    max_position_count: int = DEFAULT_MAX_POSITION_COUNT


@dataclass
class RequestPacer:
    min_interval_sec: float
    last_request_at: float | None = None

    def wait(self, clock: Any) -> None:
        now = clock.time()
        if self.last_request_at is not None:
            remaining = self.min_interval_sec - (now - self.last_request_at)
            if remaining > 0:
                clock.sleep(remaining)
                now = clock.time()
        self.last_request_at = now


@dataclass(frozen=True)
class PositionSnapshot:
    symbol: str
    pos_side: str
    total: Decimal
    available: Decimal
    frozen: Decimal
    margin_mode: str

    def flat(self) -> bool:
        return self.total == 0 and self.available == 0 and self.frozen == 0

    def to_summary(self) -> dict[str, str]:
        return {
            "symbol": self.symbol,
            "pos_side": self.pos_side,
            "total": decimal_summary_text(self.total),
            "available": decimal_summary_text(self.available),
            "frozen": decimal_summary_text(self.frozen),
            "margin_mode": self.margin_mode,
        }


@dataclass(frozen=True)
class OpenOrder:
    symbol: str
    order_id: str
    client_oid: str

    def to_summary(self) -> dict[str, str]:
        return {
            "symbol": self.symbol,
            "order_id": self.order_id,
            "client_oid": self.client_oid,
        }


Requester = Callable[[ApiRequest], Any]


class SystemClock:
    def time(self) -> float:
        return time.time()

    def sleep(self, seconds: float) -> None:
        time.sleep(seconds)


def decimal_summary_text(value: Decimal) -> str:
    return format(value, "f")


def normalize_symbols(symbols: Iterable[str]) -> list[str]:
    normalized: list[str] = []
    seen: set[str] = set()
    for symbol in symbols:
        value = normalize_symbol(symbol)
        if value in seen:
            continue
        normalized.append(value)
        seen.add(value)
    return normalized


def validate_config(config: FlattenConfig) -> None:
    account.normalize_category(config.category)
    if config.scope not in {"allowlist", "dedicated-account"}:
        raise ScopeRefused("scope must be allowlist or dedicated-account")
    if not math.isfinite(config.poll_timeout_sec) or config.poll_timeout_sec < 0:
        raise ScopeRefused("poll-timeout-sec must be finite and non-negative")
    if not math.isfinite(config.poll_interval_sec) or config.poll_interval_sec <= 0:
        raise ScopeRefused("poll-interval-sec must be finite and positive")
    if config.max_position_count <= 0:
        raise ScopeRefused("max-position-count must be positive")
    if config.scope == "allowlist" and not normalize_symbols(config.symbols):
        raise ScopeRefused("allowlist scope requires at least one symbol")
    if config.scope == "dedicated-account" and not config.confirm_dedicated_account:
        raise ScopeRefused(
            "dedicated-account scope requires --confirm-dedicated-account"
        )


def _response_list(data: Any, label: str) -> list[Any]:
    if not isinstance(data, dict):
        raise RestFailure(f"{label} response must be an object")
    values = data.get("list")
    if not isinstance(values, list):
        raise RestFailure(f"{label} response missing list")
    return values


def _response_page(data: Any, label: str) -> tuple[list[Any], str]:
    values = _response_list(data, label)
    cursor = data.get("cursor", "")
    if cursor is None:
        cursor = ""
    if not isinstance(cursor, str):
        raise RestFailure(f"{label} response cursor must be a string")
    return values, cursor.strip()


def _response_symbol(
    item: dict[str, Any],
    label: str,
    allowed_symbols: set[str] | None,
) -> str:
    raw_symbol = item.get("symbol")
    if not isinstance(raw_symbol, str) or not raw_symbol.strip():
        raise RestFailure(f"{label} missing symbol")
    symbol = normalize_symbol(raw_symbol)
    if allowed_symbols is not None and symbol not in allowed_symbols:
        raise RestFailure(f"{label} symbol {symbol} is outside allowlist")
    return symbol


def _decimal_field(item: dict[str, Any], field: str, label: str) -> Decimal:
    raw_value = item.get(field)
    if raw_value is None or str(raw_value).strip() == "":
        raise RestFailure(f"{label} missing {field}")
    try:
        value = Decimal(str(raw_value))
    except (InvalidOperation, ValueError) as exc:
        raise RestFailure(f"{label}.{field} must be a finite decimal") from exc
    if not value.is_finite():
        raise RestFailure(f"{label}.{field} must be a finite decimal")
    if value < 0:
        raise RestFailure(f"{label}.{field} must be non-negative")
    return value


def parse_open_orders(
    data: Any,
    allowed_symbols: set[str] | None,
) -> list[OpenOrder]:
    orders: list[OpenOrder] = []
    for index, value in enumerate(_response_list(data, "open orders")):
        if not isinstance(value, dict):
            raise RestFailure(f"open orders[{index}] must be an object")
        label = f"open orders[{index}]"
        symbol = _response_symbol(value, label, allowed_symbols)
        order_id = str(value.get("orderId") or "").strip()
        client_oid = str(value.get("clientOid") or "").strip()
        if not order_id and not client_oid:
            raise RestFailure(f"{label} missing order identity")
        orders.append(
            OpenOrder(
                symbol=symbol,
                order_id=order_id,
                client_oid=client_oid,
            )
        )
    return orders


def parse_positions(
    data: Any,
    allowed_symbols: set[str] | None,
) -> list[PositionSnapshot]:
    positions: list[PositionSnapshot] = []
    for index, value in enumerate(_response_list(data, "positions")):
        if not isinstance(value, dict):
            raise RestFailure(f"positions[{index}] must be an object")
        label = f"positions[{index}]"
        symbol = _response_symbol(value, label, allowed_symbols)
        pos_side = str(value.get("posSide") or "").strip().lower()
        if pos_side not in {"long", "short"}:
            raise RestFailure(f"{label}.posSide must be long or short")
        hold_mode = str(value.get("holdMode") or "").strip().lower()
        if hold_mode != "one_way_mode":
            raise RestFailure(f"{label}.holdMode must be one_way_mode")
        margin_mode = str(value.get("marginMode") or "").strip().lower()
        if margin_mode not in {"crossed", "isolated"}:
            raise RestFailure(f"{label}.marginMode must be crossed or isolated")
        positions.append(
            PositionSnapshot(
                symbol=symbol,
                pos_side=pos_side,
                total=_decimal_field(value, "total", label),
                available=_decimal_field(value, "available", label),
                frozen=_decimal_field(value, "frozen", label),
                margin_mode=margin_mode,
            )
        )
    return positions


def final_state_is_flat(
    positions: list[PositionSnapshot],
    open_orders: list[OpenOrder],
) -> bool:
    return not open_orders and all(position.flat() for position in positions)


def build_open_orders_request(
    category: str,
    symbol: str | None,
    cursor: str | None = None,
) -> ApiRequest:
    return ApiRequest(
        method="GET",
        endpoint_path="/api/v3/trade/unfilled-orders",
        query_string=account.build_query_string(
            [
                ("category", account.normalize_category(category)),
                ("symbol", normalize_symbol(symbol) if symbol else None),
                ("limit", str(OPEN_ORDER_PAGE_SIZE)),
                ("cursor", cursor),
            ]
        ),
    )


def build_positions_request(category: str, symbol: str | None) -> ApiRequest:
    return ApiRequest(
        method="GET",
        endpoint_path="/api/v3/position/current-position",
        query_string=account.build_query_string(
            [
                ("category", account.normalize_category(category)),
                ("symbol", normalize_symbol(symbol) if symbol else None),
            ]
        ),
    )


def build_cancel_symbol_orders_request(
    category: str,
    symbol: str | None,
) -> ApiRequest:
    payload = {"category": account.normalize_category(category)}
    if symbol is not None:
        payload["symbol"] = normalize_symbol(symbol)
    return ApiRequest(
        method="POST",
        endpoint_path="/api/v3/trade/cancel-symbol-order",
        body=json.dumps(payload, ensure_ascii=False, separators=(",", ":")),
    )


def query_open_orders(
    requester: Requester,
    category: str,
    symbols: list[str] | None,
) -> list[OpenOrder]:
    allowed_symbols = set(symbols) if symbols is not None else None
    orders: list[OpenOrder] = []
    cursor: str | None = None
    seen_cursors: set[str] = set()
    for _ in range(MAX_OPEN_ORDER_PAGES):
        data = requester(build_open_orders_request(category, None, cursor))
        values, next_cursor = _response_page(data, "open orders")
        page_orders = parse_open_orders({"list": values}, None)
        if allowed_symbols is not None:
            page_orders = [
                order for order in page_orders if order.symbol in allowed_symbols
            ]
        orders.extend(page_orders)
        if len(values) < OPEN_ORDER_PAGE_SIZE:
            return orders
        if not next_cursor:
            raise RestFailure("open orders full page missing cursor")
        if next_cursor in seen_cursors:
            raise RestFailure("open orders cursor repeated")
        seen_cursors.add(next_cursor)
        cursor = next_cursor
    raise RestFailure(
        f"open orders exceeded {MAX_OPEN_ORDER_PAGES} pagination requests"
    )


def query_positions(
    requester: Requester,
    category: str,
    symbols: list[str] | None,
) -> list[PositionSnapshot]:
    positions = parse_positions(
        requester(build_positions_request(category, None)),
        None,
    )
    if symbols is None:
        return positions
    allowed_symbols = set(symbols)
    return [position for position in positions if position.symbol in allowed_symbols]


def query_flat_snapshot(
    requester: Requester,
    category: str,
    symbols: list[str] | None,
) -> tuple[list[PositionSnapshot], list[OpenOrder]]:
    open_orders_before = query_open_orders(requester, category, symbols)
    positions = query_positions(requester, category, symbols)
    if open_orders_before or not all(position.flat() for position in positions):
        return positions, open_orders_before
    open_orders_after = query_open_orders(requester, category, symbols)
    return positions, open_orders_after


def _open_order_summaries(open_orders: Iterable[OpenOrder]) -> list[dict[str, str]]:
    return [order.to_summary() for order in open_orders]


def _position_summaries(
    positions: Iterable[PositionSnapshot],
) -> list[dict[str, str]]:
    return [position.to_summary() for position in positions]


def plan_positions_to_close(
    positions: Iterable[PositionSnapshot],
) -> list[dict[str, str]]:
    plan: list[dict[str, str]] = []
    for position in positions:
        if position.flat():
            continue
        plan.append(
            {
                "symbol": position.symbol,
                "pos_side": position.pos_side,
                "side": "sell" if position.pos_side == "long" else "buy",
                "qty": decimal_summary_text(position.total),
                "margin_mode": position.margin_mode,
                "order_type": "market",
                "reduce_only": "yes",
            }
        )
    return plan


def initial_summary(config: FlattenConfig) -> dict[str, Any]:
    return {
        "ok": False,
        "result": "not_started",
        "category": account.normalize_category(config.category),
        "dry_run": config.dry_run,
        "scope": {
            "mode": config.scope,
            "requested_symbols": list(config.symbols),
            "symbols": [],
            "confirm_dedicated_account": config.confirm_dedicated_account,
        },
        "plan": {
            "open_orders_to_cancel": [],
            "positions_to_close": [],
        },
        "initial_open_orders": [],
        "initial_positions": [],
        "post_cancel_positions": [],
        "orders_cancelled": [],
        "close_orders_submitted": [],
        "post_close_open_orders": [],
        "final_open_orders": [],
        "final_positions": [],
        "polls": 0,
        "errors": [],
    }


def _discovered_symbols(
    positions: Iterable[PositionSnapshot],
    open_orders: Iterable[OpenOrder],
) -> list[str]:
    symbols: list[str] = []
    seen: set[str] = set()
    for symbol in [position.symbol for position in positions] + [
        order.symbol for order in open_orders
    ]:
        if symbol in seen:
            continue
        symbols.append(symbol)
        seen.add(symbol)
    return symbols


def enforce_position_count_limit(
    positions: Iterable[PositionSnapshot],
    max_position_count: int,
) -> None:
    non_flat_position_count = sum(
        1 for position in positions if not position.flat()
    )
    if non_flat_position_count > max_position_count:
        raise ScopeRefused(
            "max-position-count exceeded: "
            f"{non_flat_position_count} > {max_position_count}"
        )


def validate_cancel_symbol_orders_response(data: Any) -> None:
    values = _response_list(data, "cancel symbol orders")
    for index, value in enumerate(values):
        if not isinstance(value, dict):
            raise RestFailure(f"cancel symbol orders[{index}] must be an object")
        code = value.get("code")
        if code != "00000":
            raise RestFailure(
                f"cancel symbol orders[{index}] code={code} msg={value.get('msg')}"
            )


def cancel_scoped_open_orders(
    requester: Requester,
    category: str,
    symbols: list[str],
    dedicated_account: bool,
    phase: str,
    summary: dict[str, Any],
    clock: Any,
    pacer: RequestPacer,
) -> None:
    targets: list[str | None] = [None] if dedicated_account else list(symbols)
    for symbol in targets:
        pacer.wait(clock)
        request = build_cancel_symbol_orders_request(category, symbol)
        response = requester(request)
        summary["orders_cancelled"].append(
            {
                "phase": phase,
                "scope": "category" if symbol is None else "symbol",
                "symbol": symbol or "",
                "request": request.to_public_dict(),
                "response": response,
            }
        )
        validate_cancel_symbol_orders_response(response)


def build_close_client_oid(clock: Any, sequence: int) -> str:
    epoch_ms = int(clock.time() * 1000)
    client_oid = f"a-flat-{epoch_ms}-{sequence}"
    if len(client_oid) > 32:
        raise ValueError("emergency close clientOid exceeds 32 characters")
    return client_oid


def submit_reduce_only_close_orders(
    requester: Requester,
    category: str,
    positions: list[PositionSnapshot],
    clock: Any,
    summary: dict[str, Any],
    pacer: RequestPacer | None = None,
) -> None:
    pacer = RequestPacer(CLOSE_MIN_INTERVAL_SEC) if pacer is None else pacer
    sequence = 0
    for position in positions:
        if position.total == 0:
            continue
        side = "sell" if position.pos_side == "long" else "buy"
        request = build_place_order_request(
            category=category,
            symbol=position.symbol,
            qty=position.total,
            side=side,
            margin_mode=position.margin_mode,
            client_oid=build_close_client_oid(clock, sequence),
            reduce_only=True,
        )
        sequence += 1
        pacer.wait(clock)
        response = requester(request)
        summary["close_orders_submitted"].append(
            {
                "symbol": position.symbol,
                "pos_side": position.pos_side,
                "side": side,
                "qty": decimal_summary_text(position.total),
                "request": request.to_public_dict(),
                "response": response,
            }
        )


def poll_until_flat(
    requester: Requester,
    category: str,
    symbols: list[str] | None,
    timeout_sec: float,
    interval_sec: float,
    clock: Any,
) -> tuple[bool, int, list[PositionSnapshot], list[OpenOrder]]:
    deadline = clock.time() + timeout_sec
    polls = 0
    while True:
        polls += 1
        positions, open_orders = query_flat_snapshot(
            requester,
            category,
            symbols,
        )
        if final_state_is_flat(positions, open_orders):
            return True, polls, positions, open_orders
        now = clock.time()
        if now >= deadline:
            return False, polls, positions, open_orders
        clock.sleep(min(interval_sec, max(0.0, deadline - now)))


def finish_summary(
    summary: dict[str, Any],
    verified: bool,
    polls: int,
    final_positions: list[PositionSnapshot],
    final_open_orders: list[OpenOrder],
) -> tuple[int, dict[str, Any]]:
    summary["polls"] = polls
    summary["final_positions"] = _position_summaries(final_positions)
    summary["final_open_orders"] = _open_order_summaries(final_open_orders)
    if verified:
        summary["ok"] = True
        summary["result"] = "verified_flat"
        return EXIT_OK, summary
    summary["ok"] = False
    summary["result"] = "not_flat"
    summary["errors"].append(
        "timeout or final verification still has positions or open orders"
    )
    return EXIT_NOT_FLAT, summary


def verify_after_mutation_error(
    config: FlattenConfig,
    requester: Requester,
    symbols: list[str] | None,
    summary: dict[str, Any],
    error: Exception,
) -> tuple[int, dict[str, Any]]:
    summary["errors"].append(str(error))
    try:
        final_positions, final_open_orders = query_flat_snapshot(
            requester,
            config.category,
            symbols,
        )
    except Exception as verify_error:
        summary["errors"].append(f"final REST verification failed: {verify_error}")
        summary["result"] = "rest_failed"
        return EXIT_REST_FAILED, summary
    summary["final_positions"] = _position_summaries(final_positions)
    summary["final_open_orders"] = _open_order_summaries(final_open_orders)
    if final_state_is_flat(final_positions, final_open_orders):
        summary["ok"] = True
        summary["result"] = "verified_flat_after_unknown"
        return EXIT_OK, summary
    summary["result"] = "rest_failed"
    return EXIT_REST_FAILED, summary


def run_emergency_flatten(
    config: FlattenConfig,
    requester: Requester,
    clock: Any | None = None,
) -> tuple[int, dict[str, Any]]:
    clock = SystemClock() if clock is None else clock
    try:
        validate_config(config)
    except ScopeRefused as exc:
        summary = initial_summary(config)
        summary["result"] = "scope_refused"
        summary["errors"].append(str(exc))
        return EXIT_SCOPE_REFUSED, summary

    summary = initial_summary(config)
    try:
        symbols = (
            normalize_symbols(config.symbols) if config.scope == "allowlist" else None
        )
        open_orders = query_open_orders(requester, config.category, symbols)
        positions = query_positions(requester, config.category, symbols)
        if config.scope == "dedicated-account":
            enforce_position_count_limit(positions, config.max_position_count)
            scoped_symbols = _discovered_symbols(positions, open_orders)
        else:
            scoped_symbols = symbols or []
        summary["scope"]["symbols"] = scoped_symbols
        summary["initial_open_orders"] = _open_order_summaries(open_orders)
        if positions:
            summary["initial_positions"] = _position_summaries(positions)
        summary["plan"]["open_orders_to_cancel"] = _open_order_summaries(
            open_orders
        )
        if positions:
            summary["plan"]["positions_to_close"] = plan_positions_to_close(
                positions
            )
        if config.dry_run:
            summary["ok"] = True
            summary["result"] = "dry_run"
            return EXIT_OK, summary

        cancel_pacer = RequestPacer(CANCEL_MIN_INTERVAL_SEC)
        close_pacer = RequestPacer(CLOSE_MIN_INTERVAL_SEC)
        try:
            cancel_scoped_open_orders(
                requester=requester,
                category=config.category,
                symbols=scoped_symbols,
                dedicated_account=config.scope == "dedicated-account",
                phase="before_close",
                summary=summary,
                clock=clock,
                pacer=cancel_pacer,
            )
            positions = query_positions(requester, config.category, symbols)
            if config.scope == "dedicated-account":
                enforce_position_count_limit(positions, config.max_position_count)
            summary["post_cancel_positions"] = _position_summaries(positions)
            positions_to_close = [
                position for position in positions if not position.flat()
            ]
            summary["plan"]["positions_to_close"] = plan_positions_to_close(
                positions_to_close
            )
            submit_reduce_only_close_orders(
                requester,
                config.category,
                positions_to_close,
                clock,
                summary,
                close_pacer,
            )
            post_close_open_orders = query_open_orders(
                requester, config.category, symbols
            )
            summary["post_close_open_orders"] = _open_order_summaries(
                post_close_open_orders
            )
            cancel_scoped_open_orders(
                requester=requester,
                category=config.category,
                symbols=scoped_symbols,
                dedicated_account=config.scope == "dedicated-account",
                phase="after_close",
                summary=summary,
                clock=clock,
                pacer=cancel_pacer,
            )
        except ScopeRefused:
            raise
        except Exception as mutation_error:
            return verify_after_mutation_error(
                config, requester, symbols, summary, mutation_error
            )

        verified, polls, final_positions, final_open_orders = poll_until_flat(
            requester=requester,
            category=config.category,
            symbols=symbols,
            timeout_sec=config.poll_timeout_sec,
            interval_sec=config.poll_interval_sec,
            clock=clock,
        )
        return finish_summary(
            summary,
            verified,
            polls,
            final_positions,
            final_open_orders,
        )
    except ScopeRefused as exc:
        summary["result"] = "scope_refused"
        summary["errors"].append(str(exc))
        return EXIT_SCOPE_REFUSED, summary
    except (RestFailure, RuntimeError, ValueError) as exc:
        summary["result"] = "rest_failed"
        summary["errors"].append(str(exc))
        return EXIT_REST_FAILED, summary


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Cancel Bitget UTA futures orders and flatten positions."
    )
    parser.add_argument("--api-key", default=account.DEFAULT_API_KEY_ENV)
    parser.add_argument("--api-secret", default=account.DEFAULT_API_SECRET_ENV)
    parser.add_argument(
        "--api-passphrase", default=account.DEFAULT_API_PASSPHRASE_ENV
    )
    parser.add_argument("--base-url", default=account.DEFAULT_BASE_URL)
    parser.add_argument("--timeout", type=float, default=account.DEFAULT_TIMEOUT)
    parser.add_argument("--category", default=account.DEFAULT_CATEGORY)
    parser.add_argument(
        "--scope", choices=("allowlist", "dedicated-account"), default="allowlist"
    )
    parser.add_argument("--symbol", action="append", default=[])
    parser.add_argument("--confirm-dedicated-account", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument(
        "--poll-timeout-sec", type=float, default=DEFAULT_POLL_TIMEOUT_SEC
    )
    parser.add_argument(
        "--poll-interval-sec", type=float, default=DEFAULT_POLL_INTERVAL_SEC
    )
    parser.add_argument(
        "--max-position-count", type=int, default=DEFAULT_MAX_POSITION_COUNT
    )
    parser.add_argument(
        "--pretty", action=argparse.BooleanOptionalAction, default=True
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    config = FlattenConfig(
        category=args.category,
        scope=args.scope,
        symbols=args.symbol,
        confirm_dedicated_account=args.confirm_dedicated_account,
        dry_run=args.dry_run,
        poll_timeout_sec=args.poll_timeout_sec,
        poll_interval_sec=args.poll_interval_sec,
        max_position_count=args.max_position_count,
    )
    credential_names = (args.api_key, args.api_secret, args.api_passphrase)
    credential_values = [account.get_env_value(name) for name in credential_names]
    for name, value in zip(credential_names, credential_values):
        if value is None:
            print(f"[FAIL] missing env var {name}", file=sys.stderr)
            return EXIT_SCOPE_REFUSED
    client = SignedBitgetTradingClient(
        api_key=credential_values[0],
        api_secret=credential_values[1],
        api_passphrase=credential_values[2],
        base_url=args.base_url,
        timeout=args.timeout,
    )
    exit_code, summary = run_emergency_flatten(config, client.request_json)
    print(
        json.dumps(
            summary,
            ensure_ascii=False,
            indent=2 if args.pretty else None,
            sort_keys=True,
        )
    )
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
