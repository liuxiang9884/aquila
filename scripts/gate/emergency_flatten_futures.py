#!/home/liuxiang/dev/pyenv/lx/bin/python

import argparse
import json
import sys
import time
from dataclasses import dataclass
from decimal import Decimal, InvalidOperation
from typing import Any, Callable, Iterable

import query_gate_account as account
from place_futures_order import (
    ApiRequest,
    SignedGateTradingClient,
    build_cancel_order_request,
    build_place_order_request,
)


EXIT_OK = 0
EXIT_NOT_FLAT = 2
EXIT_SCOPE_REFUSED = 3
EXIT_REST_FAILED = 4

DEFAULT_POLL_TIMEOUT_SEC = 30.0
DEFAULT_POLL_INTERVAL_SEC = 1.0
DEFAULT_MAX_POSITION_COUNT = 8

Requester = Callable[[ApiRequest], Any]


class ScopeRefused(RuntimeError):
    pass


class RestFailure(RuntimeError):
    pass


class SystemClock:
    def time(self) -> float:
        return time.time()

    def sleep(self, seconds: float) -> None:
        time.sleep(seconds)


@dataclass(frozen=True)
class FlattenConfig:
    settle: str
    scope: str
    contracts: list[str]
    confirm_dedicated_account: bool
    dry_run: bool
    poll_timeout_sec: float
    poll_interval_sec: float
    max_position_count: int


@dataclass(frozen=True)
class PositionSnapshot:
    contract: str
    size: int
    pending_orders: int = 0

    def to_summary(self) -> dict[str, Any]:
        return {
            "contract": self.contract,
            "size": self.size,
            "pending_orders": self.pending_orders,
        }


@dataclass(frozen=True)
class OpenOrder:
    contract: str
    order_id: str

    def to_summary(self) -> dict[str, str]:
        return {"contract": self.contract, "order_id": self.order_id}


def normalize_settle(settle: str) -> str:
    value = account.normalize_settle_path(settle)
    if not value:
        raise ScopeRefused("settle must not be empty")
    return value


def normalize_contracts(contracts: Iterable[str]) -> list[str]:
    normalized: list[str] = []
    seen: set[str] = set()
    for contract in contracts:
        try:
            value = account.normalize_contract(contract)
        except ValueError as exc:
            raise ScopeRefused(str(exc)) from exc
        if value in seen:
            continue
        normalized.append(value)
        seen.add(value)
    return normalized


def normalize_rest_contract(value: Any) -> str:
    try:
        return account.normalize_contract(str(value))
    except ValueError as exc:
        raise RestFailure(str(exc)) from exc


def validate_config(config: FlattenConfig) -> None:
    normalize_settle(config.settle)
    if config.scope not in {"dedicated-account", "allowlist"}:
        raise ScopeRefused("scope must be dedicated-account or allowlist")
    if config.poll_timeout_sec < 0:
        raise ScopeRefused("poll-timeout-sec must be non-negative")
    if config.poll_interval_sec <= 0:
        raise ScopeRefused("poll-interval-sec must be positive")
    if config.max_position_count <= 0:
        raise ScopeRefused("max-position-count must be positive")
    if config.scope == "allowlist" and not normalize_contracts(config.contracts):
        raise ScopeRefused("allowlist scope requires one or more --contract values")
    if config.scope == "dedicated-account" and not config.confirm_dedicated_account:
        raise ScopeRefused("dedicated-account scope requires --confirm-dedicated-account")


def initial_summary(config: FlattenConfig) -> dict[str, Any]:
    return {
        "ok": False,
        "result": "not_started",
        "settle": account.normalize_settle_path(config.settle),
        "dry_run": config.dry_run,
        "scope": {
            "mode": config.scope,
            "requested_contracts": list(config.contracts),
            "contracts": [],
            "confirm_dedicated_account": config.confirm_dedicated_account,
            "limitations": [],
        },
        "plan": {
            "contracts": [],
            "open_orders_to_cancel": [],
            "positions_to_close": [],
        },
        "initial_open_orders": [],
        "initial_positions": [],
        "orders_cancelled": [],
        "close_orders_submitted": [],
        "post_close_open_orders": [],
        "final_open_orders": [],
        "final_positions": [],
        "polls": 0,
        "errors": [],
    }


