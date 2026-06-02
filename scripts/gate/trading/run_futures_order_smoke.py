#!/home/liuxiang/dev/pyenv/lx/bin/python

import argparse
import json
import sys
import time
import urllib.parse
from dataclasses import dataclass
from decimal import Decimal, InvalidOperation, ROUND_CEILING, ROUND_FLOOR
from pathlib import Path
from typing import Any, Callable

GATE_ACCOUNT_SCRIPT_DIR = Path(__file__).resolve().parents[1] / "account"
if str(GATE_ACCOUNT_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(GATE_ACCOUNT_SCRIPT_DIR))

from place_futures_order import (
    ApiRequest,
    SignedGateTradingClient,
    build_cancel_order_request,
    build_order_payload,
    build_place_order_request,
)
from query_gate_account import (
    DEFAULT_API_KEY_ENV,
    DEFAULT_API_SECRET_ENV,
    DEFAULT_BASE_URL,
    DEFAULT_TIMEOUT,
    get_env_value,
    normalize_contract,
)


DEFAULT_CONTRACT = "BTC_USDT"
DEFAULT_SETTLE = "usdt"
DEFAULT_ITERATIONS = 5
DEFAULT_SIZE = 1
DEFAULT_MAX_OPEN_SIZE = 2
DEFAULT_FILL_TIMEOUT = 60.0
DEFAULT_POLL_INTERVAL = 2.0
DEFAULT_CLOSE_TIMEOUT = 10.0
DEFAULT_MARGIN_BPS = Decimal("5")


Requester = Callable[[ApiRequest], Any]


class SystemClock:
    def time(self) -> float:
        return time.time()

    def sleep(self, seconds: float) -> None:
        time.sleep(seconds)


def normalize_settle(settle: str) -> str:
    value = settle.strip().lower()
    if not value:
        raise ValueError("settle must not be empty")
    return value


def parse_decimal(value: Any, label: str) -> Decimal:
    try:
        result = Decimal(str(value))
    except (InvalidOperation, ValueError) as exc:
        raise RuntimeError(f"invalid {label}: {value}") from exc
    if result <= 0:
        raise RuntimeError(f"{label} must be positive: {value}")
    return result


def format_decimal(value: Decimal) -> str:
    return format(value.normalize(), "f")


def limit_price_from_ticker(
    last_price: Decimal, tick: Decimal, side: str, margin_bps: Decimal
) -> str:
    if tick <= 0:
        raise RuntimeError(f"tick must be positive: {tick}")
    side_value = side.strip().lower()
    if side_value == "buy":
        raw = last_price * (Decimal("1") + margin_bps / Decimal("10000"))
        units = (raw / tick).to_integral_value(rounding=ROUND_CEILING)
    elif side_value == "sell":
        raw = last_price * (Decimal("1") - margin_bps / Decimal("10000"))
        units = (raw / tick).to_integral_value(rounding=ROUND_FLOOR)
    else:
        raise ValueError("side must be buy or sell")
    return format_decimal(units * tick)


def build_query_string(params: list[tuple[str, str | None]]) -> str:
    return urllib.parse.urlencode(
        [(key, value) for key, value in params if value is not None and value != ""]
    )


def build_contract_request(settle: str, contract: str) -> ApiRequest:
    encoded_contract = urllib.parse.quote(normalize_contract(contract), safe="")
    return ApiRequest(
        method="GET",
        endpoint_path=f"/futures/{normalize_settle(settle)}/contracts/{encoded_contract}",
    )


def build_ticker_request(settle: str, contract: str) -> ApiRequest:
    return ApiRequest(
        method="GET",
        endpoint_path=f"/futures/{normalize_settle(settle)}/tickers",
        query_string=build_query_string([("contract", normalize_contract(contract))]),
    )


def build_position_request(settle: str, contract: str) -> ApiRequest:
    encoded_contract = urllib.parse.quote(normalize_contract(contract), safe="")
    return ApiRequest(
        method="GET",
        endpoint_path=f"/futures/{normalize_settle(settle)}/positions/{encoded_contract}",
    )


def build_open_orders_request(settle: str, contract: str) -> ApiRequest:
    return ApiRequest(
        method="GET",
        endpoint_path=f"/futures/{normalize_settle(settle)}/orders",
        query_string=build_query_string(
            [("contract", normalize_contract(contract)), ("status", "open")]
        ),
    )


def build_get_order_request(settle: str, order_id: str | int) -> ApiRequest:
    encoded_order_id = urllib.parse.quote(str(order_id), safe="")
    return ApiRequest(
        method="GET",
        endpoint_path=f"/futures/{normalize_settle(settle)}/orders/{encoded_order_id}",
    )


def position_size(position: Any) -> int:
    if not isinstance(position, dict):
        raise RuntimeError(f"position response is not an object: {position}")
    return int(position.get("size") or 0)


def open_order_count(orders: Any) -> int:
    if not isinstance(orders, list):
        raise RuntimeError(f"open orders response is not a list: {orders}")
    return len(orders)


