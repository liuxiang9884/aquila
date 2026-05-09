# Demo strategy runtime implementation plan

## Goal

实现一个生产可运行的 strategy runtime：

- 从 `market_data::DataReader<>` 读取行情。
- 通过 Gate private order session 实盘下单、撤单。
- 从 order feedback SHM 读取回报，并回调到 user strategy。
- 在 `tools/` 下提供可执行的 `demo` 测试策略，但本轮不做实盘测试。

`demo` 策略行为：

- 读取行情后，以当前 ask price 下 1 手 `BTC_USDT` buy limit 单。
- 只接受 Gate `BTC_USDT` 行情，不用 Binance 同 symbol_id 的 tick 驱动 Gate 下单。
- 等待 `wait_seconds` 秒。
- 如果 buy 已成交，则发 market sell reduce-only 平仓。
- 如果 buy 未成交，则撤单。
- 完成 `rounds` 轮后退出。
- `wait_seconds` 和 `rounds` 来自策略参数配置。

## Current Status

已实现并验证，未做实盘测试。当前 production Gate path 采用同线程 hook mode：
`GateOrderSessionAdapter::Start()` 在 StrategyThread 内运行 Gate `OrderSession`，`SetRuntimeHook()`
让 runtime 在 WebSocket active spin loop 内 poll feedback / data，`BindRuntime()` 让 Gate response
handler 同线程直接调用 `StrategyRuntime::OnOrderResponse()`。production path 不再保留
`GateOrderResponseQueue`、background order session thread 或 command queue。关键入口：

- `core/strategy/strategy_runtime.h`
- `tools/gate/strategy_runtime_adapter.h`
- `tools/gate/demo_strategy.h`
- `tools/gate/demo_strategy.cpp`
- `config/strategies/demo_strategy.toml`
- `config/strategies/demo.toml`

## Task 1: Core StrategyRuntime

文件范围：

- `core/strategy/strategy_runtime.h`
- `core/strategy/strategy_context.h`（仅当 runtime 控制接口确实需要）
- `test/core/strategy/strategy_runtime_test.cpp`

实现内容：

1. 给 `StrategyRuntime<UserStrategyT, OrderSessionT, DataReaderT = market_data::DataReader<>>` 增加生产 `Create(...)`。
   - 输入 `config::StrategyConfig`。
   - 输入已解析的 `config::DataReaderConfig`。
   - 输入 `OrderSessionT` factory。
   - 构造 `data_reader_`、`order_session_`、`order_manager_`、`context_`、`user_strategy_`。
   - 如果 `[strategy.feedback].enabled = true`，打开 order feedback SHM 并 claim 当前 `strategy_id` lane。
2. 实现 `Run()` 主循环。
   - 启动 order session。
   - 如果 `OrderSessionT` 支持 `SetRuntimeHook()`，使用同线程 hook mode 在 order session active loop 内轮询 runtime。
   - 如果 `OrderSessionT` 支持 `BindRuntime()`，让 response handler 同线程直接进入 `OnOrderResponse()`。
   - 兼容测试 session 可以继续通过 fallback `PollOrderResponses(runtime)` 轮询 order response。
   - 轮询 feedback reader。
   - order session ready 后轮询 data reader。
   - 支持 `spin` / `yield` idle policy。
   - 支持 `max_loop_seconds = 0` 表示无限运行。
   - 支持 user strategy 可选 hook：`OnStart(context)`、`OnIdle(context)`、`OnStop(context)`、`ShouldStop()`。
3. 对外暴露 handler 方法。
   - `OnBookTicker(const BookTicker&)`
   - `OnOrderResponse(const OrderResponseEvent&)`
   - `OnOrderFeedback(const OrderFeedbackEvent&)`
   - 这些方法在 runtime 线程内调用，先更新 `OrderManager`，再调用 user strategy。
4. 保持热路径约束。
   - market data hot path 不做动态分配。
   - Gate production path 不引入 response queue、command queue、background order session thread 或每个 tick 的额外锁。
5. 测试覆盖。
   - fake data reader 能驱动 `OnBookTicker`。
   - fake order session 能驱动 order response。
   - `ShouldStop()` 能停止 loop。
   - order session 未 ready 前不消费行情。

## Task 2: Gate order session adapter

文件范围：

- `tools/gate/strategy_runtime_adapter.h`
- `test/tools/gate/strategy_runtime_adapter_test.cpp`

实现内容：

