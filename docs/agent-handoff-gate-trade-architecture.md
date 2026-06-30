# Gate 交易 WebSocket 架构接手说明

## 目的

本文是继续 Gate 交易链路时的轻量 handoff。旧的设计讨论、执行计划、实验流水账和逐轮 benchmark 记录已清理；当前事实以本文件、`docs/project_onboarding_guide.md`、`docs/strategy_order_component_model.md` 和代码为准。

新接手时先运行：

```bash
git -C /home/liuxiang/dev/aquila status --short --branch
git -C /home/liuxiang/dev/aquila log --oneline -8
```

然后读取：

```text
AGENTS.md
README.md
docs/project_onboarding_guide.md
docs/evaluation_support.md
docs/strategy_order_component_model.md
```

## 当前状态

- Gate futures SBE BBO 行情链路已落地：schema、generated headers、message dispatch、trusted BBO decode、market data client/session、data session config、probe、tests 和 benchmark。
- Gate `OrderSession` submit/cancel 已落地：WS login、固定缓冲区 request encoder、place/cancel、ack/result/error parse、request correlation、exchange order id cache 和同步 response callback。
- 公共 order / runtime contract 已迁到 `core/trading/*` + `aquila::core`；Gate runtime adapter 在 `exchange/gate/trading/order_session_runtime_adapter.h`。
- Order feedback Task1 / Task2 已落地：fixed lane SHM transport、Gate private `futures.orders` parser、`OrderFeedbackSession`、SHM publish、continuity lost event 和 `OrderManager::OnOrderFeedback()`。
- Trading runtime production loop 已落地：Gate order WS active spin loop 同线程调用 runtime hook，runtime drain feedback SHM，并在 `OrderSession::Ready()` 后 poll data reader。
- `gate_demo_strategy` 已完成 3 轮 BTC_USDT live smoke；LeadLag 已完成 ZEC_USDT 小额 filled open / close 和 unfilled-cancel smoke。
- Decimal-size order contract 已落地：C++ 使用 `double quantity` + `quantity_text`，Gate WS 带 `X-Gate-Size-Decimal: 1` 时把 JSON `size` quote 为 string；REST final check / emergency flatten 也已支持 decimal position residual。
- 多路 `OrderSession` / `MultiOrderSessionGateway` 仍是架构讨论结论，尚未实现；当前推荐 V1 边界见本文“多路 OrderSession 讨论结论”。

## Gate 协议事实

- `futures.login` 用于 Gate futures WebSocket 交易 API 登录。
- `futures.order_place` / `futures.order_cancel` 是上行交易指令；place 可能先返回 `ack=true`，再返回 `ack=false` 的最终结果。
- `ack=true` 只表示 Gate 收到请求，不代表订单已进入订单簿，也不提供最终成交 / 拒绝事实。
- final submit response 才可能提供 `exchange_order_id`；private `futures.orders` 的 `_new` / terminal feedback 才是策略订单状态推进的事实源。
- `futures.orders`、`futures.usertrades`、`futures.positions` 是私有推送频道；V1 order feedback 只使用 `futures.orders`。
- SBE endpoint 可能在同一连接上混合 JSON text frame 和 SBE binary frame；JSON 用于 login / subscribe / ack，SBE binary 用于行情或 private update payload。
- Gate decimal-size 合约需要使用 decimal size header；未 quote 的 JSON number 小数 size 会被 Gate 按 `int64` schema 拒绝。

## 线程和进程边界

当前推荐交易线程模型：

```text
StrategyThread + Gate OrderSession
GateOrderFeedbackThread + Gate OrderFeedbackSession
feedback SHM lane -> StrategyThread
```

生产进程建议：

