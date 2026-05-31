# Gate OrderSession RTT Probe 设计

## 目的

本文记录 Gate `OrderSession` 多连接 RTT 测量和后续连接寻优方向的当前设计。第一版目标是 measurement-only：对多组
`connect_ip` 采集真实订单路径 RTT，写入结构化 log / CSV，暂不自动打分、暂不自动切换 live 策略使用的
`OrderSession`。

最终目标是为后续 order session 连接寻优提供证据：找到 Ack RTT 更低、更稳定的 Gate WebSocket 连接，并为未来
`1 hot primary + low-duty scout` 的动态旁路测量和安全切换打基础。

## 当前结论

- 第一版新增独立 C++ tool `gate_order_session_rtt_probe`；不改 LeadLag live runtime，不接入生产策略下单路径。
- 当前已落地 V1a dry-run scaffold 和 live sample 前置纯逻辑：`tools/gate/order_session_rtt_probe/`、
  `config/order_session_rtt_probe/gate_order_session_rtt_probe.toml` 和 `gate_order_session_rtt_probe_test`。已覆盖配置解析、
  connections CSV 读取、重复 `connect_ip` 保留、single-session / multi-session run plan、pinned `connect_ip` order session
  config 派生、passive order 构造、
  GTC place -> cancel -> IOC place 的 Ack 状态流转、GTC cancel reject 后立即派发 reduce-only close、sample executor
  订单派发、sample `local_order_id` lane 分配和 sample CSV schema / writer；sample flow 已保存每个 Ack 的
  `*_ack_receive_local_ns` 和 stage status，并校验 Ack / final response 的 `local_order_id` 必须匹配当前 stage 的
  expected id；纯状态机已覆盖 GTC cancel reject、feedback fill / timeout 后进入 reduce-only close、close Ack 后等待
  terminal feedback、close timeout fail 和 close filled terminal confirmation；配置已支持 `probe.order.order_mode =
  "ioc" | "gtc" | "ioc+gtc"`，以及同一 cycle 内 order session 之间的 `probe.sampling.order_session_interval_ms`
  非阻塞 pacing。连接维度已从 TOML `candidate_ip_file` / `endpoint_overrides` 迁到
  `config/order_session_rtt_probe/gate_order_session_rtt_connections.csv`。
- 第一版只采集数据，不输出自动 score，不选择最优连接，不写回生产配置。
- RTT 主指标来自真实 Gate order session order Ack：`request_send_local_ns -> gate_order_response kind=kAck.local_receive_ns`。
- 不使用 WebSocket handshake RTT、login RTT 或 no-order probe RTT 作为第一版主指标；这些路径不等价于 `futures.order_place`。
- `TCP_INFO` 只作为辅助诊断，用于解释网络层 RTT、RTT variance 和 retrans，不直接作为寻优主指标。
- 默认 `ioc+gtc` 模式下每个样本按长表 action 行采集三条主 RTT：`gtc/open`、`gtc/cancel`、
  `ioc/open` 的 `ack_rtt_ns`。`ioc` / `gtc` 模式只跑对应子路径，用于单独压测或缩小风险面。
- GTC 和 IOC 均使用下单前最新 Gate BBO 推导出的非成交 passive price；quantity 使用 instrument catalog 中的最小可下单量并按 step / decimal 约束格式化。
- live safety path 设计上优先走 WebSocket reduce-only market close：GTC cancel reject / terminal 不确定时立即补 close；IOC
  place Ack 后因没有 cancel，也立即补 close。无仓位时 Gate reduce-only close 可能被拒绝；只有在 terminal feedback、
  REST 或 position-known-flat 能证明 flat 时，该 reject 才能标记为预期 `rejected_flat_safe`。当前代码仍是
  Ack-driven sample executor，已在纯状态机层接入 GTC cancel reject、feedback fill / timeout safety transition 和
  close terminal 确认；尚未把 feedback reader、REST guard 或真实 single-session sample 接到 live executor，这些仍是启用
  `--execute` 前的 blocker。
