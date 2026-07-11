# Bitget UTA OrderGateway Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 增加与 Gate 一一对应的 Bitget OrderGateway worker/process，并让 LeadLag 使用已有 lag metadata 通过 OrderGateway 向 Bitget 构造订单。

**Architecture:** 复用现有 OrderGateway SHM ABI 和 `OrderGatewayClient`；`bitget_order_gateway` 为每个 route 创建独占线程与 Bitget `OrderSession`，worker 在 session runtime hook 中消费 command 并发布 operation response。订单终态继续由独立 Bitget `OrderFeedbackSession` 提供。

**Tech Stack:** C++20、CMake、gtest、Google Benchmark、toml++、CLI11、Abseil、现有 lock-free SPSC SHM queue。

---

## 文件结构

- Create: `exchange/bitget/trading/multi_order_session_gateway.h` — 与 Gate 同构的 Bitget direct multi-session route adapter。
- Create: `exchange/bitget/trading/order_gateway_worker.h` — Bitget SHM command worker、session callback handler 和 event publisher。
- Modify: `exchange/bitget/trading/order_types.h` — 增加 gateway route adapter 需要的 `kInvalidRoute`。
- Modify: `exchange/bitget/CMakeLists.txt` — 登记新增 Bitget headers。
- Create: `tools/bitget/bitget_order_gateway.cpp` — 与 Gate 同构的 Bitget gateway 进程装配。
- Modify: `tools/CMakeLists.txt` — 增加 `bitget_order_gateway` target。
- Create: `config/order_gateways/bitget_order_gateway.toml` — 单 route 默认 dry-run 配置，引用现有 Bitget OrderSession config。
- Create: `test/exchange/bitget/trading/multi_order_session_gateway_test.cpp` — Gate 对应测试的 Bitget 版本。
- Create: `test/exchange/bitget/trading/order_gateway_worker_test.cpp` — 使用真实 OrderGateway SHM 的 Bitget worker gtest。
- Modify: `test/exchange/bitget/trading/CMakeLists.txt` — 登记新增 gtest targets。
- Modify: `tools/lead_lag/live_strategy.h` — 用 pair lag metadata 替换 Gate execution 硬编码。
- Modify: `test/tools/lead_lag/live_strategy_test.cpp` — 增加 Bitget lag ticker/order metadata 回归。
- Modify: `benchmark/strategy/lead_lag_submit_breakdown_benchmark.cpp` — 仅在现有 benchmark fixture 需要显式 Gate metadata 时保持行为，不新增生产抽象。

### Task 1: Bitget MultiOrderSessionGateway 对齐 Gate

**Files:**
- Create: `exchange/bitget/trading/multi_order_session_gateway.h`
- Modify: `exchange/bitget/trading/order_types.h`
- Modify: `exchange/bitget/CMakeLists.txt`
- Create: `test/exchange/bitget/trading/multi_order_session_gateway_test.cpp`
- Modify: `test/exchange/bitget/trading/CMakeLists.txt`

- [ ] **Step 1: 写失败测试并登记 target**

复制 Gate 测试的 route 选择、round-robin、cancel 回原 route、cache/forget、capacity、readiness 全部场景到 Bitget namespace，订单 symbol 使用 `BTCUSDT`。核心断言包括：

```cpp
using Gateway = MultiOrderSessionGateway<FakeSession>;

TEST(MultiOrderSessionGatewayTest, ExplicitRouteSendsToSelectedSession) {
  std::vector<FakeSession> sessions(4);
  Gateway gateway = MakeGateway(sessions);
  const OrderSendResult sent = gateway.PlaceOrder(MakeOrder(101, 2));
  EXPECT_EQ(sent.status, OrderSendStatus::kOk);
  EXPECT_EQ(sessions[2].placed, std::vector<std::uint64_t>({101}));
}

TEST(MultiOrderSessionGatewayTest, InvalidRouteRejectsWithoutSending) {
  std::vector<FakeSession> sessions(4);
  Gateway gateway = MakeGateway(sessions);
  EXPECT_EQ(gateway.PlaceOrder(MakeOrder(401, 4)).status,
            OrderSendStatus::kInvalidRoute);
}
```

在 Bitget test CMake 中登记：

