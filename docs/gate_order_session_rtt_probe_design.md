# Gate OrderSession RTT Probe 设计

## 目的

`gate_order_session_rtt_probe` 用真实 Gate `OrderSession` 下单路径测量多连接 Ack RTT。它是 measurement-only 工具：采集、归档、离线分析，不自动打分、不自动切换 LeadLag 或生产 `OrderSession`。

第一版关注三件事：

- 不同 `connect_ip` / connection 的 `futures.order_place` Ack RTT 是否稳定不同。
- 同一 private link 上不同连接 / owner CPU 是否出现明显差异。
- Ack outlier 发生时，能否用 runtime / write path / socket timestamping 字段把延迟缩到更小阶段。

## 当前状态

- 入口：`tools/gate/order_session_rtt_probe/`。
- 默认配置：`config/order_session_rtt_probe/gate_order_session_rtt_probe.toml`。
- 默认连接 CSV：`config/order_session_rtt_probe/gate_order_session_rtt_connections.csv`。
- 8 条 private plain 全阶段配置：`config/order_session_rtt_probe/gate_order_session_rtt_probe_private8_plain_allstage.toml`。
- 8 条 private plain 连接 CSV：`config/order_session_rtt_probe/gate_order_session_rtt_private8_plain_connections.csv`。
- 全阶段 base order session 配置：`config/order_sessions/gate_order_session_rtt_probe_allstage.toml`。

已落地：

- connections CSV 解析，保留重复 `connect_ip`，`name` 唯一。
- `--connections-file` / `--samples-per-session` 覆盖。
- `--live-preflight` 打印 single-session / multi-session plan，不连接、不下单。
- `--execute` 支持 `order_mode="ioc"` 的 single-session / multi-session live sample。
- multi-session 下每行 CSV 对应一条独立 `OrderSession`，可覆盖 `host`、`connect_ip`、`port`、`enable_tls`、`worker_cpu_id`。
- feedback SHM reader 已接入，用 `strategy_id` claim feedback lane，并按 `local_order_id` 路由到对应 session。
- sample CSV 已包含 runtime hook、DriveRead、write path、socket send queue、`TCP_INFO` 和 socket timestamping 字段。
- `cycle_cooldown_us` / `order_session_interval_us` 已支持微秒级 pacing；旧 `*_ms` 只作为兼容入口。

仍是边界：

- `--execute` 当前只支持 `probe.order.order_mode="ioc"`；`gtc` / `ioc+gtc` 的状态流转和配置解析已在纯逻辑层覆盖，但 live execute 会拒绝。
- `probe.safety.preflight_rest_check`、`run_end_rest_check` 和 REST 输出路径已解析并保留为安全意图，但工具内 REST preflight / run-end guard 尚未真正执行。真实测试前后仍需外部 REST / 人工确认 flat，直到该 guard 落地。
- 工具不输出自动 score，也不写回生产配置。

## 配置模型

TOML 保存运行参数，CSV 保存连接列表，避免 IP 过多时 TOML 膨胀。

```toml
[probe.inputs]
order_session_config = "config/order_sessions/gate_order_session.toml"
data_reader_config = "config/data_readers/strategy_data_reader_requested_20260521.toml"
connections_file = "config/order_session_rtt_probe/gate_order_session_rtt_connections.csv"

[probe.sampling]
samples_per_session = 1
cycle_cooldown_us = 500000
order_session_interval_us = 500000

[probe.order]
order_mode = "ioc"
```

connections CSV schema：

```text
name,group,host,connect_ip,port,enable_tls,worker_cpu_id
```

语义：

- 行顺序就是 session 顺序。
- `name` 必须唯一。
- `group` 用于离线分组，例如 `private-plain`、`public-tls`。
- `connect_ip` 可重复；重复表示对同一目标 IP 建多条独立 TCP/WebSocket 连接。
- `enable_tls=false` 用 private plain link；TLS SNI / WebSocket Host 仍来自 `host`。
- `worker_cpu_id=-1` 表示沿用 base order session config。

## 常用命令

dry-run / plan：

```bash
./build/debug/tools/gate_order_session_rtt_probe \
  --config config/order_session_rtt_probe/gate_order_session_rtt_probe.toml
```

live preflight：

```bash
./build/debug/tools/gate_order_session_rtt_probe \
  --config config/order_session_rtt_probe/gate_order_session_rtt_probe.toml \
  --live-preflight
```

8 条 private plain 全阶段预检：

```bash
./build/release/tools/gate_order_session_rtt_probe \
  --config config/order_session_rtt_probe/gate_order_session_rtt_probe_private8_plain_allstage.toml \
  --live-preflight
```

live IOC sample：

```bash
./build/release/tools/gate_order_session_rtt_probe \
  --config config/order_session_rtt_probe/gate_order_session_rtt_probe_private8_plain_allstage.toml \
  --samples-per-session 1800 \
  --duration-sec 1800 \
  --execute
```

运行前要求：

- Gate data session 已发布 `BookTicker` SHM。
- Gate order feedback session 已发布 feedback SHM。
- 使用 dedicated / probe account，并在外部 REST 确认无 open orders、position flat。
- 设置 `PROBE_KEY` / `PROBE_SECRET`。

## 采样路径

当前 live execute 的 IOC sample：