- REST 不作为正常 cycle 的逐样本 final flat；REST 只做 run start preflight、run end 整体账户兜底和 fatal / continuity-lost 场景。run end 如发现持仓或 open order，使用市价 reduce-only close / cancel 处理到 flat。

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
| `ack_rtt_ns` where `type=gtc, action=open` | GTC place 的 `send_ns -> ack_recv_ns` | 观察普通 place 请求路径。 |
| `ack_rtt_ns` where `type=gtc, action=cancel` | GTC cancel 的 `send_ns -> ack_recv_ns` | 观察 cancel 请求路径。 |
| `ack_rtt_ns` where `type=ioc, action=open` | IOC place 的 `send_ns -> ack_recv_ns` | 观察更接近生产 IOC place 的 Ack 路径。 |
| `tcp_info_rtt_us` / `tcp_info_rttvar_us` / retrans | `TCP_INFO` snapshot | 辅助解释网络层状态，不作为主排名。 |
| `ack_rtt_ns` where `action=close` | safety reduce-only close 的 `send_ns -> ack_recv_ns` | 安全审计字段，不参与连接 RTT 排名。 |
| `exchange_lifecycle_ns` / `ack_to_finish_local_ns` | IOC terminal feedback / report | 辅助观察 terminal lifecycle，不解释 Ack RTT。 |

后续分析时三组 Ack RTT 分开统计，例如 count、p50、p90、p99、max、timeout、reject、unexpected fill。第一版不定义
score，也不按 score 输出“最优连接”结论。

## 统计目标

第一版数据采集要支持下面五类问题，但统计计算放在离线分析脚本或后续 report 中完成，不放入下单热路径：

| 目标 | 分组维度 | 统计口径 |
| --- | --- | --- |
| 单个指定 IP 的 Ack RTT 分布 | `ip + type + action` | 对 valid sample 计算 count、avg、p50、p90、p99、max。 |
| 过去 N 秒 rolling Ack RTT | `ip + type + action + rolling_window_sec` | 按 `ack_recv_ns` 滚动聚合；默认建议先用 `300s`，分析脚本允许改成 `60s` / `600s`。 |
| 同一指定 IP 在不同 symbol 上是否不同 | `ip + contract + type + action` | 同一 `ip` 下按 contract 分组比较三类 Ack RTT。 |
| 同一指定 IP 在不同时间是否稳定 | `ip + type + action + time_window` | 与 rolling 统计同一套数据口径；重点看 p90 / p99 / max 是否漂移。 |
| 同一指定 IP 断开重连后是否变化 | planned connection log + `ip + type + action` | 同一 IP 每次重新建连递增 generation 后再比较不同 generation 的分布。 |

这里的主统计 action 固定为 `type=gtc,action=open`、`type=gtc,action=cancel`、`type=ioc,action=open`。第一版不把三类
Ack RTT 混成一个总 RTT，以免掩盖 place、cancel 和 IOC path 的差异。

## Probe 订单形态

第一版不是固定只测 `ZEC_USDT`；probe 由 Gate `BookTicker` 行情事件触发。收到行情后，以该行情对应的 symbol 作为本轮
cycle 的 contract。若当前已有 cycle 在运行，新到行情只更新 latest BBO，不排队新的 cycle，避免行情频率高于下单节奏时积压。

每次实际发 place 前都读取该 symbol 的最新 Gate BBO，并按 instrument catalog 计算不会成交的 passive buy price：

```text
max_deviation = price_limit_down * passive_price_limit_fraction
raw_price = best_bid * (1 - max_deviation)
price = floor_to_tick(raw_price)
quantity = instrument catalog 中最小下单量，按 quantity_step / decimal places 对齐
```

`probe.order.order_mode` 控制本轮 sample 的订单形态：

- `ioc`：只发 IOC place，Ack 后进入 IOC reduce-only close safety path。
- `gtc`：只发 GTC place，Ack 后 cancel，cancel Ack 后 sample 完成；cancel reject / fill / timeout 仍进入 GTC close safety path。
- `ioc+gtc`：默认值，先跑 GTC place / cancel，再跑 IOC place；`gtc+ioc` 作为配置别名解析为同一模式。

