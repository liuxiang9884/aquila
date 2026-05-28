# Gate OrderSession RTT Probe 设计

## 目的

本文记录 Gate `OrderSession` 多连接 RTT 测量和后续连接寻优方向的当前设计。第一版目标是 measurement-only：对多组
`connect_ip` 采集真实订单路径 RTT，写入结构化 log / CSV，暂不自动打分、暂不自动切换 live 策略使用的
`OrderSession`。

最终目标是为后续 order session 连接寻优提供证据：找到 Ack RTT 更低、更稳定的 Gate WebSocket 连接，并为未来
`1 hot primary + low-duty scout` 的动态旁路测量和安全切换打基础。

## 当前结论

- 第一版新增独立 C++ tool，建议命名为 `gate_order_session_rtt_probe`；不改 LeadLag live runtime，不接入生产策略下单路径。
- 第一版只采集数据，不输出自动 score，不选择最优连接，不写回生产配置。
- RTT 主指标来自真实 Gate order session order Ack：`request_send_local_ns -> gate_order_response kind=kAck.local_receive_ns`。
- 不使用 WebSocket handshake RTT、login RTT 或 no-order probe RTT 作为第一版主指标；这些路径不等价于 `futures.order_place`。
- `TCP_INFO` 只作为辅助诊断，用于解释网络层 RTT、RTT variance 和 retrans，不直接作为寻优主指标。
- 每个样本同时采集三条 RTT：`gtc_place_ack_rtt_ns`、`gtc_cancel_ack_rtt_ns`、`ioc_place_ack_rtt_ns`。
- GTC 和 IOC 使用同一份从当前 Gate BBO 推导出的非成交 passive price；quantity 使用 instrument catalog 中的最小可下单量并按 step / decimal 约束格式化。
- 若任一 probe order 意外成交，立即 reduce-only market close，标记该样本 invalid，并通过 REST 复核 open orders 为空、position flat。

## 非目标

- 第一版不做动态连接切换。
- 第一版不把多个 `OrderSession` 接入 `OrderManager` 或 LeadLag runtime。
- 第一版不把 GTC / IOC RTT 混成一个分数；score 设计等拿到真实分布后再定。
- 第一版不基于 `TCP_INFO` 或 login RTT 直接选连接。
- 第一版不试图解释 2026-05-25 的 `219.023ms` Ack RTT outlier；该问题等待复现后按现有 diagnostic 分析。
- 第一版不把 terminal lifecycle latency 和 Ack RTT 混在一起；IOC terminal feedback 只作为安全和辅助观测。

## RTT 口径

第一版每个 `connect_ip` 按样本分别记录：

| 指标 | 来源 | 用途 |
| --- | --- | --- |
| `gtc_place_ack_rtt_ns` | GTC place 的 `request_send_local_ns -> Ack local_receive_ns` | 观察普通 place 请求路径。 |
| `gtc_cancel_ack_rtt_ns` | GTC cancel 的 `request_send_local_ns -> Ack local_receive_ns` | 观察 cancel 请求路径。 |
| `ioc_place_ack_rtt_ns` | IOC place 的 `request_send_local_ns -> Ack local_receive_ns` | 观察更接近生产 IOC place 的 Ack 路径。 |
| `tcp_info_rtt_us` / `tcp_info_rttvar_us` / retrans | `TCP_INFO` snapshot | 辅助解释网络层状态，不作为主排名。 |
| `exchange_lifecycle_ns` / `ack_to_finish_local_ns` | IOC terminal feedback / report | 辅助观察 terminal lifecycle，不解释 Ack RTT。 |

后续分析时三组 Ack RTT 分开统计，例如 count、p50、p90、p99、max、timeout、reject、unexpected fill。第一版不定义
score，也不按 score 输出“最优连接”结论。

## 统计目标

第一版数据采集要支持下面五类问题，但统计计算放在离线分析脚本或后续 report 中完成，不放入下单热路径：