def ticker_last_price(tickers: Any, contract: str) -> Decimal:
    rows = tickers if isinstance(tickers, list) else [tickers]
    normalized = normalize_contract(contract)
    for row in rows:
        if isinstance(row, dict) and row.get("contract") == normalized:
            return parse_decimal(row.get("last"), "last price")
    raise RuntimeError(f"ticker for {normalized} not found: {tickers}")


def contract_price_tick(contract_detail: Any) -> Decimal:
    if not isinstance(contract_detail, dict):
        raise RuntimeError(f"contract response is not an object: {contract_detail}")
    return parse_decimal(contract_detail.get("order_price_round"), "order_price_round")


def order_id(order: Any) -> str:
    if not isinstance(order, dict) or order.get("id") is None:
        raise RuntimeError(f"order response has no id: {order}")
    return str(order["id"])


def order_left(order: Any) -> int:
    if not isinstance(order, dict):
        raise RuntimeError(f"order response is not an object: {order}")
    return int(order.get("left") or 0)


def order_is_filled(order: Any) -> bool:
    if not isinstance(order, dict):
        return False
    if order.get("status") == "finished" and order.get("finish_as") == "filled":
        return True
    return order.get("status") == "finished" and order_left(order) == 0


def smoke_text(iteration: int, role: str) -> str:
    stamp = int(time.time() * 1000) % 100000000
    return f"t-aq{stamp:08d}{iteration}{role}"


