# Bitget OrderSession RTT Probe 设计

- 日期：2026-07-10
- 状态：已实现；已完成 HA 与推断高速 private endpoint 的真实 passive IOC 验证
- 范围：Bitget UTA v3 `OrderSession` 真实 IOC 下单 RTT probe
- 参考：`docs/gate_order_session_rtt_probe_design.md`
- 后续边界：见 `docs/bitget_trading_follow_up.md`

## 1. 目的

新增 `bitget_order_session_rtt_probe`，通过真实 Bitget `OrderSession` 下单路径采集 place operation response RTT，并用独立
`OrderFeedbackSession` 发布到 SHM 的订单事实确认 IOC terminal。工具用于验证 Bitget 下单链路和比较 single / multi-session
连接，不接入 LeadLag，不自动打分、选路或写回生产配置。

第一版必须回答：

- Bitget place request 是否能经 high availability private endpoint 正常获得可关联的 operation response。
- 同一 endpoint 上单连接或多连接的 Ack RTT 分布是否可重复采集。
- operation response 与 `order` topic feedback 是否能按 `local_order_id` 对账，且不会把直接 Ack 误当成 accepted / fill /
  cancel terminal。
- 遇到 reconnect、continuity lost、timeout 或意外成交时，工具是否停止继续采样并留下可审计结果。

## 2. 已有能力与约束

现有 Bitget 生产组件已经提供：

- `exchange/bitget/trading/order_session.h`：login、limit GTC / IOC place、single cancel、request correlation 和 Ack RTT。
- `exchange/bitget/trading/order_feedback_session.h`：订阅 account-wide `order` topic，并向现有 order feedback SHM 发布累计订单事实。
- `core/trading/order_feedback_shm.h`：按 `LocalOrderIdCodec::StrategyId` 路由的 8 lane SPSC channel 和 continuity 语义。
- Bitget `BookTicker` source / fusion SHM，以及 instrument catalog 中的 Bitget price tick、quantity step、最小数量和价格限制。

当前限制：

- Bitget `OrderResponse` 只有 request / response 时间、exchange 时间、Ack RTT、error code 和 connection hash，没有 Gate
  `x_in_time` / `x_out_time` 或 Gate probe 的完整 write-path / socket timestamping 分段。
- Bitget 首次订阅 order topic 不补历史订单，也没有 sequence；运行前仍需要外部 REST baseline 或人工确认 dedicated account flat。
- Bitget REST reconcile、account rate limiter 和工具内 REST guard 尚未实现。
- 实现阶段的出站 IP 白名单阻断已经解除，dedicated account 已完成真实 passive IOC 验证；IP、权限和余额仍是每次 live
  前必须重新查询的外部状态。

## 3. 方案选择

采用 Bitget 专用垂直实现，不先重构 Gate probe：

- 保留 Gate probe 的 CLI、connections CSV、single / multi-session 顺序调度、run directory 和 CSV 采集习惯。
- 只复制或重写 Bitget 第一版实际需要的逻辑，不 include Gate probe 的内部 header，也不让 Bitget 类型伪装成 Gate 类型。
- 不为了消除少量重复而改动已经实测过的 Gate live probe。

未采用的方案：

- 单连接最小 probe：无法满足后续连接 RTT 对照，预计很快需要重写调度和输出。
- 先抽取跨交易所 probe framework：会扩大当前交易主链路改动和 Gate 回归面，收益不足以覆盖第一版风险。

## 4. 文件和组件

新增目录 `tools/bitget/order_session_rtt_probe/`，组件按职责拆分：

- `config.*`：解析 probe TOML，并校验 live safety、timeout、sampling 和 output 参数。
- `connection_plan.*`：解析 connections CSV；`name` 唯一，`connect_ip` 允许为空，非空时必须是 numeric IPv4 / IPv6。
- `passive_order_builder.h`：从 Bitget BBO 和 instrument metadata 构造 probe IOC 价格、数量及 safety-close 价格。
- `sample_id_allocator.h`：为每个样本预留 IOC place 与 safety close 的 `local_order_id`，保持多 session 可反向路由。
- `sample_flow.*`：维护单个样本的 send、operation response、feedback terminal、timeout 和 safety close 状态机。
- `live_runner.*`：在 session owner loop 中读取 BBO、发送订单、消费本 session feedback queue、执行 timeout 和停止逻辑。
- `sample_csv_writer.*`：输出 Bitget 能真实提供的 sample / action 字段。
- `main.cpp`：CLI、config、credential、SHM reader、single / multi-session 生命周期、顺序 coordinator 和 run summary。