`probe.sampling.order_session_interval_ms` 控制同一个 cycle 内不同 order session 下单之间的间隔：

- `0`：发完当前 order session 后，下一条 Gate `BookTicker` 行情到来时再给下一个 order session 下单。
- 非 `0`：发完当前 order session 后记录一个 `N ms` deadline；主事件循环后续检查到点后，直接使用当时最新 BBO 给下一个
  order session 下单，不额外要求再等一条新行情，也不 sleep / busy loop。

`passive_price_limit_fraction` 第一版默认 `0.5`，即使用 catalog 中 `price_limit_down` 的 50% 作为最大下行偏离。
默认使用 buy side，原因是意外成交时仓位方向明确，可以用 reduce-only sell market close 处理。如果某个 symbol 缺少有效
`price_limit_down`、`price_tick` 或 BBO，跳过该行情触发并记录 skip reason。

第一版每个 cycle 使用当前 active group 的 `N=8` 个指定 IP / `OrderSession`，按两轮执行：

```text
GTC round:
  对 group 内 8 个 OrderSession 依次执行：
    1. 读取该 symbol 最新 BBO。
    2. 计算 `price_text` / `quantity_text`。
    3. 发送 GTC place，写入 `gtc/open` action 行并记录 `ack_rtt_ns`。
    4. 收到 GTC place Ack 后立即 cancel，不等待额外 cooldown。
    5. 写入 `gtc/cancel` action 行并记录 `ack_rtt_ns`。
    6. 若 cancel rejected、cancel timeout、terminal 不确定或 feedback 显示 fill / partial fill，立即发送 reduce-only sell market close。
       若账户无仓位，Gate 对 reduce-only close 的 reject 视为 flat-safe safety result。

IOC round:
  对同一 group 内 8 个 OrderSession 依次执行：
    1. 重新读取该 symbol 最新 BBO。
    2. 计算 `price_text` / `quantity_text`。
    3. 发送 IOC place，写入 `ioc/open` action 行并记录 `ack_rtt_ns`。
    4. 收到 IOC place Ack 后立即发送 reduce-only sell market close；若账户无仓位，Gate 对 reduce-only close 的 reject
       视为 flat-safe safety result。
    5. 等 IOC terminal feedback / close terminal feedback，用于标记样本安全状态；不把 terminal lifecycle 混入 Ack RTT。

cycle 完成：
  汇总 sample / feedback / close safety 状态。
  然后进入 cycle-level cooldown，第一版默认 cooldown_ms=500。
```

cooldown 只作用于完整 cycle 之后，不作用于单个 order 之间。一个 `N=8` 的 cycle 包含 8 个 GTC place、8 个 GTC cancel 和
8 个 IOC place 请求；按用户口径是 8 个 GTC place + 8 个 IOC place 共 16 个 place order 后 cooldown `500ms`。

若任何阶段出现 fill / partial fill：

```text
1. 停止当前样本后续普通 probe。
2. 立即通过同一个 OrderSession 提交 reduce-only sell market close。
3. 若 reduce-only close 被 Gate rejected 且没有仓位，这是预期 flat-safe 结果；若 close accepted / filled，等待 terminal feedback。
4. 将样本标记 `fill=true` / `invalid=true`。
```

REST 兜底时机：