| 目标 | 分组维度 | 统计口径 |
| --- | --- | --- |
| 单个指定 IP 的 Ack RTT 分布 | `connect_ip + stage` | 对 valid sample 计算 count、avg、p50、p90、p99、max。 |
| 过去 N 秒 rolling Ack RTT | `connect_ip + stage + rolling_window_sec` | 按 Ack 接收时间或 `sample_end_ns` 滚动聚合；默认建议先用 `300s`，分析脚本允许改成 `60s` / `600s`。 |
| 同一指定 IP 在不同 symbol 上是否不同 | `connect_ip + contract + stage` | 同一 `connect_ip` 下按 contract 分组比较三类 Ack RTT。 |
| 同一指定 IP 在不同时间是否稳定 | `connect_ip + stage + time_window` | 与 rolling 统计同一套数据口径；重点看 p90 / p99 / max 是否漂移。 |
| 同一指定 IP 断开重连后是否变化 | `connect_ip + connection_generation + stage` | 同一 IP 每次重新建连递增 generation，比较不同 generation 的分布。 |

这里的 stage 固定为 `gtc_place`、`gtc_cancel`、`ioc_place`。第一版不把三类 Ack RTT 混成一个总 RTT，以免掩盖 place、
cancel 和 IOC path 的差异。

## Probe 订单形态

第一版使用目标 contract 的最新 Gate BBO 计算 passive buy price：

```text
price = floor_to_tick(best_bid - passive_offset_ticks * tick_size)
quantity = instrument catalog 中最小下单量，按 quantity_step / decimal places 对齐
```

GTC place 和 IOC place 使用同一个 `price_text` 和 `quantity_text`。默认使用 buy side，原因是意外成交时仓位方向明确，
可以用 reduce-only sell market close 处理。`passive_offset_ticks` 必须可配置；如果价格过远导致 Gate price band /
invalid price reject，样本应标记为 reject，不纳入正常 RTT 分布。

每个样本建议流程：

```text
1. 从 Gate BookTicker SHM 读取当前 latest BBO。
2. 按 instrument catalog 生成 quantity_text 和 passive price_text。
3. 发送 GTC place，记录 gtc_place_ack_rtt_ns。
4. 等 accepted feedback 或 REST open order 可见。
5. 发送 cancel，记录 gtc_cancel_ack_rtt_ns。
6. 等 cancelled feedback 或 REST open order 消失。
7. 使用同一个 price_text / quantity_text 发送 IOC place，记录 ioc_place_ack_rtt_ns。
8. 等 IOC terminal feedback 或 REST 证明无 open order。
9. REST final check：目标 contract open orders 为空、position `size=0`，且 `value` / `margin` residual 符合现有 flat 判断。
```

若任何阶段出现 fill / partial fill：

```text
1. 停止当前样本后续普通 probe。
2. 立即提交 reduce-only market close。
3. REST 轮询确认 position flat、open orders 为空。
4. 将样本标记 unexpected_fill=true / invalid_for_rtt_distribution=true。
```

## 工具形态

### IP Discovery

第一版先用 Python 脚本完成 RTT probe 前置的 IP discovery，不下单、不读取账户、不修改账户状态。脚本入口：

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/gate/discover_gate_ws_ips.py \
  --host fx-ws.gateio.ws \
  --target /v4/ws/usdt \
  --duration-sec 180 \
  --interval-sec 5 \
  --resolver 1.1.1.1 \
  --resolver 8.8.8.8 \
  --resolver 9.9.9.9 \
  --history-log /home/liuxiang/log/lead_lag_strategy*.log \
  --output /home/liuxiang/tmp/<run_id>/candidate_ips.jsonl \
  --text-output /home/liuxiang/tmp/<run_id>/candidate_ips.txt