构建目标为 `bitget_order_session_rtt_probe`。新增默认配置：

- `config/order_session_rtt_probe/bitget_order_session_rtt_probe.toml`
- `config/order_session_rtt_probe/bitget_order_session_rtt_connections.csv`
- `config/data_readers/bitget_order_session_rtt_probe.toml`
- `config/order_feedback/bitget_order_feedback_shm.toml`

## 5. CLI 和配置

CLI 与 Gate probe 对齐：

```text
--config <path>
--connections-file <path>
--samples-per-session <n>
--duration-sec <seconds>
--live-preflight
--execute
--confirm-dedicated-account
```

语义：

- 默认只解析 config 并打印 run plan，不连接、不下单。
- `--live-preflight` 完成全部静态配置、instrument、SHM path、连接计划和 safety 参数检查，但不连接、不下单。
- `--execute` 必须同时提供 `--confirm-dedicated-account`，否则启动失败。
- `--execute` 只支持 `probe.order.order_mode="ioc"`；不为未实现的 GTC 或 mixed mode 保留伪入口。
- `probe.order.symbol` 显式选择被测内部 symbol，默认 `BTC_USDT`；runner 只缓存该 symbol 的 Bitget BBO，避免多 symbol
  channel 的其他行情覆盖 probe 或 safety-close 价格锚点。
- `samples_per_session` 必须大于 0；`duration_sec` 是整个 run 的硬上限。全部 session 完成目标样本数后可以提前成功退出；未完成目标样本数的提前退出不能算成功。

connections CSV schema 沿用：

```text
name,group,host,connect_ip,port,enable_tls,worker_cpu_id
```

Bitget 默认使用 `vip-ws-uta.bitget.com:443`、TLS 和 `/v3/ws/private`。`connect_ip` 为空时走正常 DNS；指定 numeric IP
时仍使用 `host` 做 TLS SNI 和 WebSocket Host。

## 6. 正常数据流

每个样本的正常路径：

```text
fresh Bitget BookTicker
  -> Bitget instrument metadata
  -> 构造最低数量、远离当前 bid 的 buy limit IOC
  -> OrderSession::PlaceOrder
  -> 记录 operation response Ack RTT
  -> 等待 order feedback Cancelled
  -> 校验 cumulative_filled_quantity == 0
  -> 写 sample CSV
```

正常完成必须同时满足：

- 本地 send status 为 `kOk`。
- 收到 request sequence / request type / `local_order_id` 一致的 `OrderResponseKind::kAck`。
- 收到同一 `local_order_id` 的 `OrderFeedbackKind::kCancelled`。
- `cumulative_filled_quantity == 0`，且 terminal feedback 位于没有 continuity loss 的同一可观测窗口。

operation Ack 只证明直接请求响应成功，不改变 terminal 判定。feedback 先于 Ack 到达时先缓存事实，待 Ack 到达后再完成样本；不得因
跨 connection 的到达顺序不同误判失败。

## 7. 订单构造

### 7.1 Probe IOC

第一版只发送 buy limit IOC：

- `symbol` 使用 instrument 的 `exchange_symbol`。
- `quantity` 使用 instrument `min_quantity`，按 `quantity_decimal_places` 格式化。
- `price` 以最新 Bitget bid 为基准，向下偏移 `price_limit_down * passive_price_limit_fraction`，向下对齐 price tick。
- `passive_price_limit_fraction` 必须在 `(0, 1]`；默认 `0.5`。
- `reduce_only=false`，`time_in_force=IOC`，`margin_mode=crossed`，账户要求 `one_way_mode`。