```text
run start:
  REST preflight，要求 probe scope / dedicated account 没有旧 open orders 和旧仓位。

normal cycles:
  不做逐 cycle REST final flat；风险处理优先由 WebSocket reduce-only close 完成。

fatal / continuity-lost / safety close timeout:
  停止后续普通采样，进入 REST cancel / flatten / poll-until-flat。

run end:
  REST 查询整体账户 open orders / positions；如有 open order 或持仓，cancel open orders 并用市价 reduce-only close 平仓，
  直到 REST 证明账户 flat 或超时失败。
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
`aquila.gate.order_session.ip_discovery.v1`。脚本仍可输出只包含
`selected_for_rtt_probe=true` IP 的 `candidate_ips.txt`，但 RTT probe 当前不直接消费该文件；需要把目标连接整理成
connections CSV 后再运行 probe：

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

当前仓库默认 RTT probe 连接文件为
`config/order_session_rtt_probe/gate_order_session_rtt_connections.csv`。该 CSV 的每一行是一条独立
`OrderSession` 连接，字段为 `name,group,host,connect_ip,port,enable_tls,worker_cpu_id`；行顺序就是 session
顺序，`name` 必须唯一，`connect_ip` 允许重复。重复 `connect_ip` 表示对同一目标 IP 建立多条独立连接，用于比较相同
remote endpoint 下不同连接 / owner CPU 的 Ack RTT 分布。

### RTT Probe

当前 V1a dry-run 命令形态：

```bash
./build/debug/tools/gate_order_session_rtt_probe \
  --config config/order_session_rtt_probe/gate_order_session_rtt_probe.toml
```

V1a 中 `probe.feedback.*`、`probe.safety.*` 只做配置解析和边界校验，尚未执行 feedback reader、REST preflight / final flat。
`probe.output.*` 的 sample CSV schema / writer 已落地并有本地测试。为避免下一步 live sample
误用不安全配置，parser 已要求
`probe.feedback.enabled`、`probe.safety.preflight_rest_check`、`probe.safety.run_end_rest_check`、
`probe.safety.stop_on_continuity_lost`、`probe.safety.confirm_dedicated_account` 和 `probe.order.reduce_only_close`
在 V1a 都必须为 `true`。`run_id` 为空时工具启动期会生成 `gate_order_session_rtt_probe_<ns>`。
`probe.order.order_mode` 支持 `ioc`、`gtc`、`ioc+gtc`；`probe.sampling.order_session_interval_ms` 为非负 `uint32`，
默认 `0`。dry-run / live-preflight 输出会打印当前 `order_mode` 和 `order_session_interval_ms`。

当前 sample executor 仍以 order Ack path 为主：它可以记录 GTC place / GTC cancel / IOC place Ack RTT，并保存 Ack 接收时间和
stage status；同时已处理 GTC cancel final reject、feedback fill / timeout 后派发 reduce-only close、close Ack 不直接结束
sample、close terminal filled 后完成 sample，以及 safety close rejected / timeout 在没有 flat 证明时让 sample 失败。下一步启用
真实下单前，仍必须把 feedback reader、REST guard 和 single-session live order sample 串入 live executor；REST /
position-known-flat 证明尚未接入，不能只凭 close Ack 结束 sample。

当前另有无副作用的 single-session live plan 预检模式：

```bash
./build/debug/tools/gate_order_session_rtt_probe \
  --config config/order_session_rtt_probe/gate_order_session_rtt_probe.toml \
  --live-preflight
```

`--live-preflight` 会读取 connections CSV、生成 run plan、加载 base Gate order session config，并构造 pinned `connect_ip`
session config、run output dir、sample CSV 和 REST guard 路径；它不连接 WebSocket、不启动 feedback reader、不执行 REST、
不下单。单行 CSV 生成 single-session 预检，多行 CSV 生成 multi-session 预检。

默认配置把连接维度放在 CSV 中，TOML 只保留运行参数：

```text
connections_file = config/order_session_rtt_probe/gate_order_session_rtt_connections.csv
samples_per_session = 1
cycle_cooldown_ms = 500
```

可用下面的 override 临时切换连接列表或样本数：

```bash
./build/debug/tools/gate_order_session_rtt_probe \
  --connections-file /home/liuxiang/tmp/rtt_probe_connections.csv \
  --samples-per-session 20
```

live measurement 命令形态：

```bash
./build/release/tools/gate_order_session_rtt_probe \
  --config config/order_session_rtt_probe/gate_order_session_rtt_probe.toml \
  --connections-file /home/liuxiang/tmp/rtt_probe_connections.csv \
  --samples-per-session 20 \
  --execute
