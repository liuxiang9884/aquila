#!/home/liuxiang/dev/pyenv/lx/bin/python

import argparse
import ssl
import sys
from dataclasses import dataclass

import websocket


DEFAULT_TIMEOUT = 8.0


@dataclass
class TestResult:
    url: str
    ok: bool
    detail: str


def test_connection(url: str, timeout: float) -> TestResult:
    kwargs = {"timeout": timeout}
    if url.startswith("wss://"):
        kwargs["sslopt"] = {"cert_reqs": ssl.CERT_REQUIRED}

    ws = None
    try:
        ws = websocket.create_connection(url, **kwargs)
        return TestResult(url=url, ok=True, detail="handshake succeeded")
    except Exception as exc:  # pragma: no cover - utility script
        return TestResult(url=url, ok=False, detail=f"{type(exc).__name__}: {exc}")
    finally:
        if ws is not None:
            try:
                ws.close()
            except Exception:
                pass


def main() -> int:
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
    args = parser.parse_args()

    results = [test_connection(url, args.timeout) for url in args.urls]
    for result in results:
        status = "OK" if result.ok else "FAIL"
        print(f"[{status}] {result.url} -> {result.detail}")

    return 0 if all(result.ok for result in results) else 1


if __name__ == "__main__":
    sys.exit(main())
