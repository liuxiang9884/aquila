#!/home/liuxiang/dev/pyenv/lx/bin/python

import argparse
import base64
import hashlib
import hmac
import json
import os
import socket
import ssl
import sys
import time
from dataclasses import dataclass, asdict
from typing import BinaryIO


DEFAULT_HOST = "fx-ws.gateio.ws"
DEFAULT_PORT = "443"
DEFAULT_TARGET = "/v4/ws/usdt"
DEFAULT_TIMEOUT = 8.0
DEFAULT_API_KEY_ENV = "TEST_KEY"
DEFAULT_API_SECRET_ENV = "TEST_SECRET"
LOGIN_CHANNEL = "futures.login"
SIZE_DECIMAL_HEADER = "X-Gate-Size-Decimal: 1"


@dataclass(frozen=True)
class LoginParseResult:
    ok: bool
    status: str = ""
    uid: str = ""
    request_id: str = ""
    conn_id: str = ""
    conn_trace_id: str = ""
    detail: str = ""


@dataclass
class ProbeResult:
    connect_ip: str
    host: str
    port: str
    target: str
    tls: bool
    ok: bool
    websocket_status: str = ""
    local_endpoint: str = ""
    remote_endpoint: str = ""
    tcp_connect_ms: float | None = None
    tls_handshake_ms: float | None = None
    websocket_handshake_ms: float | None = None
    login_ms: float | None = None
    total_ms: float | None = None
    login_ok: bool | None = None
    login_status: str = ""
    login_uid: str = ""
    login_request_id: str = ""
    conn_id: str = ""
    conn_trace_id: str = ""
    error: str = ""


def build_login_signature(secret: str, channel: str, timestamp: int) -> str:
    message = f"api\n{channel}\n\n{timestamp}"
    return hmac.new(secret.encode(), message.encode(), hashlib.sha512).hexdigest()


def build_login_request(
    api_key: str,
    api_secret: str,
    request_id: str,
    timestamp: int | None = None,
) -> str:
    ts = int(time.time()) if timestamp is None else timestamp
    payload = {
        "time": ts,
        "channel": LOGIN_CHANNEL,
        "event": "api",
        "payload": {
            "api_key": api_key,
            "signature": build_login_signature(api_secret, LOGIN_CHANNEL, ts),
            "timestamp": str(ts),
            "req_id": request_id,
        },
    }
    return json.dumps(payload, ensure_ascii=False, separators=(",", ":"))


def parse_login_response(message: str) -> LoginParseResult:
    try:
        payload = json.loads(message)
    except json.JSONDecodeError as exc:
        return LoginParseResult(ok=False, detail=f"invalid json: {exc}")

    header = payload.get("header") or {}
    data = payload.get("data") or {}
    status = str(header.get("status", ""))
    request_id = str(payload.get("request_id", ""))
    conn_id = str(header.get("conn_id", ""))
    conn_trace_id = str(header.get("conn_trace_id", ""))
    if status == "200":
        result = data.get("result") or {}
        uid = str(result.get("uid", ""))
        return LoginParseResult(
            ok=True,
            status=status,
            uid=uid,
            request_id=request_id,
            conn_id=conn_id,
            conn_trace_id=conn_trace_id,
            detail="login succeeded",
        )

    errs = data.get("errs") or {}
    parts = [
        part
        for part in [
            f"status={status}" if status else None,
            str(errs.get("label")) if errs.get("label") else None,
            str(errs.get("message")) if errs.get("message") else None,
        ]
        if part
    ]
    if not parts:
        parts = [f"unexpected response: {message}"]
    return LoginParseResult(
        ok=False,
        status=status,
        request_id=request_id,
        conn_id=conn_id,
        conn_trace_id=conn_trace_id,
        detail=", ".join(parts),
    )


def build_handshake_request(
    host: str,
    target: str,
    websocket_key: str,
    extra_headers: list[str] | None = None,
) -> bytes:
    lines = [
        f"GET {target} HTTP/1.1",
        f"Host: {host}",
        "Upgrade: websocket",
        "Connection: Upgrade",
        f"Sec-WebSocket-Key: {websocket_key}",
        "Sec-WebSocket-Version: 13",
    ]
    lines.extend(extra_headers or [])
    return ("\r\n".join(lines) + "\r\n\r\n").encode("ascii")


