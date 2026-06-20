# SKYAI_USDT 归档字段说明

本文档说明 `reports/20260619_SKYAI_fillability/` 中主要数据文件的字段含义。CSV 字段按文件分组列出；候选表如果与主表共享 schema，会在同一节列出。

## 通用约定

- 空字段表示该日志阶段没有记录、该行语义不适用，或分析时无法从原始数据恢复。
- `*_ns` 字段单位为纳秒；`*_us` 字段单位为微秒。
- `BookTicker.id` 是行情记录 id；`*_lag_id` / `*_lead_id` 是策略在对应订单阶段看到的 latest lag/lead BookTicker id。
- `any` 表示 BBO 价格穿越且对手一档量大于 0：买单 `ask <= order_price`，卖单 `bid >= order_price`。
- `full` 表示满足 `any` 且对手一档量 `>= quantity`。
- `contra_price` / `contra_volume` 表示订单对手一档；买单取 ask/ask_volume，卖单取 bid/bid_volume。
- `margin_ticks` 为对手价相对 `order_price` 的 tick 余量，正值表示价格穿越，负值表示未穿越。

## 输入订单表

适用文件：
- `inputs/orders.csv`

字段数：`134`

| 字段 | 含义 |
|---|---|
| `run_id` | 本次运行标识。 |
| `local_order_id` | 策略本地订单 id。 |
| `text_order_id` | Gate text order id，当前通常为 `t-<local_order_id>`。 |
| `request_sequence` | Gate order session 内部请求序号。 |
| `encoded_request_id` | WebSocket payload 中编码后的 request id。 |
| `exchange_order_id` | 交易所订单 id。 |
| `symbol` | 交易 symbol。 |
| `symbol_id` | instrument symbol id。 |
| `trigger_exchange` | 触发策略信号的行情来源交易所。 |
| `trigger_symbol_id` | 触发行情对应的 symbol id。 |
| `trigger_exchange_ns` | 触发 BBO 的交易所/行情源时间戳，单位 ns。 |
| `trigger_local_ns` | 触发 BBO 进入本机 data session 的本地时间戳，单位 ns。 |
| `on_book_ticker_entry_ns` | 策略进入 `OnBookTicker()` 的本地时间戳，单位 ns。 |
| `signal_decision_ns` | 策略确认 signal triggered 的本地时间戳，单位 ns。 |
| `lead_exchange_ns` | signal/order 对应 lead 侧最新 BBO 的行情源时间戳。 |
| `lead_local_ns` | signal/order 对应 lead 侧最新 BBO 的本地时间戳。 |
| `signal_lead_id` | signal/order 对应 lead 侧 BookTicker id。 |
| `lead_freshness_ns` | lead BBO freshness，通常为 `signal_decision_ns - lead_exchange_ns`。 |
| `lag_exchange_ns` | signal/order 对应 lag 侧最新 BBO 的行情源时间戳。 |
| `lag_local_ns` | signal/order 对应 lag 侧最新 BBO 的本地时间戳。 |
| `signal_lag_id` | signal/order 对应 lag 侧 BookTicker id。 |
| `lag_freshness_ns` | lag BBO freshness，通常为 `signal_decision_ns - lag_exchange_ns`。 |
| `max_lead_freshness_ns` | 开仓 freshness guard 的 lead 阈值，单位 ns。 |
| `max_lag_freshness_ns` | 开仓 freshness guard 的 lag 阈值，单位 ns。 |
| `freshness_guard_pass` | open order 是否通过 freshness guard。 |
| `freshness_reject_reason` | freshness guard 拒单原因；成功通常为 `-`。 |
| `signal_role` | 该 signal 在 pair 中的 role，例如 `kLead` 或 `kLag`。 |
| `order_role` | 订单角色，`entry` 或 `exit`。 |
| `position_id` | 策略 position id。 |
| `position_event` | position 状态事件。 |
| `position_direction` | position 方向，例如 `kLong` 或 `kShort`。 |
| `entry_local_order_id` | 所属 position 的开仓订单本地 id。 |
| `action` | 策略动作，例如 `kOpenLong`、`kOpenShort`、`kCloseLong`。 |
| `side` | 订单方向，`kBuy` 或 `kSell`。 |
| `reduce_only` | 订单是否为 reduce-only。 |
| `time_in_force` | 订单 TIF；本分析主样本为 IOC / `kImmediateOrCancel`。 |
| `status` | 状态字段，含义取决于文件：instrument 状态、订单终态或 position 状态。 |
| `finish_reason` | 订单结束原因，例如 `kImmediateOrCancel`。 |
| `reject_reason` | 拒单原因；IOC cancel no-fill 常见为 `kUnknown`。 |
| `raw_price` | 策略信号使用的原始 lag 价格。 |
| `order_price` | 实际提交到交易所的 limit price。 |
| `price_text` | 下发请求中编码后的价格文本。 |
| `price_tick` | 该合约价格 tick。 |
| `slippage_ticks` | 策略配置的下单滑点 tick 数。 |
| `order_offset_ticks` | `order_price` 相对 `raw_price` 的实际偏移 tick。 |
| `quantity` | 订单数量，单位为合约张数。 |
| `quantity_text` | 下发请求中编码后的数量文本。 |
| `cumulative_filled_quantity` | 累计成交数量。 |
| `left_quantity` | 剩余未成交数量。 |
| `cancelled_quantity` | 取消数量。 |
| `fill_rate` | 成交比例，`cumulative_filled_quantity / quantity`。 |
| `average_fill_price` | 平均成交价。 |
| `last_fill_price` | 最近一次成交价。 |
| `contract_multiplier` | 合约乘数。 |
| `filled_notional` | 成交 notional，单位 quote currency。 |
| `fill_role` | 成交回报中的角色字段。 |
| `exec_slippage_price` | 成交价相对 raw price 的价格滑点。 |
| `exec_slippage_ticks` | 成交滑点，单位 tick。 |
| `exec_slippage_bps` | 成交滑点，单位 bps。 |
| `exec_slippage_quote` | 成交滑点折算的 quote 损耗。 |
| `limit_improvement_ticks` | 成交价相对 limit price 的改善 tick。 |
| `fee_rate_config` | 策略配置中的手续费率。 |
| `fee_quote_estimated` | 估算手续费 quote。 |
| `fee_source` | 手续费来源说明。 |
| `order_session_id` | Gate OrderSession 本进程内 session id。 |
| `owner_thread_cpu` | order session owner thread 所在 CPU。 |
| `owner_thread_tid` | order session owner thread 的 Linux tid。 |
| `local_ip` | 本地 TCP endpoint IP。 |
| `local_port` | 本地 TCP endpoint port。 |
| `remote_ip` | 远端 TCP endpoint IP。 |
| `remote_port` | 远端 TCP endpoint port。 |
| `send_cpu` | 发送请求时 owner thread 所在 CPU。 |
| `ack_cpu` | 处理 Ack/response 时 owner thread 所在 CPU。 |
| `diagnostic_cpu` | 输出 latency diagnostic 时 owner thread 所在 CPU。 |
| `tcp_info_available` | 是否成功采集 Linux `TCP_INFO`。 |
| `tcp_info_rtt_us` | `TCP_INFO.tcpi_rtt`，单位 us。 |
| `tcp_info_rttvar_us` | `TCP_INFO.tcpi_rttvar`，单位 us。 |
| `tcp_info_retrans` | `TCP_INFO.tcpi_retrans`。 |
| `tcp_info_total_retrans` | `TCP_INFO.tcpi_total_retrans`。 |
| `tcp_info_unacked` | `TCP_INFO.tcpi_unacked`。 |
| `tcp_info_snd_cwnd` | `TCP_INFO.tcpi_snd_cwnd`。 |
| `ack_rtt_ns` | 从本地下发请求到收到 Ack 的 RTT，单位 ns。 |
| `ack_exchange_request_ingress_ns` | Gate Ack response header `x_in_time` 转 ns。 |
| `ack_exchange_response_egress_ns` | Gate Ack response header `x_out_time` 转 ns。 |
| `ack_exchange_process_ns` | Gate Ack response 交易所侧处理耗时，单位 ns。 |
| `ack_lead_id` | 收到 Gate Ack 时策略看到的 lead BookTicker id。 |
| `ack_lag_id` | 收到 Gate Ack 时策略看到的 lag BookTicker id。 |
| `accepted_lead_id` | 处理 Accepted response 时策略看到的 lead BookTicker id。 |
| `accepted_lag_id` | 处理 Accepted response 时策略看到的 lag BookTicker id。 |
| `partial_filled_lead_id` | 处理 partial fill feedback 时策略看到的 lead BookTicker id。 |
| `partial_filled_lag_id` | 处理 partial fill feedback 时策略看到的 lag BookTicker id。 |
| `filled_lead_id` | 处理 Filled feedback 时策略看到的 lead BookTicker id。 |
| `filled_lag_id` | 处理 Filled feedback 时策略看到的 lag BookTicker id。 |
| `cancelled_lead_id` | 处理 Cancelled feedback 时策略看到的 lead BookTicker id。 |
| `cancelled_lag_id` | 处理 Cancelled feedback 时策略看到的 lag BookTicker id。 |
| `rejected_lead_id` | 处理 rejected feedback 时策略看到的 lead BookTicker id。 |
| `rejected_lag_id` | 处理 rejected feedback 时策略看到的 lag BookTicker id。 |
| `unknown_result_lead_id` | 处理 unknown result 时策略看到的 lead BookTicker id。 |
| `unknown_result_lag_id` | 处理 unknown result 时策略看到的 lag BookTicker id。 |
| `cancel_accepted_lead_id` | 处理 cancel accepted 时策略看到的 lead BookTicker id。 |
| `cancel_accepted_lag_id` | 处理 cancel accepted 时策略看到的 lag BookTicker id。 |
| `cancel_rejected_lead_id` | 处理 cancel rejected 时策略看到的 lead BookTicker id。 |
| `cancel_rejected_lag_id` | 处理 cancel rejected 时策略看到的 lag BookTicker id。 |
| `continuity_lost_lead_id` | 处理 feedback continuity lost 时策略看到的 lead BookTicker id。 |
| `continuity_lost_lag_id` | 处理 feedback continuity lost 时策略看到的 lag BookTicker id。 |
| `latency_diagnostic_reason` | Gate order session Ack latency diagnostic 触发原因；空值表示未触发。 |
| `latency_diagnostic_ack_rtt_ns` | 对应请求/响应 RTT，单位 ns。 |
| `send_to_first_after_hook_ns` | 纳秒级时间戳或耗时字段，具体事件由字段名前缀表示。 |
| `send_to_first_drive_read_ns` | 纳秒级时间戳或耗时字段，具体事件由字段名前缀表示。 |
| `drive_read_duration_ns` | 纳秒级时间戳或耗时字段，具体事件由字段名前缀表示。 |
| `max_observed_drive_read_duration_ns` | 纳秒级时间戳或耗时字段，具体事件由字段名前缀表示。 |
| `latency_diagnostic_inflight_at_send` | 发送该订单后 order session 中的 inflight 请求数量。 |
| `max_runtime_loop_gap_ns` | 纳秒级时间戳或耗时字段，具体事件由字段名前缀表示。 |
| `runtime_loop_iterations_before_ack` | diagnostic window armed 后到 Ack 处理前经过的 runtime loop 迭代次数。 |
| `order_encode_done_ns` | 纳秒级时间戳或耗时字段，具体事件由字段名前缀表示。 |
| `ws_frame_encode_done_ns` | 纳秒级时间戳或耗时字段，具体事件由字段名前缀表示。 |
| `write_enqueue_ns` | 纳秒级时间戳或耗时字段，具体事件由字段名前缀表示。 |
| `drive_write_enter_ns` | 纳秒级时间戳或耗时字段，具体事件由字段名前缀表示。 |
| `write_some_enter_ns` | 纳秒级时间戳或耗时字段，具体事件由字段名前缀表示。 |
| `write_some_return_ns` | 纳秒级时间戳或耗时字段，具体事件由字段名前缀表示。 |
| `write_complete_ns` | 纳秒级时间戳或耗时字段，具体事件由字段名前缀表示。 |
| `write_some_bytes` | 单次 `send()` / TLS write 返回的写入字节数。 |
| `write_complete_bytes` | 当前 request frame 完整写入 transport 的总字节数。 |
| `write_errno` | write syscall / TLS write errno；成功为 `0`。 |
| `write_eagain` | 写路径是否遇到 EAGAIN / would-block。 |
| `pending_write_count_after` | enqueue 或 write 后 pending business write 数量。 |
| `socket_send_queue_available` | 是否成功采集 socket send queue snapshot。 |
| `tcp_sendq_bytes` | Linux socket send queue 中未被远端 ACK 的字节数。 |
| `tcp_notsent_bytes` | 已进入 TCP 发送队列但尚未发送到网络的字节数。 |
| `request_send_local_ns` | 本地发送下单请求成功的时间戳，单位 ns。 |
| `ack_local_receive_ns` | 本地收到 Ack 的时间戳，单位 ns。 |
| `order_finished_local_ns` | 策略层完成订单终态处理的本地时间戳，单位 ns。 |
| `source_schema` | order detail 行的主要来源 schema。 |
| `warnings` | 分析生成时发现的缺失字段或异常标记。 |

