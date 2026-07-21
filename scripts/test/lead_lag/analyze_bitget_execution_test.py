#!/home/liuxiang/dev/pyenv/lx/bin/python

import json
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

import numpy as np

LEAD_LAG_DIR = Path(__file__).resolve().parents[2] / "lead_lag"
MARKET_DATA_DIR = Path(__file__).resolve().parents[2] / "market_data"
for script_dir in (LEAD_LAG_DIR, MARKET_DATA_DIR):
    if str(script_dir) not in sys.path:
        sys.path.insert(0, str(script_dir))

import analyze_bitget_execution as bitget_analysis
import typed_binary


def write_file(path: Path, text: str) -> None:
    path.write_text(textwrap.dedent(text).strip() + "\n", encoding="utf-8")


def order_rows() -> list[dict[str, str]]:
    return [
        {
            "run_id": "bitget-run",
            "source_schema": "submitted_v1",
            "local_order_id": "101",
            "exchange_order_id": "501",
            "exchange": "bitget",
            "symbol": "ALLOUSDT",
            "symbol_id": "25",
            "order_role": "entry",
            "position_id": "1",
            "side": "kBuy",
            "status": "kFilled",
            "order_price": "100.2",
            "price_tick": "0.1",
            "slippage_ticks": "2",
            "quantity": "2",
            "cumulative_filled_quantity": "2",
            "signal_lag_id": "11",
            "lag_local_ns": "999800000",
            "lag_freshness_ns": "300000",
            "request_send_local_ns": "1000000000",
            "ack_local_receive_ns": "1002500000",
            "place_creation_exchange_ns": "1000000000",
            "feedback_updated_exchange_ns": "1001000000",
            "feedback_local_receive_ns": "1003000000",
        },
        {
            "run_id": "bitget-run",
            "source_schema": "submitted_v1",
            "local_order_id": "102",
            "exchange_order_id": "502",
            "exchange": "bitget",
            "symbol": "ALLOUSDT",
            "symbol_id": "25",
            "order_role": "entry",
            "position_id": "2",
            "side": "kBuy",
            "status": "kCancelled",
            "order_price": "100.2",
            "price_tick": "0.1",
            "slippage_ticks": "2",
            "quantity": "2",
            "cumulative_filled_quantity": "0",
            "signal_lag_id": "21",
            "lag_local_ns": "1999800000",
            "lag_freshness_ns": "300000",
            "request_send_local_ns": "2000000000",
            "ack_local_receive_ns": "2002500000",
            "place_creation_exchange_ns": "2000000000",
            "feedback_updated_exchange_ns": "2002000000",
            "feedback_local_receive_ns": "2003000000",
            "cancel_reason": "ioc_not_full_cancel",
        },
    ]


