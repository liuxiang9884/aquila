#!/home/liuxiang/dev/pyenv/lx/bin/python

import csv
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parents[2] / "lead_lag"
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import analyze_order_detail as orders


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


def write_bitget_config(path: Path) -> None:
    write_file(
        path,
        """
        [lead_lag]
        name = "lead_lag"
        version = "1.0"

        [[lead_lag.pairs]]
        symbol = "ALLO_USDT"
        symbol_id = 25
        lead_exchange = "binance"
        lag_exchange = "bitget"
        lag_taker_fee = 0.0002

        [lead_lag.pairs.execute]
        open_notional = 10.0
        trailing_stop = 0.01
        open_slippage_ticks = 8
        close_slippage_ticks = 8
        stoploss_slippage_ticks = 20
        close_retry_times = 2
        close_retry_slippage_step_ticks = 8
        """,
    )


def write_multi_exchange_catalog(path: Path) -> None:
    path.write_text(
        "symbol_id,symbol,exchange,exchange_symbol,base_asset,quote_asset,"
        "settle_asset,product_type,status,contract_type,price_tick,"
        "price_decimal_places,quantity_step,quantity_decimal_places,"
        "min_quantity,max_quantity,max_market_quantity,min_notional,"
        "notional_multiplier,contract_multiplier,price_limit_up,"
        "price_limit_down,market_price_bound\n"
        "25,ALLO_USDT,gate,ALLO_USDT,ALLO,USDT,USDT,linear_perpetual,"
        "TRADING,direct,0.00001,5,1.0,0,1.0,600000,400000,,10.0,10.0,"
        "0.1,0.1,0.025\n"
        "25,ALLO_USDT,bitget,ALLOUSDT,ALLO,USDT,USDT,linear_perpetual,"
        "online,perpetual,0.00001,5,1.0,0,1.0,1100000,170000,5.0,1.0,"
        "1.0,0.15,0.15,\n",
        encoding="utf-8",
    )