## 输入信号表

适用文件：
- `inputs/signals.csv`

字段数：`48`

| 字段 | 含义 |
|---|---|
| `run_id` | 本次运行标识。 |
| `signal_index` | signal 在 `signals.csv` 中的序号。 |
| `log_time` | signal 日志的 wall-clock 时间文本。 |
| `trigger_exchange` | 触发策略信号的行情来源交易所。 |
| `trigger_symbol_id` | 触发行情对应的 symbol id。 |
| `trigger_exchange_ns` | 触发 BBO 的交易所/行情源时间戳，单位 ns。 |
| `trigger_local_ns` | 触发 BBO 进入本机 data session 的本地时间戳，单位 ns。 |
| `on_book_ticker_entry_ns` | 策略进入 `OnBookTicker()` 的本地时间戳，单位 ns。 |
| `signal_decision_ns` | 策略确认 signal triggered 的本地时间戳，单位 ns。 |
| `lead_exchange_ns` | signal/order 对应 lead 侧最新 BBO 的行情源时间戳。 |
| `lead_local_ns` | signal/order 对应 lead 侧最新 BBO 的本地时间戳。 |
| `signal_lead_id` | signal/order 对应 lead 侧 BookTicker id。 |
| `lead_freshness_ns` | lead BBO freshness，通常为 `signal_decision_ns - lead_exchange_ns`。 |
| `lag_exchange_ns` | signal/order 对应 lag 侧最新 BBO 的行情源时间戳。 |
| `lag_local_ns` | signal/order 对应 lag 侧最新 BBO 的本地时间戳。 |
| `signal_lag_id` | signal/order 对应 lag 侧 BookTicker id。 |
| `lag_freshness_ns` | lag BBO freshness，通常为 `signal_decision_ns - lag_exchange_ns`。 |
| `symbol` | 交易 symbol。 |
| `symbol_id` | instrument symbol id。 |
| `signal_role` | 该 signal 在 pair 中的 role，例如 `kLead` 或 `kLag`。 |
| `action` | 策略动作，例如 `kOpenLong`、`kOpenShort`、`kCloseLong`。 |
| `side` | 订单方向，`kBuy` 或 `kSell`。 |
| `reduce_only` | 订单是否为 reduce-only。 |
| `signal_position_id` | signal log 中记录的 position id。 |
| `raw_price` | 策略信号使用的原始 lag 价格。 |
| `local_order_id` | 策略本地订单 id。 |
| `request_sequence` | Gate order session 内部请求序号。 |
| `request_send_local_ns` | 本地发送下单请求成功的时间戳，单位 ns。 |
| `bbo_to_strategy_ns` | `on_book_ticker_entry_ns - trigger_local_ns`，单位 ns。 |
| `strategy_to_signal_ns` | `signal_decision_ns - on_book_ticker_entry_ns`，单位 ns。 |
| `signal_to_request_send_ns` | `request_send_local_ns - signal_decision_ns`，单位 ns。 |
| `trigger_to_request_send_ns` | `request_send_local_ns - trigger_local_ns`，单位 ns。 |
| `exchange_order_id` | 交易所订单 id。 |
| `order_role` | 订单角色，`entry` 或 `exit`。 |
| `order_position_id` | 与 signal 关联的订单最终使用的 position id。 |
| `position_event` | position 状态事件。 |
| `position_direction` | position 方向，例如 `kLong` 或 `kShort`。 |
| `order_price` | 实际提交到交易所的 limit price。 |
| `price_tick` | 该合约价格 tick。 |
| `slippage_ticks` | 策略配置的下单滑点 tick 数。 |
| `quantity` | 订单数量，单位为合约张数。 |
| `status` | 状态字段，含义取决于文件：instrument 状态、订单终态或 position 状态。 |
| `cumulative_filled_quantity` | 累计成交数量。 |
| `average_fill_price` | 平均成交价。 |
| `exec_slippage_ticks` | 成交滑点，单位 tick。 |
| `ack_rtt_ns` | 从本地下发请求到收到 Ack 的 RTT，单位 ns。 |
| `order_finished_local_ns` | 策略层完成订单终态处理的本地时间戳，单位 ns。 |
| `warnings` | 分析生成时发现的缺失字段或异常标记。 |

## 输入持仓表

适用文件：
- `inputs/positions.csv`

字段数：`51`

| 字段 | 含义 |
|---|---|
| `run_id` | 本次运行标识。 |
| `position_key` | position 分析行唯一键。 |
| `symbol` | 交易 symbol。 |
| `symbol_id` | instrument symbol id。 |
| `position_id` | 策略 position id。 |
| `position_direction` | position 方向，例如 `kLong` 或 `kShort`。 |
| `status` | 状态字段，含义取决于文件：instrument 状态、订单终态或 position 状态。 |
| `entry_local_order_id` | 所属 position 的开仓订单本地 id。 |
| `exit_local_order_id` | position exit 侧字段；含义与去掉 `exit_` 前缀后的订单/行情字段一致。 |
| `entry_exchange_order_id` | 开仓订单交易所 id。 |
| `exit_exchange_order_id` | 平仓订单交易所 id。 |
| `entry_lead_exchange_ns` | position entry 侧字段；含义与去掉 `entry_` 前缀后的订单/行情字段一致。 |
| `entry_lead_local_ns` | position entry 侧字段；含义与去掉 `entry_` 前缀后的订单/行情字段一致。 |
| `entry_lead_freshness_ns` | position entry 侧字段；含义与去掉 `entry_` 前缀后的订单/行情字段一致。 |
| `entry_lag_exchange_ns` | position entry 侧字段；含义与去掉 `entry_` 前缀后的订单/行情字段一致。 |
| `entry_lag_local_ns` | position entry 侧字段；含义与去掉 `entry_` 前缀后的订单/行情字段一致。 |
| `entry_lag_freshness_ns` | position entry 侧字段；含义与去掉 `entry_` 前缀后的订单/行情字段一致。 |
| `exit_lead_exchange_ns` | position exit 侧字段；含义与去掉 `exit_` 前缀后的订单/行情字段一致。 |
| `exit_lead_local_ns` | position exit 侧字段；含义与去掉 `exit_` 前缀后的订单/行情字段一致。 |
| `exit_lead_freshness_ns` | position exit 侧字段；含义与去掉 `exit_` 前缀后的订单/行情字段一致。 |
| `exit_lag_exchange_ns` | position exit 侧字段；含义与去掉 `exit_` 前缀后的订单/行情字段一致。 |
| `exit_lag_local_ns` | position exit 侧字段；含义与去掉 `exit_` 前缀后的订单/行情字段一致。 |
| `exit_lag_freshness_ns` | position exit 侧字段；含义与去掉 `exit_` 前缀后的订单/行情字段一致。 |
| `entry_ns` | position entry 时间戳，单位 ns。 |
| `exit_ns` | position exit 时间戳，单位 ns。 |
| `holding_ns` | 持仓时间，单位 ns。 |
| `entry_side` | 开仓订单 side。 |
| `exit_side` | 平仓订单 side。 |
| `entry_raw_price` | 开仓 signal 原始价格。 |
| `exit_raw_price` | 平仓 signal 原始价格。 |
| `entry_order_price` | 开仓 limit price。 |
| `exit_order_price` | 平仓 limit price。 |
| `entry_price` | 开仓成交价。 |
| `exit_price` | 平仓成交价。 |
| `entry_volume` | 本 position 行参与匹配的开仓成交量。 |
| `exit_volume` | 本 position 行参与匹配的平仓成交量。 |
| `matched_volume` | entry/exit 匹配成交量。 |
| `remaining_entry_volume` | 未匹配的 entry 剩余成交量。 |
| `contract_multiplier` | 合约乘数。 |
| `entry_notional` | position entry 侧字段；含义与去掉 `entry_` 前缀后的订单/行情字段一致。 |
| `exit_notional` | position exit 侧字段；含义与去掉 `exit_` 前缀后的订单/行情字段一致。 |
| `gross_pnl` | 不含手续费的 PnL。 |
| `entry_fee_quote_estimated` | entry 侧估算手续费。 |
| `exit_fee_quote_estimated` | exit 侧估算手续费。 |
| `total_fee_quote_estimated` | entry 与 exit 两侧估算手续费之和。 |
| `net_pnl` | 扣除估算手续费后的净 PnL。 |
| `entry_ack_rtt_ns` | position entry 侧字段；含义与去掉 `entry_` 前缀后的订单/行情字段一致。 |
| `exit_ack_rtt_ns` | position exit 侧字段；含义与去掉 `exit_` 前缀后的订单/行情字段一致。 |
| `entry_fee_source` | position entry 侧字段；含义与去掉 `entry_` 前缀后的订单/行情字段一致。 |
| `exit_fee_source` | position exit 侧字段；含义与去掉 `exit_` 前缀后的订单/行情字段一致。 |
| `warnings` | 分析生成时发现的缺失字段或异常标记。 |

## 输入延迟表

适用文件：
- `inputs/latency.csv`

字段数：`119`

