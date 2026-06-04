#!/home/liuxiang/dev/pyenv/lx/bin/python

from decimal import Decimal
import sys
import unittest
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parents[1]
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import lead_lag_replay_pnl as pnl


class LeadLagReplayPnlTest(unittest.TestCase):
    def test_calculates_long_and_short_net_pnl(self):
        rows = [
            {
                "symbol_id": "3",
                "exchange_ns": "100",
                "local_ns": "100",
                "event_ns": "100",
                "action": "kOpenLong",
                "side": "kBuy",
                "raw_price": "100",
                "reduce_only": "false",
            },
            {
                "symbol_id": "3",
                "exchange_ns": "110",
                "local_ns": "110",
                "event_ns": "110",
                "action": "kCloseLong",
                "side": "kSell",
                "raw_price": "110",
                "reduce_only": "true",
            },
            {
                "symbol_id": "3",
                "exchange_ns": "120",
                "local_ns": "120",
                "event_ns": "120",
                "action": "kOpenShort",
                "side": "kSell",
                "raw_price": "200",
                "reduce_only": "false",
            },
            {
                "symbol_id": "3",
                "exchange_ns": "130",
                "local_ns": "130",
                "event_ns": "130",
                "action": "kStoplossShort",
                "side": "kBuy",
                "raw_price": "180",
                "reduce_only": "true",
            },
        ]

        result = pnl.calculate_pnl(
            rows, open_notional=Decimal("1000"), fee_rate=Decimal("0.0002")
        )

        self.assertEqual(result.summary.closed_trades, 2)
        self.assertEqual(result.summary.open_positions, 0)
        self.assertEqual(result.summary.gross_pnl, Decimal("200"))
        self.assertEqual(result.summary.fees, Decimal("0.80"))
        self.assertEqual(result.summary.net_pnl, Decimal("199.20"))
        self.assertEqual(result.trades[0].gross_pnl, Decimal("100"))
        self.assertEqual(result.trades[0].fees, Decimal("0.4200"))
        self.assertEqual(result.trades[0].net_pnl, Decimal("99.5800"))
        self.assertEqual(result.trades[1].gross_pnl, Decimal("100"))
        self.assertEqual(result.trades[1].fees, Decimal("0.3800"))
        self.assertEqual(result.trades[1].net_pnl, Decimal("99.6200"))

    def test_rejects_close_without_open(self):
        rows = [
            {
                "symbol_id": "3",
                "exchange_ns": "100",
                "local_ns": "100",
                "event_ns": "100",
                "action": "kCloseLong",
                "side": "kSell",
                "raw_price": "110",
                "reduce_only": "true",
            }
        ]

        with self.assertRaisesRegex(ValueError, "close without open"):
            pnl.calculate_pnl(
                rows, open_notional=Decimal("1000"), fee_rate=Decimal("0.0002")
            )

    def test_applies_slippage_against_signal_side(self):
        rows = [
            {
                "symbol_id": "3",
                "exchange_ns": "100",
                "local_ns": "100",
                "event_ns": "100",
                "action": "kOpenLong",
                "side": "kBuy",
                "raw_price": "10",
                "reduce_only": "false",
            },
            {
                "symbol_id": "3",
                "exchange_ns": "110",
                "local_ns": "110",
                "event_ns": "110",
                "action": "kCloseLong",
                "side": "kSell",
                "raw_price": "13",
                "reduce_only": "true",
            },
        ]

        result = pnl.calculate_pnl(
            rows,
            open_notional=Decimal("100"),
            fee_rate=Decimal("0"),
            tick_size=Decimal("1"),
            slippage_ticks=1,
        )

        self.assertEqual(result.trades[0].open_price, Decimal("11"))
        self.assertEqual(result.trades[0].close_price, Decimal("12"))
        self.assertEqual(result.trades[0].gross_pnl, Decimal("100") / Decimal("11"))
        self.assertEqual(result.summary.net_pnl, Decimal("100") / Decimal("11"))


if __name__ == "__main__":
    unittest.main()