def summarize_failure(
    config: FlattenConfig,
    result: str,
    error: Exception,
    exit_code: int,
) -> tuple[int, dict[str, Any]]:
    summary = initial_summary(config)
    summary["result"] = result
    summary["exit_code"] = exit_code
    summary["errors"].append(str(error))
    return exit_code, summary


def _request(requester: Requester, api_request: ApiRequest) -> Any:
    try:
        return requester(api_request)
    except Exception as exc:
        raise RestFailure(
            f"{api_request.method} {api_request.endpoint_path} failed: {type(exc).__name__}: {exc}"
        ) from exc


def _to_integral_size(value: Any, label: str) -> int:
    if isinstance(value, bool) or value is None:
        raise RestFailure(f"invalid {label}: expected integer size")
    try:
        parsed = Decimal(str(value).strip())
    except (InvalidOperation, ValueError) as exc:
        raise RestFailure(f"invalid {label}: {value}") from exc
    if not parsed.is_finite() or parsed != parsed.to_integral_value():
        raise RestFailure(f"invalid {label}: expected integer size, got {value}")
    return int(parsed)


def _pending_orders(value: Any) -> int:
    if value is None:
        return 0
    return _to_integral_size(value, "pending_orders")


def _position_contract(position: dict[str, Any], fallback_contract: str | None) -> str:
    raw_contract = position.get("contract", fallback_contract)
    if raw_contract is None:
        raise RestFailure("invalid position response: missing contract")
    return normalize_rest_contract(raw_contract)


def parse_position(value: Any, fallback_contract: str | None = None) -> PositionSnapshot:
    if isinstance(value, list):
        if fallback_contract is None:
            raise RestFailure("position response list requires fallback contract")
        normalized = normalize_rest_contract(fallback_contract)
        for item in value:
            if isinstance(item, dict) and str(item.get("contract", "")).upper() == normalized:
                return parse_position(item, fallback_contract=normalized)
        raise RestFailure(f"position response missing contract {normalized}")
    if not isinstance(value, dict):
        raise RestFailure(f"invalid position response: expected object, got {type(value).__name__}")
    contract = _position_contract(value, fallback_contract)
    return PositionSnapshot(
        contract=contract,
        size=_to_integral_size(value.get("size"), f"{contract}.size"),
        pending_orders=_pending_orders(value.get("pending_orders")),
    )


def parse_position_list(value: Any) -> list[PositionSnapshot]:
    if not isinstance(value, list):
        raise RestFailure("invalid positions response: expected list")
    positions = []
    for index, item in enumerate(value):
        if not isinstance(item, dict):
            raise RestFailure(f"invalid positions[{index}]: expected object")
        positions.append(parse_position(item))
    return positions


def parse_open_orders(value: Any, fallback_contract: str) -> list[OpenOrder]:
    if not isinstance(value, list):
        raise RestFailure("invalid open orders response: expected list")
    orders = []
    for index, order in enumerate(value):
        if not isinstance(order, dict):
            raise RestFailure(f"invalid open_orders[{index}]: expected object")
        order_id = order.get("id")
        if order_id is None or str(order_id).strip() == "":
            order_id = order.get("text")
        if order_id is None or str(order_id).strip() == "":
            raise RestFailure(f"invalid open_orders[{index}]: missing id or text")
        contract = normalize_rest_contract(order.get("contract", fallback_contract))
        orders.append(OpenOrder(contract=contract, order_id=str(order_id).strip()))
    return orders


def _convert_account_request(method: str, api_request: account.ApiRequest) -> ApiRequest:
    return ApiRequest(
        method=method,
        endpoint_path=api_request.endpoint_path,
        query_string=api_request.query_string,
    )


def build_open_orders_request(settle: str, contract: str) -> ApiRequest:
    account_request = account.build_order_query_plan(
        settle=settle,
        contract=contract,
        status="open",
        order_id=None,
        limit=None,
        last_id=None,
    )[0]
    return _convert_account_request("GET", account_request)


def build_position_request(settle: str, contract: str | None) -> ApiRequest:
    account_request = account.build_position_query_plan(settle=settle, contract=contract)[0]
    return _convert_account_request("GET", account_request)


def query_all_positions(
    requester: Requester,
    settle: str,
) -> list[PositionSnapshot]:
    response = _request(requester, build_position_request(settle, None))
    return parse_position_list(response)


