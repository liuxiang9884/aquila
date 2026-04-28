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

try:
    import websocket
except ImportError as exc:  # pragma: no cover - utility script
    raise SystemExit(
        "missing dependency: websocket-client. Install with `pip install websocket-client`."
    ) from exc


DEFAULT_URL = "wss://fx-ws.gateio.ws/v4/ws/usdt"
DEFAULT_TIMEOUT = 8.0
DEFAULT_API_KEY_ENV = "TEST_KEY"
DEFAULT_API_SECRET_ENV = "TEST_SECRET"
LOGIN_CHANNEL = "futures.login"
SIZE_DECIMAL_HEADER = "X-Gate-Size-Decimal: 1"


@dataclass
class LoginResult:
    name: str
    ok: bool
    uid: str
    request_id: str
    conn_id: str
    conn_trace_id: str
    detail: str


def build_login_signature(secret: str, channel: str, timestamp: int) -> str:
    message = f"api\n{channel}\n\n{timestamp}"
    return hmac.new(secret.encode(), message.encode(), hashlib.sha512).hexdigest()


def build_login_request(api_key: str, api_secret: str, name: str) -> dict:
    timestamp = int(time.time())
    request_id = f"{name}-login-{int(time.time() * 1000)}"
    return {
        "time": timestamp,
        "channel": LOGIN_CHANNEL,
        "event": "api",
        "payload": {
            "api_key": api_key,
            "signature": build_login_signature(api_secret, LOGIN_CHANNEL, timestamp),
            "timestamp": str(timestamp),
            "req_id": request_id,
        },
    }


def parse_login_response(name: str, message: str) -> LoginResult:
    try:
        payload = json.loads(message)
    except json.JSONDecodeError as exc:
        return LoginResult(name, False, "", "", "", "", f"invalid json: {exc}")

    header = payload.get("header") or {}
    data = payload.get("data") or {}
    request_id = str(payload.get("request_id", ""))
    status = str(header.get("status", ""))
    conn_id = str(header.get("conn_id", ""))
    conn_trace_id = str(header.get("conn_trace_id", ""))

    if status == "200":
        result = data.get("result") or {}
        uid = str(result.get("uid", ""))
        return LoginResult(
            name=name,
            ok=True,
            uid=uid,
            request_id=request_id,
            conn_id=conn_id,
            conn_trace_id=conn_trace_id,
            detail="login succeeded",
        )

    errs = data.get("errs") or {}
    label = errs.get("label")
    error_message = errs.get("message")
    parts = [
        part
        for part in [
            f"status={status}" if status else None,
            str(label) if label else None,
            str(error_message) if error_message else None,
        ]
        if part
    ]
    if not parts:
        parts = [f"unexpected response: {message}"]
    return LoginResult(name, False, "", request_id, conn_id, conn_trace_id, ", ".join(parts))


def create_ws(url: str, timeout: float):
    kwargs = {
        "timeout": timeout,
        "header": [SIZE_DECIMAL_HEADER],
    }
    if url.startswith("wss://"):
        kwargs["sslopt"] = {"cert_reqs": ssl.CERT_REQUIRED}
    return websocket.create_connection(url, **kwargs)


def login_pair(url: str, timeout: float, api_key: str, api_secret: str) -> list[LoginResult]:
    sockets = []
    try:
        for name in ("A", "B"):
            sockets.append((name, create_ws(url, timeout)))

        for name, ws in sockets:
            request = build_login_request(api_key, api_secret, name)
            ws.send(json.dumps(request, separators=(",", ":")))

        results = []
        for name, ws in sockets:
            results.append(parse_login_response(name, ws.recv()))
        return results
    finally:
        for _, ws in sockets:
            try:
                ws.close()
            except Exception:
                pass


def get_required_env(name: str) -> str:
    value = os.getenv(name)
    if not value:
        raise RuntimeError(f"missing env var {name}")
    return value


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Open two Gate futures WebSocket connections and login both with the same account."
    )
    parser.add_argument("--url", default=DEFAULT_URL, help="Gate futures WebSocket URL.")
    parser.add_argument(
        "--timeout",
        type=float,
        default=DEFAULT_TIMEOUT,
        help="Connection and recv timeout in seconds.",
    )
    parser.add_argument(
        "--api-key-env",
        default=DEFAULT_API_KEY_ENV,
        help="Environment variable holding the Gate API key.",
    )
    parser.add_argument(
        "--api-secret-env",
        default=DEFAULT_API_SECRET_ENV,
        help="Environment variable holding the Gate API secret.",
    )
    return parser.parse_args(argv)


def main() -> int:
    args = parse_args(sys.argv[1:])
    try:
        api_key = get_required_env(args.api_key_env)
        api_secret = get_required_env(args.api_secret_env)
        results = login_pair(args.url, args.timeout, api_key, api_secret)
    except Exception as exc:  # pragma: no cover - utility script
        print(f"[FAIL] {type(exc).__name__}: {exc}", file=sys.stderr)
        return 1

    for result in results:
        status = "OK" if result.ok else "FAIL"
        print(
            f"[{status}] ws={result.name} request_id={result.request_id} "
            f"uid={result.uid or '-'} conn_id={result.conn_id or '-'} "
            f"conn_trace_id={result.conn_trace_id or '-'} detail={result.detail}"
        )

    same_uid = len({result.uid for result in results if result.uid}) == 1
    all_ok = all(result.ok for result in results)
    if all_ok and same_uid:
        print("result=both_logged_in_same_account")
        return 0

    print("result=dual_login_failed_or_uid_mismatch")
    return 1


if __name__ == "__main__":
    sys.exit(main())