def encode_client_text_frame(text: str, mask_key: bytes | None = None) -> bytes:
    payload = text.encode("utf-8")
    if mask_key is None:
        mask_key = os.urandom(4)
    if len(mask_key) != 4:
        raise ValueError("mask_key must be exactly 4 bytes")

    header = bytearray([0x81])
    length = len(payload)
    if length < 126:
        header.append(0x80 | length)
    elif length <= 0xFFFF:
        header.append(0x80 | 126)
        header.extend(length.to_bytes(2, "big"))
    else:
        header.append(0x80 | 127)
        header.extend(length.to_bytes(8, "big"))

    masked = bytes(value ^ mask_key[index % 4] for index, value in enumerate(payload))
    return bytes(header) + mask_key + masked


def recv_exact(stream: BinaryIO, size: int) -> bytes:
    chunks: list[bytes] = []
    remaining = size
    while remaining > 0:
        chunk = stream.recv(remaining)  # type: ignore[attr-defined]
        if not chunk:
            raise RuntimeError("socket closed while reading websocket frame")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def recv_text_frame(stream: BinaryIO) -> str:
    while True:
        header = recv_exact(stream, 2)
        first, second = header[0], header[1]
        opcode = first & 0x0F
        masked = (second & 0x80) != 0
        length = second & 0x7F
        if length == 126:
            length = int.from_bytes(recv_exact(stream, 2), "big")
        elif length == 127:
            length = int.from_bytes(recv_exact(stream, 8), "big")

        mask_key = recv_exact(stream, 4) if masked else b""
        payload = recv_exact(stream, length)
        if masked:
            payload = bytes(
                value ^ mask_key[index % 4] for index, value in enumerate(payload)
            )

        if opcode == 0x1:
            return payload.decode("utf-8", errors="replace")
        if opcode == 0x8:
            raise RuntimeError("websocket close frame received")
        # Ignore continuation/control frames for this narrow probe.


def get_env_value(name: str) -> str | None:
    value = os.getenv(name)
    return value if value else None


def format_endpoint(sockname: tuple) -> str:
    if len(sockname) < 2:
        return ""
    return f"{sockname[0]}:{sockname[1]}"


def probe_connect_ip(
    connect_ip: str,
    host: str,
    port: str,
    target: str,
    timeout: float,
    use_tls: bool,
    login: bool,
    api_key: str | None,
    api_secret: str | None,
    size_decimal_header: bool,
) -> ProbeResult:
    result = ProbeResult(
        connect_ip=connect_ip,
        host=host,
        port=port,
        target=target,
        tls=use_tls,
        ok=False,
    )
    total_start = time.perf_counter_ns()
    sock = None
    try:
        tcp_start = time.perf_counter_ns()
        raw = socket.create_connection((connect_ip, int(port)), timeout=timeout)
        tcp_done = time.perf_counter_ns()
        raw.settimeout(timeout)
        raw.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        result.tcp_connect_ms = (tcp_done - tcp_start) / 1_000_000

        if use_tls:
            context = ssl.create_default_context()
            tls_start = time.perf_counter_ns()
            sock = context.wrap_socket(raw, server_hostname=host)
            tls_done = time.perf_counter_ns()
            result.tls_handshake_ms = (tls_done - tls_start) / 1_000_000
        else:
            sock = raw

        result.local_endpoint = format_endpoint(sock.getsockname())
        result.remote_endpoint = format_endpoint(sock.getpeername())

        websocket_key = base64.b64encode(os.urandom(16)).decode("ascii")
        headers = [SIZE_DECIMAL_HEADER] if size_decimal_header else []
        request = build_handshake_request(host, target, websocket_key, headers)
        ws_start = time.perf_counter_ns()
        sock.sendall(request)
        data = b""
        while b"\r\n\r\n" not in data and len(data) < 8192:
            chunk = sock.recv(4096)
            if not chunk:
                break
            data += chunk
        ws_done = time.perf_counter_ns()
        result.websocket_handshake_ms = (ws_done - ws_start) / 1_000_000
        status_line = data.split(b"\r\n", 1)[0].decode("ascii", errors="replace")
        result.websocket_status = status_line
        if " 101 " not in f" {status_line} ":
            result.error = f"websocket handshake failed: {status_line}"
            return result

        if login:
            if api_key is None or api_secret is None:
                result.error = "login requested but credentials are missing"
                return result
            request_id = f"ip-probe-login-{int(time.time() * 1000)}"
            login_request = build_login_request(api_key, api_secret, request_id)
            login_start = time.perf_counter_ns()
            sock.sendall(encode_client_text_frame(login_request))
            login_response = recv_text_frame(sock)
            login_done = time.perf_counter_ns()
            result.login_ms = (login_done - login_start) / 1_000_000
            parsed = parse_login_response(login_response)
            result.login_ok = parsed.ok
            result.login_status = parsed.status
            result.login_uid = parsed.uid
            result.login_request_id = parsed.request_id
            result.conn_id = parsed.conn_id
            result.conn_trace_id = parsed.conn_trace_id
            if not parsed.ok:
                result.error = parsed.detail
                return result

        result.ok = True
        return result
    except Exception as exc:  # pragma: no cover - exercised by live probes
        result.error = f"{type(exc).__name__}: {exc}"
        return result
    finally:
        result.total_ms = (time.perf_counter_ns() - total_start) / 1_000_000
        if sock is not None:
            try:
                sock.close()
            except Exception:
                pass