class AnalyzeBitgetExecutionTest(unittest.TestCase):
    def test_builds_execution_rows_and_reconciles_rest_fills(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            base = Path(temp_dir)
            feedback_log = base / "feedback.log"
            rest_path = base / "rest_fills.json"
            write_file(
                feedback_log,
                """
                I2026-07-20 16:33:59.439729740 1:1 order_feedback_session.h:LogFastFillSubscribe:961] bitget_fast_fill_subscribe accepted=true code=0
                I2026-07-20 16:41:24.761369308 1:1 order_feedback_session.h:LogFastFillUpdate:993] bitget_fast_fill_raw_update topic=fast-fill connection_generation=1 local_message_sequence=20 batch_data_index=0 category=usdt-futures symbol=ALLOUSDT order_id=501 client_oid=a-101 exec_id=601 side=buy hold_side=long exec_price=100.1 exec_quantity=1.5 trade_scope=taker exchange_message_time_ms=1002 exec_time_ms=1001 updated_time_ms=1001 local_receive_realtime_ns=1003500000 local_receive_monotonic_ns=9003500000
                I2026-07-20 16:41:24.761369309 1:1 order_feedback_session.h:LogFastFillUpdate:993] bitget_fast_fill_raw_update topic=fast-fill connection_generation=1 local_message_sequence=20 batch_data_index=0 category=usdt-futures symbol=ALLOUSDT order_id=501 client_oid=a-101 exec_id=602 side=buy hold_side=long exec_price=100.2 exec_quantity=0.5 trade_scope=taker exchange_message_time_ms=1002 exec_time_ms=1001 updated_time_ms=1001 local_receive_realtime_ns=1003501000 local_receive_monotonic_ns=9003501000
                """,
            )
            rest_path.write_text(
                json.dumps(
                    {
                        "fills": [
                            {
                                "execId": "601",
                                "orderId": "501",
                                "clientOid": "a-101",
                                "symbol": "ALLOUSDT",
                                "side": "buy",
                                "execPrice": "100.1",
                                "execQty": "1.5",
                                "execValue": "150.15",
                                "tradeScope": "taker",
                                "feeDetail": [
                                    {"feeCoin": "USDT", "fee": "0.03003"}
                                ],
                                "createdTime": "1001",
                                "updatedTime": "1001",
                                "execPnl": "0",
                            }
                        ]
                    }
                ),
                encoding="utf-8",
            )

            result = bitget_analysis.analyze_executions(
                feedback_log,
                order_rows(),
                rest_fills_path=rest_path,
                run_id="bitget-run",
            )

        self.assertEqual(len(result.rows), 2)
        rows = {row["exec_id"]: row for row in result.rows}
        self.assertEqual(rows["601"]["source"], "fast_fill+rest")
        self.assertEqual(rows["601"]["actual_fee_quote"], "0.03003")
        self.assertEqual(rows["601"]["exec_pnl"], "0")
        self.assertEqual(rows["601"]["creation_to_exec_ns"], "1000000")
        self.assertEqual(rows["601"]["fast_fill_after_order_feedback_ns"], "500000")
        self.assertEqual(rows["602"]["source"], "fast_fill")
        self.assertEqual(rows["602"]["fast_fill_order_quantity"], "2")
        self.assertEqual(result.stats["fast_fill_subscribed"], True)
        self.assertEqual(result.stats["fast_fill_records"], 2)
        self.assertEqual(result.stats["fast_fill_unique_orders"], 1)
        self.assertEqual(result.stats["filled_orders_missing_fast_fill"], 0)
        self.assertEqual(result.stats["quantity_mismatch_orders"], 0)
        self.assertEqual(result.stats["rest_execution_records"], 1)

    def test_classifies_filled_and_zero_fill_ioc_windows(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            base = Path(temp_dir)
            binary_path = base / "bitget.bin"
            manifest_path = base / "manifest.jsonl"
            records = np.zeros(8, dtype=typed_binary.book_ticker_dtype())
            values = [
                # Filled order signal and execTime millisecond.
                (11, 999_800_000, 999_000_000, 100.0, 100.1),
                (12, 1_000_100_000, 1_000_000_000, 100.0, 100.1),
                (13, 1_001_100_000, 1_001_000_000, 100.0, 100.1),
                # Cancelled order signal, creation, middle, and terminal.
                (21, 1_999_800_000, 1_999_000_000, 100.0, 100.1),
                (22, 2_000_100_000, 2_000_000_000, 100.1, 100.3),
                (23, 2_001_100_000, 2_001_000_000, 100.2, 100.4),
                (24, 2_002_100_000, 2_002_000_000, 100.3, 100.5),
                (25, 2_003_100_000, 2_003_000_000, 100.4, 100.6),
            ]
            for index, (record_id, local_ns, exchange_ns, bid, ask) in enumerate(values):
                records[index]["id"] = record_id
                records[index]["symbol_id"] = 25
                records[index]["exchange"] = 4
                records[index]["exchange_ns"] = exchange_ns
                records[index]["event_ns"] = exchange_ns
                records[index]["local_ns"] = local_ns
                records[index]["bid_price"] = bid
                records[index]["ask_price"] = ask
            typed_binary.write_records(binary_path, "book_ticker", records)
            manifest_path.write_text(
                json.dumps(
                    {
                        "sequence": 1,
                        "file": binary_path.name,
                        "records": len(records),
                        "feed": "book_ticker",
                        "record_size": records.dtype.itemsize,
                        "first_local_ns": int(records[0]["local_ns"]),
                        "last_local_ns": int(records[-1]["local_ns"]),
                    }
                )
                + "\n",
                encoding="utf-8",
            )
            execution_rows = [
                {
                    "local_order_id": "101",
                    "exec_time_exchange_ns": "1001000000",
                },
                {
                    "local_order_id": "102",
                    "exec_time_exchange_ns": "2001000000",
                },
            ]
            fillability_orders = order_rows()
            fillability_orders[1]["status"] = "kPartiallyCancelled"
            fillability_orders[1]["cumulative_filled_quantity"] = "0.5"

            rows = bitget_analysis.analyze_fillability(
                fillability_orders, execution_rows, manifest_path
            )

        self.assertEqual(len(rows), 2)
        by_id = {row["local_order_id"]: row for row in rows}
        filled = by_id["101"]
        self.assertEqual(filled["terminal_event"], "exec")
        self.assertEqual(filled["creation_marketability"], "all_cross")
        self.assertEqual(filled["window_marketability"], "all_cross")
        self.assertEqual(filled["terminal_marketability"], "all_cross")
        self.assertEqual(filled["marketability_observation"], "marketable_observed")

        cancelled = by_id["102"]
        self.assertEqual(cancelled["terminal_event"], "cancel")
        self.assertEqual(cancelled["terminal_event"], "cancel")
        self.assertEqual(cancelled["creation_marketability"], "no_cross")
        self.assertEqual(cancelled["window_marketability"], "no_cross")
        self.assertEqual(cancelled["terminal_marketability"], "no_cross")
        self.assertEqual(
            cancelled["marketability_observation"], "not_marketable_observed"
        )
        self.assertEqual(cancelled["first_no_cross_after_send_ns"], "100000")
        self.assertEqual(cancelled["first_no_cross_opposite_price"], "100.3")


if __name__ == "__main__":
    unittest.main()