```text
gate-data-process
  Gate DataSession -> BookTicker SHM

binance-data-process
  Binance DataSession -> BookTicker SHM

gate-feedback-process
  Gate OrderFeedbackSession -> OrderFeedback SHM lanes[8]

strategy-process
  TradingRuntime
    RealtimeDataReader
    Strategy
    OrderManager
    Gate OrderSessionRuntimeAdapter -> Gate OrderSession
    OrderFeedbackShmReader(strategy_id lane)
```

关键取舍：

- Strategy 与 Gate `OrderSession` 同线程，避免下单主路径引入 IPC 或跨线程 command queue。
- Gate `OrderFeedbackSession` 可账号级共享，独立接收 private `futures.orders`，按 `local_order_id >> 56` 路由到固定 SHM lane。
- 每个 strategy process 只 claim 自己的 lane；queue full、producer restart 或 feedback WS 断线通过 `OrderFeedbackKind::kContinuityLost` 通知 strategy。
- `OrderSession::Ready() == false` 是上行交易能力边界；runtime 仍会继续 drain order response / feedback。
- 断线时 `OrderSession` 清空 correlation，不伪造 rejected / cancelled；未知订单状态交给 REST reconcile 或 emergency stop-and-flat。
- Gate `5xx` submit / cancel error 通过 adapter 映射为 core `OrderResponseKind::kUnknownResult`，不是确定 reject；`OrderManager` 保留订单等待 private feedback，LeadLag 标记对应 symbol `needs_reconcile` 并暂停新开仓。若对应 terminal feedback 精确解决该 symbol 的所有 pending unknown order，且没有 global continuity lost / manual intervention 等更高等级 degraded 状态，LeadLag 会自动恢复该 symbol 新开仓。

### 多路 OrderSession 讨论结论

目标是允许策略或上层 OMS / execution 模块把一个策略意图拆成多个独立 child order，并尽可能低 skew 地
fanout 到 N 条 Gate trading WebSocket `OrderSession`，对冲同 IP 不同 connection 的延迟波动和交易所内部排队不确定性，
尝试提高成交率。full-size duplicate、split-size child order、winner 判定、overfill 处理和非赢家 cancel 触发点都属于
策略 / OMS 语义，不属于多路 `OrderSession` 层。

推荐 V1 形态是 `MultiOrderSessionGateway` 作为 `OrderManager` 后面的 composite gateway：

```text
OrderManager
  -> MultiOrderSessionGateway
       RoutePolicy
       RouteTable(local_order_id -> session_index)
       per-session health / ready state
       account-level submit / cancel / pending budget
       OrderSession[0..N-1]
```

关键边界：

- `OrderManager` 仍是唯一订单状态 owner；`MultiOrderSessionGateway` 只实现现有 gateway contract，不拥有订单生命周期。
- 每个 child order 必须有独立 `local_order_id`；full duplicate 也必须表现为多个独立 child order，不要把同一个 `local_order_id` 同时发到多条 session。
- `MultiOrderSessionGateway` 必须记录显式 route table；不要依赖 `local_order_id % N` 作为生产路由事实。
- cancel 默认回到原始下单 session，因为 `OrderSession` 的 request correlation 和 cancel exchange id cache 都是 per-session 状态；跨 session failover cancel 只有在已有 `exchange_order_id` 且经过 Gate live 验证后才考虑。
- `OrderFeedbackSession` 仍保持单条账号级事实源，不按 order session 拆分；private `futures.orders` feedback 继续经 SHM lane 回到 strategy process。
- `Ready()` V1 可定义为至少一条 session ready，也可配置 `min_ready_sessions` 作为 live 前置检查；无论 N 多大，都必须有账号级限流 / pending cap，不能把多连接视为 N 倍账户容量。
- 多路 `OrderSession` 层不解释 parent signal、child group、winner、fillability 或 overfill；这些信息只由策略 / OMS 和 `OrderManager` 的订单事实组合处理。