BBO 必须属于 Bitget、bid / ask 有限且为正、`ask >= bid`，并满足可配置 freshness 上限。无法证明行情新鲜时不发送订单。

### 7.2 Safety close

如果 probe IOC 出现 partial / filled feedback：

- 立即把样本标为 `unexpected_fill` 和无效，不再启动新样本。
- 使用最新有效 BBO 构造 sell reduce-only limit IOC；价格向下越过当前 bid，但仍按 instrument price tick 和配置的价格限制约束。
- close quantity 使用已观察到的累计成交量，按 quantity step 向下对齐；无法构造合法数量时停止并报告 unresolved exposure。
- 只做一次 bounded safety-close attempt；只有收到 close order 的 filled terminal，且累计 close quantity 覆盖已观察成交量，才记录
  `safety_close_confirmed=true`。
- close rejected、cancelled、timeout、continuity lost 或数量不足都以非零状态退出，并明确声明未证明 flat。

Safety close 是故障缓解，不替代 run 前后 REST flat 检查，也不允许工具在异常后继续采样。

## 8. 状态机和乱序处理

单样本主要状态：

```text
Idle
  -> IocSent
  -> AckObserved / TerminalObserved
  -> Completed
```

异常分支：

```text
IocSent
  -> Rejected / UnknownResult / Timeout / ContinuityLost -> Failed
  -> UnexpectedFill -> SafetyCloseSent
  -> SafetyCloseConfirmed | ExposureUnresolved
```

约束：

- Ack 与 terminal feedback 可以任意先后到达，两者都满足后才完成正常样本。
- 重复 Ack 或重复 feedback 不重复完成、不重复写 sample、不重复发送 safety close。
- foreign `local_order_id` 由 router 忽略；属于当前 strategy lane 但无法映射到任何 session / sample 的事件计数并使 run 失败。
- session login-not-ready、connection generation 改变或 feedback continuity lost 会使所有当前 in-flight 样本失效；恢复后也不自动重试这些
  样本。
- 任何 `OrderResponseKind::kUnknownResult` 都不能归类为 reject 或成功，立即停止该 run，等待外部 reconcile。

## 9. Single / multi-session 调度

- 每行 connections CSV 对应一条常驻 Bitget `OrderSession`。
- 每条 session 可独立覆盖 `host`、`connect_ip`、`port`、TLS 和 worker CPU。
- 所有 session login ready 后，coordinator 按 CSV 行顺序授予 dispatch。
- 同一时刻最多有一个 probe sample 处于下单或等待 terminal 状态，避免同账户真实订单重叠。
- `order_session_interval_us` 控制同一 cycle 中相邻 session，`cycle_cooldown_us` 控制完整 cycle 之间的冷却。
- 第一版不接 fanout batch；不同时向多连接广播同一真实订单。

feedback SHM 只 claim 一个 strategy lane。主 feedback reader 按 `local_order_id` 的样本编号把事件投递到对应 session 的有界本地队列。
本地队列满视为 continuity failure，停止 run。

## 10. 输出和可观测性

默认 run directory：

```text
/home/liuxiang/tmp/bitget_order_session_rtt_probe/<run_id>/
```

至少输出：

- `order_session_rtt_samples.csv`
- `order_session_rtt_run_metadata.json`
- `order_session_rtt_connections_observed.csv`

sample CSV 至少包含：

- run / session / group / endpoint / worker CPU
- sample id、`local_order_id`、request sequence、exchange order id
- symbol、side、quantity、price、BBO id 和 BBO local timestamp
- request send、response receive、exchange timestamp、Ack RTT
- response kind、error code、connection id hash
- terminal feedback kind / timestamp / cumulative fill / cancel reason
- invalid、invalid reason、unexpected fill、safety-close status

Bitget 当前没有的 Gate 专属字段不进入 Bitget CSV，不以固定 0 冒充可用证据。新增 log key、stats 或 CSV 字段时同步更新
`docs/diagnostic_fields.md`。

## 11. 启动和退出安全边界

`--execute` 前必须满足：