```cmake
add_executable(bitget_multi_order_session_gateway_test
    multi_order_session_gateway_test.cpp)
target_link_libraries(bitget_multi_order_session_gateway_test
    PRIVATE aquila_bitget aquila_core GTest::gtest_main)
add_test(NAME bitget_multi_order_session_gateway_test
         COMMAND bitget_multi_order_session_gateway_test)
```

- [ ] **Step 2: 构建并确认测试以缺少 header/enum 失败**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target bitget_multi_order_session_gateway_test -j8
```

Expected: FAIL，错误指出缺少 `exchange/bitget/trading/multi_order_session_gateway.h` 或 `OrderSendStatus::kInvalidRoute`。

- [ ] **Step 3: 实现最小 Bitget 对应组件**

在 `OrderSendStatus` 的 `kOrderIdCacheFull` 后加入：

```cpp
kInvalidRoute,
```

`MultiOrderSessionGateway<SessionT>` 的公开 contract 与 Gate 完全一致：

```cpp
struct Config {
  std::size_t min_ready_sessions{1};
  std::size_t route_table_capacity{16384};
};

[[nodiscard]] bool Ready() const noexcept;
[[nodiscard]] std::uint16_t MaxOrderSessionFanout() const noexcept;
[[nodiscard]] bool RouteReady(std::uint16_t route_id) const noexcept;
template <typename OrderT>
[[nodiscard]] OrderSendResult PlaceOrder(const OrderT& order) noexcept;
template <typename OrderT>
[[nodiscard]] OrderSendResult CancelOrder(const OrderT& order) noexcept;
void CacheExchangeOrderId(std::uint64_t local_order_id,
                          std::uint64_t exchange_order_id) noexcept;
void ForgetExchangeOrderId(std::uint64_t local_order_id) noexcept;
```

实现复制 Gate 的 route table、auto-route、rollback 和 readiness 语义，只替换 namespace/include/type。

- [ ] **Step 4: 格式化并运行测试**

Run:

```bash
clang-format -i exchange/bitget/trading/multi_order_session_gateway.h exchange/bitget/trading/order_types.h test/exchange/bitget/trading/multi_order_session_gateway_test.cpp
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target bitget_multi_order_session_gateway_test -j8
build/debug/test/exchange/bitget/trading/bitget_multi_order_session_gateway_test
```

Expected: gtest PASS。

- [ ] **Step 5: 提交**

```bash
git add exchange/bitget/trading/multi_order_session_gateway.h exchange/bitget/trading/order_types.h exchange/bitget/CMakeLists.txt test/exchange/bitget/trading/multi_order_session_gateway_test.cpp test/exchange/bitget/trading/CMakeLists.txt
git commit -m "feat: add Bitget multi order session gateway"
```

### Task 2: Bitget OrderGatewayWorker TDD

**Files:**
- Create: `exchange/bitget/trading/order_gateway_worker.h`
- Create: `test/exchange/bitget/trading/order_gateway_worker_test.cpp`
- Modify: `test/exchange/bitget/trading/CMakeLists.txt`

- [ ] **Step 1: 写与 Gate 对齐的失败 gtest**

测试使用 `OrderGatewayShmManager::Create()` 创建真实 SHM。`MakePlaceCommand()` 设置：

```cpp
command.exchange = Exchange::kBitget;
const std::string_view symbol = "BTCUSDT";
command.order_type = OrderType::kLimit;
command.time_in_force = TimeInForce::kImmediateOrCancel;
```

复刻 Gate worker 的以下场景：place、wrong route、send failure、event queue full、stop、text bounds、response metadata、not-ready clear、cancel/cache/forget、ready/not-ready、lost response。增加 Bitget 必需场景：

```cpp
TEST(OrderGatewayWorkerTest, BitgetSendStatusesMapToCoreReasons) {
  EXPECT_EQ(ToOrderGatewayRejectReason(OrderSendStatus::kOrderIdCacheFull),
            core::OrderGatewayCommandRejectReason::kInflightFull);
  EXPECT_EQ(ToOrderGatewayRejectReason(OrderSendStatus::kInvalidSymbol),
            core::OrderGatewayCommandRejectReason::kEncodeFailed);
  EXPECT_EQ(ToOrderGatewayRejectReason(OrderSendStatus::kInvalidPriceText),
            core::OrderGatewayCommandRejectReason::kEncodeFailed);
  EXPECT_EQ(ToOrderGatewayRejectReason(OrderSendStatus::kInvalidRoute),
            core::OrderGatewayCommandRejectReason::kInvalidCommand);
}