2026-06-30 已进一步锁定下一版生产 `order-gateway-process` 设计，详见 `docs/gate_order_gateway_shm_design.md`。
核心结论是：strategy 与 order gateway 拆成 2 个进程；strategy 进程 1 个 owner thread，gateway 进程 N 个
`OrderSessionWorker` thread；跨进程用一个 SHM 对象承载 N 路 `command_queue` 和 N 路 `event_queue`；`N` 是运行时参数，
编译期最大值 `16`；启动阶段 strategy 等全部 N 路 `kReady`，`startup_ready_timeout_s` 默认 `30`；运行时 `kNotReady`
对应 route 跳过并限频 warning，重连后 `kReady` 重新参与 fanout。每个 child 仍有唯一 `local_order_id`，同一 fanout batch
共享 `parent_id`，因此不修改 `LocalOrderIdCodec`。

当前最小实现已先落地 `1 thread : n OrderSession` baseline：`exchange/gate/trading/multi_order_session_gateway.h`
运行时接收 session 列表，首个测试和实验配置使用 `n=4`。gateway 使用
`core::StrategyOrder::gateway_route_id` 做显式 route hint，未指定时按 ready session round-robin；该 baseline 只验证
route / cancel / cache / forget 语义，不代表 per-session worker fanout 或 fillability 收益。Ack / final response 仍由各
`OrderSession` 按 `local_order_id` fan-in 到 `TradingRuntime -> OrderManager -> Strategy`；gateway 不解释 Ack，也不判定 winner。

下一版 fanout 目标线程 / 进程模型：

```text
strategy-process
  StrategyOrderOwnerThread
    TradingRuntime
    Strategy / OMS
    OrderManager
    MultiOrderSessionGateway
      RouteTable(local_order_id -> session_index)
    OrderFeedbackShmReader

order-gateway-process
  OrderSessionWorker[0]
    command_queue[0] consumer
    event_queue[0] producer
    OrderSession[0]

  OrderSessionWorker[1]
    command_queue[1] consumer
    event_queue[1] producer
    OrderSession[1]

  ...

  OrderSessionWorker[N-1]
    command_queue[N-1] consumer
    event_queue[N-1] producer
    OrderSession[N-1]

gate-feedback-process
  OrderFeedbackSession -> OrderFeedback SHM
```

即 `OrderManager` 和 route table 仍由 strategy order owner thread 统一拥有；每条 `OrderSessionWorker[i]`
独占一条 `OrderSession[i]` 和对应 trading WebSocket connection。`OrderGatewayClient` 负责把已生成 child order
路由到对应 `command_queue[i]`，并把 cancel / cache / forget 按 route table 发回原 session worker。该模型下
`PlaceOrder()` 的 `kOk` 只表示 command 已进入 gateway queue，不表示已经写到 socket；真实 Ack / final response
必须经 `event_queue[i]` 回到 `TradingRuntime -> OrderManager -> Strategy`。

`1 thread : N OrderSession` 仍保留为 baseline / fallback，用于对照顺序 fanout 的成本。当前架构判断是：因为目标是同一信号
尽量同时写入多条 connection，应先按独立 `order-gateway-process` + per-session worker capability 设计 ownership 和接口；
若后续实测显示 worker queue / wakeup tail 抹掉收益，再回退到单 owner thread backend。

验证顺序建议先复用或扩展 `gate_order_session_rtt_probe` 的多连接能力，记录同一 fanout batch 内各 session 的
`fanout_intent_ns`、`gateway_enqueue_ns`、`worker_dequeue_ns`、`write_start_ns`、`write_complete_ns`、
`ack_receive_ns`、Ack RTT、outlier 和 terminal feedback 路由稳定性。任何“提高成交率 / 降低延迟”的结论都需要
live smoke、benchmark 或 replay/report 证据支撑；没有实测前只能称为更匹配 fanout 目标的架构假设。

## 组件职责

