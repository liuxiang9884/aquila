#!/home/liuxiang/dev/pyenv/lx/bin/python

import math
import sys
from dataclasses import dataclass
from decimal import Decimal, InvalidOperation
from pathlib import Path
from typing import Any, Callable, Iterable


BITGET_ACCOUNT_SCRIPT_DIR = Path(__file__).resolve().parents[1] / "account"
BITGET_TRADING_SCRIPT_DIR = Path(__file__).resolve().parent
for script_dir in (BITGET_ACCOUNT_SCRIPT_DIR, BITGET_TRADING_SCRIPT_DIR):
    if str(script_dir) not in sys.path:
        sys.path.insert(0, str(script_dir))

import query_bitget_account as account  # noqa: E402
from place_futures_order import ApiRequest, normalize_symbol  # noqa: E402


EXIT_OK = 0
EXIT_NOT_FLAT = 2
EXIT_SCOPE_REFUSED = 3
EXIT_REST_FAILED = 4

DEFAULT_POLL_TIMEOUT_SEC = 30.0
DEFAULT_POLL_INTERVAL_SEC = 0.5
DEFAULT_MAX_POSITION_COUNT = 8


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


def build_open_orders_request(category: str, symbol: str | None) -> ApiRequest:
    return ApiRequest(
        method="GET",
        endpoint_path="/api/v3/trade/unfilled-orders",
        query_string=account.build_query_string(
            [
                ("category", account.normalize_category(category)),
                ("symbol", normalize_symbol(symbol) if symbol else None),
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


def query_open_orders(
    requester: Requester,
    category: str,
    symbols: list[str] | None,
) -> list[OpenOrder]:
    if symbols is None:
        return parse_open_orders(
            requester(build_open_orders_request(category, None)), None
        )
    allowed_symbols = set(symbols)
    orders: list[OpenOrder] = []
    for symbol in symbols:
        orders.extend(
            parse_open_orders(
                requester(build_open_orders_request(category, symbol)),
                allowed_symbols,
            )
        )
    return orders


def query_positions(
    requester: Requester,
    category: str,
    symbols: list[str] | None,
) -> list[PositionSnapshot]:
    if symbols is None:
        return parse_positions(requester(build_positions_request(category, None)), None)
    allowed_symbols = set(symbols)
    positions: list[PositionSnapshot] = []
    for symbol in symbols:
        positions.extend(
            parse_positions(
                requester(build_positions_request(category, symbol)),
                allowed_symbols,
            )
        )
    return positions


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


def run_emergency_flatten(
    config: FlattenConfig,
    requester: Requester,
    clock: Any | None = None,
) -> tuple[int, dict[str, Any]]:
    del clock
    try:
        validate_config(config)
    except ScopeRefused as exc:
        summary = initial_summary(config)
        summary["result"] = "scope_refused"
        summary["errors"].append(str(exc))
        return EXIT_SCOPE_REFUSED, summary

    summary = initial_summary(config)
    try:
        symbols = normalize_symbols(config.symbols) if config.scope == "allowlist" else None
        open_orders = query_open_orders(requester, config.category, symbols)
        positions = query_positions(requester, config.category, symbols)
        if config.scope == "dedicated-account":
            non_flat_positions = [position for position in positions if not position.flat()]
            if len(non_flat_positions) > config.max_position_count:
                raise ScopeRefused(
                    "max-position-count exceeded: "
                    f"{len(non_flat_positions)} > {config.max_position_count}"
                )
            scoped_symbols = _discovered_symbols(positions, open_orders)
        else:
            scoped_symbols = symbols or []
        summary["scope"]["symbols"] = scoped_symbols
        summary["initial_open_orders"] = _open_order_summaries(open_orders)
        summary["initial_positions"] = _position_summaries(positions)
        summary["plan"]["open_orders_to_cancel"] = _open_order_summaries(
            open_orders
        )
        summary["plan"]["positions_to_close"] = plan_positions_to_close(
            positions
        )
        if not config.dry_run:
            raise ScopeRefused("mutating emergency flatten requires Task 3 implementation")
        summary["ok"] = True
        summary["result"] = "dry_run"
        return EXIT_OK, summary
    except ScopeRefused as exc:
        summary["result"] = "scope_refused"
        summary["errors"].append(str(exc))
        return EXIT_SCOPE_REFUSED, summary
    except (RestFailure, RuntimeError, ValueError) as exc:
        summary["result"] = "rest_failed"
        summary["errors"].append(str(exc))
        return EXIT_REST_FAILED, summary


if __name__ == "__main__":
    raise SystemExit("CLI is added with the mutating Task 3 workflow")