```

脚本负责前置筛选：

1. 使用 system resolver 和可选 explicit UDP resolver 对 `host` 做多轮 DNS A / AAAA 采样。
2. 从历史 log 中解析 `gate_order_session_connected remote_ip=...`。
3. 对候选 IP 做 pinned WebSocket handshake：TCP 直连 `connect_ip:port`，TLS SNI / 证书校验和 WebSocket Host 仍使用
   `host`。
4. 可选 `--verify-login`：在 WebSocket handshake 成功后发送 `futures.login`，只把 login 成功的 IP 标为
   `selected_for_rtt_probe=true`。该步骤仍不下单，只验证 private WebSocket login 是否可用。

完整审计输出为 `candidate_ips.jsonl`，每个 IP 一行，schema 为
`aquila.gate.order_session.ip_discovery.v1`。后续 RTT probe 默认只消费 `candidate_ips.txt`，该文件只包含
`selected_for_rtt_probe=true` 的 IP：

```text
# schema=aquila.gate.order_session.candidate_ips.v1
# run_id=20260528_120000_gate_ip_discovery
# host=fx-ws.gateio.ws
# target=/v4/ws/usdt
# generated_at_ns=1770000185000000000
52.198.250.74
52.199.212.24
57.181.9.46
```

`--resolver` 可重复传入，支持 `system` 或 `IP[:port]`；默认包含 `system`，显式 resolver 会作为额外 DNS 来源采样。JSONL
中 `sources` 会区分 `dns_system` / `dns_udp_<ip>_<port>`，`dns.resolvers` 和 `dns.resolver_details` 记录每个 IP
来自哪些 resolver。当前 explicit resolver 使用 UDP DNS，不修改本机 `/etc/resolv.conf` 或 `systemd-resolved` 配置。
Discovery 输出不代表 RTT 优劣；未启用 `--verify-login` 时只表示该 IP 当前能用 logical host 模式完成 WebSocket
handshake，启用后表示该 IP 当前也能完成 `futures.login`。

已有候选文件可复用：

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/gate/discover_gate_ws_ips.py \
  --host fx-ws.gateio.ws \
  --target /v4/ws/usdt \
  --no-dns \
  --candidate-ip-file /home/liuxiang/tmp/<previous_run>/candidate_ips.txt \
  --verify-login \
  --output /home/liuxiang/tmp/<run_id>/candidate_ips_login.jsonl \
  --text-output /home/liuxiang/tmp/<run_id>/candidate_ips_login.txt
```

2026-05-28 已完成一轮候选池发现和 login 筛选：

| run | 输出 | 结果 |
| --- | --- | --- |
| 600s 多 resolver discovery | `/home/liuxiang/tmp/multi_resolver_600s_20260528_053329/` | `records=48`，`selected_for_rtt_probe=48`，全部通过 pinned WebSocket handshake。 |
| 1800s 多 resolver discovery | `/home/liuxiang/tmp/multi_resolver_1800s_20260528_055058/` | `records=48`，`selected_for_rtt_probe=48`；`system`、Cloudflare、Google、Quad9 主备 resolver 均覆盖 48 个 IP。 |
| 48 IP login verify | `/home/liuxiang/tmp/login_verified_candidates_20260528_072242/` | `records=48`，`login_ok=48`，`selected_for_rtt_probe=48`；`login_latency_ns` min `11703225`、p50 `16820493`、max `31351150`。 |

当前给 RTT probe 使用的候选文件为
`/home/liuxiang/tmp/login_verified_candidates_20260528_072242/candidate_ips_login.txt`。该文件 5 行 header 后是 48 行 IP；
RTT probe 读取时跳过 `#` 开头的 header。

### RTT Probe

建议第一版命令形态：

```bash
./build/release/tools/gate_order_session_rtt_probe \
  --config config/order_sessions/gate_order_session.toml \
  --data-reader-config config/data_readers/strategy_data_reader_requested_20260521.toml \
  --contract ZEC_USDT \
  --connect-ip 52.198.250.74 \
  --connect-ip 52.199.212.24 \
  --connect-ip 57.181.9.46 \
  --samples-per-ip 20 \
  --passive-offset-ticks 100 \
  --samples-output /home/liuxiang/tmp/<run_id>/order_session_rtt_samples.csv
```

第一版可以沿用 base order session config 中的 `host`、`port`、TLS、credentials、decimal-size header 和 log 设置；每个
candidate 只覆盖 `connection.connect_ip`。建议默认打开 `order_session.diagnostics.enable_tcp_info=true` 的临时副本，
但不要修改仓库默认配置。

输出分两类：

1. 连接级一次性信息使用 Nova 普通结构化 log，事件名建议为 `gate_order_session_rtt_probe_connection`：

   ```text
   run_id connect_ip order_session_id remote_ip remote_port local_ip local_port owner_thread_cpu contract
   ```