| 字段 | 含义 |
|---|---|
| `run_id` | 本次运行标识。 |
| `latency_key` | latency 行唯一键或分类键。 |
| `local_order_id` | 策略本地订单 id。 |
| `exchange_order_id` | 交易所订单 id。 |
| `symbol` | 交易 symbol。 |
| `symbol_id` | instrument symbol id。 |
| `position_id` | 策略 position id。 |
| `position_direction` | position 方向，例如 `kLong` 或 `kShort`。 |
| `order_role` | 订单角色，`entry` 或 `exit`。 |
| `action` | 策略动作，例如 `kOpenLong`、`kOpenShort`、`kCloseLong`。 |
| `side` | 订单方向，`kBuy` 或 `kSell`。 |
| `reduce_only` | 订单是否为 reduce-only。 |
| `status` | 状态字段，含义取决于文件：instrument 状态、订单终态或 position 状态。 |
| `finish_reason` | 订单结束原因，例如 `kImmediateOrCancel`。 |
| `reject_reason` | 拒单原因；IOC cancel no-fill 常见为 `kUnknown`。 |
| `request_sequence` | Gate order session 内部请求序号。 |
| `encoded_request_id` | WebSocket payload 中编码后的 request id。 |
| `trigger_exchange_ns` | 触发 BBO 的交易所/行情源时间戳，单位 ns。 |
| `trigger_local_ns` | 触发 BBO 进入本机 data session 的本地时间戳，单位 ns。 |
| `on_book_ticker_entry_ns` | 策略进入 `OnBookTicker()` 的本地时间戳，单位 ns。 |
| `signal_decision_ns` | 策略确认 signal triggered 的本地时间戳，单位 ns。 |
| `lead_exchange_ns` | signal/order 对应 lead 侧最新 BBO 的行情源时间戳。 |
| `lead_local_ns` | signal/order 对应 lead 侧最新 BBO 的本地时间戳。 |
| `signal_lead_id` | signal/order 对应 lead 侧 BookTicker id。 |
| `lead_freshness_ns` | lead BBO freshness，通常为 `signal_decision_ns - lead_exchange_ns`。 |
| `lag_exchange_ns` | signal/order 对应 lag 侧最新 BBO 的行情源时间戳。 |
| `lag_local_ns` | signal/order 对应 lag 侧最新 BBO 的本地时间戳。 |
| `signal_lag_id` | signal/order 对应 lag 侧 BookTicker id。 |
| `lag_freshness_ns` | lag BBO freshness，通常为 `signal_decision_ns - lag_exchange_ns`。 |
| `max_lead_freshness_ns` | 开仓 freshness guard 的 lead 阈值，单位 ns。 |
| `max_lag_freshness_ns` | 开仓 freshness guard 的 lag 阈值，单位 ns。 |
| `freshness_guard_pass` | open order 是否通过 freshness guard。 |
| `freshness_reject_reason` | freshness guard 拒单原因；成功通常为 `-`。 |
| `request_send_local_ns` | 本地发送下单请求成功的时间戳，单位 ns。 |
| `ack_local_receive_ns` | 本地收到 Ack 的时间戳，单位 ns。 |
| `response_local_receive_ns` | 本地收到 order response/result 的时间戳，单位 ns。 |
| `order_finished_local_ns` | 策略层完成订单终态处理的本地时间戳，单位 ns。 |
| `ack_exchange_ns` | 收到 Gate Ack 时BookTicker 的行情源时间戳，单位 ns。 |
| `ack_exchange_request_ingress_ns` | Gate Ack response header `x_in_time` 转 ns。 |
| `ack_exchange_response_egress_ns` | Gate Ack response header `x_out_time` 转 ns。 |
| `ack_exchange_process_ns` | Gate Ack response 交易所侧处理耗时，单位 ns。 |
| `ack_lead_id` | 收到 Gate Ack 时策略看到的 lead BookTicker id。 |
| `ack_lag_id` | 收到 Gate Ack 时策略看到的 lag BookTicker id。 |
| `accepted_lead_id` | 处理 Accepted response 时策略看到的 lead BookTicker id。 |
| `accepted_lag_id` | 处理 Accepted response 时策略看到的 lag BookTicker id。 |
| `partial_filled_lead_id` | 处理 partial fill feedback 时策略看到的 lead BookTicker id。 |
| `partial_filled_lag_id` | 处理 partial fill feedback 时策略看到的 lag BookTicker id。 |
| `filled_lead_id` | 处理 Filled feedback 时策略看到的 lead BookTicker id。 |
| `filled_lag_id` | 处理 Filled feedback 时策略看到的 lag BookTicker id。 |
| `cancelled_lead_id` | 处理 Cancelled feedback 时策略看到的 lead BookTicker id。 |
| `cancelled_lag_id` | 处理 Cancelled feedback 时策略看到的 lag BookTicker id。 |
| `rejected_lead_id` | 处理 rejected feedback 时策略看到的 lead BookTicker id。 |
| `rejected_lag_id` | 处理 rejected feedback 时策略看到的 lag BookTicker id。 |
| `unknown_result_lead_id` | 处理 unknown result 时策略看到的 lead BookTicker id。 |
| `unknown_result_lag_id` | 处理 unknown result 时策略看到的 lag BookTicker id。 |
| `cancel_accepted_lead_id` | 处理 cancel accepted 时策略看到的 lead BookTicker id。 |
| `cancel_accepted_lag_id` | 处理 cancel accepted 时策略看到的 lag BookTicker id。 |
| `cancel_rejected_lead_id` | 处理 cancel rejected 时策略看到的 lead BookTicker id。 |
| `cancel_rejected_lag_id` | 处理 cancel rejected 时策略看到的 lag BookTicker id。 |
| `continuity_lost_lead_id` | 处理 feedback continuity lost 时策略看到的 lead BookTicker id。 |
| `continuity_lost_lag_id` | 处理 feedback continuity lost 时策略看到的 lag BookTicker id。 |
| `response_exchange_ns` | 对应事件的交易所/行情源时间戳，单位 ns。 |
| `accepted_exchange_ns` | 处理 Accepted response 时BookTicker 的行情源时间戳，单位 ns。 |
| `finish_exchange_ns` | 对应事件的交易所/行情源时间戳，单位 ns。 |
| `ack_rtt_ns` | 从本地下发请求到收到 Ack 的 RTT，单位 ns。 |
| `response_rtt_ns` | 对应请求/响应 RTT，单位 ns。 |
| `send_to_ack_local_ns` | 对应事件的本地时间戳，单位 ns。 |
| `send_to_response_local_ns` | 对应事件的本地时间戳，单位 ns。 |
| `send_to_finish_local_ns` | 对应事件的本地时间戳，单位 ns。 |
| `ack_to_finish_local_ns` | 对应事件的本地时间戳，单位 ns。 |
| `bbo_to_strategy_ns` | `on_book_ticker_entry_ns - trigger_local_ns`，单位 ns。 |
| `strategy_to_signal_ns` | `signal_decision_ns - on_book_ticker_entry_ns`，单位 ns。 |
| `signal_to_request_send_ns` | `request_send_local_ns - signal_decision_ns`，单位 ns。 |
| `trigger_to_request_send_ns` | `request_send_local_ns - trigger_local_ns`，单位 ns。 |
| `ack_exchange_to_local_ns` | 对应事件的本地时间戳，单位 ns。 |
| `response_exchange_to_local_ns` | 对应事件的本地时间戳，单位 ns。 |
| `exchange_lifecycle_ns` | 纳秒级时间戳或耗时字段，具体事件由字段名前缀表示。 |
| `order_session_id` | Gate OrderSession 本进程内 session id。 |
| `owner_thread_cpu` | order session owner thread 所在 CPU。 |
| `owner_thread_tid` | order session owner thread 的 Linux tid。 |
| `local_ip` | 本地 TCP endpoint IP。 |
| `local_port` | 本地 TCP endpoint port。 |
| `remote_ip` | 远端 TCP endpoint IP。 |
| `remote_port` | 远端 TCP endpoint port。 |
| `send_cpu` | 发送请求时 owner thread 所在 CPU。 |
| `ack_cpu` | 处理 Ack/response 时 owner thread 所在 CPU。 |
| `diagnostic_cpu` | 输出 latency diagnostic 时 owner thread 所在 CPU。 |
| `tcp_info_available` | 是否成功采集 Linux `TCP_INFO`。 |
| `tcp_info_rtt_us` | `TCP_INFO.tcpi_rtt`，单位 us。 |
| `tcp_info_rttvar_us` | `TCP_INFO.tcpi_rttvar`，单位 us。 |
| `tcp_info_retrans` | `TCP_INFO.tcpi_retrans`。 |
| `tcp_info_total_retrans` | `TCP_INFO.tcpi_total_retrans`。 |
| `tcp_info_unacked` | `TCP_INFO.tcpi_unacked`。 |
| `tcp_info_snd_cwnd` | `TCP_INFO.tcpi_snd_cwnd`。 |
| `latency_diagnostic_reason` | Gate order session Ack latency diagnostic 触发原因；空值表示未触发。 |
| `latency_diagnostic_ack_rtt_ns` | 对应请求/响应 RTT，单位 ns。 |
| `send_to_first_after_hook_ns` | 纳秒级时间戳或耗时字段，具体事件由字段名前缀表示。 |
| `send_to_first_drive_read_ns` | 纳秒级时间戳或耗时字段，具体事件由字段名前缀表示。 |
| `drive_read_duration_ns` | 纳秒级时间戳或耗时字段，具体事件由字段名前缀表示。 |
| `max_observed_drive_read_duration_ns` | 纳秒级时间戳或耗时字段，具体事件由字段名前缀表示。 |
| `latency_diagnostic_inflight_at_send` | 发送该订单后 order session 中的 inflight 请求数量。 |
| `max_runtime_loop_gap_ns` | 纳秒级时间戳或耗时字段，具体事件由字段名前缀表示。 |
| `runtime_loop_iterations_before_ack` | diagnostic window armed 后到 Ack 处理前经过的 runtime loop 迭代次数。 |
| `order_encode_done_ns` | 纳秒级时间戳或耗时字段，具体事件由字段名前缀表示。 |
| `ws_frame_encode_done_ns` | 纳秒级时间戳或耗时字段，具体事件由字段名前缀表示。 |
| `write_enqueue_ns` | 纳秒级时间戳或耗时字段，具体事件由字段名前缀表示。 |
| `drive_write_enter_ns` | 纳秒级时间戳或耗时字段，具体事件由字段名前缀表示。 |
| `write_some_enter_ns` | 纳秒级时间戳或耗时字段，具体事件由字段名前缀表示。 |
| `write_some_return_ns` | 纳秒级时间戳或耗时字段，具体事件由字段名前缀表示。 |
| `write_complete_ns` | 纳秒级时间戳或耗时字段，具体事件由字段名前缀表示。 |
| `write_some_bytes` | 单次 `send()` / TLS write 返回的写入字节数。 |
| `write_complete_bytes` | 当前 request frame 完整写入 transport 的总字节数。 |
| `write_errno` | write syscall / TLS write errno；成功为 `0`。 |
| `write_eagain` | 写路径是否遇到 EAGAIN / would-block。 |
| `pending_write_count_after` | enqueue 或 write 后 pending business write 数量。 |
| `socket_send_queue_available` | 是否成功采集 socket send queue snapshot。 |
| `tcp_sendq_bytes` | Linux socket send queue 中未被远端 ACK 的字节数。 |
| `tcp_notsent_bytes` | 已进入 TCP 发送队列但尚未发送到网络的字节数。 |
| `warnings` | 分析生成时发现的缺失字段或异常标记。 |