| 组件 | 职责 | 不负责 |
| --- | --- | --- |
| `OrderSession` | Gate WS login、place/cancel 编码发送、ack/result/error parse、request correlation、cancel exchange id cache | 策略状态、持仓事实、REST reconcile |
| `OrderFeedbackSession` | Gate private `futures.orders` login/subscribe、binary feedback parse、发布 `OrderFeedbackEvent` | 访问 `OrderPool`、推进 strategy 状态 |
| `OrderManager` | 本地订单池、`local_order_id`、状态机、response / feedback 应用、exchange order id cache 更新 | Gate wire 编码、交易所报文解析 |
| `OrderSessionRuntimeAdapter` | Gate response kind 到 core response kind 转换、runtime hook 接线 | 长期业务状态、队列、风控 |
| `TradingRuntime` | 组装 data reader、order session、order manager、strategy、feedback reader | 交易所协议细节、REST emergency flatten |
| Strategy | 信号、execution group、策略级订单关系、收到 response / feedback 后更新策略状态 | 通用订单池、exchange parser、账户事实源 |

## 重要 contract

### ID 和 correlation

- `local_order_id` 编码为高 8 bit `strategy_id` + 低 56 bit `strategy_order_id`。
- Gate `text` 使用 `t-<local_order_id>`，feedback router 可以直接由高 8 bit 找到 strategy lane。
- `OrderSession` 内部 `request_sequence -> local_order_id` 只用于轻量 response correlation，不是 pending order table。
- accepted / `_new` feedback 到达后，`OrderManager` 保存 `exchange_order_id`，再通过 adapter 更新 Gate cancel cache。
- terminal feedback 后清理 cancel cache。

### Quantity / decimal-size

- `core::OrderCreateRequest` / `core::StrategyOrder` 使用 `double quantity` + `quantity_text`。
- strategy / risk 使用 `double` 计算数量和 notional；exchange encoder 使用 `quantity_text` 发送真实数量文本。
- Gate decimal-size WS 连接带 `X-Gate-Size-Decimal: 1`，并把 `size` 编码为 JSON string。
- `OrderFeedbackEvent` / SHM / `OrderManager` 使用 `double` 表示累计成交、剩余和撤单数量。
- `kOrderFeedbackShmVersion` 已 bump 到 2；旧 feedback SHM 需要重建。

### Feedback / continuity

- `OrderSession` 的 `ack=true` 不产生 accepted event。
- `finish_as="_new"` 映射为 accepted。
- filled / cancelled / rejected 等 terminal feedback 推进订单终态。
- `ContinuityLost` 是普通 feedback control event；strategy 收到后应暂停新开仓或 handoff，不依赖 shared epoch atomic。
- V1 不新增 account / position realtime feedback session；停机后由 REST final check / emergency flatten 校验真实账户。

### Latency timestamps

`StrategyOrder` 当前记录：

```text
request_send_local_ns
ack_local_receive_ns
response_local_receive_ns
ack_exchange_ns
response_exchange_ns
accepted_exchange_ns
finish_exchange_ns
```

- `request_send_local_ns` 是请求提交给 WebSocket 发送路径前的本地时间，不表示 TCP 帧已经完整写出。
- `ack_exchange_ns` / `response_exchange_ns` 来自 Gate submit response header，优先使用 `x_out_time`，缺失时使用 `response_time`。
- `accepted_exchange_ns` / `finish_exchange_ns` 来自 private `futures.orders` feedback 的交易所更新时间。
- `exchange_update_ns` 仍保留为最后一次已应用 feedback 的兼容字段。
- 日志低频输出在 order response / terminal feedback 等非行情热路径：`gate_order_response`、`gate_order_response_error`、`lead_lag_order_response`、`lead_lag_order_finished` 和 `gate_strategy_order` 工具日志。

## 代码入口