1. 新增 Gate-specific adapter，作为 runtime 的 `OrderSessionT`。
2. adapter 内部持有 Gate `OrderSession<ResponseHandler, WebSocketPolicy, Diagnostics>`。
3. `Start()` 同线程运行 Gate order session，不创建后台线程。
4. `Stop()` 停止 session；无需 join。
5. `PlaceOrder(...)` / `CancelOrder(...)` 直接转发到同线程 Gate order session。
6. Gate response handler 只做轻量转换和 dispatch。
   - `BindRuntime(runtime)` 保存 runtime 指针和无捕获 dispatch 函数。
   - handler 把 Gate order response 转成 `strategy::OrderResponseEvent`。
   - handler 同线程调用 `runtime.OnOrderResponse(...)`，由 runtime 先更新 `OrderManager` 再调用 user strategy。
   - 不使用 production response queue 或 `PollOrderResponses()`。
7. `SetRuntimeHook(context, hook)` 转发给 Gate `OrderSession`，让 `StrategyRuntime` 在 WebSocket active loop 内 poll feedback / data。
8. adapter 暴露 `Ready()`。
   - Gate login ready 后返回 true。
   - runtime 只在 ready 后消费行情，避免策略在 private session 未登录时下单。
9. 测试覆盖 response type 转换、同步 runtime dispatch、ready flag 和 move-only，不连接真实 Gate。

## Task 3: Demo user strategy

文件范围：

- `tools/gate/demo_strategy.h`
- `tools/gate/demo_strategy.cpp`（如工具入口需要，也可以拆出 `demo_strategy_tool.cpp`）
- `test/tools/gate/demo_strategy_test.cpp`

实现内容：

1. 定义 `DemoStrategyConfig`。
   - `contract = "BTC_USDT"`
   - `symbol_id = 0`
   - `wait_seconds = n`
   - `rounds = m`
2. 从 `config/strategies/demo.toml` 读取 `[demo]`。
3. `DemoStrategy` 状态机：
   - `WaitingTicker`
   - `BuyPending`
   - `CancelPending`
   - `ClosePending`
   - `Done`
   - `Error`
4. 下 buy limit。
   - 只处理 `ticker.exchange = Gate` 且 `ticker.symbol_id = symbol_id` 的行情。
   - `side = Buy`
   - `quantity = 1`
   - `price_text = ask price`
   - `time_in_force = GTC`
   - `reduce_only = false`
5. 到期处理。
   - buy 已 filled：发 sell market reduce-only 平仓。
   - buy 未 filled：发 cancel。
6. 完成一轮后进入下一轮，直到 `rounds` 完成。
7. `ShouldStop()` 在完成 `rounds` 轮或进入不可恢复错误后返回 true。
8. 价格字符串存储必须保证生命周期覆盖 `OrderManager` 内部 `string_view`，不能引用临时字符串。
9. 测试覆盖：
   - 收到行情后下 buy limit。
   - buy filled 后到期发 close market。
   - buy 未 filled 到期发 cancel。
   - 达到 `rounds` 后停止。

## Task 4: Tool and config

文件范围：

- `tools/gate/demo_strategy.cpp`
- `tools/CMakeLists.txt`
- `config/strategies/demo_strategy.toml`
- `config/strategies/demo.toml`

实现内容：

1. 新增可执行工具 target，建议名：`gate_demo_strategy`。
2. 默认 dry-run 只解析配置并打印关键参数，不连接 Gate，不打开 SHM，不下单。
3. 必须显式传入 `--execute` 才运行实盘链路。
4. 实盘链路：
   - 加载 strategy runtime config。
   - 加载 demo strategy config。
   - 加载 data reader config。
   - 加载 Gate order session config 和凭证。
   - 创建 `StrategyRuntime<DemoStrategy, GateOrderSessionAdapter<...>>`。
   - 调用 `Run()`。
5. 配置文件：
   - `config/strategies/demo_strategy.toml`：runtime 配置。
   - `config/strategies/demo.toml`：user strategy 参数。

## Task 5: Review, validation, docs, commit

验证命令：

```bash
./build.sh debug
./build/debug/test/core/strategy/strategy_runtime_test
./build/debug/test/tools/gate/strategy_runtime_adapter_test
./build/debug/test/tools/gate/demo_strategy_test
./build/debug/tools/gate_demo_strategy --config config/strategies/demo_strategy.toml
git diff --check
```

如果 `build.sh debug` 过慢，可以先用 `cmake --build build/debug --target ...` 做局部验证，再补跑必要目标。

Review 轮次：

1. 结构 review：确认 core 不依赖 Gate，Gate adapter 不进入 core，tool 层负责具体交易所组合。
2. 逻辑 review：确认订单生命周期、filled/cancel/close 分支、停止条件和 runtime ready gating。
3. 性能 review：确认行情 hot path 没有 response queue 锁、动态分配或格式化；demo 策略只在下单时格式化价格。
4. 冗余 review：删除未使用 helper、重复转换函数和无价值日志。

收尾：

- 更新 `doc/project_onboarding_guide.md` 和 Gate/strategy runtime 相关 handoff 文档。
- 提交代码和文档，commit message 使用英文。
- 不 push，除非用户明确要求。