## Instrument 表

适用文件：
- `inputs/instrument.csv`

字段数：`23`

| 字段 | 含义 |
|---|---|
| `symbol_id` | instrument symbol id。 |
| `symbol` | 交易 symbol。 |
| `exchange` | 交易所名称或枚举。 |
| `exchange_symbol` | 交易所在 API 中使用的 symbol。 |
| `base_asset` | base asset。 |
| `quote_asset` | quote asset。 |
| `settle_asset` | settlement asset。 |
| `product_type` | 产品类型。 |
| `status` | 状态字段，含义取决于文件：instrument 状态、订单终态或 position 状态。 |
| `contract_type` | 合约类型。 |
| `price_tick` | 该合约价格 tick。 |
| `price_decimal_places` | 价格小数位数。 |
| `quantity_step` | 数量步长。 |
| `quantity_decimal_places` | 数量小数位数。 |
| `min_quantity` | 最小下单数量。 |
| `max_quantity` | 最大下单数量。 |
| `max_market_quantity` | 最大市价单数量。 |
| `min_notional` | 最小名义金额。 |
| `notional_multiplier` | notional 乘数。 |
| `contract_multiplier` | 合约乘数。 |
| `price_limit_up` | instrument catalog 中的上行价格限制；空值表示未提供。 |
| `price_limit_down` | instrument catalog 中的下行价格限制；空值表示未提供。 |
| `market_price_bound` | instrument catalog 中的市价保护边界；空值表示未提供。 |

## 最终 cancel 窗口表

适用文件：
- `analysis/cancel_windows.csv`
- `analysis/ack_full_candidates.csv`
- `analysis/cancel_point_candidates.csv`

这些文件共享同一列 schema；候选表只是主表的行过滤子集。

字段数：`58`

| 字段 | 含义 |
|---|---|
| `local_order_id` | 策略本地订单 id。 |
| `request_sequence` | Gate order session 内部请求序号。 |
| `side` | 订单方向，`kBuy` 或 `kSell`。 |
| `action` | 策略动作，例如 `kOpenLong`、`kOpenShort`、`kCloseLong`。 |
| `order_price` | 实际提交到交易所的 limit price。 |
| `quantity` | 订单数量，单位为合约张数。 |
| `price_tick` | 该合约价格 tick。 |
| `has_accepted_lag_id` | 订单是否有非空 `accepted_lag_id`。 |
| `signal_lag_id` | signal/order 对应 lag 侧 BookTicker id。 |
| `ack_lag_id` | 收到 Gate Ack 时策略看到的 lag BookTicker id。 |
| `accepted_lag_id` | 处理 Accepted response 时策略看到的 lag BookTicker id。 |
| `cancelled_lag_id` | 处理 Cancelled feedback 时策略看到的 lag BookTicker id。 |
| `request_send_local_ns` | 本地发送下单请求成功的时间戳，单位 ns。 |
| `ack_local_receive_ns` | 本地收到 Ack 的时间戳，单位 ns。 |
| `order_finished_local_ns` | 策略层完成订单终态处理的本地时间戳，单位 ns。 |
| `ack_rtt_ns` | 从本地下发请求到收到 Ack 的 RTT，单位 ns。 |
| `send_to_finish_us` | `order_finished_local_ns - request_send_local_ns`，单位 us。 |
| `ack_to_finish_us` | `order_finished_local_ns - ack_local_receive_ns`，单位 us。 |
| `send_to_finish_start_state_found` | 在 request_send 时刻之前是否存在可作为最新状态的 BookTicker。 |
| `send_to_finish_start_state_id` | request_send 时刻最新 BookTicker 状态的 id。 |
| `send_to_finish_start_state_local_ns` | request_send 时刻最新 BookTicker 状态的本地时间戳。 |
| `send_to_finish_start_state_age_ns` | request_send 时刻最新 BookTicker 状态相对 request_send 的年龄，单位 ns。 |
| `send_to_finish_start_state_any` | request_send 时刻最新 BBO 状态是否满足 `any`。 |
| `send_to_finish_start_state_full` | request_send 时刻最新 BBO 状态是否满足 `full`。 |
| `send_to_finish_start_state_contra_price` | request_send 时刻最新 BBO 状态中的对手一档价格。 |
| `send_to_finish_start_state_contra_volume` | request_send 时刻最新 BBO 状态中的对手一档数量。 |
| `send_to_finish_records` | `send_to_finish` 窗口内 BookTicker 记录数。 |
| `send_to_finish_any_records` | `send_to_finish` 窗口内满足 `any` 的 BookTicker 记录数。 |
| `send_to_finish_full_records` | `send_to_finish` 窗口内满足 `full` 的 BookTicker 记录数。 |
| `send_to_finish_first_any_id` | `send_to_finish` 窗口内第一条满足 `any` 的 BookTicker id。 |
| `send_to_finish_first_any_local_ns` | `send_to_finish` 窗口内第一条满足 `any` 的本地时间戳。 |
| `send_to_finish_first_full_id` | `send_to_finish` 窗口内第一条满足 `full` 的 BookTicker id。 |
| `send_to_finish_first_full_local_ns` | `send_to_finish` 窗口内第一条满足 `full` 的本地时间戳。 |
| `send_to_finish_best_contra_price` | `send_to_finish` 窗口内最有利的对手一档价格；买单取最低 ask，卖单取最高 bid。 |
| `send_to_finish_best_margin_ticks` | `send_to_finish` 窗口内最有利对手价相对 order price 的 tick 余量。 |
| `send_to_finish_max_marketable_volume` | `send_to_finish` 窗口内满足 `any` 的记录中最大对手一档量。 |
| `send_to_finish_stateful_any` | request_send stateful 口径是否满足 `any`，包含 start_state 和窗口内更新。 |
| `send_to_finish_stateful_full` | request_send stateful 口径是否满足 `full`，包含 start_state 和窗口内更新。 |
| `send_to_finish_stateful_first_any_source` | stateful 口径第一次满足 `any` 的来源，`start_state` 或 `update`。 |
| `send_to_finish_stateful_first_full_source` | stateful 口径第一次满足 `full` 的来源，`start_state` 或 `update`。 |
| `after_ack_to_cancel_records` | `after_ack_to_cancel` 窗口内 BookTicker 记录数。 |
| `after_ack_to_cancel_any_records` | `after_ack_to_cancel` 窗口内满足 `any` 的 BookTicker 记录数。 |
| `after_ack_to_cancel_full_records` | `after_ack_to_cancel` 窗口内满足 `full` 的 BookTicker 记录数。 |
| `after_ack_to_cancel_first_any_id` | `after_ack_to_cancel` 窗口内第一条满足 `any` 的 BookTicker id。 |
| `after_ack_to_cancel_first_any_local_ns` | `after_ack_to_cancel` 窗口内第一条满足 `any` 的本地时间戳。 |
| `after_ack_to_cancel_first_full_id` | `after_ack_to_cancel` 窗口内第一条满足 `full` 的 BookTicker id。 |
| `after_ack_to_cancel_first_full_local_ns` | `after_ack_to_cancel` 窗口内第一条满足 `full` 的本地时间戳。 |
| `after_ack_to_cancel_best_contra_price` | `after_ack_to_cancel` 窗口内最有利的对手一档价格；买单取最低 ask，卖单取最高 bid。 |
| `after_ack_to_cancel_best_margin_ticks` | `after_ack_to_cancel` 窗口内最有利对手价相对 order price 的 tick 余量。 |
| `after_ack_to_cancel_max_marketable_volume` | `after_ack_to_cancel` 窗口内满足 `any` 的记录中最大对手一档量。 |
| `cancel_point_found` | 是否在 BookTicker split 中找到 `cancelled_lag_id` 对应记录。 |
| `cancel_point_id` | cancel point 使用的 BookTicker id，即 `cancelled_lag_id`。 |
| `cancel_point_local_ns` | 对应事件的本地时间戳，单位 ns。 |
| `cancel_point_contra_price` | cancel point 的订单对手一档价格。 |
| `cancel_point_contra_volume` | cancel point 的订单对手一档数量。 |
| `cancel_point_any` | cancel point 是否满足 `any`。 |
| `cancel_point_full` | cancel point 是否满足 `full`。 |
| `cancel_point_margin_ticks` | cancel point 对手价相对 order price 的 tick 余量。 |

## 早期 fillability 表

适用文件：
- `analysis/cancel_fillability.csv`
- `analysis/filled_control.csv`

这些文件共享同一列 schema；候选表只是主表的行过滤子集。

字段数：`185`