def result_to_public_dict(result: ProbeResult) -> dict:
    return {key: value for key, value in asdict(result).items() if value is not None}


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Probe Gate WebSocket IP pinning: TCP connects to --connect-ip while "
            "--host remains the TLS SNI and WebSocket Host header."
        )
    )
    parser.add_argument(
        "--connect-ip",
        action="append",
        required=True,
        help="Remote IP to connect. Repeat to test multiple IPs.",
    )
    parser.add_argument("--host", default=DEFAULT_HOST, help="TLS SNI and WebSocket Host.")
    parser.add_argument("--port", default=DEFAULT_PORT, help="Remote TCP port.")
    parser.add_argument("--target", default=DEFAULT_TARGET, help="WebSocket target path.")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    parser.add_argument("--plain", action="store_true", help="Disable TLS.")
    parser.add_argument(
        "--login",
        action="store_true",
        help="After WebSocket handshake, send futures.login only. Never sends orders.",
    )
    parser.add_argument("--api-key", default=DEFAULT_API_KEY_ENV)
    parser.add_argument("--api-secret", default=DEFAULT_API_SECRET_ENV)
    parser.add_argument(
        "--no-size-decimal-header",
        action="store_true",
        help="Do not send X-Gate-Size-Decimal: 1 in the WebSocket handshake.",
    )
    parser.add_argument("--pretty", action="store_true", help="Pretty-print JSON output.")
    return parser.parse_args(argv)


def main() -> int:
    args = parse_args(sys.argv[1:])
    if args.timeout <= 0:
        print("[FAIL] --timeout must be positive", file=sys.stderr)
        return 2

    api_key = None
    api_secret = None
    if args.login:
        api_key = get_env_value(args.api_key)
        if api_key is None:
            print(f"[FAIL] missing env var {args.api_key}", file=sys.stderr)
            return 2
        api_secret = get_env_value(args.api_secret)
        if api_secret is None:
            print(f"[FAIL] missing env var {args.api_secret}", file=sys.stderr)
            return 2

    results = [
        probe_connect_ip(
            connect_ip=connect_ip,
            host=args.host,
            port=args.port,
            target=args.target,
            timeout=args.timeout,
            use_tls=not args.plain,
            login=args.login,
            api_key=api_key,
            api_secret=api_secret,
            size_decimal_header=not args.no_size_decimal_header,
        )
        for connect_ip in args.connect_ip
    ]
    public_results = [result_to_public_dict(result) for result in results]
    if args.pretty:
        print(json.dumps(public_results, ensure_ascii=False, indent=2, sort_keys=True))
    else:
        for result in public_results:
            print(json.dumps(result, ensure_ascii=False, sort_keys=True))
    return 0 if all(result.ok for result in results) else 1


if __name__ == "__main__":
    sys.exit(main())
