# LeadLag Live Report CSV 字段说明

本文档说明 LeadLag live report 目录中四个分析 CSV 的字段语义：

- `signal.csv`
- `order_detail.csv`
- `position.csv`
- `latency.csv`

当前字段以 `reports/20260525_091251_12pair_live/` 生成的 CSV 表头和 `scripts/lead_lag/analyze_order_detail.py` 为准。真实订单模式不会直接写 per-signal CSV，`signal.csv` 是从 live log 中的 `lead_lag_signal_triggered` 与订单明细关联后生成的分析产物。

## 通用约定

- 空字段表示对应日志、配置或 catalog 中没有可用值，或该行语义下不适用。
- `*_ns` 字段单位都是纳秒。
- `request_send_local_ns`、`ack_local_receive_ns`、`response_local_receive_ns`、`order_finished_local_ns` 是本机时钟时间戳，可以互相相减。
- `ack_exchange_ns`、`response_exchange_ns`、`accepted_exchange_ns`、`finish_exchange_ns` 是交易所侧时间戳。交易所时钟和本地时钟不同步，不能直接把 exchange 到 local 的差值当作真实单程网络延迟。
- 价格字段使用合约价格单位；notional、fee、PnL 字段使用 quote currency，当前 USDT futures report 中可理解为 USDT。
- `trigger_exchange` 表示触发信号的行情来源交易所，不表示实际下单交易所。当前 CSV 还没有显式 `order_exchange` 字段；本轮 Gate live run 的实际下单交易所由 `gate_*` 日志隐含为 Gate。

## signal.csv

`signal.csv` 一行表示一个触发信号，并关联该信号最终提交出的订单和部分执行结果。

| 字段 | 含义 | 来源或计算 |
|---|---|---|
| `run_id` | 本次运行或 report 的标识。 | 分析时传入或由 run 目录名推导。 |
| `signal_index` | signal 在该 CSV 中的顺序号。 | 生成 `signal.csv` 时按日志顺序编号。 |
| `log_time` | signal 日志行的 wall-clock 时间文本。 | `lead_lag_signal_triggered` 日志前缀。 |
| `trigger_ticker_id` | 触发 signal 的行情 ticker id。 | `lead_lag_signal_triggered.trigger_ticker_id`。 |
| `trigger_exchange` | 触发行情来源交易所，例如 `kBinance`。 | `lead_lag_signal_triggered.trigger_exchange`。 |
| `trigger_symbol_id` | 触发行情的 symbol id。 | `lead_lag_signal_triggered.trigger_symbol_id`。 |
| `trigger_exchange_ns` | 触发 BBO 的交易所时间戳。 | `lead_lag_signal_triggered.trigger_exchange_ns`。 |
| `trigger_local_ns` | 触发 BBO 在 data session ingress 处记录的本机时间戳。 | `lead_lag_signal_triggered.trigger_local_ns`。 |
| `on_book_ticker_entry_ns` | 策略进入 `Strategy::OnBookTicker()` 的本机时间戳。 | `lead_lag_signal_triggered.on_book_ticker_entry_ns`。 |
| `signal_decision_ns` | 策略确认 signal triggered 后的本机时间戳。 | `lead_lag_signal_triggered.signal_decision_ns`。 |
| `symbol` | 策略交易 symbol，例如 `PROVE_USDT`。 | signal log，并与 order detail 对齐。 |
| `symbol_id` | 策略交易 symbol id。 | signal log，并与 order detail 对齐。 |
| `signal_role` | signal 侧 pair role，例如 `kLead` 或 `kLag`。 | `lead_lag_signal_triggered.role` 或订单提交日志中的 `signal_role`。 |
| `action` | 策略动作，例如 `kOpenLong`、`kCloseShort`。 | signal log 或订单提交日志。 |
| `side` | 下单方向，例如 `kBuy`、`kSell`。 | signal log 或订单提交日志。 |
| `reduce_only` | 是否为 reduce-only 订单。 | signal log 或订单提交日志。 |
| `signal_position_id` | signal log 中记录的 position id。 | `lead_lag_signal_triggered.position_id`。新开仓信号中可能是 `0`。 |
| `raw_price` | signal 触发时用于决策的原始价格。 | `lead_lag_signal_triggered.raw_price`。 |
| `local_order_id` | 关联到的本地订单 id。 | 通过 `trigger_ticker_id` 和订单提交日志关联。 |
| `request_sequence` | Gate order session 下发请求序号。 | 关联的 `order_detail.csv.request_sequence`。 |
| `request_send_local_ns` | Gate order session 发送请求成功后的本机时间戳。 | 关联的 `order_detail.csv.request_send_local_ns`。 |
| `bbo_to_strategy_ns` | BBO data session 本机时间到策略入口的耗时。 | `on_book_ticker_entry_ns - trigger_local_ns`。只有两个输入看起来处于同一时钟域且差值在本地 pipeline 合理窗口内时才输出；跨时钟域时留空，`latency.csv.warnings` 写 `cross_clock_bbo_to_strategy_ns`。 |
| `strategy_to_signal_ns` | 策略入口到 signal decision 的耗时。 | `signal_decision_ns - on_book_ticker_entry_ns`。 |
| `signal_to_request_send_ns` | signal decision 到订单发送成功的本机耗时。 | `request_send_local_ns - signal_decision_ns`。 |
| `trigger_to_request_send_ns` | BBO data session 本机时间到订单发送成功的本机耗时。 | `request_send_local_ns - trigger_local_ns`。只有两个输入看起来处于同一时钟域且差值在本地 pipeline 合理窗口内时才输出；跨时钟域时留空，`latency.csv.warnings` 写 `cross_clock_trigger_to_request_send_ns`。 |
| `exchange_order_id` | 交易所返回的 order id。 | 关联的 `order_detail.csv.exchange_order_id`。 |
| `order_role` | 订单角色，`entry` 或 `exit`。 | 关联的 `order_detail.csv.order_role`。 |
| `order_position_id` | 订单提交后最终使用的 position id。 | 关联的 `order_detail.csv.position_id`。 |
| `position_event` | position 状态事件，例如 `kEntrySubmit`、`kExitSubmit`。 | 关联的 `order_detail.csv.position_event`。 |
| `position_direction` | position 方向，`kLong` 或 `kShort`。 | 关联的 `order_detail.csv.position_direction`。 |
| `order_price` | 实际提交到交易所的 limit price。 | 关联的 `order_detail.csv.order_price`。 |
| `price_tick` | 该合约 price tick。 | 关联的 `order_detail.csv.price_tick`。 |
| `slippage_ticks` | 策略配置的下单滑点 tick 数。 | 关联的 `order_detail.csv.slippage_ticks`。 |
| `quantity` | 下单数量。 | 关联的 `order_detail.csv.quantity`。 |
| `status` | 订单终态，例如 `kFilled`、`kCancelled`、`kPartiallyCancelled`。 | 关联的 `order_detail.csv.status`。 |
| `cumulative_filled_quantity` | 累计成交数量。 | 关联的 `order_detail.csv.cumulative_filled_quantity`。 |
| `average_fill_price` | 平均成交价。 | 关联的 `order_detail.csv.average_fill_price`。 |
| `exec_slippage_ticks` | 实际成交价相对 raw price 的执行滑点，单位 tick。 | 关联的 `order_detail.csv.exec_slippage_ticks`。 |
| `ack_rtt_ns` | 本地下单发送到收到 Ack 的 RTT。 | 关联的 `order_detail.csv.ack_rtt_ns`。 |
| `order_finished_local_ns` | 本地收到订单终态并完成策略处理的时间戳。 | 关联的 `order_detail.csv.order_finished_local_ns`。 |
| `warnings` | 生成分析时发现的缺失或异常标记。 | signal join 或 order detail 中的 warning 汇总。 |

