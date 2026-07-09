#!/home/liuxiang/dev/pyenv/lx/bin/python

import argparse
import hashlib
import hmac
import json
import os
import ssl
import sys
import time
from dataclasses import dataclass

import websocket


DEFAULT_TIMEOUT = 8.0
DEFAULT_API_KEY_ENV = "GATE_TEST_KEY"
DEFAULT_API_SECRET_ENV = "GATE_TEST_SECRET"
LOGIN_CHANNEL = "futures.login"


@dataclass
class TestResult:
    url: str
    ok: bool
    detail: str


def build_login_signature(secret: str, channel: str, timestamp: int) -> str:
    message = f"api\n{channel}\n\n{timestamp}"
    return hmac.new(secret.encode(), message.encode(), hashlib.sha512).hexdigest()


def build_login_request(api_key: str, api_secret: str) -> dict:
    timestamp = int(time.time())
    return {
        "time": timestamp,
        "channel": LOGIN_CHANNEL,
        "event": "api",
        "payload": {
            "api_key": api_key,
            "signature": build_login_signature(api_secret, LOGIN_CHANNEL, timestamp),
            "timestamp": str(timestamp),
            "req_id": f"login-{int(time.time() * 1000)}",
        },
    }


def parse_login_response(message: str) -> TestResult:
    try:
        payload = json.loads(message)
    except json.JSONDecodeError as exc:
        return TestResult(url="", ok=False, detail=f"invalid login response: {exc}")

    header = payload.get("header", {})
    data = payload.get("data", {})
    status = str(header.get("status", ""))
    if status == "200":
        result = data.get("result", {})
        uid = result.get("uid")
        detail = "login succeeded"
        if uid:
            detail = f"login succeeded (uid={uid})"
        return TestResult(url="", ok=True, detail=detail)

    errs = data.get("errs", {})
    label = errs.get("label")
    error_message = errs.get("message")
    parts = [part for part in [f"status={status}" if status else None, label, error_message] if part]
    if not parts:
        parts = [f"unexpected login response: {message}"]
    return TestResult(url="", ok=False, detail=", ".join(parts))


def get_env_value(name: str) -> str | None:
    value = os.getenv(name)
    return value if value else None


def test_connection(
    url: str,
    timeout: float,
    api_key_env: str | None = None,
    api_secret_env: str | None = None,
) -> TestResult:
    kwargs = {"timeout": timeout}
    if url.startswith("wss://"):
        kwargs["sslopt"] = {"cert_reqs": ssl.CERT_REQUIRED}

    api_key = None
    api_secret = None
    if api_key_env is not None and api_secret_env is not None:
        api_key = get_env_value(api_key_env)
        if api_key is None:
            return TestResult(url=url, ok=False, detail=f"missing env var {api_key_env}")
        api_secret = get_env_value(api_secret_env)
        if api_secret is None:
            return TestResult(url=url, ok=False, detail=f"missing env var {api_secret_env}")

    ws = None
    try:
        ws = websocket.create_connection(url, **kwargs)
        if api_key is None or api_secret is None:
            return TestResult(url=url, ok=True, detail="handshake succeeded")

        ws.send(json.dumps(build_login_request(api_key, api_secret)))
        login_result = parse_login_response(ws.recv())
        return TestResult(url=url, ok=login_result.ok, detail=login_result.detail)
    except Exception as exc:  # pragma: no cover - utility script
        return TestResult(url=url, ok=False, detail=f"{type(exc).__name__}: {exc}")
    finally:
        if ws is not None:
            try:
                ws.close()
            except Exception:
                pass


def parse_args(argv: list[str]) -> tuple[argparse.Namespace, bool]:
    login_requested = "--api-key" in argv or "--api-secret" in argv
    parser = argparse.ArgumentParser(
        description="Test Gate websocket endpoint connectivity."
    )
    parser.add_argument(
        "urls",
        nargs="*",
        default=[
            "wss://fx-ws.gateio.ws/v4/ws/usdt",
            "ws://fx-ws.gateio.ws/v4/ws/usdt",
        ],
        help="WebSocket URLs to test.",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=DEFAULT_TIMEOUT,
        help="Connection timeout in seconds.",
    )
    parser.add_argument(
        "--api-key",
        default=DEFAULT_API_KEY_ENV,
        help=(
            "Environment variable name holding the Gate API key. "
            "Login test is enabled only when this option or --api-secret is passed."
        ),
    )
    parser.add_argument(
        "--api-secret",
        default=DEFAULT_API_SECRET_ENV,
        help=(
            "Environment variable name holding the Gate API secret. "
            "Login test is enabled only when this option or --api-key is passed."
        ),
    )
    return parser.parse_args(argv), login_requested


def main() -> int:
    args, login_requested = parse_args(sys.argv[1:])

    api_key_env = args.api_key if login_requested else None
    api_secret_env = args.api_secret if login_requested else None
    results = [
        test_connection(url, args.timeout, api_key_env=api_key_env, api_secret_env=api_secret_env)
        for url in args.urls
    ]
    for result in results:
        status = "OK" if result.ok else "FAIL"
        print(f"[{status}] {result.url} -> {result.detail}")

    return 0 if all(result.ok for result in results) else 1


if __name__ == "__main__":
    sys.exit(main())