```

第一版可以沿用 base order session config 中的 `host`、`port`、TLS、credentials、decimal-size header 和 log 设置；
`tools/gate/order_session_rtt_probe/session_config_builder.h` 已提供 pinned config builder。当前每行 connection CSV 会覆盖
`host`、`connect_ip`、`port`、`enable_tls` 和 `worker_cpu_id`；`worker_cpu_id=-1` 表示沿用 base order session config。
若要在同一次 multi-session probe 中混测 public TLS 和 private plain link，直接在 CSV 中写不同行即可。建议默认打开
`order_session.diagnostics.enable_tcp_info=true` 的临时副本，但不要修改仓库默认配置。probe 的 contract 仍由最新 Gate `BookTicker`
行情事件决定，不通过命令行固定为单个 symbol。

输出最终分两类；当前仅 sample CSV schema / writer 已落地，connection log 仍是 live executor 待实现项：

1. 连接级一次性信息使用 Nova 普通结构化 log，事件名建议为 `gate_order_session_rtt_probe_connection`：

   ```text
   run_id connect_ip order_session_id remote_ip remote_port local_ip local_port owner_thread_cpu contract
   ```

2. sample 级统计使用 Quill `CsvWriter` + `nova::LogManager::NovaFrontendOptions` 异步写入 CSV。CSV 是长表：
   每个已提交 order action 一行，`type` 取 `gtc` / `ioc`，`action` 取 `open` / `cancel` / `close`；
   place 统一写作 `open`，`gtc,close` 表示 GTC leg 触发的 safety close。字段集只保留 RTT 分析必须字段和建议诊断字段，
   并使用短字段名降低 CSV 宽度。

   ```text
   run,ip,sid,round,sample,contract,qty,price,type,action,local_id,req_seq,
   bbo_id,bbo_ns,send_ns,ack_recv_ns,ack_ex_ns,ack_ex2local_ns,ack_rtt_ns,
   resp_recv_ns,resp_ex_ns,resp_ex2local_ns,resp_rtt_ns,
   status,term_fb,fill,invalid,inv_reason
   ```

`tools/gate/order_session_rtt_probe/sample_csv_writer.*` 已按上面 schema 使用 Quill `CsvWriter` 落地；live executor 只在 sample
完成后调用 `CsvWriter::append_row()`，不在单个 order send -> Ack 计时窗口内调用。普通 Nova log 仍保留用于运行排障；CSV 是
RTT 分布分析的主产物。第一版不输出 JSONL。

3. REST guard 使用独立 CSV 和 raw JSON，不写入 sample 行：

   ```text
   order_session_rtt_rest_guard.csv:
   run_id,phase,cycle_index,contract,exit_code,ok,result,initial_open_orders,initial_positions,
   final_open_orders,final_positions,polls,json_path

   raw_rest/:
   preflight_flat.json
   fatal_flatten_<cycle_index>_<contract>.json
   run_end_flat.json
   ```

   `phase` 固定为 `preflight`、`fatal_flatten` 或 `run_end`。正常 cycle 不生成 REST guard 行。

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
overall_rtt_summary.csv      # ip + type + action 的 count / avg / p50 / p90 / p99 / max
rolling_rtt_summary.csv      # ip + type + action + rolling window 的 rolling stats
symbol_rtt_summary.csv       # ip + contract + type + action
reconnect_rtt_summary.csv    # planned connection log + ip + type + action
```

所有统计默认只使用 `invalid=false` 且对应 action status 为 acked 的样本。若 run end REST 兜底未能证明
账户整体 flat 或完成 flatten，整次 run 的 RTT 分布必须标记为需要人工复核，不直接用于选优。

## 采样顺序

为了减少时间窗口偏差，不建议按 IP 一次跑完所有样本。当前实现按 connections CSV 的完整连接集合执行每个 cycle：
每条连接各提交一个 sample，完成一个行情触发 cycle 后进入 cooldown，再重复到 `samples_per_session` 次。

```text
cycle 1: connections.csv row[0..N-1]
cycle 2: connections.csv row[0..N-1]
...
```