## order_detail.csv

`order_detail.csv` 一行表示一个本地订单，合并订单提交、Gate send、Ack、feedback 和策略终态日志。

| 字段 | 含义 | 来源或计算 |
|---|---|---|
| `run_id` | 本次运行或 report 的标识。 | 分析时传入或由 log 父目录推导。 |
| `local_order_id` | 本地订单 id。 | `lead_lag_order_submitted`、`gate_order_send_ok` 或后续回报日志。 |
| `text_order_id` | Gate text order id。 | 当前分析脚本按 `t-<local_order_id>` 推导。 |
| `request_sequence` | Gate order session 内部请求序号。 | `gate_order_send_ok.request_sequence`。 |
| `encoded_request_id` | WebSocket payload 中编码后的请求 id。 | `gate_order_send_ok.encoded_request_id`。 |
| `exchange_order_id` | 交易所返回的 order id。 | Gate Ack、feedback 或 `lead_lag_order_finished`。 |
| `symbol` | 交易 symbol。 | 策略提交日志或 Gate send log。 |
| `symbol_id` | 策略内部 symbol id。 | 策略提交或终态日志。 |
| `trigger_ticker_id` | 触发下单的行情 ticker id。 | `lead_lag_order_submitted.trigger_ticker_id`。 |
| `trigger_exchange` | 触发行情来源交易所。 | `lead_lag_order_submitted.trigger_exchange`。 |
| `trigger_symbol_id` | 触发行情的 symbol id。 | `lead_lag_order_submitted.trigger_symbol_id`。 |
| `trigger_exchange_ns` | 触发 BBO 的交易所时间戳。 | `lead_lag_order_submitted.trigger_exchange_ns`。 |
| `trigger_local_ns` | 触发 BBO 在 data session ingress 处记录的本机时间戳。 | `lead_lag_order_submitted.trigger_local_ns`。 |
| `on_book_ticker_entry_ns` | 策略进入 `Strategy::OnBookTicker()` 的本机时间戳。 | `lead_lag_order_submitted.on_book_ticker_entry_ns`。 |
| `signal_decision_ns` | 策略确认 signal triggered 后的本机时间戳。 | `lead_lag_order_submitted.signal_decision_ns`。 |
| `signal_role` | signal 侧 pair role。 | `lead_lag_order_submitted.signal_role`。 |
| `order_role` | 订单角色，`entry` 或 `exit`。 | `lead_lag_order_submitted.order_role`，缺失时由 action / reduce-only 推导。 |
| `position_id` | 策略 position id。 | `lead_lag_order_submitted.position_id` 或终态日志。 |
| `position_event` | position 状态事件。 | `lead_lag_order_submitted.position_event`。 |
| `position_direction` | position 方向，`kLong` 或 `kShort`。 | `lead_lag_order_submitted.position_direction` 或终态日志。 |
| `entry_local_order_id` | 所属 position 的开仓订单 id。 | `lead_lag_order_submitted.entry_local_order_id` 或终态日志。 |
| `action` | 策略动作。 | `lead_lag_order_submitted.action`。 |
| `side` | 下单方向，`kBuy` 或 `kSell`。 | `lead_lag_order_submitted.side` 或 Gate send log。 |
| `reduce_only` | 是否 reduce-only。 | 策略提交日志或 Gate send log。 |
| `time_in_force` | 订单 TIF，当前 live IOC 路径通常为 `ioc`。 | `gate_order_send_ok.tif`。 |
| `status` | 订单终态。 | `lead_lag_order_finished.status` 或 feedback kind。 |
| `finish_reason` | 订单结束原因。 | `lead_lag_order_feedback.finish_reason`。 |
| `reject_reason` | 拒单原因。 | `lead_lag_order_feedback.reject_reason`。 |
| `raw_price` | signal 触发时用于计算下单价格的原始价格。 | `lead_lag_order_submitted.raw_price`。 |
| `order_price` | 实际提交到交易所的 limit price。 | `lead_lag_order_submitted.order_price` 或 Gate send price。 |
| `price_text` | 实际编码下发的价格文本。 | `lead_lag_order_submitted.price_text` 或 Gate send price。 |
| `price_tick` | 合约 price tick。 | 订单提交日志或 instrument catalog。 |
| `slippage_ticks` | 配置层希望加到 raw price 上的滑点 tick 数。 | `lead_lag_order_submitted.slippage_ticks`。 |
| `order_offset_ticks` | `order_price` 相对 `raw_price` 的实际偏移 tick。买单为 `(order_price - raw_price) / price_tick`，卖单为 `(raw_price - order_price) / price_tick`。 | 分析脚本计算。 |
| `quantity` | 数值化下单数量。 | 策略提交日志或 Gate send log。 |
| `quantity_text` | 实际编码下发的数量文本，保留 decimal-size 表示。 | `lead_lag_order_submitted.quantity_text` 或 Gate send quantity。 |
| `cumulative_filled_quantity` | 累计成交数量。 | feedback 或 `lead_lag_order_finished`。 |
| `left_quantity` | 剩余未成交数量。 | feedback。 |
| `cancelled_quantity` | 已取消数量。 | feedback。 |
| `fill_rate` | 成交比例，`cumulative_filled_quantity / quantity`。 | 分析脚本计算。 |
| `average_fill_price` | 平均成交价。 | `lead_lag_order_finished.average_fill_price`。 |
| `last_fill_price` | 最近一次成交价。 | feedback `fill_price` 或终态日志。 |
| `contract_multiplier` | 合约乘数。 | instrument catalog 的 Gate 合约 metadata。 |
| `filled_notional` | 成交 notional。 | `fill_price * cumulative_filled_quantity * contract_multiplier`。 |
| `fill_role` | 回报中的成交角色字段。 | `lead_lag_order_feedback.role`。 |
| `exec_slippage_price` | 实际成交价相对 raw price 的价格滑点。买单为 `fill_price - raw_price`，卖单为 `raw_price - fill_price`。 | 分析脚本计算。 |
| `exec_slippage_ticks` | 实际成交滑点 tick 数。 | `exec_slippage_price / price_tick`。 |
| `exec_slippage_bps` | 实际成交滑点 bps。 | `exec_slippage_price / raw_price * 10000`。 |
| `exec_slippage_quote` | 实际成交滑点对应 quote 损耗。 | `exec_slippage_price * cumulative_filled_quantity * contract_multiplier`。 |
| `limit_improvement_ticks` | 实际成交价相对 limit price 的改善 tick 数。买单为 `(order_price - fill_price) / price_tick`，卖单为 `(fill_price - order_price) / price_tick`。 | 分析脚本计算。 |
| `fee_rate_config` | 策略配置中的 taker fee rate。 | strategy config 的 `lag_taker_fee`。 |
| `fee_quote_estimated` | 按配置 fee rate 估算的成交手续费。 | `filled_notional * fee_rate_config`。 |
| `fee_source` | fee 来源说明。当前估算值为 `config_estimated`。 | 分析脚本填充。 |
| `order_session_id` | Gate `OrderSession` 本进程内 session id。 | `gate_order_send_ok` / `gate_order_response` / diagnostic log。 |
| `owner_thread_cpu` | session active 时 owner thread 所在 CPU。 | `gate_order_session_connected.owner_thread_cpu`，按 `order_session_id` 合并。 |
| `owner_thread_tid` | session owner thread 的 Linux thread id。 | `gate_order_session_connected.owner_thread_tid` 或 `gate_order_ack_latency_diagnostic.owner_thread_tid`。 |
| `local_ip` / `local_port` | 本地 TCP endpoint。 | `gate_order_session_connected`，按 `order_session_id` 合并。 |
| `remote_ip` / `remote_port` | 远端 TCP endpoint。 | `gate_order_session_connected`，按 `order_session_id` 合并。 |
| `send_cpu` | 发送下单请求时 owner thread 所在 CPU。 | `gate_order_send_ok.send_cpu`。 |
| `ack_cpu` | 处理 Gate submit response 时 owner thread 所在 CPU。 | `gate_order_response.ack_cpu`。 |
| `diagnostic_cpu` | 输出 Ack latency diagnostic 时 owner thread 所在 CPU；多次触发用 `;` 合并。 | `gate_order_ack_latency_diagnostic.diagnostic_cpu`。 |
| `tcp_info_available` | 是否成功采集 Linux `TCP_INFO` snapshot。 | `gate_order_response` / diagnostic log。 |
| `tcp_info_rtt_us` | Linux `tcp_info.tcpi_rtt`，单位 microseconds；多次出现取最大值。 | `gate_order_response` / diagnostic log。 |
| `tcp_info_rttvar_us` | Linux `tcp_info.tcpi_rttvar`，单位 microseconds；多次出现取最大值。 | `gate_order_response` / diagnostic log。 |
| `tcp_info_retrans` | Linux `tcp_info.tcpi_retrans`；多次出现取最大值。 | `gate_order_response` / diagnostic log。 |
| `tcp_info_total_retrans` | Linux `tcp_info.tcpi_total_retrans`；多次出现取最大值。 | `gate_order_response` / diagnostic log。 |
| `tcp_info_unacked` | Linux `tcp_info.tcpi_unacked`；多次出现取最大值。 | `gate_order_response` / diagnostic log。 |
| `tcp_info_snd_cwnd` | Linux `tcp_info.tcpi_snd_cwnd`；多次出现取最大值。 | `gate_order_response` / diagnostic log。 |
| `ack_rtt_ns` | 本地下单发送到收到 Ack 的 RTT。 | `ack_local_receive_ns - request_send_local_ns`，或终态日志中的 `ack_rtt_ns`。 |
| `latency_diagnostic_reason` | Gate order session 分阶段 Ack latency diagnostic 触发原因；同一订单多次触发时用 `;` 合并。 | `gate_order_ack_latency_diagnostic.reason`。 |
| `latency_diagnostic_ack_rtt_ns` | diagnostic 日志中记录的最大 Ack RTT。 | `gate_order_ack_latency_diagnostic.ack_rtt_ns`。 |
| `send_to_first_after_hook_ns` | 下单发送到下一次 runtime hook 完成探针的本地耗时。 | `gate_order_ack_latency_diagnostic.send_to_first_after_hook_ns`。 |
| `send_to_first_drive_read_ns` | 下单发送到第一次进入 `DriveRead()` 前探针的本地耗时。 | `gate_order_ack_latency_diagnostic.send_to_first_drive_read_ns`。 |
| `drive_read_duration_ns` | diagnostic 窗口内观察到的最大单次 `DriveRead()` 耗时。 | `gate_order_ack_latency_diagnostic.drive_read_duration_ns`。 |
| `max_observed_drive_read_duration_ns` | diagnostic 窗口内记录的最大 `DriveRead()` 耗时。 | `gate_order_ack_latency_diagnostic.max_observed_drive_read_duration_ns`。 |
| `latency_diagnostic_inflight_at_send` | 发送该订单后 Gate order session 中的 inflight 数量。 | `gate_order_ack_latency_diagnostic.inflight_at_send`。 |
| `max_runtime_loop_gap_ns` | diagnostic 窗口内 owner runtime loop 两次迭代之间的最大间隔。 | `gate_order_ack_latency_diagnostic.max_runtime_loop_gap_ns`。 |
| `runtime_loop_iterations_before_ack` | 从 diagnostic window armed 到 Ack 处理前经过的 runtime loop 迭代次数。 | `gate_order_ack_latency_diagnostic.runtime_loop_iterations_before_ack`。 |
| `order_encode_done_ns` | Gate order JSON payload 编码完成后的本机时间戳。 | `gate_order_ack_latency_diagnostic.order_encode_done_ns`。 |
| `ws_frame_encode_done_ns` | WebSocket text frame 编码完成后的本机时间戳。 | `gate_order_ack_latency_diagnostic.ws_frame_encode_done_ns`。 |
| `write_enqueue_ns` | prepared write 进入 pending queue 的本机时间戳。 | `gate_order_ack_latency_diagnostic.write_enqueue_ns`。 |
| `drive_write_enter_ns` | 首次进入 write pump / inline flush 的本机时间戳。 | `gate_order_ack_latency_diagnostic.drive_write_enter_ns`。 |
| `write_some_enter_ns` | 调用 `send()` / `SSL_write()` 前的本机时间戳。 | `gate_order_ack_latency_diagnostic.write_some_enter_ns`。 |
| `write_some_return_ns` | `send()` / `SSL_write()` 返回后的本机时间戳。 | `gate_order_ack_latency_diagnostic.write_some_return_ns`。 |
| `write_complete_ns` | 当前 request frame 全部写入 transport 后的本机时间戳；partial write / EAGAIN 路径可能为空。 | `gate_order_ack_latency_diagnostic.write_complete_ns`。 |
| `write_some_bytes` | 本次 `send()` / `SSL_write()` 返回的写入字节数。 | `gate_order_ack_latency_diagnostic.write_some_bytes`。 |
| `write_complete_bytes` | 当前 request frame 完整写入的总字节数。 | `gate_order_ack_latency_diagnostic.write_complete_bytes`。 |
| `write_errno` | write syscall / TLS write errno；成功为 `0`。 | `gate_order_ack_latency_diagnostic.write_errno`。 |
| `write_eagain` | 写路径是否遇到 EAGAIN / would-block。 | `gate_order_ack_latency_diagnostic.write_eagain`。 |
| `pending_write_count_after` | enqueue / write 后 pending business write 数量。 | `gate_order_ack_latency_diagnostic.pending_write_count_after`。 |
| `socket_send_queue_available` | 是否成功采集 socket send queue snapshot。 | `gate_order_ack_latency_diagnostic.socket_send_queue_available`。 |
| `tcp_sendq_bytes` | Linux socket send queue 中未被远端 ACK 的字节数。 | `gate_order_ack_latency_diagnostic.tcp_sendq_bytes`。 |
| `tcp_notsent_bytes` | 已进入 TCP 发送队列但尚未发送到网络的字节数；平台不可用时配合 available 字段解读。 | `gate_order_ack_latency_diagnostic.tcp_notsent_bytes`。 |
| `request_send_local_ns` | 调用 WebSocket send 并记录发送成功后的本地时间戳。 | `gate_order_send_ok.request_send_local_ns` 或终态日志。 |
| `ack_local_receive_ns` | 本地收到 Gate Ack 的时间戳。 | `gate_order_response.kind=kAck.local_receive_ns` 或终态日志。 |
| `order_finished_local_ns` | 策略层完成订单终态处理的本地时间戳。 | `lead_lag_order_finished.order_finished_local_ns`。 |
| `source_schema` | 该 order detail 行主要来自哪个提交日志 schema。 | 正常为 `submitted_v1`；缺少提交日志时为 `unknown`。 |
| `warnings` | 缺失字段或异常情况。 | 分析脚本追加，例如 `missing_exchange_order_id`、`missing_symbol`。 |