def query_positions(
    requester: Requester,
    settle: str,
    contracts: list[str],
) -> list[PositionSnapshot]:
    positions = []
    for contract in contracts:
        response = _request(requester, build_position_request(settle, contract))
        positions.append(parse_position(response, fallback_contract=contract))
    return positions


def query_open_orders(
    requester: Requester,
    settle: str,
    contracts: list[str],
) -> list[OpenOrder]:
    orders: list[OpenOrder] = []
    for contract in contracts:
        response = _request(requester, build_open_orders_request(settle, contract))
        orders.extend(parse_open_orders(response, fallback_contract=contract))
    return orders


def determine_scope_contracts(
    requester: Requester,
    config: FlattenConfig,
    summary: dict[str, Any],
) -> tuple[list[str], list[PositionSnapshot]]:
    if config.scope == "allowlist":
        contracts = normalize_contracts(config.contracts)
        summary["scope"]["contracts"] = contracts
        return contracts, []

    positions = query_all_positions(requester, normalize_settle(config.settle))
    non_zero_positions = [position for position in positions if position.size != 0]
    if len(non_zero_positions) > config.max_position_count:
        raise ScopeRefused(
            "max-position-count exceeded: "
            f"{len(non_zero_positions)} non-zero positions > {config.max_position_count}"
        )

    contracts = []
    seen = set()
    for position in positions:
        if position.size == 0 and position.pending_orders == 0:
            continue
        if position.contract in seen:
            continue
        contracts.append(position.contract)
        seen.add(position.contract)

    summary["initial_positions"] = [position.to_summary() for position in positions]
    summary["scope"]["contracts"] = contracts
    summary["scope"]["discovered_non_zero_position_count"] = len(non_zero_positions)
    summary["scope"]["discovered_pending_order_contract_count"] = sum(
        1 for position in positions if position.pending_orders != 0
    )
    summary["scope"]["limitations"].append("all-open-orders-not-discoverable")
    return contracts, positions


def _open_order_summaries(open_orders: Iterable[OpenOrder]) -> list[dict[str, str]]:
    return [order.to_summary() for order in open_orders]


def _position_summaries(positions: Iterable[PositionSnapshot]) -> list[dict[str, Any]]:
    return [position.to_summary() for position in positions]


def build_close_order_text(clock: Any, sequence: int) -> str:
    micros = int(clock.time() * 1_000_000) % 10000000000000000
    return f"t-aqflt{micros:016d}{sequence % 1000:03d}"


def build_market_close_payload(
    position: PositionSnapshot,
    text: str,
) -> tuple[dict[str, Any], str, int]:
    if position.size == 0:
        raise ValueError("cannot build close payload for zero position")
    signed_size = -position.size
    side = "buy" if signed_size > 0 else "sell"
    payload = {
        "contract": position.contract,
        "size": signed_size,
        "iceberg": 0,
        "price": "0",
        "tif": "ioc",
        "text": text,
        "reduce_only": True,
    }
    return payload, side, abs(position.size)


def cancel_open_orders(
    requester: Requester,
    settle: str,
    open_orders: list[OpenOrder],
    phase: str,
    summary: dict[str, Any],
) -> None:
    for order in open_orders:
        cancel_request = build_cancel_order_request(settle, order.order_id)
        response = _request(requester, cancel_request)
        summary["orders_cancelled"].append(
            {
                "phase": phase,
                "contract": order.contract,
                "order_id": order.order_id,
                "request": cancel_request.to_public_dict(),
                "response": response,
            }
        )


def submit_close_orders(
    requester: Requester,
    settle: str,
    positions: list[PositionSnapshot],
    clock: Any,
    summary: dict[str, Any],
) -> None:
    sequence = 0
    for position in positions:
        if position.size == 0:
            continue
        text = build_close_order_text(clock, sequence)
        sequence += 1
        payload, side, close_size = build_market_close_payload(position, text)
        close_request = build_place_order_request(settle, payload)
        response = _request(requester, close_request)
        summary["close_orders_submitted"].append(
            {
                "contract": position.contract,
                "side": side,
                "size": close_size,
                "signed_size": payload["size"],
                "request": close_request.to_public_dict(),
                "response": response,
            }
        )


def plan_positions_to_close(positions: Iterable[PositionSnapshot]) -> list[dict[str, Any]]:
    planned = []
    for position in positions:
        if position.size == 0:
            continue
        signed_size = -position.size
        planned.append(
            {
                "contract": position.contract,
                "side": "buy" if signed_size > 0 else "sell",
                "size": abs(position.size),
                "signed_size": signed_size,
                "reduce_only": True,
                "price": "0",
                "tif": "ioc",
            }
        )
    return planned


