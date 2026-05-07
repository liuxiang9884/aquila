#!/home/liuxiang/dev/pyenv/lx/bin/python

import argparse
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


DEFAULT_BASE_URL = "https://api.gateio.ws/api/v4"
DEFAULT_TIMEOUT = 8.0
DEFAULT_API_KEY_ENV = "TEST_KEY"
DEFAULT_API_SECRET_ENV = "TEST_SECRET"
USER_AGENT = "aquila-gate-account-query/1.0"


@dataclass(frozen=True)
class ApiRequest:
    label: str
    endpoint_path: str
    query_string: str = ""


Requester = Callable[[ApiRequest], Any]


def normalize_settle_path(settle: str) -> str:
    return settle.strip().lower()


def normalize_settle_query(settle: str) -> str:
    return settle.strip().upper()


def build_query_string(params: Iterable[tuple[str, str | None]]) -> str:
    filtered = [(key, value) for key, value in params if value is not None and value != ""]
    return urllib.parse.urlencode(filtered)


def build_signature_headers(
    api_key: str,
    api_secret: str,
    method: str,
    request_path: str,
    query_string: str = "",
    body: str = "",
    timestamp: int | str | None = None,
) -> dict[str, str]:
    timestamp_text = str(int(time.time()) if timestamp is None else timestamp)
    body_hash = hashlib.sha512(body.encode("utf-8")).hexdigest()
    signature_text = "\n".join(
        [method.upper(), request_path, query_string, body_hash, timestamp_text]
    )
    signature = hmac.new(
        api_secret.encode("utf-8"), signature_text.encode("utf-8"), hashlib.sha512
    ).hexdigest()
    return {"KEY": api_key, "Timestamp": timestamp_text, "SIGN": signature}


def base_url_prefix(base_url: str) -> str:
    parsed = urllib.parse.urlsplit(base_url.rstrip("/"))
    return parsed.path or "/api/v4"


def request_path(base_url: str, endpoint_path: str) -> str:
    return f"{base_url_prefix(base_url).rstrip('/')}/{endpoint_path.lstrip('/')}"


def request_url(base_url: str, api_request: ApiRequest) -> str:
    url = f"{base_url.rstrip('/')}/{api_request.endpoint_path.lstrip('/')}"
    if api_request.query_string:
        return f"{url}?{api_request.query_string}"
    return url


def build_query_plan(
    settle: str,
    currency: str | None,
    currency_pair: str | None,
    futures_contracts: Iterable[str],
) -> list[ApiRequest]:
    settle_path = normalize_settle_path(settle)
    settle_query = normalize_settle_query(settle)
    requests = [
        ApiRequest(
            label="wallet_total_balance",
            endpoint_path="/wallet/total_balance",
            query_string=build_query_string([("currency", currency.upper() if currency else None)]),
        ),
        ApiRequest(label="futures_account", endpoint_path=f"/futures/{settle_path}/accounts"),
        ApiRequest(
            label="wallet_fee",
            endpoint_path="/wallet/fee",
            query_string=build_query_string(
                [
                    ("currency_pair", currency_pair.strip().upper() if currency_pair else None),
                    ("settle", settle_query),
                ]
            ),
        ),
    ]

    normalized_contracts = [contract.strip().upper() for contract in futures_contracts if contract.strip()]
    if not normalized_contracts:
        requests.append(ApiRequest(label="futures_fee", endpoint_path=f"/futures/{settle_path}/fee"))
        return requests

    for contract in normalized_contracts:
        requests.append(
            ApiRequest(
                label=f"futures_fee_{contract}",
                endpoint_path=f"/futures/{settle_path}/fee",
                query_string=build_query_string([("contract", contract)]),
            )
        )
    return requests


class SignedGateRestClient:
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

    def get_json(self, api_request: ApiRequest) -> Any:
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


def query_account_info(
    requester: Requester,
    settle: str,
    currency: str | None,
    currency_pair: str | None,
    futures_contracts: Iterable[str],
    allow_partial: bool = False,
) -> dict[str, Any]:
    results: dict[str, Any] = {}
    errors: dict[str, str] = {}
    for api_request in build_query_plan(
        settle=settle,
        currency=currency,
        currency_pair=currency_pair,
        futures_contracts=futures_contracts,
    ):
        try:
            results[api_request.label] = requester(api_request)
        except Exception as exc:
            if not allow_partial:
                raise
            errors[api_request.label] = f"{type(exc).__name__}: {exc}"
    return {"ok": not errors, "results": results, "errors": errors}


def get_env_value(name: str) -> str | None:
    value = os.getenv(name)
    return value if value else None


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Query Gate read-only account balances and fee rates with APIv4 credentials."
    )
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
        "--currency",
        default="USDT",
        help="Currency used by /wallet/total_balance; empty string omits this query parameter.",
    )
    parser.add_argument(
        "--currency-pair",
        help="Optional spot currency pair for /wallet/fee, e.g. BTC_USDT.",
    )
    parser.add_argument(
        "--contract",
        action="append",
        default=[],
        help=(
            "Optional futures contract for /futures/{settle}/fee. "
            "Can be repeated; omitted means query the endpoint without contract."
        ),
    )
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

    client = SignedGateRestClient(
        api_key=api_key,
        api_secret=api_secret,
        base_url=args.base_url,
        timeout=args.timeout,
    )
    try:
        result = query_account_info(
            requester=client.get_json,
            settle=args.settle,
            currency=args.currency,
            currency_pair=args.currency_pair,
            futures_contracts=args.contract,
            allow_partial=args.allow_partial,
        )
    except Exception as exc:  # pragma: no cover - utility script
        print(f"[FAIL] {type(exc).__name__}: {exc}", file=sys.stderr)
        return 1

    indent = 2 if args.pretty else None
    print(json.dumps(result, ensure_ascii=False, indent=indent, sort_keys=True))
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
