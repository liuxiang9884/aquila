#!/home/liuxiang/dev/pyenv/lx/bin/python

import argparse
import json
import re
import sys
import urllib.error
import urllib.request
from dataclasses import dataclass
from decimal import Decimal, InvalidOperation
from pathlib import Path
from typing import Any, Callable


if __package__:
    from bitget.account.query_bitget_account import (  # noqa: E402
        DEFAULT_API_KEY_ENV,
        DEFAULT_API_PASSPHRASE_ENV,
        DEFAULT_API_SECRET_ENV,
        DEFAULT_BASE_URL,
        DEFAULT_TIMEOUT,
        build_signature_headers,
        get_env_value,
        normalize_category,
        request_path,
        request_url,
    )
else:
    BITGET_ACCOUNT_SCRIPT_DIR = Path(__file__).resolve().parents[1] / "account"
    if str(BITGET_ACCOUNT_SCRIPT_DIR) not in sys.path:
        sys.path.insert(0, str(BITGET_ACCOUNT_SCRIPT_DIR))
    from query_bitget_account import (  # noqa: E402
        DEFAULT_API_KEY_ENV,
        DEFAULT_API_PASSPHRASE_ENV,
        DEFAULT_API_SECRET_ENV,
        DEFAULT_BASE_URL,
        DEFAULT_TIMEOUT,
        build_signature_headers,
        get_env_value,
        normalize_category,
        request_path,
        request_url,
    )


USER_AGENT = "aquila-bitget-uta-trading/1.0"
DEFAULT_SYMBOL = "BTCUSDT"
CLIENT_OID_PATTERN = re.compile(r"^[.A-Z:/a-z0-9_-]+$")


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


def normalize_symbol(symbol: str) -> str:
    value = symbol.strip().upper().replace("_", "").replace("/", "")
    if not value:
        raise ValueError("symbol must not be empty")
    return value


def normalize_side(side: str) -> str:
    value = side.strip().lower()
    if value not in {"buy", "sell"}:
        raise ValueError("side must be buy or sell")
    return value


def normalize_margin_mode(margin_mode: str) -> str:
    value = margin_mode.strip().lower()
    if value not in {"crossed", "isolated"}:
        raise ValueError("margin_mode must be crossed or isolated")
    return value


def validate_client_oid(client_oid: str) -> str:
    value = client_oid.strip()
    if not value:
        raise ValueError("client_oid must not be empty")
    if len(value) > 32:
        raise ValueError("client_oid must be at most 32 characters")
    if CLIENT_OID_PATTERN.fullmatch(value) is None:
        raise ValueError("client_oid contains unsupported characters")
    return value


def decimal_text(value: Decimal) -> str:
    try:
        decimal_value = Decimal(value)
    except (InvalidOperation, TypeError, ValueError) as exc:
        raise ValueError("qty must be a decimal") from exc
    if not decimal_value.is_finite() or decimal_value <= 0:
        raise ValueError("qty must be positive")
    return format(decimal_value, "f")


def build_place_order_request(
    category: str,
    symbol: str,
    qty: Decimal,
    side: str,
    margin_mode: str,
    client_oid: str,
    reduce_only: bool,
) -> ApiRequest:
    payload = {
        "category": normalize_category(category),
        "symbol": normalize_symbol(symbol),
        "qty": decimal_text(qty),
        "side": normalize_side(side),
        "orderType": "market",
        "reduceOnly": "yes" if reduce_only else "no",
        "marginMode": normalize_margin_mode(margin_mode),
        "clientOid": validate_client_oid(client_oid),
    }
    return ApiRequest(
        method="POST",
        endpoint_path="/api/v3/trade/place-order",
        body=json.dumps(payload, ensure_ascii=False, separators=(",", ":")),
    )


def build_cancel_order_request(
    category: str,
    symbol: str,
    order_id: str | None,
    client_oid: str | None,
) -> ApiRequest:
    normalized_order_id = order_id.strip() if order_id else ""
    normalized_client_oid = client_oid.strip() if client_oid else ""
    if normalized_order_id:
        identity = {"orderId": normalized_order_id}
    elif normalized_client_oid:
        identity = {"clientOid": validate_client_oid(normalized_client_oid)}
    else:
        raise ValueError("order_id or client_oid is required")
    payload = {
        **identity,
        "category": normalize_category(category),
        "symbol": normalize_symbol(symbol),
    }
    return ApiRequest(
        method="POST",
        endpoint_path="/api/v3/trade/cancel-order",
        body=json.dumps(payload, ensure_ascii=False, separators=(",", ":")),
    )