def final_state_is_flat(
    positions: list[PositionSnapshot],
    open_orders: list[OpenOrder],
) -> bool:
    return (
        not open_orders
        and all(position.size == 0 for position in positions)
        and all(position.pending_orders == 0 for position in positions)
    )


def poll_until_flat(
    requester: Requester,
    settle: str,
    contracts: list[str],
    timeout_sec: float,
    interval_sec: float,
    clock: Any,
) -> tuple[bool, int, list[PositionSnapshot], list[OpenOrder]]:
    deadline = clock.time() + timeout_sec
    polls = 0
    while True:
        polls += 1
        positions = query_positions(requester, settle, contracts)
        open_orders = query_open_orders(requester, settle, contracts)
        if final_state_is_flat(positions, open_orders):
            return True, polls, positions, open_orders
        if clock.time() >= deadline:
            return False, polls, positions, open_orders
        clock.sleep(interval_sec)


def run_emergency_flatten(
    config: FlattenConfig,
    requester: Requester,
    clock: Any | None = None,
) -> tuple[int, dict[str, Any]]:
    clock = SystemClock() if clock is None else clock
    try:
        validate_config(config)
    except ScopeRefused as exc:
        return summarize_failure(config, "scope_refused", exc, EXIT_SCOPE_REFUSED)

    summary = initial_summary(config)
    settle = normalize_settle(config.settle)
    try:
        contracts, initial_scope_positions = determine_scope_contracts(requester, config, summary)
        summary["plan"]["contracts"] = contracts
        if not contracts:
            summary["ok"] = True
            summary["result"] = "verified_flat"
            summary["exit_code"] = EXIT_OK
            return EXIT_OK, summary

        initial_open_orders = query_open_orders(requester, settle, contracts)
        summary["initial_open_orders"] = _open_order_summaries(initial_open_orders)
        summary["plan"]["open_orders_to_cancel"] = _open_order_summaries(initial_open_orders)

        if config.dry_run:
            positions = (
                initial_scope_positions
                if config.scope == "dedicated-account"
                else query_positions(requester, settle, contracts)
            )
            scoped_positions = [position for position in positions if position.contract in contracts]
            summary["initial_positions"] = _position_summaries(scoped_positions)
            summary["plan"]["positions_to_close"] = plan_positions_to_close(scoped_positions)
            summary["ok"] = True
            summary["result"] = "dry_run"
            summary["exit_code"] = EXIT_OK
            return EXIT_OK, summary

        cancel_open_orders(requester, settle, initial_open_orders, "before_close", summary)
        positions = query_positions(requester, settle, contracts)
        summary["initial_positions"] = _position_summaries(positions)
        positions_to_close = [position for position in positions if position.size != 0]
        summary["plan"]["positions_to_close"] = plan_positions_to_close(positions_to_close)
        submit_close_orders(requester, settle, positions_to_close, clock, summary)

        post_close_open_orders = query_open_orders(requester, settle, contracts)
        summary["post_close_open_orders"] = _open_order_summaries(post_close_open_orders)
        cancel_open_orders(requester, settle, post_close_open_orders, "after_close", summary)

        verified, polls, final_positions, final_open_orders = poll_until_flat(
            requester=requester,
            settle=settle,
            contracts=contracts,
            timeout_sec=config.poll_timeout_sec,
            interval_sec=config.poll_interval_sec,
            clock=clock,
        )
        summary["polls"] = polls
        summary["final_positions"] = _position_summaries(final_positions)
        summary["final_open_orders"] = _open_order_summaries(final_open_orders)
        if verified:
            summary["ok"] = True
            summary["result"] = "verified_flat"
            summary["exit_code"] = EXIT_OK
            return EXIT_OK, summary

        summary["ok"] = False
        summary["result"] = "not_flat"
        summary["exit_code"] = EXIT_NOT_FLAT
        summary["errors"].append("timeout or final verification still has positions/open orders")
        return EXIT_NOT_FLAT, summary
    except ScopeRefused as exc:
        summary["ok"] = False
        summary["result"] = "scope_refused"
        summary["exit_code"] = EXIT_SCOPE_REFUSED
        summary["errors"].append(str(exc))
        return EXIT_SCOPE_REFUSED, summary
    except RestFailure as exc:
        summary["ok"] = False
        summary["result"] = "rest_failed"
        summary["exit_code"] = EXIT_REST_FAILED
        summary["errors"].append(str(exc))
        return EXIT_REST_FAILED, summary