- 用户显式提供 `--confirm-dedicated-account`。
- credentials 环境变量完整。
- order feedback SHM 已存在且以 open-only 模式 attach；probe 不创建或删除 feedback channel。
- feedback lane claim 成功，且在下单前没有 pending continuity event。
- 所有 session login ready。
- Bitget BBO 新鲜，instrument metadata 完整。

第一版工具内不执行 REST。启动日志必须明确打印：REST preflight / run-end guard 未实现，dedicated account flat 需要外部确认。运行结束只要存在
unknown result、unexpected fill、unresolved close、continuity lost、timeout、session 异常退出或未完成目标 samples，就返回非零。

收到 SIGINT / SIGTERM 时停止发新订单；若已有样本，先在 bounded deadline 内处理 terminal / safety close，再退出。deadline 后仍无法证明
flat 时返回非零。

## 12. 测试和验证

实现使用 gtest 和 TDD，至少覆盖：

- config / CLI guard、connections CSV 和 duplicate name。
- Bitget passive IOC / safety-close price 与 quantity 对齐。
- Ack-first、feedback-first、duplicate message、reject、unknown result 和 timeout。
- zero-fill cancelled 正常完成；partial / fill 触发且只触发一次 safety close。
- safety close filled、rejected、cancelled、timeout 和 continuity lost。
- local order id 到 session 的 single / multi-session 路由。
- sequential coordinator 保证最多一个 active sample。
- feedback local queue 满、foreign event 和 unmapped strategy event。
- dry-run / `--live-preflight` 不连接、不读取 credentials、不发送订单。
- `--execute` 缺少 dedicated-account 确认时拒绝启动。

当前自动化验证还覆盖：同一 strategy lane 的 unmapped feedback fail-fast、已完成样本的 duplicate response / feedback 去重、late fill
仍优先触发 safety close、delayed place Ack 不延长 safety-close deadline，以及 open-only SHM publisher / reader 路由。

自动化集成验证使用测试 publisher / SHM fixture 和 fake session，本身不发送真实订单。实现完成后又单独执行了 guarded live
验证：HA 与推断的高速 private endpoint 均取得 direct Ack 与 zero-fill cancelled terminal feedback 双证据，且运行前后外部 REST
查询均证明无 open orders、无 position。工具内 REST guard、reconcile 和可重复 live 自动化仍未实现，本次外部证据不能替代这些能力。

## 13. 验收条件

- `bitget_order_session_rtt_probe` 能完成 dry-run 和 single / multi-session live preflight。
- fake session + Bitget feedback SHM integration 能证明 normal IOC、乱序、continuity 和 unexpected-fill safety flow。
- Debug / Release 相关 gtest 通过，项目全量测试无回归。
- 真实 passive IOC 验证只把 Ack + terminal feedback + 外部 REST flat 作为本次运行证据，不把 operation response 当作订单终态。
- 文档明确列出 REST reconcile、rate limiter、LeadLag、fanout batch 和可重复 live 自动化仍未完成。

## 14. 实现后的真实验证

2026-07-11 在 dedicated account 上按外部 REST baseline、独立 feedback ready、严格串行 IOC、terminal feedback、run-end REST
flat 的顺序执行：

- 官方 HA endpoint `wss://vip-ws-uta.bitget.com/v3/ws/private`：样本均为 zero-fill cancelled；稳态 Ack median / p90 为
  `5.497/5.605 ms`，send-to-terminal median / p90 为 `6.149/6.424 ms`。
- 推断的高速 endpoint `wss://vip-ws-uta-pri-a.bitget.com/v3/ws/private`：DNS/TLS/login 和真实订单链路均验证通过，样本均为
  zero-fill cancelled；Ack median / p90 为 `2.457/2.622 ms`，send-to-terminal median / p90 为 `2.819/3.175 ms`。
- 两组运行前后外部 REST 查询均无 open orders、无 position；未触发 safety close。

高速 endpoint 尚无公开官方文档确认，当前默认 checked-in config 继续使用官方 HA endpoint。以上数值只描述当次网络与账户环境，
不代表长期 SLA、成交时延或 fillability。重复 live 前仍受 `docs/bitget_trading_follow_up.md` 中跨进程唯一 ID 和恢复边界约束。