| 字段 | 含义 |
|---|---|
| `run_id` | 本次运行标识。 |
| `symbol` | 交易 symbol。 |
| `local_order_id` | 策略本地订单 id。 |
| `text_order_id` | Gate text order id，当前通常为 `t-<local_order_id>`。 |
| `request_sequence` | Gate order session 内部请求序号。 |
| `order_role` | 订单角色，`entry` 或 `exit`。 |
| `action` | 策略动作，例如 `kOpenLong`、`kOpenShort`、`kCloseLong`。 |
| `side` | 订单方向，`kBuy` 或 `kSell`。 |
| `reduce_only` | 订单是否为 reduce-only。 |
| `time_in_force` | 订单 TIF；本分析主样本为 IOC / `kImmediateOrCancel`。 |
| `status` | 状态字段，含义取决于文件：instrument 状态、订单终态或 position 状态。 |
| `finish_reason` | 订单结束原因，例如 `kImmediateOrCancel`。 |
| `raw_price` | 策略信号使用的原始 lag 价格。 |
| `order_price` | 实际提交到交易所的 limit price。 |
| `price_tick` | 该合约价格 tick。 |
| `order_offset_ticks` | `order_price` 相对 `raw_price` 的实际偏移 tick。 |
| `quantity` | 订单数量，单位为合约张数。 |
| `cumulative_filled_quantity` | 累计成交数量。 |
| `left_quantity` | 剩余未成交数量。 |
| `cancelled_quantity` | 取消数量。 |
| `fill_rate` | 成交比例，`cumulative_filled_quantity / quantity`。 |
| `average_fill_price` | 平均成交价。 |
| `last_fill_price` | 最近一次成交价。 |
| `request_send_local_ns` | 本地发送下单请求成功的时间戳，单位 ns。 |
| `ack_local_receive_ns` | 本地收到 Ack 的时间戳，单位 ns。 |
| `order_finished_local_ns` | 策略层完成订单终态处理的本地时间戳，单位 ns。 |
| `signal_lag_id` | signal/order 对应 lag 侧 BookTicker id。 |
| `ack_lag_id` | 收到 Gate Ack 时策略看到的 lag BookTicker id。 |
| `accepted_lag_id` | 处理 Accepted response 时策略看到的 lag BookTicker id。 |
| `cancelled_lag_id` | 处理 Cancelled feedback 时策略看到的 lag BookTicker id。 |
| `filled_lag_id` | 处理 Filled feedback 时策略看到的 lag BookTicker id。 |
| `unknown_result_lag_id` | 处理 unknown result 时策略看到的 lag BookTicker id。 |
| `terminal_stage` | 该订单用于分析的终态 stage，例如 `cancelled` 或 `filled`。 |
| `terminal_lag_id` | 终态 stage 对应 lag BookTicker id。 |
| `category` | 分析脚本给出的可成交性分类。 |
| `signal_found` | 是否在 symbol BookTicker split 中找到signal 触发时对应的记录。 |
| `signal_exchange_ns` | signal 触发时BookTicker 的行情源时间戳，单位 ns。 |
| `signal_local_ns` | signal 触发时BookTicker 的本地时间戳，单位 ns。 |
| `signal_bid_price` | signal 触发时BBO bid price。 |
| `signal_bid_volume` | signal 触发时BBO bid volume。 |
| `signal_ask_price` | signal 触发时BBO ask price。 |
| `signal_ask_volume` | signal 触发时BBO ask volume。 |
| `signal_contra_price` | signal 触发时订单对手一档价格；买单取 ask，卖单取 bid。 |
| `signal_contra_volume` | signal 触发时订单对手一档数量；买单取 ask_volume，卖单取 bid_volume。 |
| `signal_marketable` | signal 触发时是否价格穿越且对手一档量大于 0。 |
| `signal_margin_ticks` | signal 触发时对手价相对 order price 的可成交余量，单位 tick；正值表示穿越。 |
| `ack_found` | 是否在 symbol BookTicker split 中找到收到 Gate Ack 时对应的记录。 |
| `ack_exchange_ns` | 收到 Gate Ack 时BookTicker 的行情源时间戳，单位 ns。 |
| `ack_local_ns` | 收到 Gate Ack 时BookTicker 的本地时间戳，单位 ns。 |
| `ack_bid_price` | 收到 Gate Ack 时BBO bid price。 |
| `ack_bid_volume` | 收到 Gate Ack 时BBO bid volume。 |
| `ack_ask_price` | 收到 Gate Ack 时BBO ask price。 |
| `ack_ask_volume` | 收到 Gate Ack 时BBO ask volume。 |
| `ack_contra_price` | 收到 Gate Ack 时订单对手一档价格；买单取 ask，卖单取 bid。 |
| `ack_contra_volume` | 收到 Gate Ack 时订单对手一档数量；买单取 ask_volume，卖单取 bid_volume。 |
| `ack_marketable` | 收到 Gate Ack 时是否价格穿越且对手一档量大于 0。 |
| `ack_margin_ticks` | 收到 Gate Ack 时对手价相对 order price 的可成交余量，单位 tick；正值表示穿越。 |
| `accepted_found` | 是否在 symbol BookTicker split 中找到处理 Accepted response 时对应的记录。 |
| `accepted_exchange_ns` | 处理 Accepted response 时BookTicker 的行情源时间戳，单位 ns。 |
| `accepted_local_ns` | 处理 Accepted response 时BookTicker 的本地时间戳，单位 ns。 |
| `accepted_bid_price` | 处理 Accepted response 时BBO bid price。 |
| `accepted_bid_volume` | 处理 Accepted response 时BBO bid volume。 |
| `accepted_ask_price` | 处理 Accepted response 时BBO ask price。 |
| `accepted_ask_volume` | 处理 Accepted response 时BBO ask volume。 |
| `accepted_contra_price` | 处理 Accepted response 时订单对手一档价格；买单取 ask，卖单取 bid。 |
| `accepted_contra_volume` | 处理 Accepted response 时订单对手一档数量；买单取 ask_volume，卖单取 bid_volume。 |
| `accepted_marketable` | 处理 Accepted response 时是否价格穿越且对手一档量大于 0。 |
| `accepted_margin_ticks` | 处理 Accepted response 时对手价相对 order price 的可成交余量，单位 tick；正值表示穿越。 |
| `cancelled_found` | 是否在 symbol BookTicker split 中找到处理 Cancelled feedback 时对应的记录。 |
| `cancelled_exchange_ns` | 处理 Cancelled feedback 时BookTicker 的行情源时间戳，单位 ns。 |
| `cancelled_local_ns` | 处理 Cancelled feedback 时BookTicker 的本地时间戳，单位 ns。 |
| `cancelled_bid_price` | 处理 Cancelled feedback 时BBO bid price。 |
| `cancelled_bid_volume` | 处理 Cancelled feedback 时BBO bid volume。 |
| `cancelled_ask_price` | 处理 Cancelled feedback 时BBO ask price。 |
| `cancelled_ask_volume` | 处理 Cancelled feedback 时BBO ask volume。 |
| `cancelled_contra_price` | 处理 Cancelled feedback 时订单对手一档价格；买单取 ask，卖单取 bid。 |
| `cancelled_contra_volume` | 处理 Cancelled feedback 时订单对手一档数量；买单取 ask_volume，卖单取 bid_volume。 |
| `cancelled_marketable` | 处理 Cancelled feedback 时是否价格穿越且对手一档量大于 0。 |
| `cancelled_margin_ticks` | 处理 Cancelled feedback 时对手价相对 order price 的可成交余量，单位 tick；正值表示穿越。 |
| `filled_found` | 是否在 symbol BookTicker split 中找到处理 Filled feedback 时对应的记录。 |
| `filled_exchange_ns` | 处理 Filled feedback 时BookTicker 的行情源时间戳，单位 ns。 |
| `filled_local_ns` | 处理 Filled feedback 时BookTicker 的本地时间戳，单位 ns。 |
| `filled_bid_price` | 处理 Filled feedback 时BBO bid price。 |
| `filled_bid_volume` | 处理 Filled feedback 时BBO bid volume。 |
| `filled_ask_price` | 处理 Filled feedback 时BBO ask price。 |
| `filled_ask_volume` | 处理 Filled feedback 时BBO ask volume。 |
| `filled_contra_price` | 处理 Filled feedback 时订单对手一档价格；买单取 ask，卖单取 bid。 |
| `filled_contra_volume` | 处理 Filled feedback 时订单对手一档数量；买单取 ask_volume，卖单取 bid_volume。 |
| `filled_marketable` | 处理 Filled feedback 时是否价格穿越且对手一档量大于 0。 |
| `filled_margin_ticks` | 处理 Filled feedback 时对手价相对 order price 的可成交余量，单位 tick；正值表示穿越。 |
| `terminal_found` | 是否在 symbol BookTicker split 中找到订单终态 stage对应的记录。 |
| `terminal_exchange_ns` | 订单终态 stageBookTicker 的行情源时间戳，单位 ns。 |
| `terminal_local_ns` | 订单终态 stageBookTicker 的本地时间戳，单位 ns。 |
| `terminal_bid_price` | 订单终态 stageBBO bid price。 |
| `terminal_bid_volume` | 订单终态 stageBBO bid volume。 |
| `terminal_ask_price` | 订单终态 stageBBO ask price。 |
| `terminal_ask_volume` | 订单终态 stageBBO ask volume。 |
| `terminal_contra_price` | 订单终态 stage订单对手一档价格；买单取 ask，卖单取 bid。 |
| `terminal_contra_volume` | 订单终态 stage订单对手一档数量；买单取 ask_volume，卖单取 bid_volume。 |
| `terminal_marketable` | 订单终态 stage是否价格穿越且对手一档量大于 0。 |
| `terminal_margin_ticks` | 订单终态 stage对手价相对 order price 的可成交余量，单位 tick；正值表示穿越。 |
| `signal_to_ack_start_id` | `signal_to_ack` 窗口起始 BookTicker id。 |
| `signal_to_ack_end_id` | `signal_to_ack` 窗口结束 BookTicker id。 |
| `signal_to_ack_start_exclusive` | `signal_to_ack` 窗口是否排除起始 id。 |
| `signal_to_ack_valid` | `signal_to_ack` 窗口是否可计算。 |
| `signal_to_ack_records` | `signal_to_ack` 窗口内 BookTicker 记录数。 |
| `signal_to_ack_marketable_records` | `signal_to_ack` 窗口内满足 `any` 的 BookTicker 记录数。 |
| `signal_to_ack_first_marketable_id` | `signal_to_ack` 窗口内第一条满足 `any` 的 BookTicker id。 |
| `signal_to_ack_first_marketable_local_ns` | `signal_to_ack` 窗口内第一条满足 `any` 的本地时间戳。 |
| `signal_to_ack_first_marketable_contra_price` | `signal_to_ack` 窗口内第一条满足 `any` 的对手价。 |
| `signal_to_ack_last_marketable_id` | `signal_to_ack` 窗口内最后一条满足 `any` 的 BookTicker id。 |
| `signal_to_ack_last_marketable_local_ns` | `signal_to_ack` 窗口内最后一条满足 `any` 的本地时间戳。 |
| `signal_to_ack_last_marketable_contra_price` | `signal_to_ack` 窗口内最后一条满足 `any` 的对手价。 |
| `signal_to_ack_best_contra_price` | `signal_to_ack` 窗口内最有利的对手一档价格；买单取最低 ask，卖单取最高 bid。 |
| `signal_to_ack_best_margin_ticks` | `signal_to_ack` 窗口内最有利对手价相对 order price 的 tick 余量。 |
| `signal_to_terminal_start_id` | `signal_to_terminal` 窗口起始 BookTicker id。 |
| `signal_to_terminal_end_id` | `signal_to_terminal` 窗口结束 BookTicker id。 |
| `signal_to_terminal_start_exclusive` | `signal_to_terminal` 窗口是否排除起始 id。 |
| `signal_to_terminal_valid` | `signal_to_terminal` 窗口是否可计算。 |
| `signal_to_terminal_records` | `signal_to_terminal` 窗口内 BookTicker 记录数。 |
| `signal_to_terminal_marketable_records` | `signal_to_terminal` 窗口内满足 `any` 的 BookTicker 记录数。 |
| `signal_to_terminal_first_marketable_id` | `signal_to_terminal` 窗口内第一条满足 `any` 的 BookTicker id。 |
| `signal_to_terminal_first_marketable_local_ns` | `signal_to_terminal` 窗口内第一条满足 `any` 的本地时间戳。 |
| `signal_to_terminal_first_marketable_contra_price` | `signal_to_terminal` 窗口内第一条满足 `any` 的对手价。 |
| `signal_to_terminal_last_marketable_id` | `signal_to_terminal` 窗口内最后一条满足 `any` 的 BookTicker id。 |
| `signal_to_terminal_last_marketable_local_ns` | `signal_to_terminal` 窗口内最后一条满足 `any` 的本地时间戳。 |
| `signal_to_terminal_last_marketable_contra_price` | `signal_to_terminal` 窗口内最后一条满足 `any` 的对手价。 |
| `signal_to_terminal_best_contra_price` | `signal_to_terminal` 窗口内最有利的对手一档价格；买单取最低 ask，卖单取最高 bid。 |
| `signal_to_terminal_best_margin_ticks` | `signal_to_terminal` 窗口内最有利对手价相对 order price 的 tick 余量。 |
| `ack_to_terminal_start_id` | `ack_to_terminal` 窗口起始 BookTicker id。 |
| `ack_to_terminal_end_id` | `ack_to_terminal` 窗口结束 BookTicker id。 |
| `ack_to_terminal_start_exclusive` | `ack_to_terminal` 窗口是否排除起始 id。 |
| `ack_to_terminal_valid` | `ack_to_terminal` 窗口是否可计算。 |
| `ack_to_terminal_records` | `ack_to_terminal` 窗口内 BookTicker 记录数。 |
| `ack_to_terminal_marketable_records` | `ack_to_terminal` 窗口内满足 `any` 的 BookTicker 记录数。 |
| `ack_to_terminal_first_marketable_id` | `ack_to_terminal` 窗口内第一条满足 `any` 的 BookTicker id。 |
| `ack_to_terminal_first_marketable_local_ns` | `ack_to_terminal` 窗口内第一条满足 `any` 的本地时间戳。 |
| `ack_to_terminal_first_marketable_contra_price` | `ack_to_terminal` 窗口内第一条满足 `any` 的对手价。 |
| `ack_to_terminal_last_marketable_id` | `ack_to_terminal` 窗口内最后一条满足 `any` 的 BookTicker id。 |
| `ack_to_terminal_last_marketable_local_ns` | `ack_to_terminal` 窗口内最后一条满足 `any` 的本地时间戳。 |
| `ack_to_terminal_last_marketable_contra_price` | `ack_to_terminal` 窗口内最后一条满足 `any` 的对手价。 |
| `ack_to_terminal_best_contra_price` | `ack_to_terminal` 窗口内最有利的对手一档价格；买单取最低 ask，卖单取最高 bid。 |
| `ack_to_terminal_best_margin_ticks` | `ack_to_terminal` 窗口内最有利对手价相对 order price 的 tick 余量。 |
| `after_ack_to_terminal_start_id` | `after_ack_to_terminal` 窗口起始 BookTicker id。 |
| `after_ack_to_terminal_end_id` | `after_ack_to_terminal` 窗口结束 BookTicker id。 |
| `after_ack_to_terminal_start_exclusive` | `after_ack_to_terminal` 窗口是否排除起始 id。 |
| `after_ack_to_terminal_valid` | `after_ack_to_terminal` 窗口是否可计算。 |
| `after_ack_to_terminal_records` | `after_ack_to_terminal` 窗口内 BookTicker 记录数。 |
| `after_ack_to_terminal_marketable_records` | `after_ack_to_terminal` 窗口内满足 `any` 的 BookTicker 记录数。 |
| `after_ack_to_terminal_first_marketable_id` | `after_ack_to_terminal` 窗口内第一条满足 `any` 的 BookTicker id。 |
| `after_ack_to_terminal_first_marketable_local_ns` | `after_ack_to_terminal` 窗口内第一条满足 `any` 的本地时间戳。 |
| `after_ack_to_terminal_first_marketable_contra_price` | `after_ack_to_terminal` 窗口内第一条满足 `any` 的对手价。 |
| `after_ack_to_terminal_last_marketable_id` | `after_ack_to_terminal` 窗口内最后一条满足 `any` 的 BookTicker id。 |
| `after_ack_to_terminal_last_marketable_local_ns` | `after_ack_to_terminal` 窗口内最后一条满足 `any` 的本地时间戳。 |
| `after_ack_to_terminal_last_marketable_contra_price` | `after_ack_to_terminal` 窗口内最后一条满足 `any` 的对手价。 |
| `after_ack_to_terminal_best_contra_price` | `after_ack_to_terminal` 窗口内最有利的对手一档价格；买单取最低 ask，卖单取最高 bid。 |
| `after_ack_to_terminal_best_margin_ticks` | `after_ack_to_terminal` 窗口内最有利对手价相对 order price 的 tick 余量。 |
| `accepted_to_terminal_start_id` | `accepted_to_terminal` 窗口起始 BookTicker id。 |
| `accepted_to_terminal_end_id` | `accepted_to_terminal` 窗口结束 BookTicker id。 |
| `accepted_to_terminal_start_exclusive` | `accepted_to_terminal` 窗口是否排除起始 id。 |
| `accepted_to_terminal_valid` | `accepted_to_terminal` 窗口是否可计算。 |
| `accepted_to_terminal_records` | `accepted_to_terminal` 窗口内 BookTicker 记录数。 |
| `accepted_to_terminal_marketable_records` | `accepted_to_terminal` 窗口内满足 `any` 的 BookTicker 记录数。 |
| `accepted_to_terminal_first_marketable_id` | `accepted_to_terminal` 窗口内第一条满足 `any` 的 BookTicker id。 |
| `accepted_to_terminal_first_marketable_local_ns` | `accepted_to_terminal` 窗口内第一条满足 `any` 的本地时间戳。 |
| `accepted_to_terminal_first_marketable_contra_price` | `accepted_to_terminal` 窗口内第一条满足 `any` 的对手价。 |
| `accepted_to_terminal_last_marketable_id` | `accepted_to_terminal` 窗口内最后一条满足 `any` 的 BookTicker id。 |
| `accepted_to_terminal_last_marketable_local_ns` | `accepted_to_terminal` 窗口内最后一条满足 `any` 的本地时间戳。 |
| `accepted_to_terminal_last_marketable_contra_price` | `accepted_to_terminal` 窗口内最后一条满足 `any` 的对手价。 |
| `accepted_to_terminal_best_contra_price` | `accepted_to_terminal` 窗口内最有利的对手一档价格；买单取最低 ask，卖单取最高 bid。 |
| `accepted_to_terminal_best_margin_ticks` | `accepted_to_terminal` 窗口内最有利对手价相对 order price 的 tick 余量。 |
| `after_accepted_to_terminal_start_id` | `after_accepted_to_terminal` 窗口起始 BookTicker id。 |
| `after_accepted_to_terminal_end_id` | `after_accepted_to_terminal` 窗口结束 BookTicker id。 |
| `after_accepted_to_terminal_start_exclusive` | `after_accepted_to_terminal` 窗口是否排除起始 id。 |
| `after_accepted_to_terminal_valid` | `after_accepted_to_terminal` 窗口是否可计算。 |
| `after_accepted_to_terminal_records` | `after_accepted_to_terminal` 窗口内 BookTicker 记录数。 |
| `after_accepted_to_terminal_marketable_records` | `after_accepted_to_terminal` 窗口内满足 `any` 的 BookTicker 记录数。 |
| `after_accepted_to_terminal_first_marketable_id` | `after_accepted_to_terminal` 窗口内第一条满足 `any` 的 BookTicker id。 |
| `after_accepted_to_terminal_first_marketable_local_ns` | `after_accepted_to_terminal` 窗口内第一条满足 `any` 的本地时间戳。 |
| `after_accepted_to_terminal_first_marketable_contra_price` | `after_accepted_to_terminal` 窗口内第一条满足 `any` 的对手价。 |
| `after_accepted_to_terminal_last_marketable_id` | `after_accepted_to_terminal` 窗口内最后一条满足 `any` 的 BookTicker id。 |
| `after_accepted_to_terminal_last_marketable_local_ns` | `after_accepted_to_terminal` 窗口内最后一条满足 `any` 的本地时间戳。 |
| `after_accepted_to_terminal_last_marketable_contra_price` | `after_accepted_to_terminal` 窗口内最后一条满足 `any` 的对手价。 |
| `after_accepted_to_terminal_best_contra_price` | `after_accepted_to_terminal` 窗口内最有利的对手一档价格；买单取最低 ask，卖单取最高 bid。 |
| `after_accepted_to_terminal_best_margin_ticks` | `after_accepted_to_terminal` 窗口内最有利对手价相对 order price 的 tick 余量。 |

