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
- 2026-05-27 当前接手决策：IOC partial-fill / decimal filled close 不再作为本 latency 计划的 active blocker；后续如果 live run 再出现 terminal feedback、filled close 或 REST residual 异常，再按具体问题复查。

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
- Gate `OrderSession` 多连接诊断已落地：每个 session 生成进程内 `order_session_id`；`gate_order_session_connected`
  输出 owner CPU 和 TCP endpoint；`gate_order_send_ok` 输出 send CPU；`gate_order_response` 输出 ack CPU 和可选
  `TCP_INFO`；`gate_order_ack_latency_diagnostic` 输出 diagnostic CPU 和可选 `TCP_INFO`。`TCP_INFO` 由
  `order_session.diagnostics.enable_tcp_info` 打开，默认关闭；字段索引见 `docs/diagnostic_fields.md`。
- 2026-05-27 完成一次 3 条独立 Gate `OrderSession` 并发 smoke，run 目录为
  `/home/liuxiang/tmp/20260527_063410_3session_tcpinfo/`。本轮使用 3 个独立 `lead_lag_strategy
  --smoke-unfilled-cancel` 进程，分别用 `strategy_id=4/5/6`，每个进程各自打开一条 order session、发送
  1 笔 ZEC_USDT 低价 GTC buy 后 cancel；3 个进程均 `exit=0`，feedback 均完成 `Accepted -> Cancelled`，
  最终 REST 复核 ZEC_USDT open orders 为空、position `size=0`。本轮所有 session 的 owner / send / ack CPU
  均为 CPU4，`tcp_info_available=true`，`tcp_info_total_retrans=0`。由于每个 session 在独立进程中运行，
  `order_session_id` 都是进程内的 `1`，只能用 `strategy_1/2/3` 和 endpoint 区分；本轮每条连接只有 1 笔订单，
  不构成稳定 RTT 分布结论。

  | session | remote endpoint | local port | Ack RTT | TCP RTT | TCP RTT var | retrans |
  | --- | --- | ---: | ---: | ---: | ---: | ---: |
  | `strategy_1` | `52.198.250.74:443` | `34222` | `7.868ms` | `4.130ms` | `3.559ms` | `0` |
  | `strategy_2` | `52.199.212.24:443` | `53180` | `7.036ms` | `4.213ms` | `3.026ms` | `0` |
  | `strategy_3` | `57.181.9.46:443` | `42396` | `0.665ms` | `3.240ms` | `5.760ms` | `0` |

  口径说明：Ack RTT 是应用层单笔订单 `request_send_local_ns -> gate_order_response kind=kAck.local_receive_ns`；
  TCP RTT 是 Linux `TCP_INFO.tcpi_rtt` 的连接层平滑估计，不是同一笔订单的精确网络往返，因此单笔样本里可能出现
  `TCP RTT > Ack RTT`。这次结果主要证明：同一 hostname 的并发连接实际落到了不同 remote IP，endpoint / CPU /
  TCP_INFO 字段在真实订单路径上可用。

## 仍需观察

1. **Ack outlier 根因仍未证明**
   - 2026-05-26 拆核 30 分钟 run 没有复现 Ack outlier，只能说明这轮条件下 Ack RTT 正常，不能证明 2026-05-25 的 `219.023ms` 根因。
   - 下一次如果复现 Ack RTT outlier，优先看 `send_to_first_drive_read_ns`、`drive_read_duration_ns` 和 `latency_diagnostic_inflight_at_send`，再结合 `pidstat` / `sar` / `perf sched` 判断是否为 deschedule、runtime hook 推迟 read、socket/TLS read 或 decode dispatch。

2. **exchange Ack-to-finish 是独立诊断面**
   - `exchange_lifecycle_ns` 只使用 Gate exchange timestamp，不混用本地时钟。
   - 它适合观察 Gate submit Ack 到 private order terminal update 的相对间隔，不用于解释 Ack RTT，也不表示本地和交易所之间的单程网络延迟。
   - 若后续 IOC terminal lifecycle tail 继续偏高，应单独分析 Gate private `futures.orders` update 路径，不要并入 Ack RTT 根因分析。

3. **多 OrderSession / 多 WebSocket 连接的 Ack RTT 可能不同**
   - 同一个账号可以启动多个 Gate `OrderSession`，也就是多个 WebSocket 连接；即使 WebSocket URL 相同，各连接的 Ack RTT 分布也可能不同。
   - 相同 URL 只说明入口 hostname 一样，不保证每条 TCP/WebSocket 连接落到同一个远端 IP、同一个 load balancer backend、同一个 gateway shard、同一个 per-connection queue 或同一条网络路径。
   - 每条连接有独立 TCP socket、TLS state、kernel buffer、local port、remote endpoint 和交易所 session state；本地 owner thread 的调度、C-state、`DriveRead()` 时机、日志 / runtime hook 负载也会让不同连接出现不同 Ack RTT。
   - 后续不能把“URL 相同”作为 Ack path 相同的证据。分析多连接差异时必须以 connection / session 为维度保留统计。

   已落地和仍建议补充的诊断字段：

   ```text
   order_session_id                    # 已落地
   owner_thread_cpu                    # 已落地于 connected log
   send_cpu                            # 已落地于 send log
   ack_cpu                             # 已落地于 response log
   diagnostic_cpu                      # 已落地于 latency diagnostic log
   local_ip                            # 已落地于 connected log
   local_port                          # 已落地于 connected log
   remote_ip                           # 已落地于 connected log
   remote_port                         # 已落地于 connected log
   tcp_info_rtt_us                     # 已落地，需 enable_tcp_info
   tcp_info_retrans                    # 已落地，需 enable_tcp_info
   resolved_ips                        # 尚未落地
   request_sequence
   request_send_local_ns
   ack_local_receive_ns
   ack_exchange_ns
   ack_rtt_ns
   exchange_to_local_ns
   ```

   快速人工检查可以用：

   ```bash
   ss -tinp | rg '443|lead_lag|gate'
   ```

   复核重点：

   - 慢连接是否连接到不同 remote IP。
   - remote IP 相同但 local port 不同，是否仍有稳定 RTT 差异。
   - 慢连接是否有更高 `TCP_INFO` RTT 或 retrans。
   - 慢连接的 `send_to_first_drive_read_ns` 是否偏大，说明本地 owner thread 没及时进入 read。
   - `ack_exchange_ns` 到本地 receive 是否只在某条连接上异常偏大。

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
6. 如果本轮使用多个 `OrderSession` / WebSocket 连接，report 或附加日志必须按 session / connection 维度保留 remote endpoint、local port、owner CPU、Ack RTT 分布和 TCP diagnostics；不要只汇总全局 Ack RTT。

## 执行边界

- 只做报告或文档更新时不需要重新构建 C++。
- 修改 latency diagnostic、order session、feedback parser、runtime loop 或 affinity overlay 后，至少运行对应 C++ / Python 单测和 `git diff --check`。
- 真实订单 run 的临时配置、日志、调度采样和 scratch 输出写入 `/home/liuxiang/tmp/<run_id>/`。