def validate_uta_response(payload: Any) -> Any:
    if not isinstance(payload, dict):
        raise RuntimeError("Bitget REST response must be an object")
    if payload.get("code") != "00000":
        raise RuntimeError(
            f"Bitget REST code={payload.get('code')} msg={payload.get('msg')}"
        )
    if "data" not in payload:
        raise RuntimeError("Bitget REST success response missing data")
    return payload["data"]


class SignedBitgetTradingClient:
    def __init__(
        self,
        api_key: str,
        api_secret: str,
        api_passphrase: str,
        base_url: str = DEFAULT_BASE_URL,
        timeout: float = DEFAULT_TIMEOUT,
        locale: str = "en-US",
    ):
        self.api_key = api_key
        self.api_secret = api_secret
        self.api_passphrase = api_passphrase
        self.base_url = base_url
        self.timeout = timeout
        self.locale = locale

    def request_json(self, api_request: ApiRequest) -> Any:
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
                passphrase=self.api_passphrase,
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
            raise RuntimeError("Bitget REST returned an empty response")
        try:
            payload = json.loads(body)
        except json.JSONDecodeError as exc:
            raise RuntimeError(f"invalid JSON response: {exc}") from exc
        return validate_uta_response(payload)


def place_order(
    requester: Requester,
    request: ApiRequest,
    execute: bool,
) -> dict[str, Any]:
    result: dict[str, Any] = {
        "executed": execute,
        "request": request.to_public_dict(),
    }
    if not execute:
        return result
    result["response"] = requester(request)
    return result


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build or explicitly submit one Bitget UTA futures market order."
    )
    parser.add_argument("--api-key", default=DEFAULT_API_KEY_ENV)
    parser.add_argument("--api-secret", default=DEFAULT_API_SECRET_ENV)
    parser.add_argument("--api-passphrase", default=DEFAULT_API_PASSPHRASE_ENV)
    parser.add_argument("--base-url", default=DEFAULT_BASE_URL)
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    parser.add_argument("--category", default="USDT-FUTURES")
    parser.add_argument("--symbol", default=DEFAULT_SYMBOL)
    parser.add_argument("--qty", required=True)
    parser.add_argument("--side", choices=("buy", "sell"), required=True)
    parser.add_argument("--margin-mode", choices=("crossed", "isolated"), default="crossed")
    parser.add_argument("--client-oid", required=True)
    parser.add_argument("--reduce-only", action="store_true")
    parser.add_argument("--execute", action="store_true")
    parser.add_argument(
        "--pretty", action=argparse.BooleanOptionalAction, default=True
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        request = build_place_order_request(
            category=args.category,
            symbol=args.symbol,
            qty=Decimal(args.qty),
            side=args.side,
            margin_mode=args.margin_mode,
            client_oid=args.client_oid,
            reduce_only=args.reduce_only,
        )
    except (InvalidOperation, ValueError) as exc:
        print(f"[FAIL] {exc}", file=sys.stderr)
        return 2

    if not args.execute:
        result = place_order(lambda request: {}, request, execute=False)
    else:
        credential_names = (
            args.api_key,
            args.api_secret,
            args.api_passphrase,
        )
        credential_values = [get_env_value(name) for name in credential_names]
        for name, value in zip(credential_names, credential_values):
            if value is None:
                print(f"[FAIL] missing env var {name}", file=sys.stderr)
                return 2
        client = SignedBitgetTradingClient(
            api_key=credential_values[0],
            api_secret=credential_values[1],
            api_passphrase=credential_values[2],
            base_url=args.base_url,
            timeout=args.timeout,
        )
        try:
            result = place_order(client.request_json, request, execute=True)
        except Exception as exc:  # pragma: no cover - utility CLI
            print(f"[FAIL] {type(exc).__name__}: {exc}", file=sys.stderr)
            return 1

    print(
        json.dumps(
            result,
            ensure_ascii=False,
            indent=2 if args.pretty else None,
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
