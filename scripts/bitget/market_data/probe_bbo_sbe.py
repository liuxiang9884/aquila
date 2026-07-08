#!/home/liuxiang/dev/pyenv/lx/bin/python

import argparse
import json
import struct
import sys
from dataclasses import asdict, dataclass
from decimal import Decimal
from typing import Any


DEFAULT_URL = "wss://vip-ws-uta.bitget.com/v3/ws/public/sbe"
DEFAULT_SYMBOL = "BTCUSDT"
DEFAULT_INST_TYPE = "usdt-futures"
TOPIC = "books1"
BBO_TEMPLATE_ID = 1002


@dataclass(frozen=True)
class SbeHeader:
    block_length: int
    template_id: int
    schema_id: int
    version: int


@dataclass(frozen=True)
class BboFrame:
    header: SbeHeader
    ts: int
    bid1_price: str
    bid1_size: str
    ask1_price: str
    ask1_size: str
    price_exponent: int
    size_exponent: int
    seq: int
    sts: int | None
    category: int | None
    symbol: str


def scaled_decimal(mantissa: int, exponent: int) -> str:
    return str(Decimal(mantissa) * (Decimal(10) ** exponent))


def decode_header(data: bytes) -> SbeHeader:
    if len(data) < 8:
        raise ValueError(f"SBE frame too short for header: {len(data)} bytes")
    block_length, template_id, schema_id, version = struct.unpack_from("<HHHH", data, 0)
    if template_id != BBO_TEMPLATE_ID:
        raise ValueError(f"unexpected templateId: {template_id}")
    return SbeHeader(
        block_length=block_length,
        template_id=template_id,
        schema_id=schema_id,
        version=version,
    )


def decode_bbo_frame(data: bytes) -> BboFrame:
    header = decode_header(data)
    base_offset = 8
    root_end = base_offset + header.block_length
    if len(data) < root_end + 1:
        raise ValueError(
            f"SBE frame too short for root block and symbol length: "
            f"{len(data)} bytes, need at least {root_end + 1}"
        )

    offset = base_offset
    ts = struct.unpack_from("<Q", data, offset)[0]
    offset += 8
    bid1_price = struct.unpack_from("<q", data, offset)[0]
    offset += 8
    bid1_size = struct.unpack_from("<q", data, offset)[0]
    offset += 8
    ask1_price = struct.unpack_from("<q", data, offset)[0]
    offset += 8
    ask1_size = struct.unpack_from("<q", data, offset)[0]
    offset += 8
    price_exponent = struct.unpack_from("<b", data, offset)[0]
    offset += 1
    size_exponent = struct.unpack_from("<b", data, offset)[0]
    offset += 1
    seq = struct.unpack_from("<Q", data, offset)[0]
    offset += 8

    sts: int | None = None
    category: int | None = None
    if root_end - offset >= 9:
        sts = struct.unpack_from("<Q", data, offset)[0]
        offset += 8
        category = struct.unpack_from("<B", data, offset)[0]
        offset += 1

    symbol_length = struct.unpack_from("<B", data, root_end)[0]
    symbol_start = root_end + 1
    symbol_end = symbol_start + symbol_length
    if len(data) < symbol_end:
        raise ValueError(
            f"SBE frame too short for symbol: {len(data)} bytes, need {symbol_end}"
        )
    symbol = data[symbol_start:symbol_end].decode("utf-8")

    return BboFrame(
        header=header,
        ts=ts,
        bid1_price=scaled_decimal(bid1_price, price_exponent),
        bid1_size=scaled_decimal(bid1_size, size_exponent),
        ask1_price=scaled_decimal(ask1_price, price_exponent),
        ask1_size=scaled_decimal(ask1_size, size_exponent),
        price_exponent=price_exponent,
        size_exponent=size_exponent,
        seq=seq,
        sts=sts,
        category=category,
        symbol=symbol,
    )


def build_subscribe_message(inst_type: str, symbol: str) -> str:
    payload = {
        "op": "subscribe",
        "args": [{"instType": inst_type, "topic": TOPIC, "symbol": symbol}],
    }
    return json.dumps(payload, separators=(",", ":"))


def bbo_to_dict(frame: BboFrame) -> dict[str, Any]:
    payload = asdict(frame)
    payload["header"] = asdict(frame.header)
    return payload


def run_probe(args: argparse.Namespace) -> int:
    try:
        import websocket
    except ImportError as exc:  # pragma: no cover - depends on local Python env
        raise SystemExit(
            "missing dependency: websocket-client. Install with `pip install websocket-client`."
        ) from exc

    ws = websocket.create_connection(args.url, timeout=args.timeout)
    try:
        ws.send(build_subscribe_message(args.inst_type, args.symbol))
        decoded_count = 0
        text_count = 0
        while decoded_count < args.count:
            message = ws.recv()
            if isinstance(message, bytes):
                frame = decode_bbo_frame(message)
                decoded_count += 1
                print(
                    json.dumps(
                        bbo_to_dict(frame), ensure_ascii=False, separators=(",", ":")
                    )
                )
                continue

            text_count += 1
            if args.print_text:
                print(message)
            if text_count > args.max_text_messages:
                raise RuntimeError(
                    f"received {text_count} text messages without {args.count} BBO frames"
                )
    finally:
        ws.close()
    return 0


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Probe Bitget UTA public SBE books1 BBO and print decoded frames as JSON."
    )
    parser.add_argument(
        "--url",
        default=DEFAULT_URL,
        help=(
            "WebSocket SBE URL. Default is Bitget VIP/LOLA UTA public SBE; "
            "official non-VIP URL is wss://ws.bitget.com/v3/ws/public/sbe."
        ),
    )
    parser.add_argument("--inst-type", default=DEFAULT_INST_TYPE)
    parser.add_argument("--symbol", default=DEFAULT_SYMBOL)
    parser.add_argument("--count", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=8.0)
    parser.add_argument("--max-text-messages", type=int, default=8)
    parser.add_argument(
        "--print-text",
        action="store_true",
        help="Print subscription acknowledgements and other text frames.",
    )
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    return run_probe(parse_args(argv))


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
