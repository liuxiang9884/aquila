#!/home/liuxiang/dev/pyenv/lx/bin/python

import csv
import json
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parents[2] / "lead_lag"
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import generate_live_report as report


def write_file(path: Path, text: str) -> None:
    path.write_text(textwrap.dedent(text).strip() + "\n", encoding="utf-8")


def write_config(path: Path) -> None:
    write_file(
        path,
        """
        [lead_lag]
        name = "lead_lag"
        version = "1.0"

        [[lead_lag.pairs]]
        symbol = "PROVE_USDT"
        symbol_id = 4
        lead_exchange = "binance"
        lag_exchange = "gate"
        lag_taker_fee = 0.00016
        max_lead_freshness_ms = 5
        max_lag_freshness_ms = 20

        [lead_lag.pairs.execute]
        open_notional = 100.0
        trailing_stop = 0.01
        open_slippage_ticks = 3
        close_slippage_ticks = 3
        stoploss_slippage_ticks = 3
        close_retry_times = 0
        close_retry_slippage_step_ticks = 0
        """,
    )


def write_catalog(path: Path) -> None:
    path.write_text(
        "symbol_id,symbol,exchange,exchange_symbol,base_asset,quote_asset,"
        "settle_asset,product_type,status,contract_type,price_tick,"
        "price_decimal_places,quantity_step,quantity_decimal_places,"
        "min_quantity,max_quantity,max_market_quantity,min_notional,"
        "notional_multiplier,price_limit_up,price_limit_down,"
        "market_price_bound\n"
        "4,PROVE_USDT,gate,PROVE_USDT,PROVE,USDT,USDT,"
        "linear_perpetual,TRADING,direct,0.0001,4,1.0,0,"
        "1.0,68000.0,45000.0,,10.0,0.1,0.1,0.035\n",
        encoding="utf-8",
    )


def write_bitget_catalog(path: Path) -> None:
    path.write_text(
        "symbol_id,symbol,exchange,exchange_symbol,base_asset,quote_asset,"
        "settle_asset,product_type,status,contract_type,price_tick,"
        "price_decimal_places,quantity_step,quantity_decimal_places,"
        "min_quantity,max_quantity,max_market_quantity,min_notional,"
        "notional_multiplier,contract_multiplier,price_limit_up,"
        "price_limit_down,market_price_bound\n"
        "25,ALLO_USDT,bitget,ALLOUSDT,ALLO,USDT,USDT,linear_perpetual,"
        "online,perpetual,0.00001,5,1.0,0,1.0,1100000,170000,5.0,1.0,"
        "1.0,0.15,0.15,\n",
        encoding="utf-8",
    )