## source 聚合表

适用文件：
- `analysis/source_by_order.csv`

字段数：`48`

| 字段 | 含义 |
|---|---|
| `local_order_id` | 策略本地订单 id。 |
| `request_sequence` | Gate order session 内部请求序号。 |
| `side` | 订单方向，`kBuy` 或 `kSell`。 |
| `action` | 策略动作，例如 `kOpenLong`、`kOpenShort`、`kCloseLong`。 |
| `order_price` | 实际提交到交易所的 limit price。 |
| `signal_lag_id` | signal/order 对应 lag 侧 BookTicker id。 |
| `ack_lag_id` | 收到 Gate Ack 时策略看到的 lag BookTicker id。 |
| `accepted_lag_id` | 处理 Accepted response 时策略看到的 lag BookTicker id。 |
| `cancelled_lag_id` | 处理 Cancelled feedback 时策略看到的 lag BookTicker id。 |
| `canonical_category` | canonical 视角下的可成交性分类。 |
| `canonical_signal_marketable` | canonical BookTicker 视角下的对应 stage/window 分析字段。 |
| `canonical_ack_marketable` | canonical BookTicker 视角下的对应 stage/window 分析字段。 |
| `canonical_accepted_marketable` | canonical BookTicker 视角下的对应 stage/window 分析字段。 |
| `canonical_cancelled_marketable` | canonical BookTicker 视角下的对应 stage/window 分析字段。 |
| `canonical_after_ack_market_records` | canonical 中 ack 后到 cancel/terminal 的价格穿越记录数。 |
| `source_count` | 参与聚合的 source 数量。 |
| `source_signal_found_count` | source0..3 中找到对应 stage BookTicker 的 source 数量。 |
| `source_ack_found_count` | source0..3 中找到对应 stage BookTicker 的 source 数量。 |
| `source_accepted_found_count` | source0..3 中找到对应 stage BookTicker 的 source 数量。 |
| `source_cancelled_found_count` | source0..3 中找到对应 stage BookTicker 的 source 数量。 |
| `source_signal_marketable_count` | source0..3 中对应 stage/window 满足 `any` 的 source 数量。 |
| `source_ack_marketable_count` | source0..3 中对应 stage/window 满足 `any` 的 source 数量。 |
| `source_accepted_marketable_count` | source0..3 中对应 stage/window 满足 `any` 的 source 数量。 |
| `source_cancelled_marketable_count` | source0..3 中对应 stage/window 满足 `any` 的 source 数量。 |
| `source_after_ack_marketable_count` | source0..3 中对应 stage/window 满足 `any` 的 source 数量。 |
| `source_after_accepted_marketable_count` | source0..3 中对应 stage/window 满足 `any` 的 source 数量。 |
| `source_names_cancelled_marketable` | cancelled 点位可成交的 source 名称列表，用 `;` 分隔。 |
| `source_names_after_ack_marketable` | ack 后窗口内可成交的 source 名称列表，用 `;` 分隔。 |
| `source0_ack_marketable` | `source0` 在 ack 点位是否满足 `any`。 |
| `source0_accepted_marketable` | `source0` 在 accepted 点位是否满足 `any`。 |
| `source0_cancelled_marketable` | `source0` 在 cancelled 点位是否满足 `any`。 |
| `source0_after_ack_market_records` | `source0` 在 ack 后到 cancelled 窗口内满足 `any` 的记录数。 |
| `source0_after_accepted_market_records` | `source0` 在 accepted 后到 cancelled 窗口内满足 `any` 的记录数。 |
| `source1_ack_marketable` | `source1` 在 ack 点位是否满足 `any`。 |
| `source1_accepted_marketable` | `source1` 在 accepted 点位是否满足 `any`。 |
| `source1_cancelled_marketable` | `source1` 在 cancelled 点位是否满足 `any`。 |
| `source1_after_ack_market_records` | `source1` 在 ack 后到 cancelled 窗口内满足 `any` 的记录数。 |
| `source1_after_accepted_market_records` | `source1` 在 accepted 后到 cancelled 窗口内满足 `any` 的记录数。 |
| `source2_ack_marketable` | `source2` 在 ack 点位是否满足 `any`。 |
| `source2_accepted_marketable` | `source2` 在 accepted 点位是否满足 `any`。 |
| `source2_cancelled_marketable` | `source2` 在 cancelled 点位是否满足 `any`。 |
| `source2_after_ack_market_records` | `source2` 在 ack 后到 cancelled 窗口内满足 `any` 的记录数。 |
| `source2_after_accepted_market_records` | `source2` 在 accepted 后到 cancelled 窗口内满足 `any` 的记录数。 |
| `source3_ack_marketable` | `source3` 在 ack 点位是否满足 `any`。 |
| `source3_accepted_marketable` | `source3` 在 accepted 点位是否满足 `any`。 |
| `source3_cancelled_marketable` | `source3` 在 cancelled 点位是否满足 `any`。 |
| `source3_after_ack_market_records` | `source3` 在 ack 后到 cancelled 窗口内满足 `any` 的记录数。 |
| `source3_after_accepted_market_records` | `source3` 在 accepted 后到 cancelled 窗口内满足 `any` 的记录数。 |

