#!/home/liuxiang/dev/pyenv/lx/bin/python

import argparse
import json
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from typing import Any, Callable

from query_gate_account import (
    DEFAULT_API_KEY_ENV,
    DEFAULT_API_SECRET_ENV,
    DEFAULT_BASE_URL,
    DEFAULT_TIMEOUT,
    build_signature_headers,
    get_env_value,
    request_path,
    request_url,
)


USER_AGENT = "aquila-gate-futures-order-test/1.0"
DEFAULT_CONTRACT = "BTC_USDT"
DEFAULT_TEXT_PREFIX = "t-aquila-rest"
MAX_ORDER_SIZE = 5


@dataclass(frozen=True)
class ApiRequest:
    method: str
    endpoint_path: str
    query_string: str = ""
    body: str = ""

    def to_public_dict(self) -> dict[str, str]:
        return {
            "method": self.method,
            "endpoint_path": self.endpoint_path,
            "query_string": self.query_string,
            "body": self.body,
        }


Requester = Callable[[ApiRequest], Any]


def normalize_settle(settle: str) -> str:
    value = settle.strip().lower()
    if not value:
        raise ValueError("settle must not be empty")
    return value


def normalize_contract(contract: str) -> str:
    value = contract.strip().upper()
    if not value:
        raise ValueError("contract must not be empty")
    return value


def normalize_tif(tif: str) -> str:
    value = tif.strip().lower()
    if value not in {"gtc", "ioc", "poc", "fok"}:
        raise ValueError("tif must be one of: gtc, ioc, poc, fok")
    return value


def normalize_side(side: str) -> str:
    value = side.strip().lower()
    if value not in {"buy", "sell"}:
        raise ValueError("side must be buy or sell")
    return value


def validate_order_text(text: str) -> str:
    value = text.strip()
    if not value.startswith("t-"):
        raise ValueError("Gate futures order text must start with t-")
    if len(value.encode("utf-8")) > 28:
        raise ValueError("Gate futures order text must be at most 28 bytes")
    return value


def signed_order_size(side: str, size: int) -> int:
    if size <= 0:
        raise ValueError("size must be positive")
    if size > MAX_ORDER_SIZE:
        raise ValueError(f"size must be <= {MAX_ORDER_SIZE}")
    return size if normalize_side(side) == "buy" else -size


def build_order_payload(
    contract: str,
    side: str,
    size: int,
    price: str,
    tif: str,
    text: str,
    iceberg: int = 0,
    reduce_only: bool = False,
) -> dict[str, Any]:
    normalized_tif = normalize_tif(tif)
    normalized_price = str(price).strip()
    if not normalized_price:
        raise ValueError("price must not be empty")
    if normalized_price == "0" and normalized_tif != "ioc":
        raise ValueError("price=0 requires tif=ioc")
    if iceberg < 0:
        raise ValueError("iceberg must not be negative")

    payload: dict[str, Any] = {
        "contract": normalize_contract(contract),
        "size": signed_order_size(side, size),
        "iceberg": iceberg,
        "price": normalized_price,
        "tif": normalized_tif,
        "text": validate_order_text(text),
    }
    if reduce_only:
        payload["reduce_only"] = True
    return payload


def stable_json(payload: dict[str, Any]) -> str:
    return json.dumps(payload, ensure_ascii=False, separators=(",", ":"))


def build_place_order_request(settle: str, payload: dict[str, Any]) -> ApiRequest:
    return ApiRequest(
        method="POST",
        endpoint_path=f"/futures/{normalize_settle(settle)}/orders",
        body=stable_json(payload),
    )


def build_cancel_order_request(settle: str, order_id: str | int) -> ApiRequest:
    encoded_order_id = urllib.parse.quote(str(order_id), safe="")
    return ApiRequest(
        method="DELETE",
        endpoint_path=f"/futures/{normalize_settle(settle)}/orders/{encoded_order_id}",
    )