## position.csv

`position.csv` 以 `run_id + symbol_id + position_id` 配对 entry / exit。每个有成交 exit 生成一行 closed 或 partial-closed slice；仍有剩余 entry 成交量时生成 open 行。

| 字段 | 含义 | 来源或计算 |
|---|---|---|
| `run_id` | 本次运行或 report 的标识。 | 来自 entry 或 exit order。 |
| `position_key` | position 分析行唯一键。 | `run_id:symbol_id:position_id:<exit_local_order_id>`，open 行后缀为 `open`。 |
| `symbol` | 交易 symbol。 | entry 或 exit order。 |
| `symbol_id` | 策略内部 symbol id。 | entry 或 exit order。 |
| `position_id` | 策略 position id。 | entry 或 exit order。 |
| `position_direction` | position 方向，`kLong` 或 `kShort`。 | entry 或 exit order。 |
| `status` | position 行状态。 | `closed`、`partial_closed`、`open`、`missing_entry` 或 `over_closed`。 |
| `entry_local_order_id` | 开仓订单本地 id。 | entry order。 |
| `exit_local_order_id` | 平仓订单本地 id。 | exit order；open 行为空。 |
| `entry_exchange_order_id` | 开仓订单交易所 id。 | entry order。 |
| `exit_exchange_order_id` | 平仓订单交易所 id。 | exit order。 |
| `entry_ns` | entry 时间戳。 | 优先用 entry `order_finished_local_ns`，缺失时回退到 ack 或 send 时间。 |
| `exit_ns` | exit 时间戳。 | 优先用 exit `order_finished_local_ns`，缺失时回退到 ack 或 send 时间。 |
| `holding_ns` | 持仓时间。 | `exit_ns - entry_ns`。 |
| `entry_side` | 开仓订单 side。 | entry order。 |
| `exit_side` | 平仓订单 side。 | exit order。 |
| `entry_raw_price` | entry signal raw price。 | entry order。 |
| `exit_raw_price` | exit signal raw price。 | exit order。 |
| `entry_order_price` | entry 实际 limit price。 | entry order。 |
| `exit_order_price` | exit 实际 limit price。 | exit order。 |
| `entry_price` | entry 成交价。 | entry average fill price，缺失时回退 last fill price。 |
| `exit_price` | exit 成交价。 | exit average fill price，缺失时回退 last fill price。 |
| `entry_volume` | 本行参与匹配的 entry 成交量。 | closed slice 中等于 matched volume；open 行为剩余 entry volume。 |
| `exit_volume` | exit 订单实际成交量。 | exit order 的 cumulative filled quantity。 |
| `matched_volume` | 本行用于 PnL 的 entry / exit 匹配量。 | `min(exit_volume, remaining_entry_volume)`。 |
| `remaining_entry_volume` | 本行处理后剩余未平 entry 成交量。 | 分析脚本计算。 |
| `contract_multiplier` | 合约乘数。 | entry 或 exit order。 |
| `entry_notional` | 本行 entry notional。 | `entry_price * matched_or_remaining_volume * contract_multiplier`。 |
| `exit_notional` | 本行 exit notional。 | `exit_price * matched_volume * contract_multiplier`。 |
| `gross_pnl` | 未扣费 PnL。多头为 `(exit_price - entry_price) * matched_volume * multiplier`，空头为 `(entry_price - exit_price) * matched_volume * multiplier`。 | 分析脚本计算。 |
| `entry_fee_quote_estimated` | 分摊到本行的 entry 估算手续费。 | entry fee 按 matched volume 或 remaining volume 分摊。 |
| `exit_fee_quote_estimated` | 本行 exit 估算手续费。 | exit order 的 `fee_quote_estimated`。 |
| `total_fee_quote_estimated` | 本行总估算手续费。 | `entry_fee_quote_estimated + exit_fee_quote_estimated`。 |
| `net_pnl` | 扣估算手续费后的 PnL。 | `gross_pnl - total_fee_quote_estimated`。 |
| `entry_ack_rtt_ns` | entry 订单 Ack RTT。 | entry order 的 `ack_rtt_ns`。 |
| `exit_ack_rtt_ns` | exit 订单 Ack RTT。 | exit order 的 `ack_rtt_ns`。 |
| `entry_fee_source` | entry fee 来源。 | entry order 的 `fee_source`。 |
| `exit_fee_source` | exit fee 来源。 | exit order 的 `fee_source`。 |
| `warnings` | position 配对或数量异常。 | 例如 `multiple_entry_orders`、`missing_entry_order`、`exit_volume_exceeds_entry`。 |