同一时刻只运行一个 cycle。cycle 内按 session 串行发送请求，不并发下单，避免多个 probe 同时向同一账户下单，也避免订单回报和
REST 复核互相干扰。

## 线程模型选项

### 方案 A：每个活跃 OrderSession 一个 owner thread

这是第一版 measurement tool 的推荐默认方案。connections CSV 的每一行启动一条 `OrderSession`，由自己的 owner
thread 运行现有 `BasicWebSocketClient::Start()` active loop。coordinator 通过每个 worker 的 command queue 串行下发
probe command；place / cancel 实际在该 session owner thread 的 runtime hook 中执行。

优点：

- 符合现有 `OrderSession` / `BasicWebSocketClient` owner-thread 设计。
- 不需要把 WebSocket client 改成可 step 的 multi-session reactor。
- 不把单线程 multiplex 轮询延迟混入候选连接 RTT。
- 多条连接可以同时 logged-in / ready，减少候选之间因重连带来的时间窗口差。

限制：

- 每个 active-spin session 占用一个线程和 CPU 时间。
- 第一版应限制 CSV 行数，避免抢占 strategy / market data / feedback 的关键 CPU。
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
- 启动前 REST preflight 要求 probe scope / dedicated account open orders 为空、position flat。
- 正常 cycle 不做逐样本 REST final flat；GTC cancel reject / terminal 不确定和 IOC place Ack 后的安全处理由 WebSocket
  reduce-only market close 立即执行。
- unexpected fill 必须立即 reduce-only market close，不使用策略信号或 LeadLag sizing 推导数量。
- 若 feedback 不可用、safety close timeout 或出现 `ContinuityLost`，停止采样并进入 REST cancel / flatten / poll-until-flat。
- run end 必须做一次 REST 整体账户检查；若仍有 open order 或持仓，cancel open orders 并用市价 reduce-only close 平仓。
- 不复用 LeadLag live strategy 的 `OrderManager` 状态作为安全事实源；账户最终状态以 REST 为准。

## 第一版验证建议

实现后建议先跑：

```bash
cmake --build build/debug --target gate_order_session_rtt_probe gate_order_session_rtt_probe_test
ctest --test-dir build/debug -R gate_order_session_rtt_probe_test --output-on-failure
build/debug/tools/gate_order_session_rtt_probe \
  --config config/order_session_rtt_probe/gate_order_session_rtt_probe.toml
build/debug/tools/gate_order_session_rtt_probe \
  --config config/order_session_rtt_probe/gate_order_session_rtt_probe.toml \
  --live-preflight
build/debug/tools/gate_order_session_rtt_probe \
  --config config/order_session_rtt_probe/gate_order_session_rtt_probe.toml \
  --connections-file /home/liuxiang/tmp/does_not_exist_for_execute_guard.csv
ctest --test-dir build/debug -R '(gate_order_session|order_session_config|gate_submit_response_parser)' --output-on-failure
git diff --check
```

其中缺失 connections CSV smoke 期望 exit code `1`，且不能访问网络或账户。

live 前先用单行 connections CSV、`samples-per-session=1` 跑最小 smoke，确认：

- 能连到指定 remote endpoint。
- GTC place Ack / cancel Ack / IOC place Ack 都能记录。
- GTC cancel reject / terminal 不确定时会立即补 reduce-only close；IOC Ack 后会立即补 reduce-only close。
- 无仓位时 reduce-only close rejected 只有在 terminal feedback、REST 或 position-known-flat 证明 flat 后，才被标记为
  flat-safe safety result；否则该 sample 必须失败并进入人工复核 / REST guard。
- run end REST 证明整体账户 open orders 为空、position flat；如不 flat，工具会 cancel / 市价 reduce-only close 并 poll 到 flat。
- sample CSV 包含 `ip`、三类 Ack RTT、safety close 状态和样本状态字段；Nova connection log 包含 remote endpoint、local port 和 owner CPU。

多 IP 采样通过后，再用 `samples-per-ip >= 20` 收集可比较分布；第一版报告只展示分布，不输出自动 score。