class SignedGateTradingClient:
    def __init__(
        self,
        api_key: str,
        api_secret: str,
        base_url: str = DEFAULT_BASE_URL,
        timeout: float = DEFAULT_TIMEOUT,
    ):
        self.api_key = api_key
        self.api_secret = api_secret
        self.base_url = base_url
        self.timeout = timeout

    def request_json(self, api_request: ApiRequest) -> Any:
        path = request_path(self.base_url, api_request.endpoint_path)
        headers = {
            "Accept": "application/json",
            "Content-Type": "application/json",
            "User-Agent": USER_AGENT,
        }
        headers.update(
            build_signature_headers(
                api_key=self.api_key,
                api_secret=self.api_secret,
                method=api_request.method,
                request_path=path,
                query_string=api_request.query_string,
                body=api_request.body,
            )
        )
        request = urllib.request.Request(
            request_url(self.base_url, api_request),
            data=api_request.body.encode("utf-8") if api_request.body else None,
            headers=headers,
            method=api_request.method,
        )
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                body = response.read().decode("utf-8")
        except urllib.error.HTTPError as exc:
            detail = exc.read().decode("utf-8", errors="replace")
            raise RuntimeError(f"HTTP {exc.code}: {detail}") from exc
        except urllib.error.URLError as exc:
            raise RuntimeError(str(exc)) from exc

        if not body:
            return {}
        try:
            return json.loads(body)
        except json.JSONDecodeError as exc:
            raise RuntimeError(f"invalid JSON response: {exc}") from exc


def place_order(
    requester: Requester,
    settle: str,
    payload: dict[str, Any],
    execute: bool,
    keep_open: bool,
) -> dict[str, Any]:
    place_request = build_place_order_request(settle, payload)
    result: dict[str, Any] = {
        "executed": execute,
        "keep_open": keep_open,
        "place_request": place_request.to_public_dict(),
    }
    if not execute:
        return result

    place_response = requester(place_request)
    result["place_response"] = place_response
    if keep_open:
        return result

    order_id = place_response.get("id") if isinstance(place_response, dict) else None
    if order_id is None:
        order_id = payload["text"]
    cancel_request = build_cancel_order_request(settle, order_id)
    result["cancel_request"] = cancel_request.to_public_dict()
    result["cancel_response"] = requester(cancel_request)
    return result


def cancel_order(requester: Requester, settle: str, order_id: str | int) -> dict[str, Any]:
    cancel_request = build_cancel_order_request(settle, order_id)
    return {
        "cancel_request": cancel_request.to_public_dict(),
        "cancel_response": requester(cancel_request),
    }


def default_order_text() -> str:
    return f"{DEFAULT_TEXT_PREFIX}-{int(time.time())}"


def add_common_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--api-key",
        default=DEFAULT_API_KEY_ENV,
        help="Environment variable name holding the Gate API key.",
    )
    parser.add_argument(
        "--api-secret",
        default=DEFAULT_API_SECRET_ENV,
        help="Environment variable name holding the Gate API secret.",
    )
    parser.add_argument("--base-url", default=DEFAULT_BASE_URL, help="Gate API v4 base URL.")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT, help="HTTP timeout seconds.")
    parser.add_argument("--settle", default="usdt", help="Futures settlement currency, e.g. usdt.")
    parser.add_argument(
        "--pretty",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="Pretty-print JSON output.",
    )