TEST(OrderGatewayWorkerTest, AckConsumesRequestMetadata) {
  // Poll one successful place, publish Ack twice, then assert only the first
  // response carries command_seq/parent_id/send timestamps.
}
```

- [ ] **Step 2: 构建并确认测试因缺少 worker 失败**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target bitget_order_gateway_worker_test -j8
```

Expected: FAIL，错误指出缺少 `exchange/bitget/trading/order_gateway_worker.h`。

- [ ] **Step 3: 实现 publisher、handler 和 command worker**

公开类型保持 Gate 对应命名：

```cpp
struct OrderGatewayRequestMetadata;
[[nodiscard]] core::OrderGatewayCommandRejectReason
ToOrderGatewayRejectReason(OrderSendStatus status) noexcept;
class OrderGatewayWorkerPublisher;
class OrderGatewaySessionEventHandler;
template <typename SessionT>
class OrderGatewayCommandWorker;
```

publisher 映射 Bitget response：

```cpp
event.local_order_id = response.local_order_id;
event.exchange_order_id = response.exchange_order_id;
event.request_sequence = response.request_sequence;
event.local_receive_ns = response.local_receive_ns;
event.exchange_ns = response.exchange_ns;
event.route_id = route_id_;
event.kind = core::OrderGatewayEventKind::kOrderResponse;
event.response_kind = bitget::ToCoreOrderResponseKind(response.kind);
```

找到 metadata 后先复制字段，再无条件 `erase(metadata)`。不要把 `error_code` 填入 `http_status`，不要修改 SHM ABI。command dispatch、bounds 检查、cache/forget 和 publisher failure stop 复制 Gate 行为。

- [ ] **Step 4: 格式化并运行 worker gtest**

Run:

```bash
clang-format -i exchange/bitget/trading/order_gateway_worker.h test/exchange/bitget/trading/order_gateway_worker_test.cpp
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target bitget_order_gateway_worker_test -j8
build/debug/test/exchange/bitget/trading/bitget_order_gateway_worker_test
```

Expected: gtest PASS，Ack 第二次 response 的 metadata 字段为零。

- [ ] **Step 5: 提交**

```bash
git add exchange/bitget/trading/order_gateway_worker.h test/exchange/bitget/trading/order_gateway_worker_test.cpp test/exchange/bitget/trading/CMakeLists.txt
git commit -m "feat: add Bitget order gateway worker"
```

### Task 3: bitget_order_gateway 进程与配置

**Files:**
- Create: `tools/bitget/bitget_order_gateway.cpp`
- Modify: `tools/CMakeLists.txt`
- Create: `config/order_gateways/bitget_order_gateway.toml`

- [ ] **Step 1: 复制 Gate 进程结构并只替换交易所适配**

`PreparedRoute` 使用：

```cpp
struct PreparedRoute {
  aq_config::OrderGatewayRouteConfig route_config;
  aq_bitget::OrderSessionConfig order_session_config;
  aq_bitget::LoginCredentials credentials;
};
```

credentials 读取三个现有 config env name：

```cpp
credentials->api_key = api_key;
credentials->api_secret = api_secret;
credentials->passphrase = api_passphrase;
```

route worker 使用：

```cpp
using Session = aq_bitget::OrderSession<
    aq_bitget::OrderGatewaySessionEventHandler, WebSocketPolicy,
    aq_bitget::NoopOrderSessionDiagnostics>;
using CommandWorker = aq_bitget::OrderGatewayCommandWorker<Session>;
```

保留 Gate 的 signal、CPU affinity、runtime hook、TLS consistency、SHM create、worker start/stop/join 逻辑。CLI 精确对齐：

```cpp
app.add_option("--config", config_path, "order gateway TOML path");
app.add_flag("--connect", connect, "connect order sessions");
app.add_flag("--validate-only", validate_only,
             "Validate config and route session configs without connecting");
app.add_flag("--remove-existing-shm", remove_existing_shm,
             "unlink existing order gateway SHM before create");
app.add_option("--max-runtime-ms", max_runtime_ms, "0 means unlimited");
```