| 范围 | 文件 |
| --- | --- |
| 公共订单类型 | `core/trading/order_types.h`、`core/trading/order_decimal.h`、`core/trading/order_latency.h` |
| Order manager | `core/trading/order_manager.h`、`core/trading/order_pool.h`、`core/trading/strategy_context.h` |
| Feedback SHM | `core/trading/order_feedback_event.h`、`core/trading/order_feedback_shm.h` |
| Runtime | `core/trading/trading_runtime.h` |
| Gate order session | `exchange/gate/trading/order_session.h`、`order_request_encoder.h`、`submit_response_parser.h`、`order_codecs.h` |
| Gate adapter | `exchange/gate/trading/order_session_runtime_adapter.h` |
| Gate feedback | `exchange/gate/trading/order_feedback_parser.h`、`order_feedback_session.h`、`order_feedback_session_config.*` |
| Gate market data | `exchange/gate/market_data/client.h`、`data_session.h`、`subscription_controller.h`、`text_envelope_parser.h` |
| Gate SBE | `exchange/gate/sbe/message_dispatcher.h`、`book_ticker_decoder.h`、`generated/` |
| Tools | `tools/gate/strategy_order.cpp`、`tools/gate/order_feedback_session.cpp`、`tools/gate/order_session_failure_probe.cpp`、`tools/gate/demo_strategy.*` |
| Config | `config/order_sessions/gate_order_session.toml`、`config/order_feedback/gate_order_feedback_session.toml`、`config/order_feedback/gate_order_feedback_shm.toml` |

## 验证入口

常用 focused tests：

```bash
ctest --test-dir build/debug -R '(gate|order_feedback|trading_runtime|strategy_order|order_latency)' --output-on-failure
```

重点 test / benchmark 文件：

```text
test/exchange/gate/trading/order_session_test.cpp
test/exchange/gate/trading/order_request_encoder_test.cpp
test/exchange/gate/trading/submit_response_parser_test.cpp
test/exchange/gate/trading/order_feedback_parser_test.cpp
test/exchange/gate/trading/order_feedback_session_test.cpp
test/exchange/gate/trading/order_session_runtime_adapter_test.cpp
test/core/trading/order_feedback_shm_test.cpp
test/core/trading/order_latency_test.cpp
benchmark/exchange/gate/trading/order_session_benchmark.cpp
benchmark/exchange/gate/trading/order_feedback_parser_benchmark.cpp
benchmark/core/trading/order_feedback_shm_benchmark.cpp
```

Live probe / smoke 只在明确允许真实连接或真实下单时运行：

```bash
./build/release/tools/gate_order_feedback_session --config config/order_feedback/gate_order_feedback_session.toml --connect
./build/release/tools/gate_strategy_order --config config/order_sessions/gate_order_session.toml --execute
./build/release/tools/gate_demo_strategy --execute
./build/release/tools/gate_order_session_failure_probe --execute
```

## 未完成 / 下一步

- REST reconcile / resume：feedback WS 断线或 unknown window 后恢复订单事实、本地状态和人工介入边界。
- Failure response live evidence：`gate_order_session_failure_probe` 已有诊断入口，但安全 IOC、zero-size submit 和 nonexistent cancel 探测未收到最终 failure response；继续前先确认 Gate 可返回最终 error 的安全请求形态。
- 更完整的 submit 前 risk / metadata check：tick、quantity、notional、reduce-only、price band 等。
- Batch place、amend、cancel-all 和 order status/list 仍未接入。
- Account / position realtime feedback 是 V2 可选能力，不作为当前 LeadLag V1 前置项。
- 真实链路性能结论必须通过 benchmark、profile 或 live probe 重新验证；不要从历史 microbenchmark 推断公网交易延迟。

## 相关文档

- `docs/project_onboarding_guide.md`
- `docs/strategy_order_component_model.md`
- `docs/data_session_config.md`
- `docs/data_reader_config.md`
- `docs/futures_contract_metadata_fields.md`
- `docs/websocket_read_write_benchmark_comparison.md`
- `docs/websocket_client_future_optimizations.md`
- `docs/lead_lag_reconcile_design.md`
- `docs/lead_lag_live_runtime_plan.md`
