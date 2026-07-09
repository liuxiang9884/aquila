#!/home/liuxiang/dev/pyenv/lx/bin/python

import argparse
import base64
import hashlib
import hmac
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from typing import Any, Callable, Iterable


DEFAULT_BASE_URL = "https://api.bitget.com"
DEFAULT_TIMEOUT = 8.0
DEFAULT_API_KEY_ENV = "BITGET_TEST_KEY"
DEFAULT_API_SECRET_ENV = "BITGET_TEST_SECRET"
DEFAULT_API_PASSPHRASE_ENV = "BITGET_TEST_PASSPHRASE"
DEFAULT_CATEGORY = "USDT-FUTURES"
USER_AGENT = "aquila-bitget-account-query/1.0"


@dataclass(frozen=True)
class ApiRequest:
    label: str
    endpoint_path: str
    query_string: str = ""


Requester = Callable[[ApiRequest], Any]


def normalize_category(category: str) -> str:
    value = category.strip().upper()
    if not value:
        raise ValueError("category must not be empty")
    return value


def normalize_symbol(symbol: str) -> str:
    value = symbol.strip().upper().replace("_", "").replace("/", "")
    if not value:
        raise ValueError("symbol must not be empty")
    return value


def normalize_optional_symbol(symbol: str | None) -> str | None:
    if symbol is None or symbol.strip() == "":
        return None
    return normalize_symbol(symbol)


def normalize_pos_side(pos_side: str | None) -> str | None:
    if pos_side is None or pos_side.strip() == "":
        return None
    value = pos_side.strip().lower()
    if value not in {"long", "short"}:
        raise ValueError("pos_side must be long or short")
    return value


def build_query_string(params: Iterable[tuple[str, str | None]]) -> str:
    filtered = [(key, value) for key, value in params if value is not None and value != ""]
    return urllib.parse.urlencode(sorted(filtered, key=lambda item: item[0]))


def build_signature_headers(
    api_key: str,
    api_secret: str,
    passphrase: str,
    method: str,
    request_path: str,
    query_string: str = "",
    body: str = "",
    timestamp: int | str | None = None,
) -> dict[str, str]:
    timestamp_text = str(int(time.time() * 1000) if timestamp is None else timestamp)
    method_text = method.upper()
    if query_string:
        signature_text = f"{timestamp_text}{method_text}{request_path}?{query_string}{body}"
    else:
        signature_text = f"{timestamp_text}{method_text}{request_path}{body}"
    digest = hmac.new(
        api_secret.encode("utf-8"), signature_text.encode("utf-8"), hashlib.sha256
    ).digest()
    signature = base64.b64encode(digest).decode("ascii")
    return {
        "ACCESS-KEY": api_key,
        "ACCESS-SIGN": signature,
        "ACCESS-PASSPHRASE": passphrase,
        "ACCESS-TIMESTAMP": timestamp_text,
    }


def base_url_prefix(base_url: str) -> str:
    parsed = urllib.parse.urlsplit(base_url.rstrip("/"))
    return parsed.path or ""


def request_path(base_url: str, endpoint_path: str) -> str:
    prefix = base_url_prefix(base_url).rstrip("/")
    endpoint = endpoint_path if endpoint_path.startswith("/") else f"/{endpoint_path}"
    return f"{prefix}{endpoint}" if prefix else endpoint


def request_url(base_url: str, api_request: ApiRequest) -> str:
    url = f"{base_url.rstrip('/')}/{api_request.endpoint_path.lstrip('/')}"
    if api_request.query_string:
        return f"{url}?{api_request.query_string}"
    return url


def build_account_query_plan() -> list[ApiRequest]:
    return [
        ApiRequest(label="account_assets", endpoint_path="/api/v3/account/assets"),
        ApiRequest(label="account_settings", endpoint_path="/api/v3/account/settings"),
    ]


def build_position_query_plan(
    category: str,
    symbol: str | None,
    pos_side: str | None,
) -> list[ApiRequest]:
    return [
        ApiRequest(
            label="current_positions",
            endpoint_path="/api/v3/position/current-position",
            query_string=build_query_string(
                [
                    ("category", normalize_category(category)),
                    ("symbol", normalize_optional_symbol(symbol)),
                    ("posSide", normalize_pos_side(pos_side)),
                ]
            ),
        )
    ]