class GenerateLiveReportTest(unittest.TestCase):
    def test_generates_bitget_execution_report_from_split_logs(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            base = Path(temp_dir)
            strategy_log = base / "strategy.log"
            gateway_log = base / "gateway.log"
            feedback_log = base / "feedback.log"
            rest_fills_path = base / "fills.json"
            run_definition_path = base / "run_definition.json"
            catalog_path = base / "catalog.csv"
            schema_path = base / "schema.md"
            output_root = base / "reports"
            write_bitget_catalog(catalog_path)
            schema_path.write_text("# schema\n", encoding="utf-8")
            write_file(
                strategy_log,
                """
                I2026-07-20 16:37:14.241220000 1:1 strategy.h:LogStrategySignalTriggered:1] lead_lag_signal_triggered trigger_exchange=kBinance trigger_symbol_id=25 trigger_exchange_ns=1000000000 trigger_local_ns=1000000100 on_book_ticker_entry_ns=1000000200 signal_decision_ns=1000000300 lead_exchange_ns=1000000000 lead_local_ns=1000000100 signal_lead_id=10 lead_freshness_ns=300 lag_exchange_ns=999000000 lag_local_ns=999000100 signal_lag_id=11 lag_freshness_ns=1000300 symbol=ALLOUSDT symbol_id=25 role=kLead action=kOpenLong side=kBuy reduce_only=false position_id=0 raw_price=100.1
                I2026-07-20 16:37:14.241223501 1:1 strategy.h:LogStrategyOrderSubmitted:390] lead_lag_order_submitted local_order_id=101 trigger_exchange=kBinance trigger_symbol_id=25 trigger_exchange_ns=1000000000 trigger_local_ns=1000000100 on_book_ticker_entry_ns=1000000200 signal_decision_ns=1000000300 lead_exchange_ns=1000000000 lead_local_ns=1000000100 signal_lead_id=10 lead_freshness_ns=300 lag_exchange_ns=999000000 lag_local_ns=999000100 signal_lag_id=11 lag_freshness_ns=1000300 symbol=ALLOUSDT symbol_id=25 signal_role=kLead order_role=entry action=kOpenLong side=kBuy reduce_only=false position_id=1 position_event=kEntrySubmit position_direction=kLong entry_local_order_id=101 quantity=2 raw_price=100.1 order_price=100.2 slippage_ticks=1 price_tick=0.1 target_open_notional=200 estimated_notional=200.4 active_groups=1 place_status=kOk
                I2026-07-20 16:37:14.261971994 1:1 strategy.h:LogStrategyOrderFeedback:582] lead_lag_order_feedback kind=kFilled local_order_id=101 exchange_order_id=501 cumulative_filled_quantity=2 left_quantity=0 cancelled_quantity=0 fill_price=100.1 role=kTaker finish_reason=kUnknown reject_reason=kUnknown exchange_update_ns=1001000000 local_receive_ns=1003000000 lead_exchange_ns=1000000000 lag_exchange_ns=1000000000
                I2026-07-20 16:37:14.261973319 1:1 strategy.h:LogStrategyOrderFinished:681] lead_lag_order_finished local_order_id=101 symbol_id=25 symbol=ALLOUSDT status=kFilled reduce_only=false position_id=1 position_direction=kLong order_role=entry entry_local_order_id=101 order_finished_local_ns=1003001000 quantity=2 cumulative_filled_quantity=2 average_fill_price=100.1 last_fill_price=100.1 exchange_order_id=501 active_groups=1 request_send_local_ns=1000000400 ack_local_receive_ns=1002500000 response_local_receive_ns=0 ack_exchange_ns=1001000000 response_exchange_ns=0 accepted_exchange_ns=0 finish_exchange_ns=1001000000 ack_rtt_ns=2499600 response_rtt_ns=0 ack_exchange_to_local_ns=1500000 response_exchange_to_local_ns=0 exchange_lifecycle_ns=0
                """,
            )
            write_file(
                gateway_log,
                """
                I2026-07-20 16:37:14.241236349 2:2 order_session.h:LogOrderSend:855] bitget_order_send request_type=kPlaceOrder request_sequence=1 local_order_id=101 request_send_local_ns=1000000400 request_send_monotonic_ns=9000000400 order_encode_done_realtime_ns=1000000350 inflight=1
                I2026-07-20 16:37:14.261843452 2:2 order_session.h:LogOrderResponse:871] bitget_order_response response_kind=kAck request_type=kPlaceOrder request_sequence=1 local_order_id=101 exchange_order_id=501 error_code=0 request_send_local_ns=1000000400 local_receive_ns=1002500000 exchange_ns=1001000000 ack_rtt_ns=2499600 request_send_realtime_ns=1000000400 request_send_monotonic_ns=9000000400 write_complete_realtime_ns=1000000500 write_complete_monotonic_ns=9000000500 ack_receive_realtime_ns=1002500000 ack_receive_monotonic_ns=9002500000 ack_rtt_monotonic_ns=2499600 write_complete_to_ack_monotonic_ns=2499500 place_creation_time_ms=1000 exchange_message_time_ms=1001
                """,
            )
            write_file(
                feedback_log,
                """
                I2026-07-20 16:37:14.261900000 3:3 order_feedback_session.h:LogOrderProtocolUpdate:972] bitget_order_feedback_protocol_update topic=order connection_generation=1 local_message_sequence=10 batch_data_index=0 client_oid=a-101 order_id=501 order_status=filled cancel_reason= exchange_message_time_ms=1001 created_time_ms=1000 updated_time_ms=1001 local_receive_realtime_ns=1003000000 local_receive_monotonic_ns=9003000000
                I2026-07-20 16:33:59.439729740 3:3 order_feedback_session.h:LogFastFillSubscribe:961] bitget_fast_fill_subscribe accepted=true code=0
                I2026-07-20 16:41:24.761369308 3:3 order_feedback_session.h:LogFastFillUpdate:993] bitget_fast_fill_raw_update topic=fast-fill connection_generation=1 local_message_sequence=20 batch_data_index=0 category=usdt-futures symbol=ALLOUSDT order_id=501 client_oid=a-101 exec_id=601 side=buy hold_side=long exec_price=100.1 exec_quantity=2 trade_scope=taker exchange_message_time_ms=1002 exec_time_ms=1001 updated_time_ms=1001 local_receive_realtime_ns=1003500000 local_receive_monotonic_ns=9003500000
                """,
            )
            rest_fills_path.write_text(
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
                                "execQty": "2",
                                "execValue": "200.2",
                                "tradeScope": "taker",
                                "feeDetail": [{"feeCoin": "USDT", "fee": "0.04"}],
                                "createdTime": "1001",
                                "updatedTime": "1001",
                                "execPnl": "0.12",
                            }
                        ]
                    }
                ),
                encoding="utf-8",
            )
            run_definition_path.write_text(
                json.dumps(
                    {
                        "commit": "abc1234",
                        "duration_sec": 43200,
                        "limiter": "absent",
                        "order_fanout": 1,
                        "market_fanout": {"bitget": 6, "bitget_ha": 3, "bitget_hs": 3},
                    }
                ),
                encoding="utf-8",
            )

            result = report.generate_live_report(
                log_path=strategy_log,
                additional_log_paths=[gateway_log, feedback_log],
                exchange="bitget",
                feedback_log_path=feedback_log,
                rest_fills_path=rest_fills_path,
                run_definition_path=run_definition_path,
                config_path=None,
                instrument_catalog_path=catalog_path,
                run_id="bitget-run",
                output_root=output_root,
                schema_path=schema_path,
            )

            report_dir = result.report_dir
            with (report_dir / "execution_detail.csv").open(
                newline="", encoding="utf-8"
            ) as input_file:
                execution_rows = list(csv.DictReader(input_file))
            with (report_dir / "order_fillability.csv").open(
                newline="", encoding="utf-8"
            ) as input_file:
                fillability_rows = list(csv.DictReader(input_file))
            report_text = (report_dir / "report.md").read_text(encoding="utf-8")

        self.assertEqual(result.execution_rows, 1)
        self.assertEqual(result.fillability_rows, 0)
        self.assertEqual(execution_rows[0]["source"], "fast_fill+rest")
        self.assertEqual(execution_rows[0]["actual_fee_quote"], "0.04")
        self.assertEqual(fillability_rows, [])
        self.assertIn("- exchange: `bitget`", report_text)
        self.assertIn("## Fast-fill 成交对账", report_text)
        self.assertIn("- authoritative filled orders: `1`", report_text)
        self.assertIn("- filled orders missing fast-fill: `0`", report_text)
        self.assertIn("- REST execPnl: `0.12 USDT`", report_text)
        self.assertIn("- REST actual fee: `0.04 USDT`", report_text)
        self.assertIn("- REST net PnL: `0.08 USDT`", report_text)
        self.assertIn("- limiter: `absent`", report_text)
        self.assertIn("- Bitget fusion: `6` (HA=`3`, HS=`3`)", report_text)
        self.assertNotIn("Gate x_in", report_text)

    def test_drift_guard_rejected_intent_joins_signal_without_missing_order(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            base = Path(temp_dir)
            log_path = base / "run.log"
            config_path = base / "strategy.toml"
            catalog_path = base / "catalog.csv"
            schema_path = base / "schema.md"
            output_root = base / "reports"
            write_config(config_path)
            write_catalog(catalog_path)
            schema_path.write_text("# schema\n", encoding="utf-8")
            write_file(
                log_path,
                """
                I2026-06-25 09:00:00.000000100 1:1 strategy.h:LogStrategySignalTriggered:155] lead_lag_signal_triggered trigger_exchange=kBinance trigger_symbol_id=4 trigger_exchange_ns=1782360000000000000 trigger_local_ns=1782360000000010000 on_book_ticker_entry_ns=1782360000000015000 signal_decision_ns=1782360000000020000 lead_exchange_ns=1782360000000000000 lead_local_ns=1782360000000010000 signal_lead_id=7001 lead_freshness_ns=20000 lag_exchange_ns=1782359999999990000 lag_local_ns=1782360000000005000 signal_lag_id=7002 lag_freshness_ns=30000 symbol=PROVE_USDT symbol_id=4 role=kLead action=kOpenLong side=kBuy reduce_only=false position_id=0 raw_price=0.2711
                W2026-06-25 09:00:00.000000200 1:1 strategy.h:LogStrategyOrderIntentRejected:407] lead_lag_order_intent_rejected reason=drift_guard trigger_exchange_ns=1782360000000000000 trigger_local_ns=1782360000000010000 on_book_ticker_entry_ns=1782360000000015000 signal_decision_ns=1782360000000020000 lead_exchange_ns=1782360000000000000 lead_local_ns=1782360000000010000 signal_lead_id=7001 lead_freshness_ns=20000 lag_exchange_ns=1782359999999990000 lag_local_ns=1782360000000005000 signal_lag_id=7002 lag_freshness_ns=30000 symbol=PROVE_USDT symbol_id=4 action=kOpenLong side=kBuy reduce_only=false position_id=0 quantity=0 price=0.2711 raw_price=0.2711 order_price=0.2711 slippage_ticks=0 price_tick=0.0001 target_open_notional=100 estimated_notional=0 gross_before=0 gross_after=0 max_gross_notional=0 local_order_id=0 place_status=-
                """,
            )

            result = report.generate_live_report(
                log_path=log_path,
                config_path=config_path,
                instrument_catalog_path=catalog_path,
                run_id="run-1",
                output_root=output_root,
                schema_path=schema_path,
            )

            report_dir = output_root / "run-1"
            with (report_dir / "signal.csv").open(newline="", encoding="utf-8") as input_file:
                signal_rows = list(csv.DictReader(input_file))
            with (report_dir / "order_detail.csv").open(newline="", encoding="utf-8") as input_file:
                order_rows = list(csv.DictReader(input_file))
            with (report_dir / "latency.csv").open(newline="", encoding="utf-8") as input_file:
                latency_rows = list(csv.DictReader(input_file))
            report_text = (report_dir / "report.md").read_text(encoding="utf-8")

        self.assertEqual(result.latency_rows, 0)
        self.assertEqual(latency_rows, [])
        self.assertEqual(len(signal_rows), 1)
        signal_row = signal_rows[0]
        self.assertNotIn("missing_order", signal_row["warnings"])
        self.assertEqual(signal_row["status"], "kRejected")
        self.assertEqual(signal_row["local_order_id"], "0")
        self.assertEqual(signal_row["order_price"], "0.2711")
        self.assertEqual(signal_row["quantity"], "0")

        self.assertEqual(len(order_rows), 1)
        order_row = order_rows[0]
        self.assertEqual(order_row["source_schema"], "intent_rejected_v1")
        self.assertEqual(order_row["status"], "kRejected")
        self.assertEqual(order_row["reject_reason"], "drift_guard")
        self.assertEqual(order_row["local_order_id"], "0")
        self.assertEqual(order_row["symbol"], "PROVE_USDT")
        self.assertEqual(order_row["symbol_id"], "4")
        self.assertEqual(order_row["action"], "kOpenLong")
        self.assertEqual(order_row["side"], "kBuy")
        self.assertEqual(order_row["reduce_only"], "false")
        self.assertEqual(order_row["raw_price"], "0.2711")
        self.assertEqual(order_row["order_price"], "0.2711")
        self.assertEqual(order_row["price_tick"], "0.0001")
        self.assertEqual(order_row["slippage_ticks"], "0")
        self.assertEqual(order_row["quantity"], "0")
        self.assertIn("- submitted order: `0`", report_text)

    def test_parallel_limit_rejected_intent_joins_signal_without_missing_order(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            base = Path(temp_dir)
            log_path = base / "run.log"
            config_path = base / "strategy.toml"
            catalog_path = base / "catalog.csv"
            schema_path = base / "schema.md"
            output_root = base / "reports"
            write_config(config_path)
            write_catalog(catalog_path)
            schema_path.write_text("# schema\n", encoding="utf-8")
            write_file(
                log_path,
                """
                I2026-06-25 09:00:00.000000100 1:1 strategy.h:LogStrategySignalTriggered:155] lead_lag_signal_triggered trigger_exchange=kBinance trigger_symbol_id=4 trigger_exchange_ns=1782360000000000000 trigger_local_ns=1782360000000010000 on_book_ticker_entry_ns=1782360000000015000 signal_decision_ns=1782360000000020000 lead_exchange_ns=1782360000000000000 lead_local_ns=1782360000000010000 signal_lead_id=7001 lead_freshness_ns=20000 lag_exchange_ns=1782359999999990000 lag_local_ns=1782360000000005000 signal_lag_id=7002 lag_freshness_ns=30000 symbol=PROVE_USDT symbol_id=4 role=kLead action=kOpenLong side=kBuy reduce_only=false position_id=0 raw_price=0.2711
                W2026-06-25 09:00:00.000000200 1:1 strategy.h:LogStrategyOrderIntentRejected:407] lead_lag_order_intent_rejected reason=parallel_limit trigger_exchange_ns=1782360000000000000 trigger_local_ns=1782360000000010000 on_book_ticker_entry_ns=1782360000000015000 signal_decision_ns=1782360000000020000 lead_exchange_ns=1782360000000000000 lead_local_ns=1782360000000010000 signal_lead_id=7001 lead_freshness_ns=20000 lag_exchange_ns=1782359999999990000 lag_local_ns=1782360000000005000 signal_lag_id=7002 lag_freshness_ns=30000 symbol=PROVE_USDT symbol_id=4 action=kOpenLong side=kBuy reduce_only=false position_id=0 quantity=0 price=0.2711 raw_price=0.2711 order_price=0.2711 slippage_ticks=0 price_tick=0.0001 target_open_notional=100 estimated_notional=0 gross_before=0 gross_after=0 max_gross_notional=0 local_order_id=0 place_status=-
                """,
            )

            result = report.generate_live_report(
                log_path=log_path,
                config_path=config_path,
                instrument_catalog_path=catalog_path,
                run_id="run-1",
                output_root=output_root,
                schema_path=schema_path,
            )

            report_dir = output_root / "run-1"
            with (report_dir / "signal.csv").open(
                newline="", encoding="utf-8"
            ) as input_file:
                signal_rows = list(csv.DictReader(input_file))
            with (report_dir / "order_detail.csv").open(
                newline="", encoding="utf-8"
            ) as input_file:
                order_rows = list(csv.DictReader(input_file))
            report_text = (report_dir / "report.md").read_text(encoding="utf-8")

        self.assertEqual(result.latency_rows, 0)
        self.assertEqual(len(signal_rows), 1)
        self.assertNotIn("missing_order", signal_rows[0]["warnings"])
        self.assertEqual(signal_rows[0]["status"], "kRejected")
        self.assertEqual(signal_rows[0]["local_order_id"], "0")
        self.assertEqual(len(order_rows), 1)
        self.assertEqual(order_rows[0]["source_schema"], "intent_rejected_v1")
        self.assertEqual(order_rows[0]["reject_reason"], "parallel_limit")
        self.assertIn("- submitted order: `0`", report_text)

    def test_additional_rejected_intents_join_signals_without_missing_order(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            base = Path(temp_dir)
            log_path = base / "run.log"
            config_path = base / "strategy.toml"
            catalog_path = base / "catalog.csv"
            schema_path = base / "schema.md"
            output_root = base / "reports"
            write_config(config_path)
            write_catalog(catalog_path)
            schema_path.write_text("# schema\n", encoding="utf-8")
            write_file(
                log_path,
                """
                I2026-06-25 09:00:00.000000100 1:1 strategy.h:LogStrategySignalTriggered:155] lead_lag_signal_triggered trigger_exchange=kBinance trigger_symbol_id=4 trigger_exchange_ns=1782360000000000000 trigger_local_ns=1782360000000010000 on_book_ticker_entry_ns=1782360000000015000 signal_decision_ns=1782360000000020000 lead_exchange_ns=1782360000000000000 lead_local_ns=1782360000000010000 signal_lead_id=7001 lead_freshness_ns=20000 lag_exchange_ns=1782359999999990000 lag_local_ns=1782360000000005000 signal_lag_id=7002 lag_freshness_ns=30000000 symbol=PROVE_USDT symbol_id=4 role=kLead action=kOpenLong side=kBuy reduce_only=false position_id=0 raw_price=0.2711
                W2026-06-25 09:00:00.000000200 1:1 strategy.h:LogStrategyOrderIntentRejected:407] lead_lag_order_intent_rejected reason=stale_lag_quote trigger_exchange_ns=1782360000000000000 trigger_local_ns=1782360000000010000 on_book_ticker_entry_ns=1782360000000015000 signal_decision_ns=1782360000000020000 lead_exchange_ns=1782360000000000000 lead_local_ns=1782360000000010000 signal_lead_id=7001 lead_freshness_ns=20000 lag_exchange_ns=1782359999999990000 lag_local_ns=1782360000000005000 signal_lag_id=7002 lag_freshness_ns=30000000 max_lead_freshness_ns=5000000 max_lag_freshness_ns=20000000 freshness_guard_pass=false freshness_reject_reason=stale_lag_quote symbol=PROVE_USDT symbol_id=4 action=kOpenLong side=kBuy reduce_only=false position_id=0 quantity=0 price=0.2711 raw_price=0.2711 order_price=0.2711 slippage_ticks=0 price_tick=0.0001 target_open_notional=100 estimated_notional=0 gross_before=0 gross_after=0 max_gross_notional=0 local_order_id=0 place_status=-
                I2026-06-25 09:00:00.000000300 1:1 strategy.h:LogStrategySignalTriggered:155] lead_lag_signal_triggered trigger_exchange=kBinance trigger_symbol_id=4 trigger_exchange_ns=1782360000000100000 trigger_local_ns=1782360000000110000 on_book_ticker_entry_ns=1782360000000115000 signal_decision_ns=1782360000000120000 lead_exchange_ns=1782360000000100000 lead_local_ns=1782360000000110000 signal_lead_id=7011 lead_freshness_ns=20000 lag_exchange_ns=1782360000000090000 lag_local_ns=1782360000000105000 signal_lag_id=7012 lag_freshness_ns=30000 symbol=PROVE_USDT symbol_id=4 role=kLead action=kOpenLong side=kBuy reduce_only=false position_id=0 raw_price=0.2722
                W2026-06-25 09:00:00.000000400 1:1 strategy.h:LogStrategyOrderIntentRejected:407] lead_lag_order_intent_rejected reason=risk_limit trigger_exchange_ns=1782360000000100000 trigger_local_ns=1782360000000110000 on_book_ticker_entry_ns=1782360000000115000 signal_decision_ns=1782360000000120000 lead_exchange_ns=1782360000000100000 lead_local_ns=1782360000000110000 signal_lead_id=7011 lead_freshness_ns=20000 lag_exchange_ns=1782360000000090000 lag_local_ns=1782360000000105000 signal_lag_id=7012 lag_freshness_ns=30000 max_lead_freshness_ns=5000000 max_lag_freshness_ns=20000000 freshness_guard_pass=true freshness_reject_reason=none symbol=PROVE_USDT symbol_id=4 action=kOpenLong side=kBuy reduce_only=false position_id=0 quantity=36 price=0.2722 raw_price=0.2722 order_price=0.2722 slippage_ticks=0 price_tick=0.0001 target_open_notional=100 estimated_notional=97.992 gross_before=990 gross_after=1087.992 max_gross_notional=1000 local_order_id=0 place_status=-
                I2026-06-25 09:00:00.000000500 1:1 strategy.h:LogStrategySignalTriggered:155] lead_lag_signal_triggered trigger_exchange=kBinance trigger_symbol_id=4 trigger_exchange_ns=1782360000000200000 trigger_local_ns=1782360000000210000 on_book_ticker_entry_ns=1782360000000215000 signal_decision_ns=1782360000000220000 lead_exchange_ns=1782360000000200000 lead_local_ns=1782360000000210000 signal_lead_id=7021 lead_freshness_ns=20000 lag_exchange_ns=1782360000000190000 lag_local_ns=1782360000000205000 signal_lag_id=7022 lag_freshness_ns=30000 symbol=PROVE_USDT symbol_id=4 role=kLead action=kOpenLong side=kBuy reduce_only=false position_id=0 raw_price=0.2733
                W2026-06-25 09:00:00.000000600 1:1 strategy.h:LogStrategyOrderIntentRejected:407] lead_lag_order_intent_rejected reason=zero_quantity trigger_exchange_ns=1782360000000200000 trigger_local_ns=1782360000000210000 on_book_ticker_entry_ns=1782360000000215000 signal_decision_ns=1782360000000220000 lead_exchange_ns=1782360000000200000 lead_local_ns=1782360000000210000 signal_lead_id=7021 lead_freshness_ns=20000 lag_exchange_ns=1782360000000190000 lag_local_ns=1782360000000205000 signal_lag_id=7022 lag_freshness_ns=30000 max_lead_freshness_ns=5000000 max_lag_freshness_ns=20000000 freshness_guard_pass=true freshness_reject_reason=none symbol=PROVE_USDT symbol_id=4 action=kOpenLong side=kBuy reduce_only=false position_id=0 quantity=0 price=0.2733 raw_price=0.2733 order_price=0.2733 slippage_ticks=0 price_tick=0.0001 target_open_notional=100 estimated_notional=0 gross_before=0 gross_after=0 max_gross_notional=0 local_order_id=0 place_status=-
                """,
            )

            result = report.generate_live_report(
                log_path=log_path,
                config_path=config_path,
                instrument_catalog_path=catalog_path,
                run_id="run-1",
                output_root=output_root,
                schema_path=schema_path,
            )

            report_dir = output_root / "run-1"
            with (report_dir / "signal.csv").open(
                newline="", encoding="utf-8"
            ) as input_file:
                signal_rows = list(csv.DictReader(input_file))
            with (report_dir / "order_detail.csv").open(
                newline="", encoding="utf-8"
            ) as input_file:
                order_rows = list(csv.DictReader(input_file))
            report_text = (report_dir / "report.md").read_text(encoding="utf-8")

        self.assertEqual(result.latency_rows, 0)
        self.assertEqual(len(signal_rows), 3)
        self.assertEqual(len(order_rows), 3)
        reasons = {row["reject_reason"] for row in order_rows}
        self.assertEqual(reasons, {"stale_lag_quote", "risk_limit", "zero_quantity"})
        for signal_row in signal_rows:
            self.assertNotIn("missing_order", signal_row["warnings"])
            self.assertEqual(signal_row["status"], "kRejected")
            self.assertEqual(signal_row["local_order_id"], "0")
        for order_row in order_rows:
            self.assertEqual(order_row["source_schema"], "intent_rejected_v1")
            self.assertEqual(order_row["status"], "kRejected")
            self.assertEqual(order_row["local_order_id"], "0")
        self.assertIn("- signal: `3`", report_text)
        self.assertIn("- submitted order: `0`", report_text)

    def test_generates_report_directory_from_live_log(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            base = Path(temp_dir)
            log_path = base / "run.log"
            config_path = base / "strategy.toml"
            catalog_path = base / "catalog.csv"
            schema_path = base / "schema.md"
            output_root = base / "reports"
            write_config(config_path)
            write_catalog(catalog_path)
            schema_path.write_text("# schema\n\n`local_order_id`\n", encoding="utf-8")
            write_file(
                log_path,
                """
                I2026-05-25 02:29:35.105545332 1:1 strategy.h:LogStrategySignalTriggered:155] lead_lag_signal_triggered trigger_exchange=kBinance trigger_symbol_id=4 trigger_exchange_ns=1779676175105510000 trigger_local_ns=1779676175105520000 on_book_ticker_entry_ns=1779676175105530000 signal_decision_ns=1779676175105540000 lead_exchange_ns=1779676175105510000 lead_local_ns=1779676175105520000 signal_lead_id=5001 lead_freshness_ns=30000 lag_exchange_ns=1779676175105500000 lag_local_ns=1779676175105510000 signal_lag_id=5002 lag_freshness_ns=40000 symbol=PROVE_USDT symbol_id=4 role=kLead action=kOpenLong side=kBuy reduce_only=false position_id=0 raw_price=0.2711
                I2026-05-25 02:29:35.105563683 1:1 order_session.h:LogGatePlaceOrderSent:466] gate_order_send_ok type=place local_order_id=288230376151711749 request_sequence=6 encoded_request_id=144115188075855878 contract=PROVE_USDT side=kBuy quantity=36 price=0.2714 tif=kImmediateOrCancel reduce_only=false inflight=5 request_send_local_ns=1779676175105554883
                I2026-05-25 02:29:35.105566021 1:1 strategy.h:LogStrategyOrderSubmitted:1] lead_lag_order_submitted local_order_id=288230376151711749 trigger_exchange=kBinance trigger_symbol_id=4 trigger_exchange_ns=1779676175105510000 trigger_local_ns=1779676175105520000 on_book_ticker_entry_ns=1779676175105530000 signal_decision_ns=1779676175105540000 lead_exchange_ns=1779676175105510000 lead_local_ns=1779676175105520000 signal_lead_id=5001 lead_freshness_ns=30000 lag_exchange_ns=1779676175105500000 lag_local_ns=1779676175105510000 signal_lag_id=5002 lag_freshness_ns=40000 symbol=PROVE_USDT symbol_id=4 signal_role=kLead order_role=entry action=kOpenLong side=kBuy reduce_only=false position_id=1 position_event=kEntrySubmit position_direction=kLong entry_local_order_id=288230376151711749 quantity=36 quantity_text=36 raw_price=0.2711 order_price=0.2714 price_text=0.2714 slippage_ticks=3 price_tick=0.0001 target_open_notional=100 estimated_notional=97.704 active_groups=1 place_status=kOk
                I2026-05-25 02:29:35.111554171 1:1 order_session.h:LogGateOrderResponse:517] gate_order_response kind=kAck local_order_id=288230376151711749 exchange_order_id=0 request_sequence=6 channel=2 http_status=200 error_label_hash=0 error_label= error_message= local_receive_ns=1779676175111547348 exchange_ns=1779676175107083000 exchange_request_ingress_ns=1779676175107000000 exchange_response_egress_ns=1779676175107083000 exchange_process_ns=83000 exchange_to_local_ns=4464348
                W2026-05-25 02:29:35.112000000 1:1 order_session.h:LogOrderLatencyDiagnostic:1] gate_order_ack_latency_diagnostic reason=kAckRttThreshold local_order_id=288230376151711749 request_sequence=6 request_send_local_ns=1779676175105554883 ack_local_receive_ns=1779676175111547348 ack_exchange_ns=1779676175107083000 ack_rtt_ns=5992465 send_to_first_after_hook_ns=1000 send_to_first_drive_read_ns=3499001 drive_read_duration_ns=1300001 max_observed_drive_read_duration_ns=1300001 inflight_at_send=7
                I2026-05-25 02:29:35.117544030 1:1 strategy.h:LogStrategyOrderFeedback:272] lead_lag_order_feedback kind=kFilled local_order_id=288230376151711749 exchange_order_id=260082878984634644 cumulative_filled_quantity=36 left_quantity=0 cancelled_quantity=0 fill_price=0.2714 role=kTaker finish_reason=kUnknown reject_reason=kUnknown
                I2026-05-25 02:29:35.117545430 1:1 strategy.h:LogStrategyOrderFinished:335] lead_lag_order_finished local_order_id=288230376151711749 symbol_id=4 symbol=PROVE_USDT status=kFilled reduce_only=false position_id=1 position_direction=kLong order_role=entry entry_local_order_id=288230376151711749 order_finished_local_ns=1779676175117545430 quantity=36 cumulative_filled_quantity=36 average_fill_price=0.2714 last_fill_price=0.2714 exchange_order_id=260082878984634644 active_groups=1 request_send_local_ns=1779676175105554883 ack_local_receive_ns=1779676175111547348 response_local_receive_ns=0 ack_exchange_ns=1779676175107083000 response_exchange_ns=0 accepted_exchange_ns=0 finish_exchange_ns=1779676175113000000 ack_rtt_ns=5992465 response_rtt_ns=0 ack_exchange_to_local_ns=4464348 response_exchange_to_local_ns=0 exchange_lifecycle_ns=0
                """,
            )

            result = report.generate_live_report(
                log_path=log_path,
                config_path=config_path,
                instrument_catalog_path=catalog_path,
                run_id="run-1",
                output_root=output_root,
                schema_path=schema_path,
            )

            report_dir = output_root / "run-1"
            self.assertEqual(result.report_dir, report_dir)
            for name in (
                "signal.csv",
                "order_detail.csv",
                "position.csv",
                "latency.csv",
                "lead_lag_live_report_csv_schema.md",
                "report.md",
            ):
                self.assertTrue((report_dir / name).exists(), name)

            with (report_dir / "signal.csv").open(newline="", encoding="utf-8") as input_file:
                signal_row = next(csv.DictReader(input_file))
            with (report_dir / "latency.csv").open(newline="", encoding="utf-8") as input_file:
                latency_row = next(csv.DictReader(input_file))
            report_text = (report_dir / "report.md").read_text(encoding="utf-8")

        self.assertEqual(signal_row["run_id"], "run-1")
        self.assertEqual(signal_row["signal_index"], "1")
        self.assertEqual(signal_row["log_time"], "2026-05-25 02:29:35.105545332")
        self.assertNotIn("trigger_ticker_id", signal_row)
        self.assertEqual(signal_row["trigger_exchange"], "kBinance")
        self.assertEqual(signal_row["lead_exchange_ns"], "1779676175105510000")
        self.assertEqual(signal_row["lag_exchange_ns"], "1779676175105500000")
        self.assertEqual(signal_row["signal_lead_id"], "5001")
        self.assertEqual(signal_row["signal_lag_id"], "5002")
        self.assertEqual(signal_row["trigger_local_ns"], "1779676175105520000")
        self.assertEqual(signal_row["bbo_to_strategy_ns"], "10000")
        self.assertEqual(signal_row["strategy_to_signal_ns"], "10000")
        self.assertEqual(signal_row["signal_to_request_send_ns"], "14883")
        self.assertEqual(signal_row["trigger_to_request_send_ns"], "34883")
        self.assertEqual(signal_row["local_order_id"], "288230376151711749")
        self.assertEqual(signal_row["exchange_order_id"], "260082878984634644")
        self.assertEqual(signal_row["order_role"], "entry")
        self.assertEqual(signal_row["order_position_id"], "1")
        self.assertEqual(signal_row["status"], "kFilled")
        self.assertEqual(latency_row["latency_diagnostic_reason"], "kAckRttThreshold")
        self.assertEqual(latency_row["ack_exchange_process_ns"], "83000")
        self.assertEqual(latency_row["exchange_lifecycle_ns"], "5917000")
        self.assertIn("- signal: `1`", report_text)
        self.assertIn("- submitted order: `1`", report_text)
        self.assertIn("## Pair Freshness 参数", report_text)
        self.assertIn(
            "| PROVE_USDT | 4 | binance | gate | 5 | 20 |",
            report_text,
        )
        self.assertIn("- latency diagnostic outliers: `1`", report_text)
        self.assertIn("- exchange Ack-to-finish min: `5.917 ms`", report_text)
        self.assertIn("- exchange Ack-to-finish median: `5.917 ms`", report_text)
        self.assertIn("- exchange Ack-to-finish avg: `5.917 ms`", report_text)
        self.assertIn("- exchange Ack-to-finish p95: `5.917 ms`", report_text)
        self.assertIn("- exchange Ack-to-finish max: `5.917 ms`", report_text)
        self.assertIn("- Gate Ack process min: `0.083 ms`", report_text)
        self.assertIn("- Gate Ack process max: `0.083 ms`", report_text)
        self.assertIn(
            "| 288230376151711749 | PROVE_USDT | kFilled | kUnknown | 5.917 | 5.998 | 11.991 |",
            report_text,
        )
        self.assertIn("- 字段参考: `lead_lag_live_report_csv_schema.md`", report_text)

    def test_refuses_to_overwrite_existing_report_without_flag(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            base = Path(temp_dir)
            log_path = base / "run.log"
            config_path = base / "strategy.toml"
            catalog_path = base / "catalog.csv"
            schema_path = base / "schema.md"
            output_root = base / "reports"
            (output_root / "run-1").mkdir(parents=True)
            write_config(config_path)
            write_catalog(catalog_path)
            schema_path.write_text("# schema\n", encoding="utf-8")
            write_file(
                log_path,
                """
                I2026-05-25 02:29:35.105545332 1:1 strategy.h:LogStrategySignalTriggered:155] lead_lag_signal_triggered trigger_exchange=kBinance trigger_symbol_id=4 trigger_exchange_ns=1779676175105510000 trigger_local_ns=1779676175105520000 on_book_ticker_entry_ns=1779676175105530000 signal_decision_ns=1779676175105540000 lead_exchange_ns=1779676175105510000 lag_exchange_ns=1779676175105500000 symbol=PROVE_USDT symbol_id=4 role=kLead action=kOpenLong side=kBuy reduce_only=false position_id=0 raw_price=0.2711
                """,
            )

            with self.assertRaises(FileExistsError):
                report.generate_live_report(
                    log_path=log_path,
                    config_path=config_path,
                    instrument_catalog_path=catalog_path,
                    run_id="run-1",
                    output_root=output_root,
                    schema_path=schema_path,
                )

    def test_report_includes_affinity_summary_from_guard_stdout(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            base = Path(temp_dir)
            log_path = base / "run.log"
            config_path = base / "strategy.toml"
            catalog_path = base / "catalog.csv"
            schema_path = base / "schema.md"
            guard_stdout_path = base / "guard.stdout"
            generated_strategy_path = base / "tmp" / "configs" / "strategy.toml"
            generated_strategy_path.parent.mkdir(parents=True)
            generated_strategy_path.write_text("[strategy]\n", encoding="utf-8")
            output_root = base / "reports"
            write_config(config_path)
            write_catalog(catalog_path)
            schema_path.write_text("# schema\n", encoding="utf-8")
            write_file(
                log_path,
                """
                I2026-05-25 02:29:35.105545332 1:1 strategy.h:LogStrategySignalTriggered:155] lead_lag_signal_triggered trigger_exchange=kBinance trigger_symbol_id=4 trigger_exchange_ns=1779676175105510000 trigger_local_ns=1779676175105520000 on_book_ticker_entry_ns=1779676175105530000 signal_decision_ns=1779676175105540000 lead_exchange_ns=1779676175105510000 lag_exchange_ns=1779676175105500000 symbol=PROVE_USDT symbol_id=4 role=kLead action=kOpenLong side=kBuy reduce_only=false position_id=0 raw_price=0.2711
                """,
            )
            guard_stdout_path.write_text(
                """
                noisy child log {"ignored": true}
                {
                  "affinity": {
                    "profile_name": "lead_lag_requested_12symbols_node0",
                    "affinity_split": true,
                    "output_dir": "/home/liuxiang/tmp/run-1/configs",
                    "core_path": {
                      "gate_market_data_cpu": 2,
                      "binance_market_data_cpu": 3,
                      "strategy_order_owner_cpu": 4,
                      "gate_order_feedback_cpu": 6,
                      "log_backend_cpu": 5
                    },
                    "generated_configs": {
                      "strategy": "%s"
                    }
                  }
                }
                """
                % generated_strategy_path,
                encoding="utf-8",
            )

            report.generate_live_report(
                log_path=log_path,
                config_path=config_path,
                instrument_catalog_path=catalog_path,
                run_id="run-1",
                output_root=output_root,
                schema_path=schema_path,
                guard_stdout_path=guard_stdout_path,
            )

            report_text = (output_root / "run-1" / "report.md").read_text(
                encoding="utf-8"
            )
            copied_config_exists = (
                output_root
                / "run-1"
                / "runtime_configs"
                / "strategy__strategy.toml"
            ).exists()

        self.assertIn("- affinity profile: `lead_lag_requested_12symbols_node0`", report_text)
        self.assertIn("- affinity split: `true`", report_text)
        self.assertIn("- strategy_order_owner_cpu: `4`", report_text)
        self.assertIn("- generated strategy config:", report_text)
        self.assertTrue(copied_config_exists)

    def test_markdown_report_includes_ack_split_slippage_raw_pnl_and_win_rates(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            output_path = Path(temp_dir) / "report.md"
            report.write_markdown_report(
                output_path=output_path,
                run_id="run-1",
                log_path=Path("/tmp/run.log"),
                config_path=Path("/tmp/strategy.toml"),
                guard_stdout_path=None,
                signal_rows=[],
                order_rows=[
                    {
                        "local_order_id": "1",
                        "order_role": "entry",
                        "status": "kFilled",
                        "cumulative_filled_quantity": "1",
                        "exec_slippage_ticks": "2",
                        "limit_improvement_ticks": "8",
                    },
                    {
                        "local_order_id": "2",
                        "order_role": "exit",
                        "status": "kFilled",
                        "cumulative_filled_quantity": "1",
                        "exec_slippage_ticks": "-1",
                        "limit_improvement_ticks": "11",
                    },
                ],
                position_rows=[
                    {
                        "symbol": "PROVE_USDT",
                        "position_direction": "kLong",
                        "matched_volume": "1",
                        "contract_multiplier": "1",
                        "entry_raw_price": "100",
                        "exit_raw_price": "108",
                        "gross_pnl": "10",
                        "total_fee_quote_estimated": "1",
                        "net_pnl": "9",
                    },
                    {
                        "symbol": "PROVE_USDT",
                        "position_direction": "kShort",
                        "matched_volume": "1",
                        "contract_multiplier": "1",
                        "entry_raw_price": "50",
                        "exit_raw_price": "55",
                        "gross_pnl": "-4",
                        "total_fee_quote_estimated": "1",
                        "net_pnl": "-5",
                    },
                ],
                latency_rows=[
                    {
                        "local_order_id": "1",
                        "request_send_local_ns": "1000",
                        "ack_exchange_request_ingress_ns": "2000",
                        "ack_exchange_response_egress_ns": "3000",
                        "ack_exchange_process_ns": "1000",
                        "ack_local_receive_ns": "5000",
                        "ack_rtt_ns": "4000",
                    },
                    {
                        "local_order_id": "2",
                        "request_send_local_ns": "10000",
                        "ack_exchange_request_ingress_ns": "10500",
                        "ack_exchange_response_egress_ns": "22000",
                        "ack_exchange_process_ns": "11500",
                        "ack_local_receive_ns": "22500",
                        "ack_rtt_ns": "12500",
                    },
                ],
            )

            report_text = output_path.read_text(encoding="utf-8")

        self.assertIn("### Ack RTT 三段拆解", report_text)
        self.assertIn("| 上行 send->Gate x_in | 2 |", report_text)
        self.assertIn("| Gate x_in->x_out | 2 |", report_text)
        self.assertIn("| 下行 Gate x_out->local | 2 |", report_text)
        self.assertIn("### 滑点分析", report_text)
        self.assertIn("| all filled | 2 | 0.5 |", report_text)
        self.assertIn("| entry | 1 | 2 |", report_text)
        self.assertIn("| exit | 1 | -1 |", report_text)
        self.assertIn("### Raw PnL 和胜率", report_text)
        self.assertIn("- actual win rate: `50.00%`", report_text)
        self.assertIn("- raw win rate: `50.00%`", report_text)
        self.assertIn("- raw gross PnL: `3`", report_text)
        self.assertIn("- raw net PnL: `1`", report_text)

    def test_loads_pair_freshness_from_live_wrapper_config(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            base = Path(temp_dir)
            nested_config_path = base / "configs" / "lead_lag.toml"
            nested_config_path.parent.mkdir(parents=True)
            write_config(nested_config_path)
            wrapper_config_path = base / "strategy.toml"
            write_file(
                wrapper_config_path,
                """
                [strategy]
                name = "lead_lag"
                config = "configs/lead_lag.toml"
                """,
            )

            rows = report.load_pair_freshness_rows(wrapper_config_path)

        self.assertEqual(
            rows,
            [
                {
                    "symbol": "PROVE_USDT",
                    "symbol_id": "4",
                    "lead_exchange": "binance",
                    "lag_exchange": "gate",
                    "max_lead_freshness_ms": "5",
                    "max_lag_freshness_ms": "20",
                }
            ],
        )


if __name__ == "__main__":
    unittest.main()