- [ ] **Step 2: 登记 executable 并创建单 route 默认配置**

```cmake
add_executable(bitget_order_gateway bitget/bitget_order_gateway.cpp)
target_link_libraries(bitget_order_gateway PRIVATE aquila_config aquila_core
    aquila_bitget CLI11::CLI11 fmt::fmt-header-only nova PkgConfig::tomlplusplus)
```

默认配置设置 `route_count = 1`，route 引用 `config/order_sessions/bitget_order_session.toml`，不新增 gateway config 字段。

- [ ] **Step 3: 构建并验证 dry-run/validate-only 不创建 SHM**

Run:

```bash
clang-format -i tools/bitget/bitget_order_gateway.cpp
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target bitget_order_gateway -j8
build/debug/tools/bitget_order_gateway --config config/order_gateways/bitget_order_gateway.toml --validate-only
build/debug/tools/bitget_order_gateway --config config/order_gateways/bitget_order_gateway.toml
```

Expected: 两条命令 exit 0，打印 Bitget dry-run route 信息；未设置 credentials 也能成功；`/dev/shm` 中不出现配置的 `shm_name`。

- [ ] **Step 4: 验证 connect 才要求 credentials，但不实际连接**

Run:

```bash
env -u BITGET_TEST_KEY -u BITGET_TEST_SECRET -u BITGET_TEST_PASSPHRASE \
  build/debug/tools/bitget_order_gateway \
  --config config/order_gateways/bitget_order_gateway.toml --connect
```

Expected: exit 1，错误为缺少 API key env；失败发生在创建 SHM 和网络连接前。

- [ ] **Step 5: 提交**

```bash
git add tools/bitget/bitget_order_gateway.cpp tools/CMakeLists.txt config/order_gateways/bitget_order_gateway.toml
git commit -m "feat: add Bitget order gateway process"
```

### Task 4: LeadLag lag exchange/symbol 泛化

**Files:**
- Modify: `test/tools/lead_lag/live_strategy_test.cpp`
- Modify: `tools/lead_lag/live_strategy.h`

- [ ] **Step 1: 写 Bitget lag metadata 失败测试**

在测试 config 中设置：

```cpp
config.pairs[0].lag_exchange = Exchange::kBitget;
config.pairs[0].lag_instrument.exchange = Exchange::kBitget;
config.pairs[0].lag_instrument.exchange_symbol = "BTCUSDT";
```

分别覆盖 `LiveOpenCloseSmokeStrategy`、`LiveUnfilledCancelSmokeStrategy` 和
`LiveSubmitRejectSmokeStrategy`：Gate ticker 不触发，Bitget ticker 触发；捕获的 order 满足：

```cpp
EXPECT_EQ(context.orders[0].exchange, Exchange::kBitget);
EXPECT_EQ(context.orders[0].symbol, "BTCUSDT");
```

- [ ] **Step 2: 运行测试并确认硬编码导致失败**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target lead_lag_live_strategy_test -j8
build/debug/test/tools/lead_lag/lead_lag_live_strategy_test
```

Expected: 新 Bitget case FAIL，因为 strategy 仍过滤 `Exchange::kGate` 或生成 Gate order。

- [ ] **Step 3: 最小替换 Gate execution 硬编码**

三个 live smoke strategy 的 ticker filter 改为：

```cpp
if (ticker.exchange != pair_->lag_instrument.exchange ||
    ticker.symbol_id != pair_->symbol_id) {
  return;
}
```

下单字段改为：

```cpp
.exchange = pair_->lag_instrument.exchange,
.symbol = LagSymbol(),
```

将三个私有 helper 从 `GateSymbol()` 重命名为 `LagSymbol()`，fallback 仍为 `pair_->symbol`。不新增 runtime 类型或配置字段。

- [ ] **Step 4: 运行 Bitget 与既有 Gate 回归**

Run:

```bash
clang-format -i tools/lead_lag/live_strategy.h test/tools/lead_lag/live_strategy_test.cpp
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target lead_lag_live_strategy_test -j8
build/debug/test/tools/lead_lag/lead_lag_live_strategy_test
```

Expected: 新 Bitget cases 与全部原 Gate cases PASS。

- [ ] **Step 5: 提交**

```bash
git add tools/lead_lag/live_strategy.h test/tools/lead_lag/live_strategy_test.cpp
git commit -m "feat: support Bitget lag execution metadata"
```

### Task 5: 组合链路、Release benchmark 与回归

**Files:**
- Modify only if required by compilation: `benchmark/strategy/lead_lag_submit_breakdown_benchmark.cpp`

- [ ] **Step 1: Debug 相关目标与测试**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target \
  bitget_multi_order_session_gateway_test bitget_order_gateway_worker_test \
  bitget_order_gateway lead_lag_live_strategy_test -j8
ctest --test-dir build/debug --output-on-failure -R \
  'bitget_(multi_order_session_gateway|order_gateway_worker)|lead_lag_live_strategy'
```