def build_order_query_plan(
    category: str,
    symbol: str | None,
    status: str,
    order_id: str | None,
    client_oid: str | None,
    start_time: str | None,
    end_time: str | None,
    limit: int | None,
    cursor: str | None,
) -> list[ApiRequest]:
    if limit is not None and limit <= 0:
        raise ValueError("limit must be positive")
    if order_id or client_oid:
        return [
            ApiRequest(
                label="order_info",
                endpoint_path="/api/v3/trade/order-info",
                query_string=build_query_string(
                    [
                        ("orderId", order_id.strip() if order_id else None),
                        ("clientOid", client_oid.strip() if client_oid else None),
                    ]
                ),
            )
        ]

    normalized_status = status.strip().lower()
    if normalized_status == "open":
        label = "open_orders"
        endpoint_path = "/api/v3/trade/unfilled-orders"
    elif normalized_status == "history":
        label = "history_orders"
        endpoint_path = "/api/v3/trade/history-orders"
    else:
        raise ValueError("status must be open or history")

    return [
        ApiRequest(
            label=label,
            endpoint_path=endpoint_path,
            query_string=build_query_string(
                [
                    ("category", normalize_category(category)),
                    ("symbol", normalize_optional_symbol(symbol)),
                    ("startTime", start_time),
                    ("endTime", end_time),
                    ("limit", str(limit) if limit is not None else None),
                    ("cursor", cursor),
                ]
            ),
        )
    ]


class SignedBitgetRestClient:
    def __init__(
        self,
        api_key: str,
        api_secret: str,
        passphrase: str,
        base_url: str = DEFAULT_BASE_URL,
        timeout: float = DEFAULT_TIMEOUT,
        locale: str = "en-US",
    ):
        self.api_key = api_key
        self.api_secret = api_secret
        self.passphrase = passphrase
        self.base_url = base_url
        self.timeout = timeout
        self.locale = locale

    def get_json(self, api_request: ApiRequest) -> Any:
        path = request_path(self.base_url, api_request.endpoint_path)
        headers = {
            "Accept": "application/json",
            "Content-Type": "application/json",
            "User-Agent": USER_AGENT,
            "locale": self.locale,
        }
        headers.update(
            build_signature_headers(
                api_key=self.api_key,
                api_secret=self.api_secret,
                passphrase=self.passphrase,
                method="GET",
                request_path=path,
                query_string=api_request.query_string,
            )
        )
        request = urllib.request.Request(
            request_url(self.base_url, api_request), headers=headers, method="GET"
        )
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                body = response.read().decode("utf-8")
        except urllib.error.HTTPError as exc:
            detail = exc.read().decode("utf-8", errors="replace")
            raise RuntimeError(f"HTTP {exc.code}: {detail}") from exc
        except urllib.error.URLError as exc:
            raise RuntimeError(str(exc)) from exc

        try:
            return json.loads(body)
        except json.JSONDecodeError as exc:
            raise RuntimeError(f"invalid JSON response: {exc}") from exc


def query_requests(
    requester: Requester,
    requests: Iterable[ApiRequest],
    allow_partial: bool = False,
) -> dict[str, Any]:
    results: dict[str, Any] = {}
    errors: dict[str, str] = {}
    for api_request in requests:
        try:
            results[api_request.label] = requester(api_request)
        except Exception as exc:
            if not allow_partial:
                raise
            errors[api_request.label] = f"{type(exc).__name__}: {exc}"
    return {"ok": not errors, "results": results, "errors": errors}


def query_account_info(
    requester: Requester,
    allow_partial: bool = False,
) -> dict[str, Any]:
    return query_requests(
        requester=requester,
        requests=build_account_query_plan(),
        allow_partial=allow_partial,
    )


def query_position_info(
    requester: Requester,
    category: str,
    symbol: str | None,
    pos_side: str | None,
    allow_partial: bool = False,
) -> dict[str, Any]:
    return query_requests(
        requester=requester,
        requests=build_position_query_plan(category=category, symbol=symbol, pos_side=pos_side),
        allow_partial=allow_partial,
    )


def query_order_info(
    requester: Requester,
    category: str,
    symbol: str | None,
    status: str,
    order_id: str | None,
    client_oid: str | None,
    start_time: str | None,
    end_time: str | None,
    limit: int | None,
    cursor: str | None,
    allow_partial: bool = False,
) -> dict[str, Any]:
    return query_requests(
        requester=requester,
        requests=build_order_query_plan(
            category=category,
            symbol=symbol,
            status=status,
            order_id=order_id,
            client_oid=client_oid,
            start_time=start_time,
            end_time=end_time,
            limit=limit,
            cursor=cursor,
        ),
        allow_partial=allow_partial,
    )


def get_env_value(name: str) -> str | None:
    value = os.getenv(name)
    return value if value else None


