# LeadLag Runtime Latency 当前计划

## 目的

本文只保留 LeadLag live-orders Ack latency 相关的当前事实、已落地改动和仍需验证的事项。早期讨论中的已完成实现步骤、重复命令和单次运行流水账已经删除；历史细节以 commit、report 和 `docs/lead_lag_ack_latency_outlier_analysis.md` 为准。

相关事实源：

- `docs/lead_lag_ack_latency_outlier_analysis.md`
- `docs/lead_lag_live_operations_pipeline.md`
- `docs/lead_lag_live_report_csv_schema.md`
- `reports/20260525_133511_12pair_live_2ticks/`
- `reports/20260526_043440_12pair_live_30m/`

## 当前事实

- 2026-05-25 的 12-symbol guarded live-orders 1 小时 run 正常退出 flat，`signals=58`、`orders=58`、`finished=58`、`filled=0`，最大 Ack RTT 为 `219.023ms`。
- 该 outlier 的 `request_send_local_ns` 与 `ack_local_receive_ns` 都由同一个 strategy / Gate order-session owner thread 记录；它不是跨线程消息传递延迟，也不是 `gate_order_feedback_session` 处理 Ack 慢。
- 2026-05-25 run 所在 10 分钟 sysstat 窗口中 CPU4 满载，且 strategy / order session / feedback session 当时都绑 CPU4 并 active-spin；本地调度或同核竞争仍是强候选，但原始日志不能证明具体根因。
- 2026-05-26 的 12-symbol guarded live-orders 30 分钟拆核运行正常退出 flat，`signals=10`、`orders=10`、`finished=10`、`filled=0`，最大 Ack RTT 为 `6.738ms`，没有复现 `219ms` 级别 Ack outlier。
- 2026-05-26 run 中最大 send-to-finish 为 DASH_USDT 的 `45.977ms`；其中 Gate exchange timestamp 的 Ack-to-finish 为 `37.336ms`。这属于 IOC submit Ack 后到 Gate private order terminal update 的 lifecycle 延迟，不是 Ack path outlier。

## 已落地

- Gate `OrderSession` 专用 Ack latency diagnostic 已落地：只在订单发送后的诊断窗口内工作，正常无订单时不输出日志。
- diagnostic 字段已进入 `order_detail.csv` / `latency.csv` 和 report：
  - `latency_diagnostic_reason`
  - `latency_diagnostic_ack_rtt_ns`
  - `send_to_first_after_hook_ns`
  - `send_to_first_drive_read_ns`
  - `drive_read_duration_ns`
  - `max_observed_drive_read_duration_ns`
  - `latency_diagnostic_inflight_at_send`
- `run_live_with_guard.py` 已支持 runtime affinity profile overlay：从 `config/runtime_affinity/lead_lag_requested_12symbols_node0.toml` 生成临时 strategy、data session 和 feedback TOML。
- 当前核心链路绑核目标是 Gate market data CPU2、Binance market data CPU3、strategy / Gate order owner CPU4、Gate order feedback CPU6、log backend CPU5。recorder、TUI、guard、report 和 shell 不属于核心交易链路，不能抢占 CPU2 / CPU3 / CPU4 / CPU6。
- Gate submit final result parser / correlation cleanup 已修复旧 run 中的 `gate_order_response_ignored reason=unknown_kind` / inflight 长期递增风险；该问题不再作为本 latency 计划的未完成项。
- report 已能解析 guard affinity summary、latency diagnostic outlier、feedback session event，以及 `exchange_lifecycle_ns = finish_exchange_ns - ack_exchange_ns`。字段说明见 `docs/lead_lag_live_report_csv_schema.md`。

## 仍未完成

1. **Ack outlier 根因仍未证明**
   - 2026-05-26 拆核 30 分钟 run 没有复现 Ack outlier，只能说明这轮条件下 Ack RTT 正常，不能证明 2026-05-25 的 `219.023ms` 根因。
   - 下一次如果复现 Ack RTT outlier，优先看 `send_to_first_drive_read_ns`、`drive_read_duration_ns` 和 `latency_diagnostic_inflight_at_send`，再结合 `pidstat` / `sar` / `perf sched` 判断是否为 deschedule、runtime hook 推迟 read、socket/TLS read 或 decode dispatch。

2. **exchange Ack-to-finish 是独立诊断面**
   - `exchange_lifecycle_ns` 只使用 Gate exchange timestamp，不混用本地时钟。
   - 它适合观察 Gate submit Ack 到 private order terminal update 的相对间隔，不用于解释 Ack RTT，也不表示本地和交易所之间的单程网络延迟。
   - 若后续 IOC terminal lifecycle tail 继续偏高，应单独分析 Gate private `futures.orders` update 路径，不要并入 Ack RTT 根因分析。

3. **IOC partial-fill / decimal filled close blocker 仍需 live 复核**
   - 2026-05-25 和 2026-05-26 两轮 12-symbol run 都没有成交，不能证明 IOC partial-fill / decimal filled close 修复已通过。
   - 复核前不要启动无人值守真实订单长跑。

## 下一轮验证要求

下一轮真实订单测试如果目标是继续复核 Ack latency，必须满足：

1. 使用 `docs/lead_lag_live_operations_pipeline.md` 的 live run pipeline。
2. 使用 affinity profile `config/runtime_affinity/lead_lag_requested_12symbols_node0.toml`，并在 guard summary / report 中明确记录实际 split 状态；如果外部 data session 或 feedback session 没有使用本轮临时 TOML，不能声称 full affinity split。
3. 同步采集调度证据：

```bash
pidstat -wt -p <strategy_pid>,<feedback_pid> 1
sar -u -P 2,3,4,5,6 1
sar -q 1
sar -w 1
```

4. 如权限允许，异常窗口短时间采集：

```bash
perf sched record -p <strategy_pid> -- sleep <duration>
perf sched latency
```

5. report 摘要必须同时列出 Ack RTT、send-to-finish 本地闭环和 exchange Ack-to-finish，避免混淆 Ack receive 延迟与交易所终态 lifecycle 延迟。

## 执行边界

- 只做报告或文档更新时不需要重新构建 C++。
- 修改 latency diagnostic、order session、feedback parser、runtime loop 或 affinity overlay 后，至少运行对应 C++ / Python 单测和 `git diff --check`。
- 真实订单 run 的临时配置、日志、调度采样和 scratch 输出写入 `/home/liuxiang/tmp/<run_id>/`。