def add_place_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--contract", default=DEFAULT_CONTRACT, help="Gate futures contract.")
    parser.add_argument("--side", choices=("buy", "sell"), default="buy", help="Order side.")
    parser.add_argument(
        "--size",
        type=int,
        default=1,
        help=f"Positive contract size. Hard risk limit: <= {MAX_ORDER_SIZE}.",
    )
    parser.add_argument("--price", default="1", help="Limit price. price=0 requires --tif ioc.")
    parser.add_argument(
        "--tif",
        choices=("gtc", "ioc", "poc", "fok"),
        default="gtc",
        help="Time-in-force.",
    )
    parser.add_argument("--iceberg", type=int, default=0, help="Iceberg size, 0 disables iceberg.")
    parser.add_argument("--text", default=None, help="Client order id. Must start with t-.")
    parser.add_argument("--reduce-only", action="store_true", help="Set reduce_only=true.")
    parser.add_argument(
        "--execute",
        action="store_true",
        help="Actually submit the order. Omitted means dry-run only.",
    )
    parser.add_argument(
        "--keep-open",
        action="store_true",
        help="Do not cancel the order after a successful submit.",
    )


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Create or cancel a Gate USDT futures order through APIv4 REST."
    )
    subparsers = parser.add_subparsers(dest="command")

    place_parser = subparsers.add_parser("place", help="Create a futures order.")
    add_common_args(place_parser)
    add_place_args(place_parser)

    cancel_parser = subparsers.add_parser("cancel", help="Cancel a single futures order.")
    add_common_args(cancel_parser)
    cancel_parser.add_argument("order_id", help="Gate order id or client text id to cancel.")

    add_common_args(parser)
    add_place_args(parser)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    command = args.command or "place"
    if command == "cancel":
        return run_cancel(args)
    return run_place(args)


def run_place(args: argparse.Namespace) -> int:
    text = args.text if args.text is not None else default_order_text()
    try:
        payload = build_order_payload(
            contract=args.contract,
            side=args.side,
            size=args.size,
            price=args.price,
            tif=args.tif,
            text=text,
            iceberg=args.iceberg,
            reduce_only=args.reduce_only,
        )
    except ValueError as exc:
        print(f"[FAIL] {exc}", file=sys.stderr)
        return 2

    if not args.execute:
        result = place_order(
            requester=lambda api_request: None,
            settle=args.settle,
            payload=payload,
            execute=False,
            keep_open=args.keep_open,
        )
        indent = 2 if args.pretty else None
        print(json.dumps(result, ensure_ascii=False, indent=indent, sort_keys=True))
        return 0

    api_key = get_env_value(args.api_key)
    if api_key is None:
        print(f"[FAIL] missing env var {args.api_key}", file=sys.stderr)
        return 2
    api_secret = get_env_value(args.api_secret)
    if api_secret is None:
        print(f"[FAIL] missing env var {args.api_secret}", file=sys.stderr)
        return 2

    client = SignedGateTradingClient(
        api_key=api_key,
        api_secret=api_secret,
        base_url=args.base_url,
        timeout=args.timeout,
    )
    try:
        result = place_order(
            requester=client.request_json,
            settle=args.settle,
            payload=payload,
            execute=True,
            keep_open=args.keep_open,
        )
    except Exception as exc:  # pragma: no cover - utility script
        print(f"[FAIL] {type(exc).__name__}: {exc}", file=sys.stderr)
        return 1

    indent = 2 if args.pretty else None
    print(json.dumps(result, ensure_ascii=False, indent=indent, sort_keys=True))
    return 0


def run_cancel(args: argparse.Namespace) -> int:
    api_key = get_env_value(args.api_key)
    if api_key is None:
        print(f"[FAIL] missing env var {args.api_key}", file=sys.stderr)
        return 2
    api_secret = get_env_value(args.api_secret)
    if api_secret is None:
        print(f"[FAIL] missing env var {args.api_secret}", file=sys.stderr)
        return 2

    client = SignedGateTradingClient(
        api_key=api_key,
        api_secret=api_secret,
        base_url=args.base_url,
        timeout=args.timeout,
    )
    try:
        result = cancel_order(
            requester=client.request_json,
            settle=args.settle,
            order_id=args.order_id,
        )
    except Exception as exc:  # pragma: no cover - utility script
        print(f"[FAIL] {type(exc).__name__}: {exc}", file=sys.stderr)
        return 1

    indent = 2 if args.pretty else None
    print(json.dumps(result, ensure_ascii=False, indent=indent, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