def add_common_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--api-key",
        default=DEFAULT_API_KEY_ENV,
        help="Environment variable name holding the Bitget API key.",
    )
    parser.add_argument(
        "--api-secret",
        default=DEFAULT_API_SECRET_ENV,
        help="Environment variable name holding the Bitget API secret.",
    )
    parser.add_argument(
        "--api-passphrase",
        default=DEFAULT_API_PASSPHRASE_ENV,
        help="Environment variable name holding the Bitget API passphrase.",
    )
    parser.add_argument("--base-url", default=DEFAULT_BASE_URL, help="Bitget UTA REST base URL.")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT, help="HTTP timeout seconds.")
    parser.add_argument("--locale", default="en-US", help="Bitget locale header value.")
    parser.add_argument(
        "--allow-partial",
        action="store_true",
        help="Keep successful endpoint results and report per-endpoint errors.",
    )
    parser.add_argument(
        "--pretty",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="Pretty-print JSON output.",
    )


def add_positions_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--category",
        default=DEFAULT_CATEGORY,
        help="Bitget UTA futures category, e.g. USDT-FUTURES.",
    )
    parser.add_argument(
        "--symbol",
        help="Optional Bitget symbol. Omitted means query all positions in the category.",
    )
    parser.add_argument("--pos-side", choices=("long", "short"), help="Optional position side.")


def add_orders_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--category",
        default=DEFAULT_CATEGORY,
        help="Bitget UTA product category, e.g. USDT-FUTURES.",
    )
    parser.add_argument("--symbol", help="Optional Bitget symbol, e.g. BTCUSDT or BTC_USDT.")
    parser.add_argument(
        "--status",
        choices=("open", "history"),
        default="open",
        help="Order list status. Ignored when --order-id or --client-oid is set.",
    )
    parser.add_argument("--order-id", help="Optional Bitget order ID for single order details.")
    parser.add_argument("--client-oid", help="Optional client order ID for single order details.")
    parser.add_argument("--start-time", help="Optional start timestamp in milliseconds.")
    parser.add_argument("--end-time", help="Optional end timestamp in milliseconds.")
    parser.add_argument("--limit", type=int, help="Optional page limit.")
    parser.add_argument("--cursor", help="Optional pagination cursor.")


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Query Bitget UTA read-only account, order, and position information."
    )
    subparsers = parser.add_subparsers(dest="command")

    account_parser = subparsers.add_parser("account", help="Query UTA account assets and settings.")
    add_common_args(account_parser)

    orders_parser = subparsers.add_parser("orders", help="Query UTA order information.")
    add_common_args(orders_parser)
    add_orders_args(orders_parser)

    positions_parser = subparsers.add_parser("positions", help="Query UTA futures positions.")
    add_common_args(positions_parser)
    add_positions_args(positions_parser)

    add_common_args(parser)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    command = args.command or "account"
    api_key = get_env_value(args.api_key)
    if api_key is None:
        print(f"[FAIL] missing env var {args.api_key}", file=sys.stderr)
        return 2
    api_secret = get_env_value(args.api_secret)
    if api_secret is None:
        print(f"[FAIL] missing env var {args.api_secret}", file=sys.stderr)
        return 2
    passphrase = get_env_value(args.api_passphrase)
    if passphrase is None:
        print(f"[FAIL] missing env var {args.api_passphrase}", file=sys.stderr)
        return 2

    client = SignedBitgetRestClient(
        api_key=api_key,
        api_secret=api_secret,
        passphrase=passphrase,
        base_url=args.base_url,
        timeout=args.timeout,
        locale=args.locale,
    )
    try:
        if command == "account":
            result = query_account_info(
                requester=client.get_json,
                allow_partial=args.allow_partial,
            )
        elif command == "orders":
            result = query_order_info(
                requester=client.get_json,
                category=args.category,
                symbol=args.symbol,
                status=args.status,
                order_id=args.order_id,
                client_oid=args.client_oid,
                start_time=args.start_time,
                end_time=args.end_time,
                limit=args.limit,
                cursor=args.cursor,
                allow_partial=args.allow_partial,
            )
        elif command == "positions":
            result = query_position_info(
                requester=client.get_json,
                category=args.category,
                symbol=args.symbol,
                pos_side=args.pos_side,
                allow_partial=args.allow_partial,
            )
        else:
            raise RuntimeError(f"unknown command: {command}")
    except Exception as exc:  # pragma: no cover - utility script
        print(f"[FAIL] {type(exc).__name__}: {exc}", file=sys.stderr)
        return 1

    indent = 2 if args.pretty else None
    print(json.dumps(result, ensure_ascii=False, indent=indent, sort_keys=True))
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