## source 长表

适用文件：
- `analysis/source_long.csv`

字段数：`62`

| 字段 | 含义 |
|---|---|
| `local_order_id` | 策略本地订单 id。 |
| `request_sequence` | Gate order session 内部请求序号。 |
| `side` | 订单方向，`kBuy` 或 `kSell`。 |
| `action` | 策略动作，例如 `kOpenLong`、`kOpenShort`、`kCloseLong`。 |
| `order_price` | 实际提交到交易所的 limit price。 |
| `price_tick` | 该合约价格 tick。 |
| `signal_lag_id` | signal/order 对应 lag 侧 BookTicker id。 |
| `ack_lag_id` | 收到 Gate Ack 时策略看到的 lag BookTicker id。 |
| `accepted_lag_id` | 处理 Accepted response 时策略看到的 lag BookTicker id。 |
| `cancelled_lag_id` | 处理 Cancelled feedback 时策略看到的 lag BookTicker id。 |
| `canonical_category` | canonical 视角下的可成交性分类。 |
| `canonical_cancelled_marketable` | canonical BookTicker 视角下的对应 stage/window 分析字段。 |
| `canonical_after_ack_market_records` | canonical 中 ack 后到 cancel/terminal 的价格穿越记录数。 |
| `feed` | 用于 source crosscheck 的行情源：`canonical`、`source0`..`source3`。 |
| `signal_id` | signal 触发时用于分析的 BookTicker id。 |
| `signal_found` | 是否在 symbol BookTicker split 中找到signal 触发时对应的记录。 |
| `signal_local_ns` | signal 触发时BookTicker 的本地时间戳，单位 ns。 |
| `signal_contra_price` | signal 触发时订单对手一档价格；买单取 ask，卖单取 bid。 |
| `signal_marketable` | signal 触发时是否价格穿越且对手一档量大于 0。 |
| `signal_margin_ticks` | signal 触发时对手价相对 order price 的可成交余量，单位 tick；正值表示穿越。 |
| `ack_id` | 收到 Gate Ack 时用于分析的 BookTicker id。 |
| `ack_found` | 是否在 symbol BookTicker split 中找到收到 Gate Ack 时对应的记录。 |
| `ack_local_ns` | 收到 Gate Ack 时BookTicker 的本地时间戳，单位 ns。 |
| `ack_contra_price` | 收到 Gate Ack 时订单对手一档价格；买单取 ask，卖单取 bid。 |
| `ack_marketable` | 收到 Gate Ack 时是否价格穿越且对手一档量大于 0。 |
| `ack_margin_ticks` | 收到 Gate Ack 时对手价相对 order price 的可成交余量，单位 tick；正值表示穿越。 |
| `accepted_id` | 处理 Accepted response 时用于分析的 BookTicker id。 |
| `accepted_found` | 是否在 symbol BookTicker split 中找到处理 Accepted response 时对应的记录。 |
| `accepted_local_ns` | 处理 Accepted response 时BookTicker 的本地时间戳，单位 ns。 |
| `accepted_contra_price` | 处理 Accepted response 时订单对手一档价格；买单取 ask，卖单取 bid。 |
| `accepted_marketable` | 处理 Accepted response 时是否价格穿越且对手一档量大于 0。 |
| `accepted_margin_ticks` | 处理 Accepted response 时对手价相对 order price 的可成交余量，单位 tick；正值表示穿越。 |
| `cancelled_id` | 处理 Cancelled feedback 时用于分析的 BookTicker id。 |
| `cancelled_found` | 是否在 symbol BookTicker split 中找到处理 Cancelled feedback 时对应的记录。 |
| `cancelled_local_ns` | 处理 Cancelled feedback 时BookTicker 的本地时间戳，单位 ns。 |
| `cancelled_contra_price` | 处理 Cancelled feedback 时订单对手一档价格；买单取 ask，卖单取 bid。 |
| `cancelled_marketable` | 处理 Cancelled feedback 时是否价格穿越且对手一档量大于 0。 |
| `cancelled_margin_ticks` | 处理 Cancelled feedback 时对手价相对 order price 的可成交余量，单位 tick；正值表示穿越。 |
| `signal_to_ack_valid` | `signal_to_ack` 窗口是否可计算。 |
| `signal_to_ack_records` | `signal_to_ack` 窗口内 BookTicker 记录数。 |
| `signal_to_ack_marketable_records` | `signal_to_ack` 窗口内满足 `any` 的 BookTicker 记录数。 |
| `signal_to_ack_first_marketable_id` | `signal_to_ack` 窗口内第一条满足 `any` 的 BookTicker id。 |
| `signal_to_ack_last_marketable_id` | `signal_to_ack` 窗口内最后一条满足 `any` 的 BookTicker id。 |
| `signal_to_ack_best_margin_ticks` | `signal_to_ack` 窗口内最有利对手价相对 order price 的 tick 余量。 |
| `after_ack_to_cancelled_valid` | `(ack_lag_id, cancelled_lag_id]` 窗口是否可计算。 |
| `after_ack_to_cancelled_records` | `(ack_lag_id, cancelled_lag_id]` 窗口内 BookTicker 记录数。 |
| `after_ack_to_cancelled_marketable_records` | `(ack_lag_id, cancelled_lag_id]` 窗口内满足 `any` 的 BookTicker 记录数。 |
| `after_ack_to_cancelled_first_marketable_id` | `(ack_lag_id, cancelled_lag_id]` 窗口内第一条满足 `any` 的 BookTicker id。 |
| `after_ack_to_cancelled_last_marketable_id` | `(ack_lag_id, cancelled_lag_id]` 窗口内最后一条满足 `any` 的 BookTicker id。 |
| `after_ack_to_cancelled_best_margin_ticks` | `(ack_lag_id, cancelled_lag_id]` 窗口内最有利对手价相对 order price 的 tick 余量。 |
| `accepted_to_cancelled_valid` | `accepted_to_cancelled` 窗口是否可计算。 |
| `accepted_to_cancelled_records` | `accepted_to_cancelled` 窗口内 BookTicker 记录数。 |
| `accepted_to_cancelled_marketable_records` | `accepted_to_cancelled` 窗口内满足 `any` 的 BookTicker 记录数。 |
| `accepted_to_cancelled_first_marketable_id` | `accepted_to_cancelled` 窗口内第一条满足 `any` 的 BookTicker id。 |
| `accepted_to_cancelled_last_marketable_id` | `accepted_to_cancelled` 窗口内最后一条满足 `any` 的 BookTicker id。 |
| `accepted_to_cancelled_best_margin_ticks` | `accepted_to_cancelled` 窗口内最有利对手价相对 order price 的 tick 余量。 |
| `after_accepted_to_cancelled_valid` | `after_accepted_to_cancelled` 窗口是否可计算。 |
| `after_accepted_to_cancelled_records` | `after_accepted_to_cancelled` 窗口内 BookTicker 记录数。 |
| `after_accepted_to_cancelled_marketable_records` | `after_accepted_to_cancelled` 窗口内满足 `any` 的 BookTicker 记录数。 |
| `after_accepted_to_cancelled_first_marketable_id` | `after_accepted_to_cancelled` 窗口内第一条满足 `any` 的 BookTicker id。 |
| `after_accepted_to_cancelled_last_marketable_id` | `after_accepted_to_cancelled` 窗口内最后一条满足 `any` 的 BookTicker id。 |
| `after_accepted_to_cancelled_best_margin_ticks` | `after_accepted_to_cancelled` 窗口内最有利对手价相对 order price 的 tick 余量。 |

## volume 补充表

适用文件：
- `analysis/volume_by_order.csv`

字段数：`75`