```text
Gate BookTicker event
  -> 选择对应 contract 的最新 BBO
  -> 用 catalog price_tick / price_limit_down 计算 passive buy price
  -> 用最小 quantity 并按 step / decimal 格式化
  -> 发送 IOC place
  -> 记录 futures.order_place Ack
  -> 等待 private futures.orders terminal feedback
  -> 写 sample CSV
```

multi-session 调度：

- 所有 session 常驻并 logged-in。
- coordinator 按 CSV 行顺序授予 dispatch。
- `order_session_interval_us` 控制同一 cycle 内相邻 session 的下单间隔。
- `cycle_cooldown_us` 控制完整 cycle 结束后的冷却。
- 同一时刻只 dispatch 一个普通 probe order，避免同账户风险叠加。

## 指标口径

主指标只使用同一机器本地时钟：

```text
ack_rtt_ns = ack_recv_ns - send_ns
```

CSV 中需要分开分析：

| 字段组 | 用途 |
| --- | --- |
| `session,group,ip,sid,contract,type,action` | 连接和样本分组。 |
| `send_ns,ack_recv_ns,ack_rtt_ns` | Ack RTT 主口径。 |
| `send_to_drive_read_ns,drive_read_ns,max_loop_gap_ns` | owner runtime / DriveRead 归因。 |
| `order_encode_done_ns ... write_complete_ns` | Gate order write path 分段。 |
| `socket_sendq_available,tcp_sendq_bytes,tcp_notsent_bytes` | socket send queue 辅助诊断。 |
| `tcp_info_*` | Linux `TCP_INFO` 辅助诊断，不作为主排名。 |
| `ts_*` | socket timestamping software-level 分段。 |
| `status,term_fb,fill,invalid,inv_reason` | 样本有效性和安全审计。 |

完整字段说明见 `docs/diagnostic_fields.md`。

## Socket Timestamping 归因

private plain transport 成功 apply `SO_TIMESTAMPING` 后，sample 可记录：

```text
write_complete -> tx_software -> tx_ack -> rx_software -> ack_receive
```

注意：

- TLS 或 apply 失败时 `ts_available=false`，不要把 0 当真实时间。
- 当前主要是 software timestamping，不能严格证明 packet leaves / returns NIC。
- RX software timestamp 是 `recvmsg()` 粒度，多 Ack 合并时可能共享 RX 时间戳。
- 要确认 NIC / 网络 / Gate 侧边界，需要 hardware timestamp 或 pcap。

## 分析方法

离线统计默认只使用：

```text
invalid=false
status=acked 或 sample completed
```

建议最小分组：

- `session + type + action`
- `group + type + action`
- `ip + type + action`
- `contract + type + action`
- 时间窗口，例如 60s / 300s rolling p50 / p95 / p99 / max

解释 outlier 时按顺序看：

1. `send_to_drive_read_ns` / `max_loop_gap_ns` 是否大：owner thread 没及时读。
2. write path 字段是否大：encode、enqueue、write pump、`send()` / `SSL_write()`。
3. `socket_sendq` / `tcp_notsent` / `TCP_INFO` 是否异常：本机 socket / TCP 状态。
4. `ts_write_to_tx_software_ns` 是否大：kernel / qdisc / driver 出站前排队嫌疑。
5. `ts_tx_software_to_tx_ack_ns` 是否大：request bytes 发出后到 TCP ACK 返回慢。
6. `ts_tx_ack_to_rx_software_ns` 是否大：远端已 TCP ACK request，但业务 Ack 回来慢。
7. `ts_rx_software_to_ack_receive_ns` 是否大：包到 kernel 后用户态读取 / parse / 调度慢。

不要把 terminal lifecycle tail 并入 Ack RTT。Gate private `futures.orders` terminal feedback 是另一个诊断面。

## 安全边界

- 必须显式 `--execute` 才允许下单。
- 临时输出写入 `/home/liuxiang/tmp` 或配置的 `probe.output.root_dir`。
- 当前 live execute 只做 IOC；GTC / cancel / close live 继续等 REST guard 和安全闭环补齐后再启用。
- 出现 feedback `ContinuityLost`、terminal timeout、unexpected fill 或 sample invalid 时，该 run 的统计结果需要人工复核。
- 任何自动选优、自动切换、写回生产配置都不属于第一版。

## 下一步

1. 在工具内补 REST preflight / run-end flat guard，并把 `order_session_rtt_rest_guard.csv` 与 `raw_rest/` 真正写出。
2. 复核 IOC execute 的 unexpected fill / terminal timeout 处理，确认失败时退出码和 sample invalid 语义稳定。
3. 在 REST guard 完成后再启用 `gtc` / `ioc+gtc` live execute。
4. 增加离线分析脚本，输出按 session / group / ip / symbol / rolling window 的分布。
5. 拿到足够 live 样本后再设计 score；score 不进入当前热路径。

## 验证命令

```bash
cmake --build build/debug --target gate_order_session_rtt_probe gate_order_session_rtt_probe_test order_session_config_test
ctest --test-dir build/debug -R 'gate_order_session_rtt_probe_test|order_session_config_test' --output-on-failure
./build/debug/tools/gate_order_session_rtt_probe --config config/order_session_rtt_probe/gate_order_session_rtt_probe.toml
./build/debug/tools/gate_order_session_rtt_probe --config config/order_session_rtt_probe/gate_order_session_rtt_probe.toml --live-preflight
git diff --check
```