2. sample 级统计使用 Quill `CsvWriter` + `nova::LogManager::NovaFrontendOptions` 异步写入 CSV。每个 sample
   一行，sample 定义为 GTC place -> GTC cancel -> IOC place -> final check：

   ```text
   run_id,connect_ip,order_session_id,connection_generation,round_index,sample_index,contract,price_text,quantity_text,
   sample_start_ns,sample_end_ns,
   gtc_place_ack_receive_local_ns,gtc_place_ack_rtt_ns,
   gtc_cancel_ack_receive_local_ns,gtc_cancel_ack_rtt_ns,
   ioc_place_ack_receive_local_ns,ioc_place_ack_rtt_ns,
   gtc_place_status,gtc_cancel_status,ioc_place_status,
   unexpected_fill,invalid_for_rtt_distribution,invalid_reason,final_flat
   ```

`CsvWriter::append_row()` 只在 sample 完成后调用，不在单个 order send -> Ack 计时窗口内调用。普通 Nova log 仍保留用于
运行排障；CSV 是 RTT 分布分析的主产物。第一版不输出 JSONL。

连接级 log 必须同时记录 `connection_generation` 和 `connected_at_ns`：

```text
gate_order_session_rtt_probe_connection
  run_id connect_ip order_session_id connection_generation connected_at_ns
  remote_ip remote_port local_ip local_port owner_thread_cpu contract
```

`connection_generation` 按 `connect_ip` 从 0 开始递增。同一个指定 IP 关闭并重新连接后，新的 `OrderSession` 必须使用新的
generation；离线统计用它判断“同一个 IP 重连前后 Ack RTT 是否显著变化”。

建议第一版附带一个离线分析脚本，输入 `order_session_rtt_samples.csv`，输出：

```text
overall_rtt_summary.csv      # connect_ip + stage 的 count / avg / p50 / p90 / p99 / max
rolling_rtt_summary.csv      # connect_ip + stage + rolling window 的 rolling stats
symbol_rtt_summary.csv       # connect_ip + contract + stage
reconnect_rtt_summary.csv    # connect_ip + connection_generation + stage
```

所有统计默认只使用 `invalid_for_rtt_distribution=false`、`final_flat=true` 且对应 stage status 为 acked 的样本。

## 采样顺序

为了减少时间窗口偏差，不建议按 IP 一次跑完所有样本。第一版应按 round-robin / rotate order 采样：

```text
round 1: IP_A, IP_B, IP_C
round 2: IP_B, IP_C, IP_A
round 3: IP_C, IP_A, IP_B
...
```

同一时刻只允许一个 candidate 执行真实 probe order 序列，避免多个 probe 同时向同一账户下单，也避免订单回报和 REST
复核互相干扰。

## 线程模型选项

### 方案 A：每个活跃 OrderSession 一个 owner thread

这是第一版 measurement tool 的推荐默认方案。每个 candidate `connect_ip` 启动一条 `OrderSession`，由自己的 owner
thread 运行现有 `BasicWebSocketClient::Start()` active loop。coordinator 通过每个 worker 的 command queue 串行下发
probe command；place / cancel 实际在该 session owner thread 的 runtime hook 中执行。

优点：

- 符合现有 `OrderSession` / `BasicWebSocketClient` owner-thread 设计。
- 不需要把 WebSocket client 改成可 step 的 multi-session reactor。
- 不把单线程 multiplex 轮询延迟混入候选连接 RTT。
- 多条连接可以同时 logged-in / ready，减少候选之间因重连带来的时间窗口差。

限制：

- 每个 active-spin session 占用一个线程和 CPU 时间。
- 第一版应限制 candidate 数量或允许配置 `max_active_sessions`，避免抢占 strategy / market data / feedback 的关键 CPU。
- coordinator 同一时刻只下发一个真实 probe order 序列，降低账户风险。

### 方案 B：Rotating worker

只启动 1 条或少量 scout `OrderSession`，按候选 `connect_ip` 轮转连接、采样、断开。该方案资源消耗低，更接近未来
生产 `low-duty scout`，但样本之间时间窗口差更大，也无法观察多个 ready connection 在同一时间段的差异。