class AnalyzeOrderDetailTest(unittest.TestCase):
    def test_bitget_catalog_supports_strategy_and_exchange_symbol_aliases(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            catalog_path = Path(temp_dir) / "catalog.csv"
            write_multi_exchange_catalog(catalog_path)

            instruments = orders.load_instrument_catalog(
                catalog_path, exchange="bitget"
            )

        self.assertEqual(instruments["ALLO_USDT"]["contract_multiplier"], "1.0")
        self.assertEqual(instruments["ALLOUSDT"]["contract_multiplier"], "1.0")
        self.assertEqual(instruments["ALLOUSDT"]["price_tick"], "0.00001")

    def test_merges_bitget_gateway_feedback_and_late_ack_from_separate_logs(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            base = Path(temp_dir)
            strategy_log = base / "strategy.log"
            gateway_log = base / "gateway.log"
            feedback_log = base / "feedback.log"
            config_path = base / "strategy.toml"
            catalog_path = base / "catalog.csv"
            write_bitget_config(config_path)
            write_multi_exchange_catalog(catalog_path)
            write_file(
                strategy_log,
                """
                I2026-07-20 16:37:14.241223501 1:1 strategy.h:LogStrategyOrderSubmitted:390] lead_lag_order_submitted local_order_id=432345564227567617 parent_id=1 route_id=0 trigger_exchange=kBinance trigger_symbol_id=25 trigger_exchange_ns=1784565434240000000 trigger_local_ns=1784565434241210884 on_book_ticker_entry_ns=1784565434241211810 signal_decision_ns=1784565434241212771 lead_exchange_ns=1784565434240000000 lead_local_ns=1784565434241210884 signal_lead_id=11092224700137 lead_freshness_ns=1212771 lag_exchange_ns=1784565434233000000 lag_local_ns=1784565434233965440 signal_lag_id=427561015091 lag_freshness_ns=8212771 max_lead_freshness_ns=3000000 max_lag_freshness_ns=500000000 freshness_guard_pass=true freshness_reject_reason=- symbol=ALLOUSDT symbol_id=25 signal_role=kLead order_role=entry action=kOpenLong side=kBuy reduce_only=false position_id=1 position_event=kEntrySubmit position_direction=kLong entry_local_order_id=432345564227567617 quantity=20 raw_price=0.48834 order_price=0.48842 slippage_ticks=8 price_tick=0.00001 target_open_notional=10 estimated_notional=9.7684 active_groups=1 place_status=kOk
                I2026-07-20 16:37:14.261971994 1:1 strategy.h:LogStrategyOrderFeedback:582] lead_lag_order_feedback kind=kCancelled local_order_id=432345564227567617 parent_id=1 route_id=0 exchange_order_id=1463138967177801733 cumulative_filled_quantity=0 left_quantity=20 cancelled_quantity=20 fill_price=0 role=kNone finish_reason=kImmediateOrCancel reject_reason=kUnknown exchange_update_ns=1784565434261000000 local_receive_ns=1784565434261959456 lead_exchange_ns=1784565434260000000 lag_exchange_ns=1784565434260000000 cancelled_lead_id=11092224702816 cancelled_lag_id=427561016251
                I2026-07-20 16:37:14.261973319 1:1 strategy.h:LogStrategyOrderFinished:681] lead_lag_order_finished local_order_id=432345564227567617 parent_id=1 route_id=0 symbol_id=25 symbol=ALLOUSDT status=kCancelled reduce_only=false position_id=1 position_direction=kLong order_role=entry entry_local_order_id=432345564227567617 order_finished_local_ns=1784565434261973115 quantity=20 cumulative_filled_quantity=0 average_fill_price=0 last_fill_price=0 exchange_order_id=1463138967177801733 active_groups=0 request_send_local_ns=1784565434241221520 ack_local_receive_ns=0 response_local_receive_ns=0 ack_exchange_ns=0 response_exchange_ns=0 accepted_exchange_ns=0 finish_exchange_ns=1784565434261000000 ack_rtt_ns=0 response_rtt_ns=0 ack_exchange_to_local_ns=0 response_exchange_to_local_ns=0 exchange_lifecycle_ns=0 lead_exchange_ns=1784565434260000000 lag_exchange_ns=1784565434260000000
                """,
            )
            write_file(
                gateway_log,
                """
                I2026-07-20 16:37:14.241236349 2:2 order_session.h:LogOrderSend:855] bitget_order_send request_type=kPlaceOrder request_sequence=1 local_order_id=432345564227567617 request_send_local_ns=1784565434241228182 request_send_monotonic_ns=11764488381693989 order_encode_done_realtime_ns=1784565434241228143 inflight=1
                I2026-07-20 16:37:14.261843452 2:2 order_session.h:LogOrderResponse:871] bitget_order_response response_kind=kAck request_type=kPlaceOrder request_sequence=1 local_order_id=432345564227567617 exchange_order_id=1463138967177801733 error_code=0 request_send_local_ns=1784565434241228182 local_receive_ns=1784565434261833430 exchange_ns=1784565434261000000 ack_rtt_ns=20605248 connection_id_hash=16228680375236014991 request_send_realtime_ns=1784565434241228182 request_send_monotonic_ns=11764488381693989 write_complete_realtime_ns=1784565434241235858 write_complete_monotonic_ns=11764488381701548 ack_receive_realtime_ns=1784565434261833430 ack_receive_monotonic_ns=11764488402299108 ack_rtt_monotonic_ns=20605119 write_complete_to_ack_monotonic_ns=20597560 place_creation_time_ms=1784565434259 exchange_message_time_ms=1784565434261
                """,
            )
            write_file(
                feedback_log,
                """
                I2026-07-20 16:37:14.261968699 3:3 order_feedback_session.h:LogOrderProtocolUpdate:972] bitget_order_feedback_protocol_update topic=order connection_generation=1 local_message_sequence=10 batch_data_index=0 client_oid=a-432345564227567617 order_id=1463138967177801733 order_status=cancelled cancel_reason=ioc_not_full_cancel exchange_message_time_ms=1784565434261 created_time_ms=1784565434259 updated_time_ms=1784565434261 local_receive_realtime_ns=1784565434261959456 local_receive_monotonic_ns=11764488402425136
                """,
            )

            result = orders.analyze_order_detail(
                strategy_log,
                additional_log_paths=[gateway_log, feedback_log],
                config_path=config_path,
                instrument_catalog_path=catalog_path,
                exchange="bitget",
                run_id="bitget-run",
            )
            latency_rows = orders.build_latency_detail_rows(result.rows)

        self.assertEqual(len(result.rows), 1)
        row = result.rows[0]
        self.assertEqual(row["exchange"], "bitget")
        self.assertEqual(row["contract_multiplier"], "1.0")
        self.assertEqual(row["fee_rate_config"], "0.0002")
        self.assertEqual(row["request_sequence"], "1")
        self.assertEqual(row["request_send_local_ns"], "1784565434241228182")
        self.assertEqual(row["ack_local_receive_ns"], "1784565434261833430")
        self.assertEqual(row["ack_rtt_ns"], "20605248")
        self.assertEqual(row["place_creation_exchange_ns"], "1784565434259000000")
        self.assertEqual(row["ack_exchange_ns"], "1784565434261000000")
        self.assertEqual(row["write_complete_local_ns"], "1784565434241235858")
        self.assertEqual(row["cancel_reason"], "ioc_not_full_cancel")
        self.assertEqual(row["feedback_created_exchange_ns"], "1784565434259000000")
        self.assertEqual(row["feedback_updated_exchange_ns"], "1784565434261000000")
        self.assertEqual(row["feedback_local_receive_ns"], "1784565434261959456")
        self.assertEqual(row["status"], "kCancelled")
        self.assertEqual(len(latency_rows), 1)
        self.assertEqual(latency_rows[0]["send_to_write_complete_local_ns"], "7676")
        self.assertEqual(latency_rows[0]["write_complete_to_ack_local_ns"], "20597560")
        self.assertEqual(latency_rows[0]["bitget_creation_to_terminal_ns"], "2000000")

    def test_catalog_contract_multiplier_overrides_notional_multiplier(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            base = Path(temp_dir)
            catalog_path = base / "catalog.csv"
            catalog_path.write_text(
                "symbol_id,symbol,exchange,exchange_symbol,base_asset,quote_asset,"
                "settle_asset,product_type,status,contract_type,price_tick,"
                "price_decimal_places,quantity_step,quantity_decimal_places,"
                "min_quantity,max_quantity,max_market_quantity,min_notional,"
                "notional_multiplier,contract_multiplier,price_limit_up,"
                "price_limit_down,market_price_bound\n"
                "4,PROVE_USDT,gate,PROVE_USDT,PROVE,USDT,USDT,"
                "linear_perpetual,TRADING,direct,0.0001,4,1.0,0,"
                "1.0,68000.0,45000.0,,10.0,12.5,0.1,0.1,0.035\n",
                encoding="utf-8",
            )

            instruments = orders.load_instrument_catalog(catalog_path)

        self.assertEqual(instruments["PROVE_USDT"]["contract_multiplier"], "12.5")

    def test_parses_drift_guard_rejected_order_intent(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            base = Path(temp_dir)
            log_path = base / "run.log"
            write_file(
                log_path,
                """
                I2026-06-25 09:00:00.000000100 1:1 strategy.h:LogStrategySignalTriggered:155] lead_lag_signal_triggered trigger_exchange=kBinance trigger_symbol_id=4 trigger_exchange_ns=1782360000000000000 trigger_local_ns=1782360000000010000 on_book_ticker_entry_ns=1782360000000015000 signal_decision_ns=1782360000000020000 lead_exchange_ns=1782360000000000000 lead_local_ns=1782360000000010000 signal_lead_id=7001 lead_freshness_ns=20000 lag_exchange_ns=1782359999999990000 lag_local_ns=1782360000000005000 signal_lag_id=7002 lag_freshness_ns=30000 symbol=PROVE_USDT symbol_id=4 role=kLead action=kOpenLong side=kBuy reduce_only=false position_id=0 raw_price=0.2711
                W2026-06-25 09:00:00.000000200 1:1 strategy.h:LogStrategyOrderIntentRejected:407] lead_lag_order_intent_rejected reason=drift_guard trigger_exchange_ns=1782360000000000000 trigger_local_ns=1782360000000010000 on_book_ticker_entry_ns=1782360000000015000 signal_decision_ns=1782360000000020000 lead_exchange_ns=1782360000000000000 lead_local_ns=1782360000000010000 signal_lead_id=7001 lead_freshness_ns=20000 lag_exchange_ns=1782359999999990000 lag_local_ns=1782360000000005000 signal_lag_id=7002 lag_freshness_ns=30000 symbol=PROVE_USDT symbol_id=4 action=kOpenLong side=kBuy reduce_only=false position_id=0 quantity=0 price=0.2711 raw_price=0.2711 order_price=0.2711 slippage_ticks=0 price_tick=0.0001 target_open_notional=100 estimated_notional=0 gross_before=0 gross_after=0 max_gross_notional=0 local_order_id=0 place_status=-
                """,
            )

            result = orders.analyze_order_detail(log_path)

        self.assertEqual(len(result.rows), 1)
        row = result.rows[0]
        self.assertEqual(row["source_schema"], "intent_rejected_v1")
        self.assertEqual(row["status"], "kRejected")
        self.assertEqual(row["reject_reason"], "drift_guard")
        self.assertEqual(row["local_order_id"], "0")
        self.assertEqual(row["symbol"], "PROVE_USDT")
        self.assertEqual(row["symbol_id"], "4")
        self.assertEqual(row["action"], "kOpenLong")
        self.assertEqual(row["side"], "kBuy")
        self.assertEqual(row["reduce_only"], "false")
        self.assertEqual(row["raw_price"], "0.2711")
        self.assertEqual(row["order_price"], "0.2711")
        self.assertEqual(row["price_text"], "0.2711")
        self.assertEqual(row["price_tick"], "0.0001")
        self.assertEqual(row["slippage_ticks"], "0")
        self.assertEqual(row["quantity"], "0")
        self.assertEqual(row["quantity_text"], "0")
        self.assertEqual(row["estimated_notional"], "0")
        self.assertEqual(row["signal_decision_ns"], "1782360000000020000")
        self.assertEqual(row["lead_exchange_ns"], "1782360000000000000")
        self.assertEqual(row["lag_exchange_ns"], "1782359999999990000")
        self.assertNotIn("missing_submitted_log", row["warnings"])
        self.assertNotIn("missing_symbol", row["warnings"])

    def test_parses_parallel_limit_rejected_order_intent(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            base = Path(temp_dir)
            log_path = base / "run.log"
            write_file(
                log_path,
                """
                I2026-06-25 09:00:00.000000100 1:1 strategy.h:LogStrategySignalTriggered:155] lead_lag_signal_triggered trigger_exchange=kBinance trigger_symbol_id=4 trigger_exchange_ns=1782360000000000000 trigger_local_ns=1782360000000010000 on_book_ticker_entry_ns=1782360000000015000 signal_decision_ns=1782360000000020000 lead_exchange_ns=1782360000000000000 lead_local_ns=1782360000000010000 signal_lead_id=7001 lead_freshness_ns=20000 lag_exchange_ns=1782359999999990000 lag_local_ns=1782360000000005000 signal_lag_id=7002 lag_freshness_ns=30000 symbol=PROVE_USDT symbol_id=4 role=kLead action=kOpenLong side=kBuy reduce_only=false position_id=0 raw_price=0.2711
                W2026-06-25 09:00:00.000000200 1:1 strategy.h:LogStrategyOrderIntentRejected:407] lead_lag_order_intent_rejected reason=parallel_limit trigger_exchange_ns=1782360000000000000 trigger_local_ns=1782360000000010000 on_book_ticker_entry_ns=1782360000000015000 signal_decision_ns=1782360000000020000 lead_exchange_ns=1782360000000000000 lead_local_ns=1782360000000010000 signal_lead_id=7001 lead_freshness_ns=20000 lag_exchange_ns=1782359999999990000 lag_local_ns=1782360000000005000 signal_lag_id=7002 lag_freshness_ns=30000 symbol=PROVE_USDT symbol_id=4 action=kOpenLong side=kBuy reduce_only=false position_id=0 quantity=0 price=0.2711 raw_price=0.2711 order_price=0.2711 slippage_ticks=0 price_tick=0.0001 target_open_notional=100 estimated_notional=0 gross_before=0 gross_after=0 max_gross_notional=0 local_order_id=0 place_status=-
                """,
            )

            result = orders.analyze_order_detail(log_path)

        self.assertEqual(len(result.rows), 1)
        row = result.rows[0]
        self.assertEqual(row["source_schema"], "intent_rejected_v1")
        self.assertEqual(row["status"], "kRejected")
        self.assertEqual(row["reject_reason"], "parallel_limit")
        self.assertEqual(row["local_order_id"], "0")
        self.assertEqual(row["symbol"], "PROVE_USDT")
        self.assertEqual(row["symbol_id"], "4")
        self.assertEqual(row["action"], "kOpenLong")
        self.assertEqual(row["side"], "kBuy")
        self.assertEqual(row["reduce_only"], "false")
        self.assertNotIn("missing_submitted_log", row["warnings"])
        self.assertNotIn("missing_symbol", row["warnings"])

    def test_parses_additional_rejected_order_intent_reasons(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            base = Path(temp_dir)
            log_path = base / "run.log"
            write_file(
                log_path,
                """
                W2026-06-25 09:00:00.000000200 1:1 strategy.h:LogStrategyOrderIntentRejected:407] lead_lag_order_intent_rejected reason=stale_lag_quote trigger_exchange_ns=1782360000000000000 trigger_local_ns=1782360000000010000 on_book_ticker_entry_ns=1782360000000015000 signal_decision_ns=1782360000000020000 lead_exchange_ns=1782360000000000000 lead_local_ns=1782360000000010000 signal_lead_id=7001 lead_freshness_ns=20000 lag_exchange_ns=1782359999999990000 lag_local_ns=1782360000000005000 signal_lag_id=7002 lag_freshness_ns=30000000 max_lead_freshness_ns=5000000 max_lag_freshness_ns=20000000 freshness_guard_pass=false freshness_reject_reason=stale_lag_quote symbol=PROVE_USDT symbol_id=4 action=kOpenLong side=kBuy reduce_only=false position_id=0 quantity=0 price=0.2711 raw_price=0.2711 order_price=0.2711 slippage_ticks=0 price_tick=0.0001 target_open_notional=100 estimated_notional=0 gross_before=0 gross_after=0 max_gross_notional=0 local_order_id=0 place_status=-
                W2026-06-25 09:00:00.000000300 1:1 strategy.h:LogStrategyOrderIntentRejected:407] lead_lag_order_intent_rejected reason=risk_limit trigger_exchange_ns=1782360000000100000 trigger_local_ns=1782360000000110000 on_book_ticker_entry_ns=1782360000000115000 signal_decision_ns=1782360000000120000 lead_exchange_ns=1782360000000100000 lead_local_ns=1782360000000110000 signal_lead_id=7011 lead_freshness_ns=20000 lag_exchange_ns=1782360000000090000 lag_local_ns=1782360000000105000 signal_lag_id=7012 lag_freshness_ns=30000 max_lead_freshness_ns=5000000 max_lag_freshness_ns=20000000 freshness_guard_pass=true freshness_reject_reason=none symbol=PROVE_USDT symbol_id=4 action=kOpenLong side=kBuy reduce_only=false position_id=0 quantity=36 price=0.2722 raw_price=0.2722 order_price=0.2722 slippage_ticks=0 price_tick=0.0001 target_open_notional=100 estimated_notional=97.992 gross_before=990 gross_after=1087.992 max_gross_notional=1000 local_order_id=0 place_status=-
                W2026-06-25 09:00:00.000000400 1:1 strategy.h:LogStrategyOrderIntentRejected:407] lead_lag_order_intent_rejected reason=zero_quantity trigger_exchange_ns=1782360000000200000 trigger_local_ns=1782360000000210000 on_book_ticker_entry_ns=1782360000000215000 signal_decision_ns=1782360000000220000 lead_exchange_ns=1782360000000200000 lead_local_ns=1782360000000210000 signal_lead_id=7021 lead_freshness_ns=20000 lag_exchange_ns=1782360000000190000 lag_local_ns=1782360000000205000 signal_lag_id=7022 lag_freshness_ns=30000 max_lead_freshness_ns=5000000 max_lag_freshness_ns=20000000 freshness_guard_pass=true freshness_reject_reason=none symbol=PROVE_USDT symbol_id=4 action=kOpenLong side=kBuy reduce_only=false position_id=0 quantity=0 price=0.2733 raw_price=0.2733 order_price=0.2733 slippage_ticks=0 price_tick=0.0001 target_open_notional=100 estimated_notional=0 gross_before=0 gross_after=0 max_gross_notional=0 local_order_id=0 place_status=-
                """,
            )

            result = orders.analyze_order_detail(log_path)

        self.assertEqual(len(result.rows), 3)
        rows = {row["reject_reason"]: row for row in result.rows}
        self.assertEqual(
            set(rows), {"stale_lag_quote", "risk_limit", "zero_quantity"}
        )
        freshness = rows["stale_lag_quote"]
        self.assertEqual(freshness["source_schema"], "intent_rejected_v1")
        self.assertEqual(freshness["status"], "kRejected")
        self.assertEqual(freshness["freshness_guard_pass"], "false")
        self.assertEqual(freshness["freshness_reject_reason"], "stale_lag_quote")
        self.assertEqual(freshness["max_lag_freshness_ns"], "20000000")
        self.assertNotIn("missing_submitted_log", freshness["warnings"])

        risk = rows["risk_limit"]
        self.assertEqual(risk["quantity"], "36")
        self.assertEqual(risk["estimated_notional"], "97.992")
        self.assertEqual(risk["local_order_id"], "0")
        self.assertNotIn("missing_submitted_log", risk["warnings"])

        zero_quantity = rows["zero_quantity"]
        self.assertEqual(zero_quantity["quantity"], "0")
        self.assertEqual(zero_quantity["quantity_text"], "0")
        self.assertEqual(zero_quantity["price_text"], "0.2733")
        self.assertNotIn("missing_submitted_log", zero_quantity["warnings"])

    def test_parses_submitted_order_and_calculates_fill_quality(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            base = Path(temp_dir)
            log_path = base / "run.log"
            config_path = base / "strategy.toml"
            catalog_path = base / "catalog.csv"
            output_path = base / "orders.csv"
            write_config(config_path)
            write_catalog(catalog_path)
            write_file(
                log_path,
                """
                I2026-05-25 02:29:35.105545332 1:1 strategy.h:LogStrategySignalTriggered:155] lead_lag_signal_triggered trigger_exchange=kBinance trigger_symbol_id=4 trigger_exchange_ns=1779676175105510000 trigger_local_ns=1779676175105520000 on_book_ticker_entry_ns=1779676175105530000 signal_decision_ns=1779676175105540000 lead_exchange_ns=1779676175105510000 lead_local_ns=1779676175105520000 signal_lead_id=5001 lead_freshness_ns=30000 lag_exchange_ns=1779676175105500000 lag_local_ns=1779676175105510000 signal_lag_id=5002 lag_freshness_ns=40000 symbol=PROVE_USDT symbol_id=4 role=kLead action=kOpenLong side=kBuy reduce_only=false position_id=0 raw_price=0.2711
                I2026-05-25 02:29:35.105549026 1:1 strategy.h:LogStrategyOrderIntent:189] lead_lag_order_intent trigger_exchange_ns=1779676175105510000 trigger_local_ns=1779676175105520000 on_book_ticker_entry_ns=1779676175105530000 signal_decision_ns=1779676175105540000 lead_exchange_ns=1779676175105510000 lead_local_ns=1779676175105520000 signal_lead_id=5001 lead_freshness_ns=30000 lag_exchange_ns=1779676175105500000 lag_local_ns=1779676175105510000 signal_lag_id=5002 lag_freshness_ns=40000 symbol=PROVE_USDT symbol_id=4 action=kOpenLong side=kBuy reduce_only=false position_id=0 quantity=36 price=0.2714 raw_price=0.2711 order_price=0.2714 slippage_ticks=3 price_tick=0.0001 target_open_notional=100 estimated_notional=97.704 active_groups=0
                I2026-05-25 02:29:35.105560000 1:1 order_session.h:LogGateOrderSessionConnected:1] gate_order_session_connected order_session_id=9 owner_thread_cpu=4 endpoint_available=true local_ip=10.0.0.1 local_port=12345 remote_ip=1.2.3.4 remote_port=443
                I2026-05-25 02:29:35.105563683 1:1 order_session.h:LogGatePlaceOrderSent:466] gate_order_send_ok type=place group_id=777 route_id=3 local_order_id=288230376151711749 request_sequence=6 encoded_request_id=144115188075855878 contract=PROVE_USDT side=kBuy quantity=36 price=0.2714 tif=kImmediateOrCancel reduce_only=false inflight=5 request_send_local_ns=1779676175105554883 order_session_id=9 send_cpu=4
                I2026-05-25 02:29:35.105566021 1:1 strategy.h:LogStrategyOrderSubmitted:1] lead_lag_order_submitted local_order_id=288230376151711749 group_id=777 route_id=3 trigger_exchange=kBinance trigger_symbol_id=4 trigger_exchange_ns=1779676175105510000 trigger_local_ns=1779676175105520000 on_book_ticker_entry_ns=1779676175105530000 signal_decision_ns=1779676175105540000 lead_exchange_ns=1779676175105510000 lead_local_ns=1779676175105520000 signal_lead_id=5001 lead_freshness_ns=30000 lag_exchange_ns=1779676175105500000 lag_local_ns=1779676175105510000 signal_lag_id=5002 lag_freshness_ns=40000 symbol=PROVE_USDT symbol_id=4 signal_role=kLead order_role=entry action=kOpenLong side=kBuy reduce_only=false position_id=1 position_event=kEntrySubmit position_direction=kLong entry_local_order_id=288230376151711749 quantity=36 quantity_text=36 raw_price=0.2711 order_price=0.2714 price_text=0.2714 slippage_ticks=3 price_tick=0.0001 target_open_notional=100 estimated_notional=97.704 active_groups=1 place_status=kOk
                I2026-05-25 02:29:35.111554171 1:1 order_session.h:LogGateOrderResponse:517] gate_order_response kind=kAck group_id=777 route_id=3 local_order_id=288230376151711749 exchange_order_id=0 request_sequence=6 channel=2 http_status=200 error_label_hash=0 error_label= error_message= local_receive_ns=1779676175111547348 exchange_ns=1779676175107083000 exchange_request_ingress_ns=1779676175107000000 exchange_response_egress_ns=1779676175107083000 exchange_process_ns=83000 exchange_to_local_ns=4464348 order_session_id=9 ack_cpu=4 tcp_info_available=true tcp_info_rtt_us=7123 tcp_info_rttvar_us=456 tcp_info_retrans=0 tcp_info_total_retrans=1 tcp_info_unacked=0 tcp_info_snd_cwnd=10
                I2026-05-25 02:29:35.111555000 1:1 strategy.h:LogStrategyOrderResponse:391] lead_lag_order_response kind=kAck local_order_id=288230376151711749 group_id=777 route_id=3 exchange_order_id=0 local_receive_ns=1779676175111547348 exchange_ns=1779676175107083000 exchange_to_local_ns=4464348 ack_rtt_ns=5992465 response_rtt_ns=0 lead_exchange_ns=1779676175111000000 lag_exchange_ns=1779676175110900000 ack_lead_id=7001 ack_lag_id=7002
                I2026-05-25 02:29:35.117544030 1:1 strategy.h:LogStrategyOrderFeedback:272] lead_lag_order_feedback kind=kFilled local_order_id=288230376151711749 group_id=777 route_id=3 exchange_order_id=260082878984634644 cumulative_filled_quantity=36 left_quantity=0 cancelled_quantity=0 fill_price=0.2714 role=kTaker finish_reason=kUnknown reject_reason=kUnknown filled_lead_id=8001 filled_lag_id=8002
                I2026-05-25 02:29:35.117545430 1:1 strategy.h:LogStrategyOrderFinished:335] lead_lag_order_finished local_order_id=288230376151711749 group_id=777 route_id=3 symbol_id=4 symbol=PROVE_USDT status=kFilled reduce_only=false position_id=1 position_direction=kLong order_role=entry entry_local_order_id=288230376151711749 order_finished_local_ns=1779676175117545430 quantity=36 cumulative_filled_quantity=36 average_fill_price=0.2714 last_fill_price=0.2714 exchange_order_id=260082878984634644 active_groups=1 request_send_local_ns=1779676175105554883 ack_local_receive_ns=1779676175111547348 response_local_receive_ns=0 ack_exchange_ns=1779676175107083000 response_exchange_ns=0 accepted_exchange_ns=0 finish_exchange_ns=1779676175113000000 ack_rtt_ns=5992465 response_rtt_ns=0 ack_exchange_to_local_ns=4464348 response_exchange_to_local_ns=0 exchange_lifecycle_ns=0
                """,
            )

            result = orders.analyze_order_detail(
                log_path,
                config_path=config_path,
                instrument_catalog_path=catalog_path,
            )
            latency_rows = orders.build_latency_detail_rows(result.rows)
            orders.write_order_detail_csv(result.rows, output_path)

            with output_path.open(newline="", encoding="utf-8") as input_file:
                row = next(csv.DictReader(input_file))

        self.assertEqual(row["local_order_id"], "288230376151711749")
        self.assertEqual(row["group_id"], "777")
        self.assertEqual(row["route_id"], "3")
        self.assertEqual(row["text_order_id"], "t-288230376151711749")
        self.assertEqual(row["exchange_order_id"], "260082878984634644")
        self.assertEqual(row["signal_role"], "kLead")
        self.assertNotIn("trigger_ticker_id", row)
        self.assertEqual(row["lead_exchange_ns"], "1779676175105510000")
        self.assertEqual(row["lag_exchange_ns"], "1779676175105500000")
        self.assertEqual(row["signal_lead_id"], "5001")
        self.assertEqual(row["signal_lag_id"], "5002")
        self.assertEqual(row["ack_lead_id"], "7001")
        self.assertEqual(row["ack_lag_id"], "7002")
        self.assertEqual(row["filled_lead_id"], "8001")
        self.assertEqual(row["filled_lag_id"], "8002")
        self.assertEqual(latency_rows[0]["signal_lead_id"], "5001")
        self.assertEqual(latency_rows[0]["group_id"], "777")
        self.assertEqual(latency_rows[0]["route_id"], "3")
        self.assertEqual(latency_rows[0]["signal_lag_id"], "5002")
        self.assertEqual(latency_rows[0]["ack_lead_id"], "7001")
        self.assertEqual(latency_rows[0]["ack_lag_id"], "7002")
        self.assertEqual(latency_rows[0]["filled_lead_id"], "8001")
        self.assertEqual(latency_rows[0]["filled_lag_id"], "8002")
        self.assertEqual(row["order_role"], "entry")
        self.assertEqual(row["position_id"], "1")
        self.assertEqual(row["position_event"], "kEntrySubmit")
        self.assertEqual(row["position_direction"], "kLong")
        self.assertEqual(row["entry_local_order_id"], "288230376151711749")
        self.assertEqual(row["fill_role"], "kTaker")
        self.assertEqual(row["time_in_force"], "kImmediateOrCancel")
        self.assertEqual(row["raw_price"], "0.2711")
        self.assertEqual(row["order_price"], "0.2714")
        self.assertEqual(row["quantity_text"], "36")
        self.assertEqual(row["contract_multiplier"], "10.0")
        self.assertEqual(row["filled_notional"], "97.704")
        self.assertEqual(row["exec_slippage_price"], "0.0003")
        self.assertEqual(row["exec_slippage_ticks"], "3")
        self.assertEqual(row["exec_slippage_quote"], "0.108")
        self.assertEqual(row["fee_rate_config"], "0.00016")
        self.assertEqual(row["fee_quote_estimated"], "0.01563264")
        self.assertEqual(row["fee_source"], "config_estimated")
        self.assertEqual(row["ack_rtt_ns"], "5992465")
        self.assertEqual(
            row["ack_exchange_request_ingress_ns"], "1779676175107000000"
        )
        self.assertEqual(
            row["ack_exchange_response_egress_ns"], "1779676175107083000"
        )
        self.assertEqual(row["ack_exchange_process_ns"], "83000")
        self.assertEqual(row["order_session_id"], "9")
        self.assertEqual(row["owner_thread_cpu"], "4")
        self.assertEqual(row["local_ip"], "10.0.0.1")
        self.assertEqual(row["local_port"], "12345")
        self.assertEqual(row["remote_ip"], "1.2.3.4")
        self.assertEqual(row["remote_port"], "443")
        self.assertEqual(row["send_cpu"], "4")
        self.assertEqual(row["ack_cpu"], "4")
        self.assertEqual(row["tcp_info_available"], "true")
        self.assertEqual(row["tcp_info_rtt_us"], "7123")
        self.assertEqual(row["tcp_info_total_retrans"], "1")
        self.assertEqual(row["order_finished_local_ns"], "1779676175117545430")
        self.assertEqual(row["source_schema"], "submitted_v2")
        self.assertEqual(row["warnings"], "")

    def test_maps_cancelled_feedback_stage_book_ticker_ids(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            base = Path(temp_dir)
            log_path = base / "run.log"
            write_file(
                log_path,
                """
                I2026-05-25 02:29:35.105566021 1:1 strategy.h:LogStrategyOrderSubmitted:1] lead_lag_order_submitted local_order_id=288230376151711750 group_id=1 trigger_exchange=kBinance trigger_symbol_id=4 trigger_exchange_ns=1779676175125510000 trigger_local_ns=1779676175125520000 on_book_ticker_entry_ns=1779676175125530000 signal_decision_ns=1779676175125540000 lead_exchange_ns=1779676175125510000 lead_local_ns=1779676175125520000 signal_lead_id=5101 lead_freshness_ns=30000 lag_exchange_ns=1779676175125500000 lag_local_ns=1779676175125510000 signal_lag_id=5102 lag_freshness_ns=40000 symbol=PROVE_USDT symbol_id=4 signal_role=kLead order_role=entry action=kOpenLong side=kBuy reduce_only=false position_id=1 position_event=kEntrySubmit position_direction=kLong entry_local_order_id=288230376151711750 quantity=36 quantity_text=36 raw_price=0.2711 order_price=0.2714 price_text=0.2714 slippage_ticks=3 price_tick=0.0001 target_open_notional=100 estimated_notional=97.704 active_groups=1 place_status=kOk
                I2026-05-25 02:29:35.117544030 1:1 strategy.h:LogStrategyOrderFeedback:272] lead_lag_order_feedback kind=kCancelled local_order_id=288230376151711750 exchange_order_id=260082878984634645 cumulative_filled_quantity=0 left_quantity=36 cancelled_quantity=36 fill_price=0 role=kTaker finish_reason=kImmediateOrCancel reject_reason=kUnknown exchange_update_ns=1779676175129000000 local_receive_ns=1779676175130000000 lead_exchange_ns=1779676175128000000 lag_exchange_ns=1779676175127900000 cancelled_lead_id=8101 cancelled_lag_id=8102
                """,
            )

            result = orders.analyze_order_detail(log_path)
            latency_rows = orders.build_latency_detail_rows(result.rows)

        self.assertEqual(len(result.rows), 1)
        row = result.rows[0]
        self.assertEqual(row["signal_lead_id"], "5101")
        self.assertEqual(row["signal_lag_id"], "5102")
        self.assertEqual(row["cancelled_lead_id"], "8101")
        self.assertEqual(row["cancelled_lag_id"], "8102")
        self.assertEqual(latency_rows[0]["cancelled_lead_id"], "8101")
        self.assertEqual(latency_rows[0]["cancelled_lag_id"], "8102")

    def test_prefers_submitted_log_for_final_group_and_quantity_text(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            base = Path(temp_dir)
            log_path = base / "run.log"
            write_file(
                log_path,
                """
                I2026-05-25 02:29:35.105549026 1:1 strategy.h:LogStrategyOrderSubmitted:1] lead_lag_order_submitted local_order_id=288230376151711750 group_id=1 trigger_exchange=kBinance trigger_symbol_id=4 trigger_exchange_ns=1779676175125510000 trigger_local_ns=1779676175125520000 on_book_ticker_entry_ns=1779676175125530000 signal_decision_ns=1779676175125540000 lead_exchange_ns=1779676175125510000 lag_exchange_ns=1779676175125500000 symbol=PROVE_USDT symbol_id=4 signal_role=kLead order_role=exit action=kCloseLong side=kSell reduce_only=true position_id=1 position_event=kExitSubmit position_direction=kLong entry_local_order_id=288230376151711749 quantity=36 quantity_text=36 raw_price=0.2713 order_price=0.271 price_text=0.2710 slippage_ticks=3 price_tick=0.0001 target_open_notional=100 estimated_notional=97.56 active_groups=1 place_status=kOk
                I2026-05-25 02:29:35.125612335 1:1 order_session.h:LogGatePlaceOrderSent:466] gate_order_send_ok type=place local_order_id=288230376151711750 request_sequence=7 encoded_request_id=144115188075855879 contract=PROVE_USDT side=kSell quantity=36 price=0.2710 tif=kImmediateOrCancel reduce_only=true inflight=6 request_send_local_ns=1779676175125607942
                I2026-05-25 02:29:35.129547287 1:1 order_session.h:LogGateOrderResponse:517] gate_order_response kind=kAck local_order_id=288230376151711750 exchange_order_id=0 request_sequence=7 channel=2 http_status=200 error_label_hash=0 error_label= error_message= local_receive_ns=1779676175129546235 exchange_ns=1779676175127009000 exchange_to_local_ns=2537235
                I2026-05-25 02:29:35.135543485 1:1 strategy.h:LogStrategyOrderFinished:335] lead_lag_order_finished local_order_id=288230376151711750 symbol_id=4 symbol=PROVE_USDT status=kFilled reduce_only=true position_id=1 position_direction=kLong order_role=exit entry_local_order_id=288230376151711749 order_finished_local_ns=1779676175135543485 quantity=36 cumulative_filled_quantity=36 average_fill_price=0.271244 last_fill_price=0.271244 exchange_order_id=260082878984634672 active_groups=0 request_send_local_ns=1779676175125607942 ack_local_receive_ns=1779676175129546235 response_local_receive_ns=0 ack_exchange_ns=1779676175127009000 response_exchange_ns=0 accepted_exchange_ns=0 finish_exchange_ns=1779676175128000000 ack_rtt_ns=3938293 response_rtt_ns=0 ack_exchange_to_local_ns=2537235 response_exchange_to_local_ns=0 exchange_lifecycle_ns=0
                """,
            )

            result = orders.analyze_order_detail(log_path)

        self.assertEqual(len(result.rows), 1)
        row = result.rows[0]
        self.assertEqual(row["source_schema"], "submitted_v2")

        self.assertEqual(row["position_id"], "1")
        self.assertEqual(row["position_event"], "kExitSubmit")
        self.assertEqual(row["position_direction"], "kLong")
        self.assertEqual(row["entry_local_order_id"], "288230376151711749")
        self.assertEqual(row["order_role"], "exit")
        self.assertEqual(row["quantity_text"], "36")
        self.assertEqual(row["price_text"], "0.2710")
        self.assertEqual(row["order_finished_local_ns"], "1779676175135543485")
        self.assertEqual(row["warnings"], "")

    def test_ignores_legacy_submitted_log_without_group_id(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            log_path = Path(temp_dir) / "run.log"
            write_file(
                log_path,
                """
                I2026-05-25 02:29:35.105566021 1:1 strategy.h:LogStrategyOrderSubmitted:1] lead_lag_order_submitted local_order_id=42 parent_id=777 route_id=3 symbol=PROVE_USDT symbol_id=4 action=kOpenLong side=kBuy reduce_only=false quantity=1 order_price=1 place_status=kOk
                """,
            )

            result = orders.analyze_order_detail(log_path)

        self.assertEqual(result.rows, [])

    def test_multi_group_identity_isolated_across_symbol_run_and_fanout(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            base = Path(temp_dir)
            run_a_log = base / "run-a.log"
            run_b_log = base / "run-b.log"
            write_file(
                run_a_log,
                """
                I] lead_lag_order_submitted local_order_id=1001 group_id=1 route_id=0 symbol=ALPHA_USDT symbol_id=4 order_role=entry action=kOpenLong side=kBuy reduce_only=false position_id=1 position_direction=kLong entry_local_order_id=1001 quantity=1 order_price=100 place_status=kOk
                I] lead_lag_order_submitted local_order_id=1002 group_id=1 route_id=1 symbol=ALPHA_USDT symbol_id=4 order_role=entry action=kOpenLong side=kBuy reduce_only=false position_id=1 position_direction=kLong entry_local_order_id=1001 quantity=1 order_price=100 place_status=kOk
                I] lead_lag_order_submitted local_order_id=1003 group_id=2 route_id=0 symbol=ALPHA_USDT symbol_id=4 order_role=entry action=kOpenLong side=kBuy reduce_only=false position_id=2 position_direction=kLong entry_local_order_id=1003 quantity=1 order_price=101 place_status=kOk
                I] lead_lag_order_submitted local_order_id=1004 group_id=2 route_id=0 symbol=ALPHA_USDT symbol_id=4 order_role=exit action=kCloseLong side=kSell reduce_only=true position_id=2 position_direction=kLong entry_local_order_id=1003 quantity=0.5 order_price=102 place_status=kOk
                I] lead_lag_order_submitted local_order_id=1005 group_id=2 route_id=1 symbol=ALPHA_USDT symbol_id=4 order_role=exit action=kCloseLong side=kSell reduce_only=true position_id=2 position_direction=kLong entry_local_order_id=1003 quantity=0.5 order_price=103 place_status=kOk
                I] lead_lag_order_submitted local_order_id=2001 group_id=1 route_id=0 symbol=BETA_USDT symbol_id=7 order_role=entry action=kOpenLong side=kBuy reduce_only=false position_id=1 position_direction=kLong entry_local_order_id=2001 quantity=1 order_price=200 place_status=kOk
                I] lead_lag_order_submitted local_order_id=9999 parent_id=1 route_id=3 symbol=ALPHA_USDT symbol_id=4 order_role=entry action=kOpenLong side=kBuy reduce_only=false position_id=9999 quantity=1 order_price=999 place_status=kOk
                I] lead_lag_order_feedback kind=kPartialFilled local_order_id=1003 group_id=2 route_id=0 cumulative_filled_quantity=0.4 left_quantity=0.6 fill_price=101 exchange_update_ns=30 local_receive_ns=31
                I] lead_lag_order_feedback kind=kFilled local_order_id=1003 group_id=2 route_id=0 cumulative_filled_quantity=1 left_quantity=0 fill_price=101 exchange_update_ns=40 local_receive_ns=41
                I] lead_lag_order_finished local_order_id=2001 group_id=1 route_id=0 symbol=BETA_USDT symbol_id=7 status=kFilled reduce_only=false position_id=1 position_direction=kLong order_role=entry entry_local_order_id=2001 cumulative_filled_quantity=1 average_fill_price=200 exchange_order_id=62001 order_finished_local_ns=70
                I] lead_lag_order_finished local_order_id=1005 group_id=2 route_id=1 symbol=ALPHA_USDT symbol_id=4 status=kFilled reduce_only=true position_id=2 position_direction=kLong order_role=exit entry_local_order_id=1003 cumulative_filled_quantity=0.5 average_fill_price=103 exchange_order_id=61005 order_finished_local_ns=65
                I] lead_lag_order_finished local_order_id=1004 group_id=2 route_id=0 symbol=ALPHA_USDT symbol_id=4 status=kFilled reduce_only=true position_id=2 position_direction=kLong order_role=exit entry_local_order_id=1003 cumulative_filled_quantity=0.5 average_fill_price=102 exchange_order_id=61004 order_finished_local_ns=60
                I] lead_lag_order_finished local_order_id=1003 group_id=2 route_id=0 symbol=ALPHA_USDT symbol_id=4 status=kFilled reduce_only=false position_id=2 position_direction=kLong order_role=entry entry_local_order_id=1003 cumulative_filled_quantity=1 average_fill_price=101 exchange_order_id=61003 order_finished_local_ns=50
                I] lead_lag_order_finished local_order_id=1002 group_id=1 route_id=1 symbol=ALPHA_USDT symbol_id=4 status=kFilled reduce_only=false position_id=1 position_direction=kLong order_role=entry entry_local_order_id=1001 cumulative_filled_quantity=1 average_fill_price=100 exchange_order_id=61002 order_finished_local_ns=45
                I] lead_lag_order_finished local_order_id=1001 group_id=1 route_id=0 symbol=ALPHA_USDT symbol_id=4 status=kFilled reduce_only=false position_id=1 position_direction=kLong order_role=entry entry_local_order_id=1001 cumulative_filled_quantity=1 average_fill_price=100 exchange_order_id=61001 order_finished_local_ns=44
                """,
            )
            write_file(
                run_b_log,
                """
                I] lead_lag_order_submitted local_order_id=3001 group_id=1 route_id=0 symbol=ALPHA_USDT symbol_id=4 order_role=entry action=kOpenLong side=kBuy reduce_only=false position_id=1 position_direction=kLong entry_local_order_id=3001 quantity=1 order_price=300 place_status=kOk
                I] lead_lag_order_finished local_order_id=3001 group_id=1 route_id=0 symbol=ALPHA_USDT symbol_id=4 status=kFilled reduce_only=false position_id=1 position_direction=kLong order_role=entry entry_local_order_id=3001 cumulative_filled_quantity=1 average_fill_price=300 exchange_order_id=63001 order_finished_local_ns=80
                """,
            )

            run_a = orders.analyze_order_detail(run_a_log, run_id="run-a")
            run_b = orders.analyze_order_detail(run_b_log, run_id="run-b")
            order_rows = run_a.rows + run_b.rows
            latency_rows = orders.build_latency_detail_rows(order_rows)
            position_rows = orders.build_position_detail_rows(order_rows)

        self.assertEqual(len(order_rows), 7)
        self.assertNotIn("9999", {row["local_order_id"] for row in order_rows})
        self.assertTrue(
            all(row["source_schema"] == "submitted_v2" for row in order_rows)
        )
        self.assertEqual(
            {
                (row["run_id"], row["symbol_id"], row["group_id"])
                for row in order_rows
            },
            {
                ("run-a", "4", "1"),
                ("run-a", "4", "2"),
                ("run-a", "7", "1"),
                ("run-b", "4", "1"),
            },
        )
        fanout_rows = [
            row
            for row in order_rows
            if (row["run_id"], row["symbol_id"], row["group_id"])
            == ("run-a", "4", "1")
        ]
        self.assertEqual(
            [(row["local_order_id"], row["route_id"]) for row in fanout_rows],
            [("1001", "0"), ("1002", "1")],
        )
        entry = next(row for row in order_rows if row["local_order_id"] == "1003")
        self.assertEqual(entry["group_id"], "2")
        self.assertEqual(entry["status"], "kFilled")
        self.assertEqual(entry["cumulative_filled_quantity"], "1")
        self.assertEqual(len(latency_rows), 7)
        self.assertEqual(
            {
                (row["run_id"], row["symbol_id"], row["group_id"])
                for row in latency_rows
            },
            {
                ("run-a", "4", "1"),
                ("run-a", "4", "2"),
                ("run-a", "7", "1"),
                ("run-b", "4", "1"),
            },
        )
        self.assertTrue(
            all(row["position_id"] == row["group_id"] for row in order_rows)
        )
        self.assertEqual(
            {
                row["entry_local_order_id"]
                for row in position_rows
                if row["run_id"] == "run-a"
                and row["symbol_id"] == "4"
                and row["position_id"] == "1"
            },
            {"1001", "1002"},
        )
        self.assertEqual(
            {
                (row["entry_local_order_id"], row["exit_local_order_id"])
                for row in position_rows
                if row["run_id"] == "run-a"
                and row["symbol_id"] == "4"
                and row["position_id"] == "2"
            },
            {("1003", "1004"), ("1003", "1005")},
        )
        self.assertIn(
            ("run-a", "7", "1", "2001"),
            {
                (
                    row["run_id"],
                    row["symbol_id"],
                    row["position_id"],
                    row["entry_local_order_id"],
                )
                for row in position_rows
            },
        )
        self.assertIn(
            ("run-b", "4", "1", "3001"),
            {
                (
                    row["run_id"],
                    row["symbol_id"],
                    row["position_id"],
                    row["entry_local_order_id"],
                )
                for row in position_rows
            },
        )

    def test_zero_gate_ack_header_fields_are_treated_as_missing(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            base = Path(temp_dir)
            log_path = base / "run.log"
            write_file(
                log_path,
                """
                I2026-05-25 02:29:35.105566021 1:1 strategy.h:LogStrategyOrderSubmitted:1] lead_lag_order_submitted local_order_id=42 group_id=1 symbol=PROVE_USDT symbol_id=4 signal_role=kLead order_role=entry action=kOpenLong side=kBuy reduce_only=false position_id=1 position_event=kEntrySubmit position_direction=kLong entry_local_order_id=42 quantity=36 quantity_text=36 raw_price=0.2711 order_price=0.2714 price_text=0.2714 slippage_ticks=3 price_tick=0.0001 place_status=kOk
                I2026-05-25 02:29:35.105563683 1:1 order_session.h:LogGatePlaceOrderSent:466] gate_order_send_ok type=place local_order_id=42 request_sequence=6 encoded_request_id=144115188075855878 contract=PROVE_USDT side=kBuy quantity=36 price=0.2714 tif=kImmediateOrCancel reduce_only=false inflight=5 request_send_local_ns=1779676175105554883
                I2026-05-25 02:29:35.111554171 1:1 order_session.h:LogGateOrderResponse:517] gate_order_response kind=kAck local_order_id=42 exchange_order_id=0 request_sequence=6 channel=2 http_status=200 error_label_hash=0 error_label= error_message= local_receive_ns=1779676175111547348 exchange_ns=1779676175107083000 exchange_request_ingress_ns=0 exchange_response_egress_ns=0 exchange_process_ns=0 exchange_to_local_ns=4464348
                I2026-05-25 02:29:35.117545430 1:1 strategy.h:LogStrategyOrderFinished:335] lead_lag_order_finished local_order_id=42 symbol_id=4 symbol=PROVE_USDT status=kFilled reduce_only=false position_id=1 position_direction=kLong order_role=entry entry_local_order_id=42 order_finished_local_ns=1779676175117545430 quantity=36 cumulative_filled_quantity=36 average_fill_price=0.2714 last_fill_price=0.2714 exchange_order_id=260082878984634644 request_send_local_ns=1779676175105554883 ack_local_receive_ns=1779676175111547348 response_local_receive_ns=0 ack_exchange_ns=1779676175107083000 response_exchange_ns=0 accepted_exchange_ns=0 finish_exchange_ns=1779676175113000000 ack_rtt_ns=5992465 response_rtt_ns=0 ack_exchange_to_local_ns=4464348 response_exchange_to_local_ns=0 exchange_lifecycle_ns=0
                """,
            )

            result = orders.analyze_order_detail(log_path)
            latency_rows = orders.build_latency_detail_rows(result.rows)

        self.assertEqual(len(result.rows), 1)
        self.assertEqual(result.rows[0]["ack_exchange_request_ingress_ns"], "")
        self.assertEqual(result.rows[0]["ack_exchange_response_egress_ns"], "")
        self.assertEqual(result.rows[0]["ack_exchange_process_ns"], "")
        self.assertEqual(len(latency_rows), 1)
        self.assertEqual(latency_rows[0]["ack_exchange_process_ns"], "")

    def test_builds_closed_position_detail_from_entry_and_exit_orders(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            base = Path(temp_dir)
            log_path = base / "run.log"
            config_path = base / "strategy.toml"
            catalog_path = base / "catalog.csv"
            output_path = base / "positions.csv"
            write_config(config_path)
            write_catalog(catalog_path)
            write_file(
                log_path,
                """
                I2026-05-25 02:29:35.105566021 1:1 strategy.h:LogStrategyOrderSubmitted:1] lead_lag_order_submitted local_order_id=288230376151711749 group_id=1 trigger_exchange=kBinance trigger_symbol_id=4 trigger_exchange_ns=1779676175105510000 trigger_local_ns=1779676175105520000 on_book_ticker_entry_ns=1779676175105530000 signal_decision_ns=1779676175105540000 lead_exchange_ns=1779676175105510000 lag_exchange_ns=1779676175105500000 symbol=PROVE_USDT symbol_id=4 signal_role=kLead order_role=entry action=kOpenLong side=kBuy reduce_only=false position_id=1 position_event=kEntrySubmit position_direction=kLong entry_local_order_id=288230376151711749 quantity=36 quantity_text=36 raw_price=0.2711 order_price=0.2714 price_text=0.2714 slippage_ticks=3 price_tick=0.0001 target_open_notional=100 estimated_notional=97.704 active_groups=1 place_status=kOk
                I2026-05-25 02:29:35.105563683 1:1 order_session.h:LogGatePlaceOrderSent:466] gate_order_send_ok type=place local_order_id=288230376151711749 request_sequence=6 encoded_request_id=144115188075855878 contract=PROVE_USDT side=kBuy quantity=36 price=0.2714 tif=kImmediateOrCancel reduce_only=false inflight=5 request_send_local_ns=1779676175105554883
                I2026-05-25 02:29:35.111554171 1:1 order_session.h:LogGateOrderResponse:517] gate_order_response kind=kAck local_order_id=288230376151711749 exchange_order_id=0 request_sequence=6 channel=2 http_status=200 error_label_hash=0 error_label= error_message= local_receive_ns=1779676175111547348 exchange_ns=1779676175107083000 exchange_request_ingress_ns=1779676175107000000 exchange_response_egress_ns=1779676175107083000 exchange_process_ns=83000 exchange_to_local_ns=4464348
                I2026-05-25 02:29:35.117544030 1:1 strategy.h:LogStrategyOrderFeedback:272] lead_lag_order_feedback kind=kFilled local_order_id=288230376151711749 exchange_order_id=260082878984634644 cumulative_filled_quantity=36 left_quantity=0 cancelled_quantity=0 fill_price=0.2714 role=kTaker finish_reason=kUnknown reject_reason=kUnknown
                I2026-05-25 02:29:35.117545430 1:1 strategy.h:LogStrategyOrderFinished:335] lead_lag_order_finished local_order_id=288230376151711749 symbol_id=4 symbol=PROVE_USDT status=kFilled reduce_only=false position_id=1 position_direction=kLong order_role=entry entry_local_order_id=288230376151711749 order_finished_local_ns=1779676175117545430 quantity=36 cumulative_filled_quantity=36 average_fill_price=0.2714 last_fill_price=0.2714 exchange_order_id=260082878984634644 active_groups=1 request_send_local_ns=1779676175105554883 ack_local_receive_ns=1779676175111547348 response_local_receive_ns=0 ack_exchange_ns=1779676175107083000 response_exchange_ns=0 accepted_exchange_ns=0 finish_exchange_ns=1779676175113000000 ack_rtt_ns=5992465 response_rtt_ns=0 ack_exchange_to_local_ns=4464348 response_exchange_to_local_ns=0 exchange_lifecycle_ns=0
                I2026-05-25 02:29:35.105549026 1:1 strategy.h:LogStrategyOrderSubmitted:1] lead_lag_order_submitted local_order_id=288230376151711750 group_id=1 trigger_exchange=kBinance trigger_symbol_id=4 trigger_exchange_ns=1779676175125510000 trigger_local_ns=1779676175125520000 on_book_ticker_entry_ns=1779676175125530000 signal_decision_ns=1779676175125540000 lead_exchange_ns=1779676175125510000 lag_exchange_ns=1779676175125500000 symbol=PROVE_USDT symbol_id=4 signal_role=kLead order_role=exit action=kCloseLong side=kSell reduce_only=true position_id=1 position_event=kExitSubmit position_direction=kLong entry_local_order_id=288230376151711749 quantity=36 quantity_text=36 raw_price=0.2713 order_price=0.271 price_text=0.2710 slippage_ticks=3 price_tick=0.0001 target_open_notional=100 estimated_notional=97.56 active_groups=1 place_status=kOk
                I2026-05-25 02:29:35.125612335 1:1 order_session.h:LogGatePlaceOrderSent:466] gate_order_send_ok type=place local_order_id=288230376151711750 request_sequence=7 encoded_request_id=144115188075855879 contract=PROVE_USDT side=kSell quantity=36 price=0.2710 tif=kImmediateOrCancel reduce_only=true inflight=6 request_send_local_ns=1779676175125607942
                I2026-05-25 02:29:35.129547287 1:1 order_session.h:LogGateOrderResponse:517] gate_order_response kind=kAck local_order_id=288230376151711750 exchange_order_id=0 request_sequence=7 channel=2 http_status=200 error_label_hash=0 error_label= error_message= local_receive_ns=1779676175129546235 exchange_ns=1779676175127009000 exchange_to_local_ns=2537235
                I2026-05-25 02:29:35.135543485 1:1 strategy.h:LogStrategyOrderFinished:335] lead_lag_order_finished local_order_id=288230376151711750 symbol_id=4 symbol=PROVE_USDT status=kFilled reduce_only=true position_id=1 position_direction=kLong order_role=exit entry_local_order_id=288230376151711749 order_finished_local_ns=1779676175135543485 quantity=36 cumulative_filled_quantity=36 average_fill_price=0.271244 last_fill_price=0.271244 exchange_order_id=260082878984634672 active_groups=0 request_send_local_ns=1779676175125607942 ack_local_receive_ns=1779676175129546235 response_local_receive_ns=0 ack_exchange_ns=1779676175127009000 response_exchange_ns=0 accepted_exchange_ns=0 finish_exchange_ns=1779676175128000000 ack_rtt_ns=3938293 response_rtt_ns=0 ack_exchange_to_local_ns=2537235 response_exchange_to_local_ns=0 exchange_lifecycle_ns=0
                """,
            )

            result = orders.analyze_order_detail(
                log_path,
                config_path=config_path,
                instrument_catalog_path=catalog_path,
                run_id="run-1",
            )
            position_rows = orders.build_position_detail_rows(result.rows)
            orders.write_position_detail_csv(position_rows, output_path)

            with output_path.open(newline="", encoding="utf-8") as input_file:
                row = next(csv.DictReader(input_file))

        self.assertEqual(len(position_rows), 1)
        self.assertEqual(row["run_id"], "run-1")
        self.assertEqual(row["position_key"], "run-1:4:1:288230376151711750")
        self.assertEqual(row["symbol"], "PROVE_USDT")
        self.assertEqual(row["position_id"], "1")
        self.assertEqual(row["position_direction"], "kLong")
        self.assertEqual(row["status"], "closed")
        self.assertEqual(row["entry_local_order_id"], "288230376151711749")
        self.assertEqual(row["exit_local_order_id"], "288230376151711750")
        self.assertEqual(row["entry_lead_exchange_ns"], "1779676175105510000")
        self.assertEqual(row["entry_lag_exchange_ns"], "1779676175105500000")
        self.assertEqual(row["exit_lead_exchange_ns"], "1779676175125510000")
        self.assertEqual(row["exit_lag_exchange_ns"], "1779676175125500000")
        self.assertEqual(row["entry_ns"], "1779676175117545430")
        self.assertEqual(row["exit_ns"], "1779676175135543485")
        self.assertEqual(row["holding_ns"], "17998055")
        self.assertEqual(row["entry_price"], "0.2714")
        self.assertEqual(row["exit_price"], "0.271244")
        self.assertEqual(row["entry_volume"], "36")
        self.assertEqual(row["exit_volume"], "36")
        self.assertEqual(row["gross_pnl"], "-0.05616")
        self.assertEqual(row["entry_fee_quote_estimated"], "0.01563264")
        self.assertEqual(row["exit_fee_quote_estimated"], "0.0156236544")
        self.assertEqual(row["total_fee_quote_estimated"], "0.0312562944")
        self.assertEqual(row["net_pnl"], "-0.0874162944")
        self.assertEqual(row["warnings"], "")

    def test_builds_latency_detail_from_order_timing(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            base = Path(temp_dir)
            log_path = base / "run.log"
            output_path = base / "latency.csv"
            write_file(
                log_path,
                """
                I2026-05-25 02:29:35.105566021 1:1 strategy.h:LogStrategyOrderSubmitted:1] lead_lag_order_submitted local_order_id=288230376151711749 group_id=1 trigger_exchange=kBinance trigger_symbol_id=4 trigger_exchange_ns=1779676175105510000 trigger_local_ns=1779676175105520000 on_book_ticker_entry_ns=1779676175105530000 signal_decision_ns=1779676175105540000 lead_exchange_ns=1779676175105510000 lag_exchange_ns=1779676175105500000 symbol=PROVE_USDT symbol_id=4 signal_role=kLead order_role=entry action=kOpenLong side=kBuy reduce_only=false position_id=1 position_event=kEntrySubmit position_direction=kLong entry_local_order_id=288230376151711749 quantity=36 quantity_text=36 raw_price=0.2711 order_price=0.2714 price_text=0.2714 slippage_ticks=3 price_tick=0.0001 target_open_notional=100 estimated_notional=97.704 active_groups=1 place_status=kOk
                I2026-05-25 02:29:35.105563683 1:1 order_session.h:LogGatePlaceOrderSent:466] gate_order_send_ok type=place local_order_id=288230376151711749 request_sequence=6 encoded_request_id=144115188075855878 contract=PROVE_USDT side=kBuy quantity=36 price=0.2714 tif=kImmediateOrCancel reduce_only=false inflight=5 request_send_local_ns=1779676175105554883
                I2026-05-25 02:29:35.111554171 1:1 order_session.h:LogGateOrderResponse:517] gate_order_response kind=kAck local_order_id=288230376151711749 exchange_order_id=0 request_sequence=6 channel=2 http_status=200 error_label_hash=0 error_label= error_message= local_receive_ns=1779676175111547348 exchange_ns=1779676175107083000 exchange_request_ingress_ns=1779676175107000000 exchange_response_egress_ns=1779676175107083000 exchange_process_ns=83000 exchange_to_local_ns=4464348
                I2026-05-25 02:29:35.117545430 1:1 strategy.h:LogStrategyOrderFinished:335] lead_lag_order_finished local_order_id=288230376151711749 symbol_id=4 symbol=PROVE_USDT status=kFilled reduce_only=false position_id=1 position_direction=kLong order_role=entry entry_local_order_id=288230376151711749 order_finished_local_ns=1779676175117545430 quantity=36 cumulative_filled_quantity=36 average_fill_price=0.2714 last_fill_price=0.2714 exchange_order_id=260082878984634644 active_groups=1 request_send_local_ns=1779676175105554883 ack_local_receive_ns=1779676175111547348 response_local_receive_ns=0 ack_exchange_ns=1779676175107083000 response_exchange_ns=0 accepted_exchange_ns=0 finish_exchange_ns=1779676175113000000 ack_rtt_ns=5992465 response_rtt_ns=0 ack_exchange_to_local_ns=4464348 response_exchange_to_local_ns=0 exchange_lifecycle_ns=0
                """,
            )

            result = orders.analyze_order_detail(log_path, run_id="run-latency")
            latency_rows = orders.build_latency_detail_rows(result.rows)
            orders.write_latency_detail_csv(latency_rows, output_path)

            with output_path.open(newline="", encoding="utf-8") as input_file:
                row = next(csv.DictReader(input_file))

        self.assertEqual(len(latency_rows), 1)
        self.assertEqual(row["run_id"], "run-latency")
        self.assertEqual(row["latency_key"], "run-latency:288230376151711749")
        self.assertEqual(row["local_order_id"], "288230376151711749")
        self.assertEqual(row["position_id"], "1")
        self.assertEqual(row["order_role"], "entry")
        self.assertEqual(row["status"], "kFilled")
        self.assertEqual(row["request_sequence"], "6")
        self.assertEqual(row["encoded_request_id"], "144115188075855878")
        self.assertEqual(row["request_send_local_ns"], "1779676175105554883")
        self.assertEqual(row["ack_local_receive_ns"], "1779676175111547348")
        self.assertEqual(row["order_finished_local_ns"], "1779676175117545430")
        self.assertEqual(row["ack_exchange_ns"], "1779676175107083000")
        self.assertEqual(
            row["ack_exchange_request_ingress_ns"], "1779676175107000000"
        )
        self.assertEqual(
            row["ack_exchange_response_egress_ns"], "1779676175107083000"
        )
        self.assertEqual(row["ack_exchange_process_ns"], "83000")
        self.assertEqual(row["finish_exchange_ns"], "1779676175113000000")
        self.assertEqual(row["ack_rtt_ns"], "5992465")
        self.assertEqual(row["send_to_finish_local_ns"], "11990547")
        self.assertEqual(row["ack_to_finish_local_ns"], "5998082")
        self.assertEqual(row["ack_exchange_to_local_ns"], "4464348")
        self.assertEqual(row["exchange_lifecycle_ns"], "5917000")
        self.assertEqual(row["warnings"], "")

    def test_latency_detail_requires_ack_and_finish_exchange_for_lifecycle(self):
        latency_rows = orders.build_latency_detail_rows(
            [
                {
                    "run_id": "run-latency",
                    "local_order_id": "1",
                    "ack_exchange_ns": "0",
                    "finish_exchange_ns": "1779676175113000000",
                    "exchange_lifecycle_ns": "123456789",
                }
            ]
        )

        self.assertEqual(len(latency_rows), 1)
        self.assertEqual(latency_rows[0]["exchange_lifecycle_ns"], "")

    def test_latency_detail_includes_signal_to_order_timing_fields(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            base = Path(temp_dir)
            log_path = base / "run.log"
            write_file(
                log_path,
                """
                I2026-05-25 02:29:35.105566021 1:1 strategy.h:LogStrategyOrderSubmitted:1] lead_lag_order_submitted local_order_id=42 group_id=1 trigger_exchange=kBinance trigger_symbol_id=4 trigger_exchange_ns=900 trigger_local_ns=1000 on_book_ticker_entry_ns=1100 signal_decision_ns=1250 lead_exchange_ns=901 lag_exchange_ns=902 symbol=PROVE_USDT symbol_id=4 signal_role=kLead order_role=entry action=kOpenLong side=kBuy reduce_only=false position_id=1 position_event=kEntrySubmit position_direction=kLong entry_local_order_id=42 quantity=36 quantity_text=36 raw_price=0.2711 order_price=0.2714 price_text=0.2714 slippage_ticks=3 price_tick=0.0001 target_open_notional=100 estimated_notional=97.704 active_groups=1 place_status=kOk
                I2026-05-25 02:29:35.105563683 1:1 order_session.h:LogGatePlaceOrderSent:466] gate_order_send_ok type=place local_order_id=42 request_sequence=6 encoded_request_id=144115188075855878 contract=PROVE_USDT side=kBuy quantity=36 price=0.2714 tif=kImmediateOrCancel reduce_only=false inflight=5 request_send_local_ns=1500
                """,
            )

            result = orders.analyze_order_detail(log_path, run_id="run-latency")
            latency_rows = orders.build_latency_detail_rows(result.rows)

        self.assertEqual(len(latency_rows), 1)
        row = latency_rows[0]
        self.assertEqual(row["trigger_exchange_ns"], "900")
        self.assertEqual(row["trigger_local_ns"], "1000")
        self.assertEqual(row["on_book_ticker_entry_ns"], "1100")
        self.assertEqual(row["signal_decision_ns"], "1250")
        self.assertEqual(row["bbo_to_strategy_ns"], "100")
        self.assertEqual(row["strategy_to_signal_ns"], "150")
        self.assertEqual(row["signal_to_request_send_ns"], "250")
        self.assertEqual(row["trigger_to_request_send_ns"], "500")

    def test_latency_detail_blanks_cross_clock_data_session_timing(self):
        latency_rows = orders.build_latency_detail_rows(
            [
                {
                    "run_id": "run-latency",
                    "local_order_id": "42",
                    "trigger_local_ns": "7324723301524054",
                    "on_book_ticker_entry_ns": "1780125669161059481",
                    "signal_decision_ns": "1780125669161060485",
                    "request_send_local_ns": "1780125669161072654",
                }
            ]
        )

        self.assertEqual(len(latency_rows), 1)
        row = latency_rows[0]
        self.assertEqual(row["bbo_to_strategy_ns"], "")
        self.assertEqual(row["strategy_to_signal_ns"], "1004")
        self.assertEqual(row["signal_to_request_send_ns"], "12169")
        self.assertEqual(row["trigger_to_request_send_ns"], "")
        self.assertIn("cross_clock_bbo_to_strategy_ns", row["warnings"])
        self.assertIn("cross_clock_trigger_to_request_send_ns", row["warnings"])

    def test_latency_detail_includes_gate_ack_diagnostic_outlier_fields(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            base = Path(temp_dir)
            log_path = base / "run.log"
            write_file(
                log_path,
                """
                I2026-05-25 02:29:35.105563683 1:1 order_session.h:LogGatePlaceOrderSent:466] gate_order_send_ok type=place local_order_id=288230376151711749 request_sequence=6 encoded_request_id=144115188075855878 contract=PROVE_USDT side=kBuy quantity=36 price=0.2714 tif=kImmediateOrCancel reduce_only=false inflight=5 request_send_local_ns=1779676175105554883
                W2026-05-25 02:29:35.125000000 1:1 order_session.h:LogOrderLatencyDiagnostic:1] gate_order_ack_latency_diagnostic reason=kSendToDriveReadThreshold local_order_id=288230376151711749 request_sequence=6 request_send_local_ns=1779676175105554883 ack_local_receive_ns=0 ack_exchange_ns=0 ack_rtt_ns=0 send_to_first_after_hook_ns=1000 send_to_first_drive_read_ns=3499001 drive_read_duration_ns=0 max_observed_drive_read_duration_ns=0 inflight_at_send=7 order_session_id=9 diagnostic_cpu=4 tcp_info_available=true tcp_info_rtt_us=7000 tcp_info_rttvar_us=450 tcp_info_retrans=0 tcp_info_total_retrans=1 tcp_info_unacked=0 tcp_info_snd_cwnd=10
                W2026-05-25 02:29:35.130000000 1:1 order_session.h:LogOrderLatencyDiagnostic:1] gate_order_ack_latency_diagnostic reason=kAckRttThreshold local_order_id=288230376151711749 request_sequence=6 request_send_local_ns=1779676175105554883 ack_local_receive_ns=1779676175324554883 ack_exchange_ns=1779676175300000000 ack_rtt_ns=219000000 send_to_first_after_hook_ns=1000 send_to_first_drive_read_ns=3499001 drive_read_duration_ns=1300001 max_observed_drive_read_duration_ns=1300001 inflight_at_send=7 order_session_id=9 diagnostic_cpu=5 tcp_info_available=true tcp_info_rtt_us=9000 tcp_info_rttvar_us=500 tcp_info_retrans=0 tcp_info_total_retrans=2 tcp_info_unacked=0 tcp_info_snd_cwnd=10
                """,
            )

            result = orders.analyze_order_detail(log_path, run_id="run-latency")
            latency_rows = orders.build_latency_detail_rows(result.rows)

        self.assertEqual(len(latency_rows), 1)
        row = latency_rows[0]
        self.assertEqual(
            row["latency_diagnostic_reason"],
            "kSendToDriveReadThreshold;kAckRttThreshold",
        )
        self.assertEqual(row["latency_diagnostic_ack_rtt_ns"], "219000000")
        self.assertEqual(row["send_to_first_after_hook_ns"], "1000")
        self.assertEqual(row["send_to_first_drive_read_ns"], "3499001")
        self.assertEqual(row["drive_read_duration_ns"], "1300001")
        self.assertEqual(row["max_observed_drive_read_duration_ns"], "1300001")
        self.assertEqual(row["latency_diagnostic_inflight_at_send"], "7")
        self.assertEqual(row["order_session_id"], "9")
        self.assertEqual(row["diagnostic_cpu"], "4;5")
        self.assertEqual(row["tcp_info_available"], "true")
        self.assertEqual(row["tcp_info_rtt_us"], "9000")
        self.assertEqual(row["tcp_info_total_retrans"], "2")

    def test_latency_detail_includes_write_path_diagnostic_fields(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            base = Path(temp_dir)
            log_path = base / "run.log"
            write_file(
                log_path,
                """
                I2026-05-25 02:29:35.105563683 1:1 order_session.h:LogGatePlaceOrderSent:466] gate_order_send_ok type=place local_order_id=42 request_sequence=6 encoded_request_id=144115188075855878 contract=PROVE_USDT side=kBuy quantity=36 price=0.2714 tif=kImmediateOrCancel reduce_only=false inflight=5 request_send_local_ns=1000
                W2026-05-25 02:29:35.130000000 1:1 order_session.h:LogOrderLatencyDiagnostic:1] gate_order_ack_latency_diagnostic reason=kAckRttThreshold local_order_id=42 request_sequence=6 request_send_local_ns=1000 ack_local_receive_ns=25000000 ack_exchange_ns=5000 ack_rtt_ns=24999000 send_to_first_after_hook_ns=100 send_to_first_drive_read_ns=200 drive_read_duration_ns=300 max_observed_drive_read_duration_ns=400 inflight_at_send=1 order_session_id=9 diagnostic_cpu=5 max_runtime_loop_gap_ns=600 runtime_loop_iterations_before_ack=7 owner_thread_tid=2468 order_encode_done_ns=1100 ws_frame_encode_done_ns=1200 write_enqueue_ns=1300 drive_write_enter_ns=1350 write_some_enter_ns=1400 write_some_return_ns=1450 write_complete_ns=1500 write_some_bytes=64 write_complete_bytes=64 write_errno=0 write_eagain=false pending_write_count_after=0 socket_send_queue_available=true tcp_sendq_bytes=8 tcp_notsent_bytes=4 tcp_info_available=false
                """,
            )

            result = orders.analyze_order_detail(log_path, run_id="run-latency")
            latency_rows = orders.build_latency_detail_rows(result.rows)

        self.assertEqual(len(latency_rows), 1)
        row = latency_rows[0]
        self.assertEqual(row["owner_thread_tid"], "2468")
        self.assertEqual(row["max_runtime_loop_gap_ns"], "600")
        self.assertEqual(row["runtime_loop_iterations_before_ack"], "7")
        self.assertEqual(row["order_encode_done_ns"], "1100")
        self.assertEqual(row["write_some_bytes"], "64")
        self.assertEqual(row["write_complete_ns"], "1500")
        self.assertEqual(row["write_complete_bytes"], "64")
        self.assertEqual(row["write_errno"], "0")
        self.assertEqual(row["write_eagain"], "false")
        self.assertEqual(row["pending_write_count_after"], "0")
        self.assertEqual(row["socket_send_queue_available"], "true")
        self.assertEqual(row["tcp_sendq_bytes"], "8")
        self.assertEqual(row["tcp_notsent_bytes"], "4")

    def test_latency_detail_includes_non_ack_submit_response_timing(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            base = Path(temp_dir)
            log_path = base / "run.log"
            write_file(
                log_path,
                """
                I2026-05-25 02:29:35.105563683 1:1 order_session.h:LogGatePlaceOrderSent:466] gate_order_send_ok type=place local_order_id=288230376151711749 request_sequence=6 encoded_request_id=144115188075855878 contract=PROVE_USDT side=kBuy quantity=36 price=0.2714 tif=kImmediateOrCancel reduce_only=false inflight=5 request_send_local_ns=1779676175105554883
                I2026-05-25 02:29:35.111554171 1:1 order_session.h:LogGateOrderResponse:517] gate_order_response kind=kResult local_order_id=288230376151711749 exchange_order_id=260082878984634644 request_sequence=6 channel=2 http_status=200 error_label_hash=0 error_label= error_message= local_receive_ns=1779676175111547348 exchange_ns=1779676175107083000 exchange_to_local_ns=4464348
                """,
            )

            result = orders.analyze_order_detail(log_path, run_id="run-latency")
            latency_rows = orders.build_latency_detail_rows(result.rows)

        self.assertEqual(len(latency_rows), 1)
        row = latency_rows[0]
        self.assertEqual(row["exchange_order_id"], "260082878984634644")
        self.assertEqual(row["response_local_receive_ns"], "1779676175111547348")
        self.assertEqual(row["response_exchange_ns"], "1779676175107083000")
        self.assertEqual(row["send_to_response_local_ns"], "5992465")
        self.assertEqual(row["response_exchange_to_local_ns"], "4464348")
        self.assertIn("missing_ack_local_receive_ns", row["warnings"])

    def test_parses_feedback_session_publish_event_as_order_feedback(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            base = Path(temp_dir)
            log_path = base / "run.log"
            config_path = base / "strategy.toml"
            catalog_path = base / "catalog.csv"
            write_config(config_path)
            write_catalog(catalog_path)
            write_file(
                log_path,
                """
                I2026-05-26 04:13:58.450501908 1:1 order_session.h:LogGatePlaceOrderSent:477] gate_order_send_ok type=place local_order_id=288230376151711745 request_sequence=2 encoded_request_id=144115188075855874 contract=PROVE_USDT side=kBuy quantity=18 price=0.5404 tif=kImmediateOrCancel reduce_only=false inflight=1 request_send_local_ns=1779768838450489176
                I2026-05-26 04:13:58.454460064 1:1 order_session.h:LogGateOrderResponse:528] gate_order_response kind=kAck local_order_id=288230376151711745 exchange_order_id=0 request_sequence=2 channel=2 http_status=200 error_label_hash=0 error_label= error_message= local_receive_ns=1779768838454451971 exchange_ns=1779768838452525000 exchange_to_local_ns=1926971
                I2026-05-26 04:13:58.461376899 1:1 order_feedback_session.cpp:Publish:207] feedback_event publish_ok=true kind=kFilled local_order_id=288230376151711745 exchange_order_id=294985777053717137 exchange_update_ns=1779768838459000000 local_receive_ns=6967892601839617 cumulative_filled_quantity=18 left_quantity=0 cancelled_quantity=0 fill_price=0.535 role=kTaker finish_reason=kUnknown reject_reason=kUnknown continuity_scope=kLane continuity_reason=kUnknown continuity_sequence=0
                """,
            )

            result = orders.analyze_order_detail(
                log_path,
                config_path=config_path,
                instrument_catalog_path=catalog_path,
                run_id="run-feedback",
            )

        self.assertEqual(len(result.rows), 1)
        row = result.rows[0]
        self.assertEqual(row["status"], "kFilled")
        self.assertEqual(row["exchange_order_id"], "294985777053717137")
        self.assertEqual(row["cumulative_filled_quantity"], "18")
        self.assertEqual(row["last_fill_price"], "0.535")
        self.assertEqual(row["fill_role"], "kTaker")
        self.assertEqual(row["filled_notional"], "96.3")
        self.assertEqual(row["finish_exchange_ns"], "1779768838459000000")
        self.assertNotIn("order_finished_local_ns", row)

    def test_builds_closed_position_from_open_close_smoke_summary(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            base = Path(temp_dir)
            log_path = base / "run.log"
            config_path = base / "strategy.toml"
            catalog_path = base / "catalog.csv"
            write_config(config_path)
            write_catalog(catalog_path)
            write_file(
                log_path,
                """
                I2026-05-26 04:13:58.450501908 1:1 order_session.h:LogGatePlaceOrderSent:477] gate_order_send_ok type=place local_order_id=288230376151711745 request_sequence=2 encoded_request_id=144115188075855874 contract=PROVE_USDT side=kBuy quantity=18 price=0.5404 tif=kImmediateOrCancel reduce_only=false inflight=1 request_send_local_ns=1779768838450489176
                I2026-05-26 04:13:58.454460064 1:1 order_session.h:LogGateOrderResponse:528] gate_order_response kind=kAck local_order_id=288230376151711745 exchange_order_id=0 request_sequence=2 channel=2 http_status=200 error_label_hash=0 error_label= error_message= local_receive_ns=1779768838454451971 exchange_ns=1779768838452525000 exchange_to_local_ns=1926971
                I2026-05-26 04:13:58.461163733 1:1 order_session.h:LogGateOrderResponse:528] gate_order_response kind=kResult local_order_id=288230376151711745 exchange_order_id=294985777053717137 request_sequence=2 channel=2 http_status=200 error_label_hash=0 error_label= error_message= local_receive_ns=1779768838461159141 exchange_ns=1779768838459293000 exchange_to_local_ns=1866141
                I2026-05-26 04:13:58.461376899 1:1 order_feedback_session.cpp:Publish:207] feedback_event publish_ok=true kind=kFilled local_order_id=288230376151711745 exchange_order_id=294985777053717137 exchange_update_ns=1779768838459000000 local_receive_ns=6967892601839617 cumulative_filled_quantity=18 left_quantity=0 cancelled_quantity=0 fill_price=0.535 role=kTaker finish_reason=kUnknown reject_reason=kUnknown continuity_scope=kLane continuity_reason=kUnknown continuity_sequence=0
                I2026-05-26 04:13:58.461553499 1:1 order_session.h:LogGatePlaceOrderSent:477] gate_order_send_ok type=place local_order_id=288230376151711746 request_sequence=3 encoded_request_id=144115188075855875 contract=PROVE_USDT side=kSell quantity=18 price=0.5293 tif=kImmediateOrCancel reduce_only=true inflight=1 request_send_local_ns=1779768838461548378
                I2026-05-26 04:13:58.465314975 1:1 order_session.h:LogGateOrderResponse:528] gate_order_response kind=kAck local_order_id=288230376151711746 exchange_order_id=0 request_sequence=3 channel=2 http_status=200 error_label_hash=0 error_label= error_message= local_receive_ns=1779768838465313629 exchange_ns=1779768838463409000 exchange_to_local_ns=1904629
                I2026-05-26 04:13:58.466279351 1:1 order_session.h:LogGateOrderResponse:528] gate_order_response kind=kResult local_order_id=288230376151711746 exchange_order_id=294985777053717146 request_sequence=3 channel=2 http_status=200 error_label_hash=0 error_label= error_message= local_receive_ns=1779768838466276654 exchange_ns=1779768838464431000 exchange_to_local_ns=1845654
                I2026-05-26 04:13:58.466470289 1:1 order_feedback_session.cpp:Publish:207] feedback_event publish_ok=true kind=kFilled local_order_id=288230376151711746 exchange_order_id=294985777053717146 exchange_update_ns=1779768838464000000 local_receive_ns=6967892606935428 cumulative_filled_quantity=18 left_quantity=0 cancelled_quantity=0 fill_price=0.5348 role=kTaker finish_reason=kUnknown reject_reason=kUnknown continuity_scope=kLane continuity_reason=kUnknown continuity_sequence=0
                lead_lag_strategy_live_open_close_smoke_summary exit_code=0 runtime_exit_code=0 emergency_handoff=false completed=true state=done book_tickers=1041 order_responses=4 order_feedbacks=2 open_local_order_id=288230376151711745 close_local_order_id=288230376151711746 open_quantity=18 close_quantity=18 target_notional=100 estimated_open_notional=99.51 used_min_quantity=false error=-
                """,
            )

            result = orders.analyze_order_detail(
                log_path,
                config_path=config_path,
                instrument_catalog_path=catalog_path,
                run_id="run-smoke",
            )
            position_rows = orders.build_position_detail_rows(result.rows)
            latency_rows = orders.build_latency_detail_rows(result.rows)

        self.assertEqual(len(position_rows), 1)
        position = position_rows[0]
        self.assertEqual(position["position_key"], "run-smoke:4:288230376151711745:288230376151711746")
        self.assertEqual(position["status"], "closed")
        self.assertEqual(position["position_direction"], "kLong")
        self.assertEqual(position["entry_local_order_id"], "288230376151711745")
        self.assertEqual(position["exit_local_order_id"], "288230376151711746")
        self.assertEqual(position["entry_ns"], "")
        self.assertEqual(position["exit_ns"], "")
        self.assertEqual(position["holding_ns"], "")
        self.assertEqual(position["entry_price"], "0.535")
        self.assertEqual(position["exit_price"], "0.5348")
        self.assertEqual(position["gross_pnl"], "-0.036")
        self.assertEqual(position["warnings"], "")
        self.assertEqual(len(latency_rows), 2)
        self.assertEqual(
            [row["order_finished_local_ns"] for row in latency_rows], ["", ""]
        )
        self.assertEqual(
            [row["send_to_finish_local_ns"] for row in latency_rows], ["", ""]
        )

    def test_builds_short_closed_and_open_position_detail_rows(self):
        rows = orders.build_position_detail_rows(
            [
                {
                    "run_id": "run-2",
                    "local_order_id": "10",
                    "exchange_order_id": "110",
                    "symbol": "PROVE_USDT",
                    "symbol_id": "4",
                    "order_role": "entry",
                    "position_id": "2",
                    "position_direction": "kShort",
                    "side": "kSell",
                    "raw_price": "0.3",
                    "order_price": "0.2997",
                    "average_fill_price": "0.2998",
                    "cumulative_filled_quantity": "10",
                    "contract_multiplier": "10",
                    "fee_quote_estimated": "0.004",
                    "fee_source": "config_estimated",
                    "ack_rtt_ns": "1000",
                    "order_finished_local_ns": "100",
                },
                {
                    "run_id": "run-2",
                    "local_order_id": "11",
                    "exchange_order_id": "111",
                    "symbol": "PROVE_USDT",
                    "symbol_id": "4",
                    "order_role": "exit",
                    "position_id": "2",
                    "position_direction": "kShort",
                    "side": "kBuy",
                    "raw_price": "0.299",
                    "order_price": "0.2993",
                    "average_fill_price": "0.2992",
                    "cumulative_filled_quantity": "10",
                    "contract_multiplier": "10",
                    "fee_quote_estimated": "0.003",
                    "fee_source": "config_estimated",
                    "ack_rtt_ns": "2000",
                    "order_finished_local_ns": "140",
                },
                {
                    "run_id": "run-2",
                    "local_order_id": "12",
                    "exchange_order_id": "112",
                    "symbol": "PROVE_USDT",
                    "symbol_id": "4",
                    "order_role": "entry",
                    "position_id": "3",
                    "position_direction": "kLong",
                    "side": "kBuy",
                    "average_fill_price": "0.5",
                    "cumulative_filled_quantity": "4",
                    "contract_multiplier": "10",
                    "fee_quote_estimated": "0.002",
                    "fee_source": "config_estimated",
                    "order_finished_local_ns": "150",
                },
            ]
        )

        self.assertEqual(len(rows), 2)
        closed = rows[0]
        self.assertEqual(closed["position_key"], "run-2:4:2:11")
        self.assertEqual(closed["position_direction"], "kShort")
        self.assertEqual(closed["status"], "closed")
        self.assertEqual(closed["gross_pnl"], "0.06")
        self.assertEqual(closed["total_fee_quote_estimated"], "0.007")
        self.assertEqual(closed["net_pnl"], "0.053")
        self.assertEqual(closed["holding_ns"], "40")

        open_row = rows[1]
        self.assertEqual(open_row["position_key"], "run-2:4:3:open")
        self.assertEqual(open_row["status"], "open")
        self.assertEqual(open_row["entry_volume"], "4")
        self.assertEqual(open_row["entry_notional"], "20")
        self.assertEqual(open_row["net_pnl"], "")

    def test_builds_position_detail_with_multiple_entry_orders(self):
        rows = orders.build_position_detail_rows(
            [
                {
                    "run_id": "run-multi",
                    "local_order_id": "10",
                    "exchange_order_id": "110",
                    "symbol": "PROVE_USDT",
                    "symbol_id": "4",
                    "order_role": "entry",
                    "position_id": "9",
                    "position_direction": "kLong",
                    "side": "kBuy",
                    "average_fill_price": "100",
                    "cumulative_filled_quantity": "3",
                    "contract_multiplier": "1",
                    "fee_quote_estimated": "0.3",
                    "fee_source": "config_estimated",
                    "order_finished_local_ns": "100",
                },
                {
                    "run_id": "run-multi",
                    "local_order_id": "11",
                    "exchange_order_id": "111",
                    "symbol": "PROVE_USDT",
                    "symbol_id": "4",
                    "order_role": "entry",
                    "position_id": "9",
                    "position_direction": "kLong",
                    "side": "kBuy",
                    "average_fill_price": "101",
                    "cumulative_filled_quantity": "3",
                    "contract_multiplier": "1",
                    "fee_quote_estimated": "0.3",
                    "fee_source": "config_estimated",
                    "order_finished_local_ns": "110",
                },
                {
                    "run_id": "run-multi",
                    "local_order_id": "20",
                    "exchange_order_id": "120",
                    "symbol": "PROVE_USDT",
                    "symbol_id": "4",
                    "order_role": "exit",
                    "position_id": "9",
                    "position_direction": "kLong",
                    "side": "kSell",
                    "average_fill_price": "102",
                    "cumulative_filled_quantity": "4",
                    "contract_multiplier": "1",
                    "fee_quote_estimated": "0.4",
                    "fee_source": "config_estimated",
                    "order_finished_local_ns": "120",
                },
                {
                    "run_id": "run-multi",
                    "local_order_id": "21",
                    "exchange_order_id": "121",
                    "symbol": "PROVE_USDT",
                    "symbol_id": "4",
                    "order_role": "exit",
                    "position_id": "9",
                    "position_direction": "kLong",
                    "side": "kSell",
                    "average_fill_price": "103",
                    "cumulative_filled_quantity": "2",
                    "contract_multiplier": "1",
                    "fee_quote_estimated": "0.2",
                    "fee_source": "config_estimated",
                    "order_finished_local_ns": "130",
                },
            ]
        )

        self.assertEqual(len(rows), 3)
        self.assertEqual([row["status"] for row in rows], ["partial_closed", "partial_closed", "closed"])
        self.assertNotIn("over_closed", {row["status"] for row in rows})
        self.assertEqual(
            [row["position_key"] for row in rows],
            [
                "run-multi:4:9:10:20",
                "run-multi:4:9:11:20",
                "run-multi:4:9:11:21",
            ],
        )
        self.assertEqual([row["entry_volume"] for row in rows], ["3", "1", "2"])
        self.assertEqual([row["matched_volume"] for row in rows], ["3", "1", "2"])
        self.assertEqual([row["remaining_entry_volume"] for row in rows], ["3", "2", "0"])
        self.assertEqual([row["gross_pnl"] for row in rows], ["6", "1", "4"])
        self.assertEqual(
            [row["entry_fee_quote_estimated"] for row in rows],
            ["0.3", "0.1", "0.2"],
        )
        self.assertEqual(
            [row["exit_fee_quote_estimated"] for row in rows],
            ["0.3", "0.1", "0.2"],
        )
        self.assertEqual([row["net_pnl"] for row in rows], ["5.4", "0.8", "3.6"])
        self.assertTrue(
            all(row["warnings"] == "multiple_entry_orders" for row in rows)
        )


if __name__ == "__main__":
    unittest.main()
