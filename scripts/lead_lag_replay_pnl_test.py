#!/home/liuxiang/dev/pyenv/lx/bin/python

from decimal import Decimal
import unittest

import lead_lag_replay_pnl as pnl


class LeadLagReplayPnlTest(unittest.TestCase):
    def test_calculates_long_and_short_net_pnl(self):
        rows = [
            {
                "ticker_id": "1",
                "symbol_id": "3",
                "exchange_ns": "100",
                "local_ns": "100",
                "action": "kOpenLong",
                "side": "kBuy",
                "price": "100",
                "reduce_only": "false",
            },
            {
                "ticker_id": "2",
                "symbol_id": "3",
                "exchange_ns": "110",
                "local_ns": "110",
                "action": "kCloseLong",
                "side": "kSell",
                "price": "110",
                "reduce_only": "true",
            },
            {
                "ticker_id": "3",
                "symbol_id": "3",
                "exchange_ns": "120",
                "local_ns": "120",
                "action": "kOpenShort",
                "side": "kSell",
                "price": "200",
                "reduce_only": "false",
            },
            {
                "ticker_id": "4",
                "symbol_id": "3",
                "exchange_ns": "130",
                "local_ns": "130",
                "action": "kStoplossShort",
                "side": "kBuy",
                "price": "180",
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
                "ticker_id": "1",
                "symbol_id": "3",
                "exchange_ns": "100",
                "local_ns": "100",
                "action": "kCloseLong",
                "side": "kSell",
                "price": "110",
                "reduce_only": "true",
            }
        ]

        with self.assertRaisesRegex(ValueError, "close without open"):
            pnl.calculate_pnl(
                rows, open_notional=Decimal("1000"), fee_rate=Decimal("0.0002")
            )


if __name__ == "__main__":
    unittest.main()