## latency.csv

`latency.csv` 一行表示一个本地订单的延迟诊断。核心分析应优先看本地时钟闭环字段，尤其是 `ack_rtt_ns`、`send_to_finish_local_ns` 和 `ack_to_finish_local_ns`。

| 字段 | 含义 | 来源或计算 |
|---|---|---|
| `run_id` | 本次运行或 report 的标识。 | 来自 order detail。 |
| `latency_key` | latency 行唯一键。 | `run_id:local_order_id`。 |
| `local_order_id` | 本地订单 id。 | order detail。 |
| `exchange_order_id` | 交易所 order id。 | order detail。 |
| `symbol` | 交易 symbol。 | order detail。 |
| `symbol_id` | 策略内部 symbol id。 | order detail。 |
| `position_id` | 策略 position id。 | order detail。 |
| `position_direction` | position 方向。 | order detail。 |
| `order_role` | 订单角色，`entry` 或 `exit`。 | order detail。 |
| `action` | 策略动作。 | order detail。 |
| `side` | 下单方向。 | order detail。 |
| `reduce_only` | 是否 reduce-only。 | order detail。 |
| `status` | 订单终态。 | order detail。 |
| `finish_reason` | 订单结束原因。 | order detail。 |
| `reject_reason` | 拒单原因。 | order detail。 |
| `request_sequence` | Gate order session 请求序号。 | order detail。 |
| `encoded_request_id` | WebSocket payload 请求 id。 | order detail。 |
| `trigger_exchange_ns` | 触发 BBO 的交易所时间戳。 | order detail。 |
| `trigger_local_ns` | 触发 BBO 在 data session ingress 处记录的本机时间戳。 | order detail。 |
| `on_book_ticker_entry_ns` | 策略进入 `Strategy::OnBookTicker()` 的本机时间戳。 | order detail。 |
| `signal_decision_ns` | 策略确认 signal triggered 后的本机时间戳。 | order detail。 |
| `request_send_local_ns` | 本地发送请求成功后的时间戳。 | Gate send log 或终态日志。 |
| `ack_local_receive_ns` | 本地收到 Ack 的时间戳。 | Gate Ack log 或终态日志。 |
| `response_local_receive_ns` | 本地收到非 Ack response 的时间戳。 | 终态日志中的 response timing。 |
| `order_finished_local_ns` | 本地订单终态处理完成时间戳。 | order detail。 |
| `ack_exchange_ns` | 交易所 Ack 时间戳。 | Gate Ack 或终态日志。 |
| `response_exchange_ns` | 交易所 response 时间戳。 | 终态日志。 |
| `accepted_exchange_ns` | 交易所接受订单时间戳。 | 终态日志。 |
| `finish_exchange_ns` | 交易所订单终态时间戳。 | 终态日志。 |
| `ack_rtt_ns` | 本地下单发送到收到 Ack 的 RTT。 | 优先取 order detail；缺失时用 `ack_local_receive_ns - request_send_local_ns`。 |
| `response_rtt_ns` | 本地下单发送到收到 response 的 RTT。 | order detail。 |
| `send_to_ack_local_ns` | 本地下单发送到收到 Ack 的本地闭环。 | `ack_local_receive_ns - request_send_local_ns`。 |
| `send_to_response_local_ns` | 本地下单发送到收到 response 的本地闭环。 | `response_local_receive_ns - request_send_local_ns`。 |
| `send_to_finish_local_ns` | 本地下单发送到订单终态处理完成的本地闭环。 | `order_finished_local_ns - request_send_local_ns`。 |
| `ack_to_finish_local_ns` | 本地收到 Ack 到订单终态处理完成的时间。 | `order_finished_local_ns - ack_local_receive_ns`。 |
| `bbo_to_strategy_ns` | BBO data session 本机时间到策略入口的耗时。 | `on_book_ticker_entry_ns - trigger_local_ns`。只有两个输入看起来处于同一时钟域且差值在本地 pipeline 合理窗口内时才输出；跨时钟域时留空并在 `warnings` 记录 `cross_clock_bbo_to_strategy_ns`。 |
| `strategy_to_signal_ns` | 策略入口到 signal decision 的耗时。 | `signal_decision_ns - on_book_ticker_entry_ns`。 |
| `signal_to_request_send_ns` | signal decision 到订单发送成功的本机耗时。 | `request_send_local_ns - signal_decision_ns`。 |
| `trigger_to_request_send_ns` | BBO data session 本机时间到订单发送成功的本机耗时。 | `request_send_local_ns - trigger_local_ns`。只有两个输入看起来处于同一时钟域且差值在本地 pipeline 合理窗口内时才输出；跨时钟域时留空并在 `warnings` 记录 `cross_clock_trigger_to_request_send_ns`。 |
| `ack_exchange_to_local_ns` | Ack exchange timestamp 到本地接收 timestamp 的差值。 | order detail。该值受本地和交易所时钟偏移影响，只能作诊断参考。 |
| `response_exchange_to_local_ns` | response exchange timestamp 到本地接收 timestamp 的差值。 | order detail。该值受本地和交易所时钟偏移影响，只能作诊断参考。 |
| `exchange_lifecycle_ns` | 交易所侧 Ack 到订单终态 update 的生命周期诊断值。 | 优先由 `finish_exchange_ns - ack_exchange_ns` 计算，只使用 Gate exchange timestamp，不混用本地时钟；如果 exchange timestamp 缺失则为空。该值仍受 Gate timestamp 字段语义限制，只作交易所侧 lifecycle 诊断。 |
| `order_session_id` | Gate `OrderSession` 本进程内 session id。 | order detail 中合并后的 session 字段。 |
| `owner_thread_cpu` | session active 时 owner thread 所在 CPU。 | order detail 中合并后的 session 字段。 |
| `owner_thread_tid` | session owner thread 的 Linux thread id。 | order detail 中合并后的 session / diagnostic 字段。 |
| `local_ip` / `local_port` | 本地 TCP endpoint。 | order detail 中合并后的 session 字段。 |
| `remote_ip` / `remote_port` | 远端 TCP endpoint。 | order detail 中合并后的 session 字段。 |
| `send_cpu` | 发送下单请求时 owner thread 所在 CPU。 | order detail 中合并后的 send 字段。 |
| `ack_cpu` | 处理 Gate submit response 时 owner thread 所在 CPU。 | order detail 中合并后的 response 字段。 |
| `diagnostic_cpu` | 输出 Ack latency diagnostic 时 owner thread 所在 CPU；多次触发用 `;` 合并。 | order detail 中合并后的 diagnostic 字段。 |
| `tcp_info_available` | 是否成功采集 Linux `TCP_INFO` snapshot。 | order detail 中合并后的 TCP_INFO 字段。 |
| `tcp_info_rtt_us` | Linux `tcp_info.tcpi_rtt`，单位 microseconds；多次出现取最大值。 | order detail 中合并后的 TCP_INFO 字段。 |
| `tcp_info_rttvar_us` | Linux `tcp_info.tcpi_rttvar`，单位 microseconds；多次出现取最大值。 | order detail 中合并后的 TCP_INFO 字段。 |
| `tcp_info_retrans` | Linux `tcp_info.tcpi_retrans`；多次出现取最大值。 | order detail 中合并后的 TCP_INFO 字段。 |
| `tcp_info_total_retrans` | Linux `tcp_info.tcpi_total_retrans`；多次出现取最大值。 | order detail 中合并后的 TCP_INFO 字段。 |
| `tcp_info_unacked` | Linux `tcp_info.tcpi_unacked`；多次出现取最大值。 | order detail 中合并后的 TCP_INFO 字段。 |
| `tcp_info_snd_cwnd` | Linux `tcp_info.tcpi_snd_cwnd`；多次出现取最大值。 | order detail 中合并后的 TCP_INFO 字段。 |
| `latency_diagnostic_reason` | Gate order session 分阶段 Ack latency diagnostic 触发原因；同一订单多次触发时用 `;` 合并。 | order detail 中合并后的 diagnostic 字段。 |
| `latency_diagnostic_ack_rtt_ns` | diagnostic 日志中记录的最大 Ack RTT。 | order detail 中合并后的 diagnostic 字段。 |
| `send_to_first_after_hook_ns` | 下单发送到下一次 runtime hook 完成探针的本地耗时。 | order detail 中合并后的 diagnostic 字段。 |
| `send_to_first_drive_read_ns` | 下单发送到第一次进入 `DriveRead()` 前探针的本地耗时。 | order detail 中合并后的 diagnostic 字段。 |
| `drive_read_duration_ns` | diagnostic 窗口内观察到的最大单次 `DriveRead()` 耗时。 | order detail 中合并后的 diagnostic 字段。 |
| `max_observed_drive_read_duration_ns` | diagnostic 窗口内记录的最大 `DriveRead()` 耗时。 | order detail 中合并后的 diagnostic 字段。 |
| `latency_diagnostic_inflight_at_send` | 发送该订单后 Gate order session 中的 inflight 数量。 | order detail 中合并后的 diagnostic 字段。 |
| `max_runtime_loop_gap_ns` | diagnostic 窗口内 owner runtime loop 两次迭代之间的最大间隔。 | order detail 中合并后的 diagnostic 字段。 |
| `runtime_loop_iterations_before_ack` | 从 diagnostic window armed 到 Ack 处理前经过的 runtime loop 迭代次数。 | order detail 中合并后的 diagnostic 字段。 |
| `order_encode_done_ns` | Gate order JSON payload 编码完成后的本机时间戳。 | order detail 中合并后的 diagnostic 字段。 |
| `ws_frame_encode_done_ns` | WebSocket text frame 编码完成后的本机时间戳。 | order detail 中合并后的 diagnostic 字段。 |
| `write_enqueue_ns` | prepared write 进入 pending queue 的本机时间戳。 | order detail 中合并后的 diagnostic 字段。 |
| `drive_write_enter_ns` | 首次进入 write pump / inline flush 的本机时间戳。 | order detail 中合并后的 diagnostic 字段。 |
| `write_some_enter_ns` | 调用 `send()` / `SSL_write()` 前的本机时间戳。 | order detail 中合并后的 diagnostic 字段。 |
| `write_some_return_ns` | `send()` / `SSL_write()` 返回后的本机时间戳。 | order detail 中合并后的 diagnostic 字段。 |
| `write_complete_ns` | 当前 request frame 全部写入 transport 后的本机时间戳；partial write / EAGAIN 路径可能为空。 | order detail 中合并后的 diagnostic 字段。 |
| `write_some_bytes` | 本次 `send()` / `SSL_write()` 返回的写入字节数。 | order detail 中合并后的 diagnostic 字段。 |
| `write_complete_bytes` | 当前 request frame 完整写入的总字节数。 | order detail 中合并后的 diagnostic 字段。 |
| `write_errno` | write syscall / TLS write errno；成功为 `0`。 | order detail 中合并后的 diagnostic 字段。 |
| `write_eagain` | 写路径是否遇到 EAGAIN / would-block。 | order detail 中合并后的 diagnostic 字段。 |
| `pending_write_count_after` | enqueue / write 后 pending business write 数量。 | order detail 中合并后的 diagnostic 字段。 |
| `socket_send_queue_available` | 是否成功采集 socket send queue snapshot。 | order detail 中合并后的 diagnostic 字段。 |
| `tcp_sendq_bytes` | Linux socket send queue 中未被远端 ACK 的字节数。 | order detail 中合并后的 diagnostic 字段。 |
| `tcp_notsent_bytes` | 已进入 TCP 发送队列但尚未发送到网络的字节数。 | order detail 中合并后的 diagnostic 字段。 |
| `warnings` | latency 分析异常。 | 例如 `missing_request_send_local_ns`、`missing_ack_local_receive_ns`。 |

## 当前建议新增字段

为了避免 `trigger_exchange` 和实际下单交易所混淆，后续建议在 `signal.csv` 和 `order_detail.csv` 增加：

| 字段 | 建议含义 | 当前获取方式 |
|---|---|---|
| `order_exchange` | 实际下单交易所，例如 `kGate`。 | 当前可由 `gate_order_send_ok` / `gate_order_response` 日志来源推导；更稳妥的做法是在日志和分析脚本中显式写出。 |

新增后，`trigger_exchange` 继续表示信号触发行情来源，`order_exchange` 表示订单执行场所。对于跨交易所策略分析，两者应同时保留。