@dataclass
class SmokeRunner:
    requester: Requester
    settle: str
    contract: str
    clock: Any
    side: str = "buy"

    def run_many(
        self,
        iterations: int,
        size: int,
        max_open_size: int,
        fill_timeout: float,
        poll_interval: float,
        margin_bps: Decimal,
        execute: bool,
        close_timeout: float = DEFAULT_CLOSE_TIMEOUT,
    ) -> dict[str, Any]:
        results = []
        for iteration in range(1, iterations + 1):
            results.append(
                self.run_once(
                    iteration=iteration,
                    size=size,
                    max_open_size=max_open_size,
                    fill_timeout=fill_timeout,
                    poll_interval=poll_interval,
                    close_timeout=close_timeout,
                    margin_bps=margin_bps,
                    execute=execute,
                )
            )
        return {
            "contract": normalize_contract(self.contract),
            "iterations_requested": iterations,
            "iterations_completed": len(results),
            "results": results,
            "outcomes": {
                "filled_and_closed": sum(
                    1 for result in results if result.get("outcome") == "filled_and_closed"
                ),
                "cancelled": sum(1 for result in results if result.get("outcome") == "cancelled"),
            },
        }

    def run_once(
        self,
        iteration: int,
        size: int,
        max_open_size: int,
        fill_timeout: float,
        poll_interval: float,
        margin_bps: Decimal,
        execute: bool,
        close_timeout: float = DEFAULT_CLOSE_TIMEOUT,
    ) -> dict[str, Any]:
        position_before = self.current_position_size()
        open_orders_before = self.current_open_order_count()
        if open_orders_before != 0:
            raise RuntimeError(f"refusing to run with {open_orders_before} existing open orders")

        planned_delta = size if self.side == "buy" else -size
        if abs(position_before + planned_delta) > max_open_size:
            raise RuntimeError(
                f"max open size would be exceeded: current={position_before}, "
                f"delta={planned_delta}, max open size={max_open_size}"
            )
        if position_before != 0:
            raise RuntimeError(f"refusing to mix smoke test with existing position size={position_before}")

        price = self.limit_price(margin_bps)
        entry_payload = build_order_payload(
            contract=self.contract,
            side=self.side,
            size=size,
            price=price,
            tif="gtc",
            text=smoke_text(iteration, "o"),
        )
        result: dict[str, Any] = {
            "iteration": iteration,
            "position_before": position_before,
            "open_orders_before": open_orders_before,
            "entry_payload": entry_payload,
            "execute": execute,
        }
        if not execute:
            result["outcome"] = "dry_run"
            return result

        place_response = self.requester(build_place_order_request(self.settle, entry_payload))
        result["entry_response"] = place_response
        entry_order_id = order_id(place_response)
        result["entry_order_id"] = entry_order_id

        final_entry = self.wait_for_fill_or_timeout(
            order_id_value=entry_order_id,
            initial_order=place_response,
            fill_timeout=fill_timeout,
            poll_interval=poll_interval,
        )
        result["entry_final"] = final_entry
        if order_is_filled(final_entry):
            close_response = self.close_position(iteration=iteration, size=size)
            result["close_response"] = close_response
            result["position_after"] = self.wait_for_position_size(
                expected_size=0,
                timeout_seconds=close_timeout,
                poll_interval=poll_interval,
            )
            result["outcome"] = (
                "filled_and_closed" if result["position_after"] == 0 else "close_unconfirmed"
            )
            return result

        cancel_response = self.requester(build_cancel_order_request(self.settle, entry_order_id))
        result["cancel_response"] = cancel_response
        result["position_after"] = self.current_position_size()
        result["outcome"] = "cancelled"
        return result

    def current_position_size(self) -> int:
        return position_size(self.requester(build_position_request(self.settle, self.contract)))

    def current_open_order_count(self) -> int:
        return open_order_count(self.requester(build_open_orders_request(self.settle, self.contract)))

    def limit_price(self, margin_bps: Decimal) -> str:
        tick = contract_price_tick(self.requester(build_contract_request(self.settle, self.contract)))
        last = ticker_last_price(self.requester(build_ticker_request(self.settle, self.contract)), self.contract)
        return limit_price_from_ticker(last, tick, self.side, margin_bps)

    def wait_for_fill_or_timeout(
        self,
        order_id_value: str,
        initial_order: Any,
        fill_timeout: float,
        poll_interval: float,
    ) -> Any:
        order = initial_order
        deadline = self.clock.time() + fill_timeout
        while True:
            if order_is_filled(order):
                return order
            if self.clock.time() >= deadline:
                return order
            self.clock.sleep(poll_interval)
            order = self.requester(build_get_order_request(self.settle, order_id_value))

    def wait_for_position_size(
        self, expected_size: int, timeout_seconds: float, poll_interval: float
    ) -> int:
        deadline = self.clock.time() + timeout_seconds
        while True:
            current = self.current_position_size()
            if current == expected_size or self.clock.time() >= deadline:
                return current
            self.clock.sleep(poll_interval)

    def close_position(self, iteration: int, size: int) -> Any:
        close_side = "sell" if self.side == "buy" else "buy"
        close_payload = build_order_payload(
            contract=self.contract,
            side=close_side,
            size=size,
            price="0",
            tif="ioc",
            text=smoke_text(iteration, "c"),
            reduce_only=True,
        )
        return self.requester(build_place_order_request(self.settle, close_payload))


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run a small Gate futures order smoke: place 1-contract BTC_USDT limit orders, cancel unfilled orders, and reduce-only market-close filled orders."
    )
    parser.add_argument("--api-key", default=DEFAULT_API_KEY_ENV)
    parser.add_argument("--api-secret", default=DEFAULT_API_SECRET_ENV)
    parser.add_argument("--base-url", default=DEFAULT_BASE_URL)
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    parser.add_argument("--settle", default=DEFAULT_SETTLE)
    parser.add_argument("--contract", default=DEFAULT_CONTRACT)
    parser.add_argument("--iterations", type=int, default=DEFAULT_ITERATIONS)
    parser.add_argument("--size", type=int, default=DEFAULT_SIZE)
    parser.add_argument("--max-open-size", type=int, default=DEFAULT_MAX_OPEN_SIZE)
    parser.add_argument("--fill-timeout", type=float, default=DEFAULT_FILL_TIMEOUT)
    parser.add_argument("--poll-interval", type=float, default=DEFAULT_POLL_INTERVAL)
    parser.add_argument("--close-timeout", type=float, default=DEFAULT_CLOSE_TIMEOUT)
    parser.add_argument("--margin-bps", default=str(DEFAULT_MARGIN_BPS))
    parser.add_argument("--execute", action="store_true")
    parser.add_argument("--pretty", action=argparse.BooleanOptionalAction, default=True)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    api_key = get_env_value(args.api_key)
    if api_key is None:
        print(f"[FAIL] missing env var {args.api_key}", file=sys.stderr)
        return 2
    api_secret = get_env_value(args.api_secret)
    if api_secret is None:
        print(f"[FAIL] missing env var {args.api_secret}", file=sys.stderr)
        return 2

    if args.iterations <= 0:
        print("[FAIL] iterations must be positive", file=sys.stderr)
        return 2
    if args.size <= 0:
        print("[FAIL] size must be positive", file=sys.stderr)
        return 2
    if args.max_open_size < args.size:
        print("[FAIL] max-open-size must be >= size", file=sys.stderr)
        return 2

    try:
        margin_bps = Decimal(str(args.margin_bps))
    except InvalidOperation:
        print(f"[FAIL] invalid margin-bps: {args.margin_bps}", file=sys.stderr)
        return 2

    client = SignedGateTradingClient(
        api_key=api_key,
        api_secret=api_secret,
        base_url=args.base_url,
        timeout=args.timeout,
    )
    runner = SmokeRunner(
        requester=client.request_json,
        settle=args.settle,
        contract=args.contract,
        clock=SystemClock(),
    )
    try:
        result = runner.run_many(
            iterations=args.iterations,
            size=args.size,
            max_open_size=args.max_open_size,
            fill_timeout=args.fill_timeout,
            poll_interval=args.poll_interval,
            close_timeout=args.close_timeout,
            margin_bps=margin_bps,
            execute=args.execute,
        )
    except Exception as exc:  # pragma: no cover - utility script
        print(f"[FAIL] {type(exc).__name__}: {exc}", file=sys.stderr)
        return 1

    indent = 2 if args.pretty else None
    print(json.dumps(result, ensure_ascii=False, indent=indent, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