def add_common_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--api-key",
        default=account.DEFAULT_API_KEY_ENV,
        help="Environment variable name holding the Gate API key.",
    )
    parser.add_argument(
        "--api-secret",
        default=account.DEFAULT_API_SECRET_ENV,
        help="Environment variable name holding the Gate API secret.",
    )
    parser.add_argument("--base-url", default=account.DEFAULT_BASE_URL, help="Gate API v4 base URL.")
    parser.add_argument("--timeout", type=float, default=account.DEFAULT_TIMEOUT, help="HTTP timeout seconds.")
    parser.add_argument("--settle", default="usdt", help="Futures settlement currency, e.g. usdt.")
    parser.add_argument(
        "--pretty",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="Pretty-print JSON output.",
    )


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Cancel in-scope Gate futures open orders and flatten positions with reduce-only market closes."
    )
    add_common_args(parser)
    parser.add_argument(
        "--scope",
        default="",
        help="Flatten scope. dedicated-account requires explicit confirmation.",
    )
    parser.add_argument(
        "--confirm-dedicated-account",
        action="store_true",
        help="Required guard before dedicated-account scope may scan and flatten account positions.",
    )
    parser.add_argument(
        "--contract",
        action="append",
        default=[],
        help="Gate futures contract for allowlist scope. Can be repeated.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Query current REST state and print the plan without cancel or close requests.",
    )
    parser.add_argument(
        "--poll-timeout-sec",
        type=float,
        default=DEFAULT_POLL_TIMEOUT_SEC,
        help="Seconds to poll REST until in-scope positions and open orders are flat.",
    )
    parser.add_argument(
        "--poll-interval-sec",
        type=float,
        default=DEFAULT_POLL_INTERVAL_SEC,
        help="Seconds to sleep between final verification polls.",
    )
    parser.add_argument(
        "--max-position-count",
        type=int,
        default=DEFAULT_MAX_POSITION_COUNT,
        help="Dedicated-account guard for maximum non-zero position count.",
    )
    return parser.parse_args(argv)


def config_from_args(args: argparse.Namespace) -> FlattenConfig:
    return FlattenConfig(
        settle=args.settle,
        scope=args.scope,
        contracts=args.contract,
        confirm_dedicated_account=args.confirm_dedicated_account,
        dry_run=args.dry_run,
        poll_timeout_sec=args.poll_timeout_sec,
        poll_interval_sec=args.poll_interval_sec,
        max_position_count=args.max_position_count,
    )


def print_summary(summary: dict[str, Any], pretty: bool) -> None:
    indent = 2 if pretty else None
    print(json.dumps(summary, ensure_ascii=False, indent=indent, sort_keys=True))


def missing_env_summary(config: FlattenConfig, env_name: str) -> dict[str, Any]:
    summary = initial_summary(config)
    summary["result"] = "cli_validation_failed"
    summary["exit_code"] = EXIT_SCOPE_REFUSED
    summary["errors"].append(f"missing env var {env_name}")
    return summary


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    config = config_from_args(args)

    try:
        validate_config(config)
    except ScopeRefused as exc:
        summary = initial_summary(config)
        summary["result"] = "scope_refused"
        summary["exit_code"] = EXIT_SCOPE_REFUSED
        summary["errors"].append(str(exc))
        print_summary(summary, args.pretty)
        return EXIT_SCOPE_REFUSED

    api_key = account.get_env_value(args.api_key)
    if api_key is None:
        print_summary(missing_env_summary(config, args.api_key), args.pretty)
        return EXIT_SCOPE_REFUSED
    api_secret = account.get_env_value(args.api_secret)
    if api_secret is None:
        print_summary(missing_env_summary(config, args.api_secret), args.pretty)
        return EXIT_SCOPE_REFUSED

    client = SignedGateTradingClient(
        api_key=api_key,
        api_secret=api_secret,
        base_url=args.base_url,
        timeout=args.timeout,
    )
    exit_code, summary = run_emergency_flatten(
        config=config,
        requester=client.request_json,
        clock=SystemClock(),
    )
    print_summary(summary, args.pretty)
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
