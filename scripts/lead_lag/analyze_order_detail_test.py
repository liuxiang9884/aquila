#!/home/liuxiang/dev/pyenv/lx/bin/python

import csv
import tempfile
import textwrap
import unittest
from pathlib import Path

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
        open_slippage = 3
        close_slippage = 3
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


class AnalyzeOrderDetailTest(unittest.TestCase):
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
                I2026-05-25 02:29:35.105545332 1:1 strategy.h:LogStrategySignalTriggered:155] lead_lag_signal_triggered trigger_ticker_id=10624277384126 trigger_exchange=kBinance trigger_symbol_id=4 symbol=PROVE_USDT symbol_id=4 role=kLead action=kOpenLong side=kBuy reduce_only=false position_id=0 raw_price=0.2711
                I2026-05-25 02:29:35.105549026 1:1 strategy.h:LogStrategyOrderIntent:189] lead_lag_order_intent trigger_ticker_id=10624277384126 symbol=PROVE_USDT symbol_id=4 action=kOpenLong side=kBuy reduce_only=false position_id=0 quantity=36 price=0.2714 raw_price=0.2711 order_price=0.2714 slippage_ticks=3 price_tick=0.0001 target_open_notional=100 estimated_notional=97.704 active_groups=0
                I2026-05-25 02:29:35.105563683 1:1 order_session.h:LogGatePlaceOrderSent:466] gate_order_send_ok type=place local_order_id=288230376151711749 request_sequence=6 encoded_request_id=144115188075855878 contract=PROVE_USDT side=kBuy quantity=36 price=0.2714 tif=kImmediateOrCancel reduce_only=false inflight=5 request_send_local_ns=1779676175105554883
                I2026-05-25 02:29:35.105566021 1:1 strategy.h:LogStrategyOrderSubmitted:1] lead_lag_order_submitted local_order_id=288230376151711749 trigger_ticker_id=10624277384126 trigger_exchange=kBinance trigger_symbol_id=4 symbol=PROVE_USDT symbol_id=4 signal_role=kLead order_role=entry action=kOpenLong side=kBuy reduce_only=false position_id=1 position_event=kEntrySubmit position_direction=kLong entry_local_order_id=288230376151711749 quantity=36 quantity_text=36 raw_price=0.2711 order_price=0.2714 price_text=0.2714 slippage_ticks=3 price_tick=0.0001 target_open_notional=100 estimated_notional=97.704 active_groups=1 place_status=kOk
                I2026-05-25 02:29:35.111554171 1:1 order_session.h:LogGateOrderResponse:517] gate_order_response kind=kAck local_order_id=288230376151711749 exchange_order_id=0 request_sequence=6 channel=2 http_status=200 error_label_hash=0 error_label= error_message= local_receive_ns=1779676175111547348 exchange_ns=1779676175107083000 exchange_to_local_ns=4464348
                I2026-05-25 02:29:35.117544030 1:1 strategy.h:LogStrategyOrderFeedback:272] lead_lag_order_feedback kind=kFilled local_order_id=288230376151711749 exchange_order_id=260082878984634644 cumulative_filled_quantity=36 left_quantity=0 cancelled_quantity=0 fill_price=0.2714 role=kTaker finish_reason=kUnknown reject_reason=kUnknown
                I2026-05-25 02:29:35.117545430 1:1 strategy.h:LogStrategyOrderFinished:335] lead_lag_order_finished local_order_id=288230376151711749 symbol_id=4 symbol=PROVE_USDT status=kFilled reduce_only=false position_id=1 position_direction=kLong order_role=entry entry_local_order_id=288230376151711749 order_finished_local_ns=1779676175117545430 quantity=36 cumulative_filled_quantity=36 average_fill_price=0.2714 last_fill_price=0.2714 exchange_order_id=260082878984634644 active_groups=1 request_send_local_ns=1779676175105554883 ack_local_receive_ns=1779676175111547348 response_local_receive_ns=0 ack_exchange_ns=1779676175107083000 response_exchange_ns=0 accepted_exchange_ns=0 finish_exchange_ns=1779676175113000000 ack_rtt_ns=5992465 response_rtt_ns=0 ack_exchange_to_local_ns=4464348 response_exchange_to_local_ns=0 exchange_lifecycle_ns=0
                """,
            )

            result = orders.analyze_order_detail(
                log_path,
                config_path=config_path,
                instrument_catalog_path=catalog_path,
            )
            orders.write_order_detail_csv(result.rows, output_path)

            with output_path.open(newline="", encoding="utf-8") as input_file:
                row = next(csv.DictReader(input_file))

        self.assertEqual(row["local_order_id"], "288230376151711749")
        self.assertEqual(row["text_order_id"], "t-288230376151711749")
        self.assertEqual(row["exchange_order_id"], "260082878984634644")
        self.assertEqual(row["signal_role"], "kLead")
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
        self.assertEqual(row["order_finished_local_ns"], "1779676175117545430")
        self.assertEqual(row["source_schema"], "submitted_v1")
        self.assertEqual(row["warnings"], "")

    def test_prefers_submitted_log_for_final_group_and_quantity_text(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            base = Path(temp_dir)
            log_path = base / "run.log"
            write_file(
                log_path,
                """
                I2026-05-25 02:29:35.105549026 1:1 strategy.h:LogStrategyOrderSubmitted:1] lead_lag_order_submitted local_order_id=288230376151711750 trigger_ticker_id=10624277385364 trigger_exchange=kBinance trigger_symbol_id=4 symbol=PROVE_USDT symbol_id=4 signal_role=kLead order_role=exit action=kCloseLong side=kSell reduce_only=true position_id=1 position_event=kExitSubmit position_direction=kLong entry_local_order_id=288230376151711749 quantity=36 quantity_text=36 raw_price=0.2713 order_price=0.271 price_text=0.2710 slippage_ticks=3 price_tick=0.0001 target_open_notional=100 estimated_notional=97.56 active_groups=1 place_status=kOk
                I2026-05-25 02:29:35.125612335 1:1 order_session.h:LogGatePlaceOrderSent:466] gate_order_send_ok type=place local_order_id=288230376151711750 request_sequence=7 encoded_request_id=144115188075855879 contract=PROVE_USDT side=kSell quantity=36 price=0.2710 tif=kImmediateOrCancel reduce_only=true inflight=6 request_send_local_ns=1779676175125607942
                I2026-05-25 02:29:35.129547287 1:1 order_session.h:LogGateOrderResponse:517] gate_order_response kind=kAck local_order_id=288230376151711750 exchange_order_id=0 request_sequence=7 channel=2 http_status=200 error_label_hash=0 error_label= error_message= local_receive_ns=1779676175129546235 exchange_ns=1779676175127009000 exchange_to_local_ns=2537235
                I2026-05-25 02:29:35.135543485 1:1 strategy.h:LogStrategyOrderFinished:335] lead_lag_order_finished local_order_id=288230376151711750 symbol_id=4 symbol=PROVE_USDT status=kFilled reduce_only=true position_id=1 position_direction=kLong order_role=exit entry_local_order_id=288230376151711749 order_finished_local_ns=1779676175135543485 quantity=36 cumulative_filled_quantity=36 average_fill_price=0.271244 last_fill_price=0.271244 exchange_order_id=260082878984634672 active_groups=0 request_send_local_ns=1779676175125607942 ack_local_receive_ns=1779676175129546235 response_local_receive_ns=0 ack_exchange_ns=1779676175127009000 response_exchange_ns=0 accepted_exchange_ns=0 finish_exchange_ns=1779676175128000000 ack_rtt_ns=3938293 response_rtt_ns=0 ack_exchange_to_local_ns=2537235 response_exchange_to_local_ns=0 exchange_lifecycle_ns=0
                """,
            )

            result = orders.analyze_order_detail(log_path)

        self.assertEqual(len(result.rows), 1)
        row = result.rows[0]
        self.assertEqual(row["source_schema"], "submitted_v1")
        self.assertEqual(row["position_id"], "1")
        self.assertEqual(row["position_event"], "kExitSubmit")
        self.assertEqual(row["position_direction"], "kLong")
        self.assertEqual(row["entry_local_order_id"], "288230376151711749")
        self.assertEqual(row["order_role"], "exit")
        self.assertEqual(row["quantity_text"], "36")
        self.assertEqual(row["price_text"], "0.2710")
        self.assertEqual(row["order_finished_local_ns"], "1779676175135543485")
        self.assertEqual(row["warnings"], "")

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
                I2026-05-25 02:29:35.105566021 1:1 strategy.h:LogStrategyOrderSubmitted:1] lead_lag_order_submitted local_order_id=288230376151711749 trigger_ticker_id=10624277384126 trigger_exchange=kBinance trigger_symbol_id=4 symbol=PROVE_USDT symbol_id=4 signal_role=kLead order_role=entry action=kOpenLong side=kBuy reduce_only=false position_id=1 position_event=kEntrySubmit position_direction=kLong entry_local_order_id=288230376151711749 quantity=36 quantity_text=36 raw_price=0.2711 order_price=0.2714 price_text=0.2714 slippage_ticks=3 price_tick=0.0001 target_open_notional=100 estimated_notional=97.704 active_groups=1 place_status=kOk
                I2026-05-25 02:29:35.105563683 1:1 order_session.h:LogGatePlaceOrderSent:466] gate_order_send_ok type=place local_order_id=288230376151711749 request_sequence=6 encoded_request_id=144115188075855878 contract=PROVE_USDT side=kBuy quantity=36 price=0.2714 tif=kImmediateOrCancel reduce_only=false inflight=5 request_send_local_ns=1779676175105554883
                I2026-05-25 02:29:35.111554171 1:1 order_session.h:LogGateOrderResponse:517] gate_order_response kind=kAck local_order_id=288230376151711749 exchange_order_id=0 request_sequence=6 channel=2 http_status=200 error_label_hash=0 error_label= error_message= local_receive_ns=1779676175111547348 exchange_ns=1779676175107083000 exchange_to_local_ns=4464348
                I2026-05-25 02:29:35.117544030 1:1 strategy.h:LogStrategyOrderFeedback:272] lead_lag_order_feedback kind=kFilled local_order_id=288230376151711749 exchange_order_id=260082878984634644 cumulative_filled_quantity=36 left_quantity=0 cancelled_quantity=0 fill_price=0.2714 role=kTaker finish_reason=kUnknown reject_reason=kUnknown
                I2026-05-25 02:29:35.117545430 1:1 strategy.h:LogStrategyOrderFinished:335] lead_lag_order_finished local_order_id=288230376151711749 symbol_id=4 symbol=PROVE_USDT status=kFilled reduce_only=false position_id=1 position_direction=kLong order_role=entry entry_local_order_id=288230376151711749 order_finished_local_ns=1779676175117545430 quantity=36 cumulative_filled_quantity=36 average_fill_price=0.2714 last_fill_price=0.2714 exchange_order_id=260082878984634644 active_groups=1 request_send_local_ns=1779676175105554883 ack_local_receive_ns=1779676175111547348 response_local_receive_ns=0 ack_exchange_ns=1779676175107083000 response_exchange_ns=0 accepted_exchange_ns=0 finish_exchange_ns=1779676175113000000 ack_rtt_ns=5992465 response_rtt_ns=0 ack_exchange_to_local_ns=4464348 response_exchange_to_local_ns=0 exchange_lifecycle_ns=0
                I2026-05-25 02:29:35.105549026 1:1 strategy.h:LogStrategyOrderSubmitted:1] lead_lag_order_submitted local_order_id=288230376151711750 trigger_ticker_id=10624277385364 trigger_exchange=kBinance trigger_symbol_id=4 symbol=PROVE_USDT symbol_id=4 signal_role=kLead order_role=exit action=kCloseLong side=kSell reduce_only=true position_id=1 position_event=kExitSubmit position_direction=kLong entry_local_order_id=288230376151711749 quantity=36 quantity_text=36 raw_price=0.2713 order_price=0.271 price_text=0.2710 slippage_ticks=3 price_tick=0.0001 target_open_notional=100 estimated_notional=97.56 active_groups=1 place_status=kOk
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
                I2026-05-25 02:29:35.105566021 1:1 strategy.h:LogStrategyOrderSubmitted:1] lead_lag_order_submitted local_order_id=288230376151711749 trigger_ticker_id=10624277384126 trigger_exchange=kBinance trigger_symbol_id=4 symbol=PROVE_USDT symbol_id=4 signal_role=kLead order_role=entry action=kOpenLong side=kBuy reduce_only=false position_id=1 position_event=kEntrySubmit position_direction=kLong entry_local_order_id=288230376151711749 quantity=36 quantity_text=36 raw_price=0.2711 order_price=0.2714 price_text=0.2714 slippage_ticks=3 price_tick=0.0001 target_open_notional=100 estimated_notional=97.704 active_groups=1 place_status=kOk
                I2026-05-25 02:29:35.105563683 1:1 order_session.h:LogGatePlaceOrderSent:466] gate_order_send_ok type=place local_order_id=288230376151711749 request_sequence=6 encoded_request_id=144115188075855878 contract=PROVE_USDT side=kBuy quantity=36 price=0.2714 tif=kImmediateOrCancel reduce_only=false inflight=5 request_send_local_ns=1779676175105554883
                I2026-05-25 02:29:35.111554171 1:1 order_session.h:LogGateOrderResponse:517] gate_order_response kind=kAck local_order_id=288230376151711749 exchange_order_id=0 request_sequence=6 channel=2 http_status=200 error_label_hash=0 error_label= error_message= local_receive_ns=1779676175111547348 exchange_ns=1779676175107083000 exchange_to_local_ns=4464348
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
        self.assertEqual(row["finish_exchange_ns"], "1779676175113000000")
        self.assertEqual(row["ack_rtt_ns"], "5992465")
        self.assertEqual(row["send_to_finish_local_ns"], "11990547")
        self.assertEqual(row["ack_to_finish_local_ns"], "5998082")
        self.assertEqual(row["ack_exchange_to_local_ns"], "4464348")
        self.assertEqual(row["exchange_lifecycle_ns"], "0")
        self.assertEqual(row["warnings"], "")

    def test_latency_detail_includes_gate_ack_diagnostic_outlier_fields(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            base = Path(temp_dir)
            log_path = base / "run.log"
            write_file(
                log_path,
                """
                I2026-05-25 02:29:35.105563683 1:1 order_session.h:LogGatePlaceOrderSent:466] gate_order_send_ok type=place local_order_id=288230376151711749 request_sequence=6 encoded_request_id=144115188075855878 contract=PROVE_USDT side=kBuy quantity=36 price=0.2714 tif=kImmediateOrCancel reduce_only=false inflight=5 request_send_local_ns=1779676175105554883
                W2026-05-25 02:29:35.125000000 1:1 order_session.h:LogOrderLatencyDiagnostic:1] gate_order_ack_latency_diagnostic reason=kSendToDriveReadThreshold local_order_id=288230376151711749 request_sequence=6 request_send_local_ns=1779676175105554883 ack_local_receive_ns=0 ack_exchange_ns=0 ack_rtt_ns=0 send_to_first_after_hook_ns=1000 send_to_first_drive_read_ns=3499001 drive_read_duration_ns=0 max_observed_drive_read_duration_ns=0 inflight_at_send=7
                W2026-05-25 02:29:35.130000000 1:1 order_session.h:LogOrderLatencyDiagnostic:1] gate_order_ack_latency_diagnostic reason=kAckRttThreshold local_order_id=288230376151711749 request_sequence=6 request_send_local_ns=1779676175105554883 ack_local_receive_ns=1779676175324554883 ack_exchange_ns=1779676175300000000 ack_rtt_ns=219000000 send_to_first_after_hook_ns=1000 send_to_first_drive_read_ns=3499001 drive_read_duration_ns=1300001 max_observed_drive_read_duration_ns=1300001 inflight_at_send=7
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


if __name__ == "__main__":
    unittest.main()