该方案适合作为第一版后续扩展，不适合作为第一批基线数据的唯一来源。

### 方案 C：单线程手写 multi-session loop

理论上可以把多个 session 放进一个 thread，按 `session A -> session B -> session C` 轮询 write/read。但当前
`BasicWebSocketClient::Start()` 是阻塞 active loop，不提供多 session step API；实现该方案需要改造 WebSocket cold path、
active loop 和 session ownership。

更重要的是，单线程轮询会改变 RTT 口径：Ack 已经到达 socket 后，还要等 loop 转回对应 session 才能记录
`ack_local_receive_ns`。测到的值会变成：

```text
真实连接 / Gate Ack RTT + 本地 multiplex 轮询调度延迟
```

因此该方案不适合作为第一版准确测量工具，也不适合作为最低延迟 hot path 默认模型。

### 方案 D：Coroutine multi-session scout

coroutine / async event loop 可以让一个 scout thread 管理多个候选 session，只在 fd readable / writable / timer ready 时恢复
对应 coroutine。它比手写固定轮询更适合资源友好的后台 scout。

但当前 WebSocket / `OrderSession` 不是 coroutine 架构；要落地需要 async transport、async TLS handshake、async WebSocket
frame read/write 和 async order session state machine。即使使用 coroutine，同一线程内多个 ready session 仍存在 event loop
恢复顺序和 decode/dispatch 排队延迟；它适合 low-duty scout，不适合替代 primary hot order session。

建议把 coroutine 作为 V2 scout implementation option，不作为第一版 measurement tool，也不作为真实订单 hot path。

## 未来动态连接选择方向

后续生产形态建议不是“多个 hot sessions 全常驻并自动抢切”，而是：

```text
PrimaryOrderSession
  1 条当前交易使用的 hot session
  owner thread / active-spin
  处理真实策略订单

ScoutOrderSession
  1 条或少量 low-duty scout session
  rotating connect_ip
  周期性采集 GTC place / GTC cancel / IOC place Ack RTT
  输出候选连接统计
```

切换只影响后续新订单，不迁移已有 inflight order。切换前至少需要满足：

- 当前 primary 无 inflight order。
- candidate 已 ready / logged in。
- candidate 在多个采样窗口内稳定优于 primary；具体 score 等第一版数据回来后再设计。
- candidate 没有 unexpected fill、REST residual、open order residue、continuity lost 或明显 TCP retrans。
- 切换动作和后续订单在 log 中明确记录 old / new endpoint、local port、owner CPU 和触发原因。

## 安全边界

- 工具必须显式 `--execute` 或等价确认才允许真实下单。
- 默认输出和临时 config 写入 `/home/liuxiang/tmp/<run_id>`。
- 启动前 REST preflight 要求目标 contract open orders 为空、position flat。
- 每个样本后 REST final check；任意无法证明 flat 的情况应停止后续样本并输出失败 summary。
- unexpected fill 必须 reduce-only market close，不使用策略信号或 LeadLag sizing 推导数量。
- 若 feedback 不可用或出现 `ContinuityLost`，停止采样并进入 REST cancel / flatten / final check。
- 不复用 LeadLag live strategy 的 `OrderManager` 状态作为安全事实源；账户最终状态以 REST 为准。

## 第一版验证建议

实现后建议先跑：

```bash
ctest --test-dir build/debug -R '(gate_order_session|order_session_config|gate_submit_response_parser)' --output-on-failure
git diff --check
```

live 前先用单个 `connect_ip`、`samples-per-ip=1` 跑最小 smoke，确认：

- 能连到指定 remote endpoint。
- GTC place Ack / cancel Ack / IOC place Ack 都能记录。
- GTC 最终 cancelled，IOC 最终 no fill 或 immediate cancel。
- REST final check 证明 open orders 为空、position flat。
- sample CSV 包含 `connect_ip`、三类 Ack RTT 和状态字段；Nova connection log 包含 remote endpoint、local port 和 owner CPU。

多 IP 采样通过后，再用 `samples-per-ip >= 20` 收集可比较分布；第一版报告只展示分布，不输出自动 score。