| 字段 | 含义 |
|---|---|
| `group` | 分析分组，例如 `cancelled_entry_unfilled` 或 `filled_or_partial_control`。 |
| `local_order_id` | 策略本地订单 id。 |
| `side` | 订单方向，`kBuy` 或 `kSell`。 |
| `action` | 策略动作，例如 `kOpenLong`、`kOpenShort`、`kCloseLong`。 |
| `status` | 状态字段，含义取决于文件：instrument 状态、订单终态或 position 状态。 |
| `order_role` | 订单角色，`entry` 或 `exit`。 |
| `order_price` | 实际提交到交易所的 limit price。 |
| `quantity` | 订单数量，单位为合约张数。 |
| `signal_lag_id` | signal/order 对应 lag 侧 BookTicker id。 |
| `ack_lag_id` | 收到 Gate Ack 时策略看到的 lag BookTicker id。 |
| `accepted_lag_id` | 处理 Accepted response 时策略看到的 lag BookTicker id。 |
| `cancelled_lag_id` | 处理 Cancelled feedback 时策略看到的 lag BookTicker id。 |
| `filled_lag_id` | 处理 Filled feedback 时策略看到的 lag BookTicker id。 |
| `terminal_lag_id` | 终态 stage 对应 lag BookTicker id。 |
| `signal_found` | 是否在 symbol BookTicker split 中找到signal 触发时对应的记录。 |
| `signal_any` | signal 触发时是否满足 `any`：价格穿越且对手一档量大于 0。 |
| `signal_full` | signal 触发时是否满足 `full`：`any` 且对手一档量覆盖整单。 |
| `signal_volume` | signal 触发时对手一档数量。 |
| `signal_contra` | signal 触发时订单对手一档价格。 |
| `ack_found` | 是否在 symbol BookTicker split 中找到收到 Gate Ack 时对应的记录。 |
| `ack_any` | 收到 Gate Ack 时是否满足 `any`：价格穿越且对手一档量大于 0。 |
| `ack_full` | 收到 Gate Ack 时是否满足 `full`：`any` 且对手一档量覆盖整单。 |
| `ack_volume` | 收到 Gate Ack 时对手一档数量。 |
| `ack_contra` | 收到 Gate Ack 时订单对手一档价格。 |
| `accepted_found` | 是否在 symbol BookTicker split 中找到处理 Accepted response 时对应的记录。 |
| `accepted_any` | 处理 Accepted response 时是否满足 `any`：价格穿越且对手一档量大于 0。 |
| `accepted_full` | 处理 Accepted response 时是否满足 `full`：`any` 且对手一档量覆盖整单。 |
| `accepted_volume` | 处理 Accepted response 时对手一档数量。 |
| `accepted_contra` | 处理 Accepted response 时订单对手一档价格。 |
| `cancelled_found` | 是否在 symbol BookTicker split 中找到处理 Cancelled feedback 时对应的记录。 |
| `cancelled_any` | 处理 Cancelled feedback 时是否满足 `any`：价格穿越且对手一档量大于 0。 |
| `cancelled_full` | 处理 Cancelled feedback 时是否满足 `full`：`any` 且对手一档量覆盖整单。 |
| `cancelled_volume` | 处理 Cancelled feedback 时对手一档数量。 |
| `cancelled_contra` | 处理 Cancelled feedback 时订单对手一档价格。 |
| `filled_found` | 是否在 symbol BookTicker split 中找到处理 Filled feedback 时对应的记录。 |
| `filled_any` | 处理 Filled feedback 时是否满足 `any`：价格穿越且对手一档量大于 0。 |
| `filled_full` | 处理 Filled feedback 时是否满足 `full`：`any` 且对手一档量覆盖整单。 |
| `filled_volume` | 处理 Filled feedback 时对手一档数量。 |
| `filled_contra` | 处理 Filled feedback 时订单对手一档价格。 |
| `signal_to_ack_records` | `signal_to_ack` 窗口内 BookTicker 记录数。 |
| `signal_to_ack_any_records` | `signal_to_ack` 窗口内满足 `any` 的 BookTicker 记录数。 |
| `signal_to_ack_full_records` | `signal_to_ack` 窗口内满足 `full` 的 BookTicker 记录数。 |
| `signal_to_ack_max_marketable_volume` | `signal_to_ack` 窗口内满足 `any` 的记录中最大对手一档量。 |
| `signal_to_ack_first_any_id` | `signal_to_ack` 窗口内第一条满足 `any` 的 BookTicker id。 |
| `signal_to_ack_first_full_id` | `signal_to_ack` 窗口内第一条满足 `full` 的 BookTicker id。 |
| `ack_to_terminal_records` | `ack_to_terminal` 窗口内 BookTicker 记录数。 |
| `ack_to_terminal_any_records` | `ack_to_terminal` 窗口内满足 `any` 的 BookTicker 记录数。 |
| `ack_to_terminal_full_records` | `ack_to_terminal` 窗口内满足 `full` 的 BookTicker 记录数。 |
| `ack_to_terminal_max_marketable_volume` | `ack_to_terminal` 窗口内满足 `any` 的记录中最大对手一档量。 |
| `ack_to_terminal_first_any_id` | `ack_to_terminal` 窗口内第一条满足 `any` 的 BookTicker id。 |
| `ack_to_terminal_first_full_id` | `ack_to_terminal` 窗口内第一条满足 `full` 的 BookTicker id。 |
| `after_ack_to_terminal_records` | `after_ack_to_terminal` 窗口内 BookTicker 记录数。 |
| `after_ack_to_terminal_any_records` | `after_ack_to_terminal` 窗口内满足 `any` 的 BookTicker 记录数。 |
| `after_ack_to_terminal_full_records` | `after_ack_to_terminal` 窗口内满足 `full` 的 BookTicker 记录数。 |
| `after_ack_to_terminal_max_marketable_volume` | `after_ack_to_terminal` 窗口内满足 `any` 的记录中最大对手一档量。 |
| `after_ack_to_terminal_first_any_id` | `after_ack_to_terminal` 窗口内第一条满足 `any` 的 BookTicker id。 |
| `after_ack_to_terminal_first_full_id` | `after_ack_to_terminal` 窗口内第一条满足 `full` 的 BookTicker id。 |
| `accepted_to_terminal_records` | `accepted_to_terminal` 窗口内 BookTicker 记录数。 |
| `accepted_to_terminal_any_records` | `accepted_to_terminal` 窗口内满足 `any` 的 BookTicker 记录数。 |
| `accepted_to_terminal_full_records` | `accepted_to_terminal` 窗口内满足 `full` 的 BookTicker 记录数。 |
| `accepted_to_terminal_max_marketable_volume` | `accepted_to_terminal` 窗口内满足 `any` 的记录中最大对手一档量。 |
| `accepted_to_terminal_first_any_id` | `accepted_to_terminal` 窗口内第一条满足 `any` 的 BookTicker id。 |
| `accepted_to_terminal_first_full_id` | `accepted_to_terminal` 窗口内第一条满足 `full` 的 BookTicker id。 |
| `after_accepted_to_terminal_records` | `after_accepted_to_terminal` 窗口内 BookTicker 记录数。 |
| `after_accepted_to_terminal_any_records` | `after_accepted_to_terminal` 窗口内满足 `any` 的 BookTicker 记录数。 |
| `after_accepted_to_terminal_full_records` | `after_accepted_to_terminal` 窗口内满足 `full` 的 BookTicker 记录数。 |
| `after_accepted_to_terminal_max_marketable_volume` | `after_accepted_to_terminal` 窗口内满足 `any` 的记录中最大对手一档量。 |
| `after_accepted_to_terminal_first_any_id` | `after_accepted_to_terminal` 窗口内第一条满足 `any` 的 BookTicker id。 |
| `after_accepted_to_terminal_first_full_id` | `after_accepted_to_terminal` 窗口内第一条满足 `full` 的 BookTicker id。 |
| `signal_to_terminal_records` | `signal_to_terminal` 窗口内 BookTicker 记录数。 |
| `signal_to_terminal_any_records` | `signal_to_terminal` 窗口内满足 `any` 的 BookTicker 记录数。 |
| `signal_to_terminal_full_records` | `signal_to_terminal` 窗口内满足 `full` 的 BookTicker 记录数。 |
| `signal_to_terminal_max_marketable_volume` | `signal_to_terminal` 窗口内满足 `any` 的记录中最大对手一档量。 |
| `signal_to_terminal_first_any_id` | `signal_to_terminal` 窗口内第一条满足 `any` 的 BookTicker id。 |
| `signal_to_terminal_first_full_id` | `signal_to_terminal` 窗口内第一条满足 `full` 的 BookTicker id。 |

## JSON 汇总文件

适用文件：
- `analysis/cancel_windows_summary.json`
- `analysis/cancel_fillability_summary.json`
- `analysis/source_summary.json`
- `analysis/volume_summary.json`
- `manifest.json`
- `market_data/split_summaries/canonical.json`
- `market_data/split_summaries/source0.json`
- `market_data/split_summaries/source1.json`
- `market_data/split_summaries/source2.json`
- `market_data/split_summaries/source3.json`

| 字段或 key | 含义 |
|---|---|
| `run_id` | 本次运行标识。 |
| `symbol` | 分析 symbol。 |
| `inputs` | 生成该 summary 使用的输入文件路径和输入记录数。 |
| `outputs` | summary 对应的输出文件路径。 |
| `selection` | 样本选择条件和样本数量。 |
| `count` | 该 summary 覆盖的订单或文件数量。 |
| `by_side` | 按 `side` 聚合的计数。 |
| `by_status` | 按订单 `status` 聚合的计数。 |
| `by_action` | 按策略 `action` 聚合的计数。 |
| `by_category` | 按分析分类聚合的计数。 |
| `stage_marketable_counts` | 各 stage 满足 `any` 的订单数。 |
| `stage_full_counts` | 各 stage 满足 `full` 的订单数。 |
| `window_marketable_counts` | 各 window 中出现过 `any` 的订单数。 |
| `window_full_order_counts` | 各 window 中出现过 `full` 的订单数。 |
| `per_feed` | 按 canonical/source0..3 聚合的 crosscheck 结果。 |
| `files` | `manifest.json` 中的文件列表，每项包含 path、size、sha256，CSV 还包含 csv_rows。 |
| `output_dir` | split 脚本输出目录。 |
| `files_processed` | split 脚本处理的输入文件数。 |
| `total_records_read` | split 脚本读取的 BookTicker 总记录数。 |
| `records_written_by_symbol` | split 输出中每个 symbol 写出的记录数。 |
| `trailing_bytes_ignored` | live-growing bin 尾部不足一条记录而被忽略的字节数。 |
| `unknown_symbol_id_records` | instrument catalog 中找不到 symbol_id 的记录数。 |

## BookTicker `.bin.zst` 文件

适用文件：`market_data/canonical.bin.zst`、`market_data/source0.bin.zst`、`market_data/source1.bin.zst`、`market_data/source2.bin.zst`、`market_data/source3.bin.zst`。解压后每条记录为 64 字节 little-endian BookTicker，字段与 `scripts/market_data/analyze_book_ticker_latency.py::book_ticker_dtype()` 一致。

| 字段 | 类型 | 含义 |
|---|---|---|
| `id` | `int64` | BookTicker 记录 id，按同一 source/canonical 单调递增。 |
| `symbol_id` | `int32` | instrument symbol id；本归档中为 `SKYAI_USDT`。 |
| `exchange` | `uint8` | 交易所枚举，Gate 为 `2`。 |
| `exchange_ns` | `int64` | 行情源/交易所时间戳，单位 ns。 |
| `local_ns` | `int64` | 本机 data session 接收/处理时间戳，单位 ns。 |
| `bid_price` | `float64` | BBO bid price。 |
| `bid_volume` | `float64` | BBO bid volume。 |
| `ask_price` | `float64` | BBO ask price。 |
| `ask_volume` | `float64` | BBO ask volume。 |

## 日志文件

- `logs/strategy.log`：保留 strategy 原始日志行，字段以 `key=value` 形式出现；不同事件类型字段不同。主要事件包括 `gate_order_send_ok`、`lead_lag_order_submitted`、`gate_order_response`、`lead_lag_order_response`、`lead_lag_order_feedback`、`lead_lag_order_finished`。这些事件中的字段与 `inputs/orders.csv`、`inputs/latency.csv` 中同名字段含义一致。
- `logs/feedback.log`：保留 Gate private feedback session 原始日志行，字段以 `key=value` 形式出现；主要字段包括 `kind`、`local_order_id`、`exchange_order_id`、`exchange_update_ns`、`local_receive_ns`、`cumulative_filled_quantity`、`left_quantity`、`cancelled_quantity`、`fill_price`、`role`、`finish_reason`、`reject_reason`、`continuity_scope`、`continuity_reason`、`continuity_sequence`。
- `logs/extraction_notes.md`：日志过滤规则和过滤后行数说明，不是机器生成分析表。