Expected: 全部 PASS。

- [ ] **Step 2: 完整 Debug 回归**

Run:

```bash
TMPDIR=/home/liuxiang/tmp ./build.sh -n 8 debug
ctest --test-dir build/debug --output-on-failure
```

Expected: 100% tests passed。

- [ ] **Step 3: Release 构建与完整回归**

Run:

```bash
TMPDIR=/home/liuxiang/tmp ./build.sh -n 8 release
ctest --test-dir build/release --output-on-failure
```

Expected: 100% tests passed。

- [ ] **Step 4: Release benchmark**

Run:

```bash
build/release/benchmark/strategy/lead_lag_submit_breakdown_benchmark \
  --benchmark_filter='OrderGateway(ClientPlaceCommandSynthetic|FanoutCurrentPlaceOrder4Routes)' \
  --benchmark_min_time=1s \
  --benchmark_out=/home/liuxiang/tmp/bitget_order_gateway_release_benchmark.json \
  --benchmark_out_format=json
```

Expected: benchmark exit 0，JSON 写入 `/home/liuxiang/tmp`。记录 median/mean 或 benchmark 输出，不在没有 A/B 数据时宣称 Bitget 更快。

- [ ] **Step 5: 边界与格式检查**

Run:

```bash
git diff --check
rg '#include "evaluation/' core exchange tools
rg 'aquila_evaluation' core exchange tools
git status --short
```

Expected: `git diff --check` 成功；两个 evaluation 边界命令无命中；只存在当前任务改动。

若 benchmark fixture 因 lag metadata 泛化需要显式 Gate exchange，只修改 benchmark fixture 初始化并单独提交：

```bash
git add benchmark/strategy/lead_lag_submit_breakdown_benchmark.cpp
git commit -m "test: preserve Gate benchmark metadata"
```

### Task 6: 多轮主会话 Review 与最终验证

**Files:**
- Review: `exchange/bitget/trading/order_session.h`
- Review: `exchange/bitget/trading/order_feedback_session.h`
- Review: `exchange/bitget/trading/order_gateway_worker.h`
- Review: `tools/bitget/bitget_order_gateway.cpp`
- Review: `tools/lead_lag/live_strategy.h`

- [ ] **Step 1: 第一轮逻辑 review**

逐项检查 control flow、request metadata 生命周期、Ack/terminal 边界、route ownership、queue failure、disconnect/not-ready、未知结果和 fake terminal 风险。Critical 必修、Important 默认修；每个修复先增加失败测试，再做最小改动。

- [ ] **Step 2: 第一轮修复验证与提交**

Run 对应最小 gtest；若有修复，使用只包含相关测试/实现的英文原子 commit。若没有问题，记录“第一轮无待修问题”。

- [ ] **Step 3: 第二轮并发与恢复 review**

重新从 session owner thread、runtime hook、SHM producer/consumer、route state memory ordering、reconnect metadata clear、FeedbackSession 独立事实通道检查，不重复第一轮结论。

- [ ] **Step 4: 第二轮修复验证与提交**

按 TDD 修复 Critical/Important；运行 worker、session、feedback、LeadLag 相关测试。无问题时停止多轮 review。

- [ ] **Step 5: verification-before-completion**

重新运行 Task 5 的 Debug/Release 相关测试、完整 `ctest`、Release benchmark、`git diff --check` 和 `git status --short --branch`。只有所有命令获得新鲜成功证据后才报告完成；不启动 `--connect`，不发送真实订单。
