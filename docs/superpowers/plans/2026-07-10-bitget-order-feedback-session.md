# Bitget UTA OrderFeedbackSession Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现 Bitget UTA v3 单路 `OrderFeedbackSession`，把 account-wide `order` topic 的累计订单事实安全映射并发布到现有 `OrderFeedbackShmPublisher`。

**Architecture:** 生产链路拆成无状态 JSON parser、authenticated WebSocket session 和现有 SHM publisher 三层。parser 负责 ownership、必需字段、scope、status/quantity/timestamp 映射；session 负责 login/subscribe/heartbeat/reconnect、同 generation decode continuity latch 和发布诊断；REST baseline/reconcile、`fill`/`fast-fill`、LeadLag 与真实订单保持在范围外。

**Tech Stack:** C++20、CMake、simdjson on-demand、fmtlib、GoogleTest、Google Benchmark、现有 WebSocket client 与 `OrderFeedbackShmPublisher`

---

## 文件结构

- Create: `exchange/bitget/trading/order_feedback_parser.h`：JSON order envelope/record 的 ownership、校验、映射和 parser stats。
- Create: `exchange/bitget/trading/order_feedback_session.h`：login/subscribe/heartbeat/reconnect 状态机、continuity latch、publisher 接线和 session stats。
- Create: `exchange/bitget/trading/order_feedback_session_config.h`
- Create: `exchange/bitget/trading/order_feedback_session_config.cpp`：V1 scope、private endpoint、credential env 与 SHM ABI 配置 fail-fast。
- Create: `config/order_feedback/bitget_order_feedback_session.toml`：high availability private endpoint 与现有 feedback SHM 参数。
- Create: `tools/bitget/bitget_order_feedback_session.cpp`：dry-run 和 login/subscribe-only probe，不提供 place/cancel 命令。
- Create: `test/exchange/bitget/trading/order_feedback_parser_test.cpp`
- Create: `test/exchange/bitget/trading/order_feedback_session_test.cpp`
- Create: `test/exchange/bitget/trading/order_feedback_session_config_test.cpp`
- Create: `test/exchange/bitget/trading/order_feedback_shm_integration_test.cpp`
- Create: `benchmark/exchange/bitget/trading/order_feedback_parser_benchmark.cpp`
- Modify: `exchange/bitget/CMakeLists.txt`
- Modify: `test/exchange/bitget/trading/CMakeLists.txt`
- Modify: `benchmark/exchange/bitget/trading/CMakeLists.txt`
- Modify: `tools/CMakeLists.txt`
- Modify: `docs/diagnostic_fields.md`
- Modify: `README.md`
- Modify: `docs/project_onboarding_guide.md`

不修改 `core/trading/order_feedback_event.h`、`core/trading/order_feedback_shm.h`、SHM version/layout 或用户 dirty 的 `core/market_data/types.h`。

### Task 1: JSON parser contract 和 ownership 快路径

**Files:**
- Create: `test/exchange/bitget/trading/order_feedback_parser_test.cpp`
- Create: `exchange/bitget/trading/order_feedback_parser.h`
- Modify: `test/exchange/bitget/trading/CMakeLists.txt`

- [ ] **Step 1: 写 ownership、envelope 和 accepted 映射的失败测试**

测试使用 padding-aware helper 调用 parser，并固定以下输入：

```cpp
constexpr std::string_view kAccepted = R"({
  "action":"snapshot",
  "arg":{"instType":"UTA","topic":"order"},
  "data":[{"category":"usdt-futures","orderId":"9988",
    "clientOid":"a-72057594037927978","qty":"1.5",
    "holdMode":"one_way_mode","marginMode":"crossed",
    "cumExecQty":"0","avgPrice":"0","orderStatus":"new",
    "updatedTime":"1750034397076"}],"ts":1750034397080})";
```

覆盖并精确断言：

```cpp
EXPECT_EQ(result.status, OrderFeedbackParseStatus::kOk);
ASSERT_EQ(events.size(), 1U);
EXPECT_EQ(events[0].kind, OrderFeedbackKind::kAccepted);
EXPECT_EQ(events[0].local_order_id, 72057594037927978ULL);
EXPECT_EQ(events[0].exchange_order_id, 9988U);
EXPECT_DOUBLE_EQ(events[0].left_quantity, 1.5);
EXPECT_EQ(events[0].exchange_update_ns, 1750034397076000000LL);
EXPECT_EQ(events[0].role, OrderRole::kNone);
```

再覆盖空/非 object root、错误 `instType`/`topic`/`action`、非 array `data`、foreign/missing `clientOid`、malformed `a-` 和 `a-18446744073709551615`。

- [ ] **Step 2: 注册 test target 并确认 RED**

Run:

```bash
cmake --build build/debug --target bitget_order_feedback_parser_test -j8
```

Expected: compile fail，提示 `exchange/bitget/trading/order_feedback_parser.h` 不存在。

- [ ] **Step 3: 实现 parser 公共 contract 和第一层 ownership**

公共类型固定为：

```cpp
enum class OrderFeedbackParseStatus : std::uint8_t {
  kOk,
  kInvalidJson,
  kUnexpectedEnvelope,
  kDecodeUnrecoverable,
};

struct OrderFeedbackParseResult {
  OrderFeedbackParseStatus status{OrderFeedbackParseStatus::kUnexpectedEnvelope};
  std::uint32_t orders_seen{0};
  std::uint32_t events_emitted{0};
  bool continuity_lost{false};
};

template <typename EventSink, typename DiagnosticSink>
OrderFeedbackParseResult ParseBitgetOrderFeedbackMessage(
    std::string_view payload, std::uint32_t readable_tail_bytes,
    std::int64_t local_receive_ns, simdjson::ondemand::parser& parser,
    OrderFeedbackParserStats& stats, EventSink&& event_sink,
    DiagnosticSink&& diagnostic_sink) noexcept;
```

实现规则：root/`arg.instType`/`arg.topic`/`action`/`data` 任一不可读即 `continuity_lost=true`；record 先只读 `clientOid`，明确 foreign/missing/empty 只增加 `foreign_orders_ignored` 或 `unroutable_orders_ignored`，`a-` prefix 但 `ClientOidCodec::Parse()` 失败才返回 `kDecodeUnrecoverable`。

- [ ] **Step 4: 运行 parser test 确认第一组 GREEN**

Run:

```bash
cmake --build build/debug --target bitget_order_feedback_parser_test -j8
./build/debug/test/exchange/bitget/trading/bitget_order_feedback_parser_test \
  --gtest_filter='*Envelope*:*Ownership*:*Accepted*'
```

Expected: selected tests PASS。

- [ ] **Step 5: 写 status、数量、scope、cancel reason 和 batch 的失败测试**

测试矩阵必须逐项断言：

```text
new               -> kAccepted, cum=0
partially_filled  -> kPartialFilled, 0<cum<qty
filled            -> kFilled, abs(cum-qty)<=epsilon
cancelled         -> kCancelled, cancelled=qty-cum
canceled          -> kCancelled, legacy_canceled_statuses +1
```

必需字段缺失/类型错误、非 finite/负数/溢出、`orderId==0`、`qty<=0`、`cumExecQty<0`、`cumExecQty>qty+epsilon`、成交时 `avgPrice<=0`、`updatedTime<=0`/ns overflow、scope mismatch 和未知 status 全部断言 `kDecodeUnrecoverable`。取消原因逐项覆盖 `normal_cancel`、`ioc_not_full_cancel`、`self_trade_cancel`、`stp_cancel`、`adl_cancel`、`burst_cancel`、`penetrate_cancel` 和 unknown fallback；batch 覆盖两条 Aquila record 与 foreign record 混合。

- [ ] **Step 6: 实现必需字段和 semantic mapping**

实现 record 中间态：

```cpp
struct ParsedOrderRecord {
  std::uint64_t local_order_id{0};
  std::uint64_t exchange_order_id{0};
  double quantity{0.0};
  double cumulative_filled_quantity{0.0};
  double average_price{0.0};
  std::int64_t exchange_update_ns{0};
  std::string_view status{};
  std::string_view cancel_reason{};
};
```

数量 epsilon 使用：

```cpp
constexpr double kQuantityEpsilon = 1e-12;
const double left = std::max(0.0, quantity - cumulative);
```

字符串数值通过 `fast_float::from_chars` 解析到 finite `double`；ID/timestamp 同时接受 JSON string 和整数，millisecond 乘 `1'000'000` 前检查 `int64_t` overflow。普通 event 始终写 `role=kNone`，绝不产生 `kRejected`。

- [ ] **Step 7: 运行完整 parser test 并提交 parser 闭环**

Run:

```bash
cmake --build build/debug --target bitget_order_feedback_parser_test -j8
ctest --test-dir build/debug -R '^bitget_order_feedback_parser_test$' --output-on-failure
git diff --check
```

Expected: PASS，`git diff --check` 无输出。

Commit:

```bash
git add exchange/bitget/trading/order_feedback_parser.h \
  test/exchange/bitget/trading/order_feedback_parser_test.cpp \
  test/exchange/bitget/trading/CMakeLists.txt
git commit -m "feat: add Bitget order feedback parser"
```

### Task 2: Session login/subscribe、heartbeat 和 continuity latch

**Files:**
- Create: `test/exchange/bitget/trading/order_feedback_session_test.cpp`
- Create: `exchange/bitget/trading/order_feedback_session.h`
- Modify: `test/exchange/bitget/trading/CMakeLists.txt`

- [ ] **Step 1: 写状态机和 publisher 失败测试**

复用 Bitget `OrderSession` plain policy 测试方式，`RecordingPublisher` 暴露：

```cpp
bool Publish(const OrderFeedbackEvent& event) noexcept;
bool PublishGlobalContinuityLost(OrderFeedbackContinuityReason reason,
                                 std::int64_t local_receive_ns) noexcept;
```

覆盖：active 发送 login；只有 active generation 的 login success 才发送固定 subscription；只有匹配 `event=subscribe`、`code=0`、`arg.instType=UTA`、`arg.topic=order` 才 ready；stale ACK、login/subscribe error、`30033`、ping/pong、heartbeat timeout、disconnect/reconnect；publisher 返回 false 仍返回 `DeliveryResult::kAccepted`。

- [ ] **Step 2: 注册 session target 并确认 RED**

Run:

```bash
cmake --build build/debug --target bitget_order_feedback_session_test -j8
```

Expected: compile fail，提示 `order_feedback_session.h` 不存在。

- [ ] **Step 3: 实现 session 状态机和 control parser**

session 对外接口固定为：

```cpp
template <typename Publisher,
          typename WebSocketPolicy = OrderFeedbackSessionDefaultTlsWebSocketPolicy,
          typename Diagnostics = NoopOrderFeedbackSessionDiagnostics>
class OrderFeedbackSession {
 public:
  bool Start() noexcept;
  void Stop() noexcept;
  websocket::DeliveryResult Handle(const websocket::MessageView&) noexcept;
  void OnConnectionPhase(websocket::ConnectionPhase) noexcept;
  [[nodiscard]] bool Ready() const noexcept;
  [[nodiscard]] bool active() const noexcept;
  [[nodiscard]] bool login_ready() const noexcept;
  [[nodiscard]] bool subscribed() const noexcept;
};
```

login 复用 `EncodeLoginRequest()`；subscribe 固定编码为：

```json
{"op":"subscribe","args":[{"instType":"UTA","topic":"order"}]}
```

`kActive` 只在 `active_==false` 时建立新 generation 并发送 login，避免 degraded recovery 重复登录。application text `ping`/`pong` 与 timeout 语义复用现有 Bitget `OrderSession`。

- [ ] **Step 4: 写 decode/disconnect continuity latch 的失败测试**

断言首次连接 active 不发布 continuity；active 后 disconnect 广播一次 `kSessionDisconnected`；同一 generation 连续两条 malformed Aquila update 只广播一次 `kDecodeUnrecoverable`；reconnect 后新 generation 再次 malformed 可再次广播；foreign record 和 optional diagnostics 缺失不触发 continuity。

- [ ] **Step 5: 实现 continuity latch 与 diagnostics**

session 保存：

```cpp
std::uint64_t connection_generation_{0};
bool decode_continuity_lost_published_{false};
```

每次新 active generation reset latch；parser `continuity_lost=true` 时只在 latch 为 false 时调用 `PublishGlobalContinuityLost(kDecodeUnrecoverable, now)` 并置 true。disconnect 只在 `active_before` 为 true 时广播 `kSessionDisconnected`。stats 精确包含设计列出的 login、subscribe、order、validation、publish、decode/disconnect continuity 和 heartbeat 计数。

- [ ] **Step 6: 运行 session test 并提交 session 闭环**

Run:

```bash
cmake --build build/debug --target bitget_order_feedback_session_test -j8
ctest --test-dir build/debug -R '^bitget_order_feedback_session_test$' --output-on-failure
git diff --check
```

Expected: PASS。

Commit:

```bash
git add exchange/bitget/trading/order_feedback_session.h \
  test/exchange/bitget/trading/order_feedback_session_test.cpp \
  test/exchange/bitget/trading/CMakeLists.txt
git commit -m "feat: add Bitget order feedback session"
```

### Task 3: Config 与 login/subscribe-only probe

**Files:**
- Create: `exchange/bitget/trading/order_feedback_session_config.h`
- Create: `exchange/bitget/trading/order_feedback_session_config.cpp`
- Create: `test/exchange/bitget/trading/order_feedback_session_config_test.cpp`
- Create: `config/order_feedback/bitget_order_feedback_session.toml`
- Create: `tools/bitget/bitget_order_feedback_session.cpp`
- Modify: `exchange/bitget/CMakeLists.txt`
- Modify: `test/exchange/bitget/trading/CMakeLists.txt`
- Modify: `tools/CMakeLists.txt`

- [ ] **Step 1: 写 config RED tests**

基准 TOML 必须含 `category=usdt-futures`、`position_mode=one_way_mode`、`margin_mode=crossed`、三个 credential env name、`/v3/ws/private`、TLS/443 和：

```toml
[order_feedback_session.shm]
shm_name = "aquila_bitget_order_feedback"
channel_name = "orders"
max_strategy_count = 8
queue_capacity = 65536
create = true
remove_existing = false
```

逐项断言 scope mismatch、missing credential、public/wrong target、non-TLS/non-443、lane count/queue capacity mismatch 和 `remove_existing=true, create=false` 失败；checked-in config 成功。

- [ ] **Step 2: 实现 config parser 并运行测试**

`OrderFeedbackSessionConfig` 包含 `name/category/position_mode/margin_mode/connection/credentials/shm`；SHM 类型直接复用 `config::OrderFeedbackShmRuntimeConfig`。Run:

```bash
cmake --build build/debug --target bitget_order_feedback_session_config_test -j8
ctest --test-dir build/debug -R '^bitget_order_feedback_session_config_test$' --output-on-failure
```

Expected: PASS。

- [ ] **Step 3: 实现 dry-run / login-subscribe-only probe**

CLI 只提供：

```text
--config <path>
--connect
--duration-s <positive>
```

dry-run 打印非敏感 config；`--connect` 才读取三个 env、创建 SHM、启动 session 并限时停止。工具不暴露 place/cancel，summary 明确输出 `ready`、login/subscribe/heartbeat/reconnect、parser/publisher/continuity stats。

- [ ] **Step 4: dry-run 验证并提交 config/probe**

Run:

```bash
cmake --build build/debug --target bitget_order_feedback_session -j8
./build/debug/tools/bitget_order_feedback_session \
  --config config/order_feedback/bitget_order_feedback_session.toml
git diff --check
```

Expected: exit 0；不读取 credentials、不连接、不创建/删除 SHM。

Commit:

```bash
git add exchange/bitget/trading/order_feedback_session_config.h \
  exchange/bitget/trading/order_feedback_session_config.cpp \
  exchange/bitget/CMakeLists.txt \
  test/exchange/bitget/trading/order_feedback_session_config_test.cpp \
  test/exchange/bitget/trading/CMakeLists.txt \
  config/order_feedback/bitget_order_feedback_session.toml \
  tools/bitget/bitget_order_feedback_session.cpp tools/CMakeLists.txt
git commit -m "feat: add Bitget feedback session probe"
```

### Task 4: Parser-to-SHM integration

**Files:**
- Create: `test/exchange/bitget/trading/order_feedback_shm_integration_test.cpp`
- Modify: `test/exchange/bitget/trading/CMakeLists.txt`

- [ ] **Step 1: 写真实 `OrderFeedbackShmPublisher` integration tests**

heap-allocate `OrderFeedbackShmChannel` 并调用 `InitializeLaneHeaders()`。使用 `LocalOrderIdCodec::Encode(3, 42)` 构造 `clientOid`，通过 parser/session 发布后断言只有 lane 3 可 pop；调用 `PublishGlobalContinuityLost()` 后断言 8 个 lane 都收到相同 sequence/scope/reason；填满单 lane 后断言 pending `kLaneQueueFull` 在 drain 后 flush。

- [ ] **Step 2: 运行 SHM integration 与现有 core SHM 回归**

Run:

```bash
cmake --build build/debug --target \
  bitget_order_feedback_shm_integration_test core_order_feedback_shm_test -j8
ctest --test-dir build/debug \
  -R '^(bitget_order_feedback_shm_integration_test|core_order_feedback_shm_test)$' \
  --output-on-failure
```

Expected: PASS；不改变 SHM magic/version/ABI/layout。

- [ ] **Step 3: 提交 integration test**

```bash
git add test/exchange/bitget/trading/order_feedback_shm_integration_test.cpp \
  test/exchange/bitget/trading/CMakeLists.txt
git commit -m "test: cover Bitget feedback SHM integration"
```

### Task 5: Release microbenchmark

**Files:**
- Create: `benchmark/exchange/bitget/trading/order_feedback_parser_benchmark.cpp`
- Modify: `benchmark/exchange/bitget/trading/CMakeLists.txt`

- [ ] **Step 1: 写 benchmark cases**

固定、预先 padding 的 payload，循环内不分配 payload，分别注册：

```cpp
BENCHMARK(BenchmarkAccepted);
BENCHMARK(BenchmarkPartialFilled);
BENCHMARK(BenchmarkTerminal);
BENCHMARK(BenchmarkForeignOwnershipFastPath);
BENCHMARK(BenchmarkMalformedAquilaContinuityPath);
BENCHMARK(BenchmarkTypicalBatchPublish);
BENCHMARK(BenchmarkParserToShmPublisherAndDrain);
```

每次 iteration 重置 parser state/stats；SHM case 预先创建 channel，publish 后立即从目标 lane pop，避免 queue growth 污染样本。

- [ ] **Step 2: 构建并运行固定 CPU release baseline**

Run:

```bash
TMPDIR=/home/liuxiang/tmp ./build.sh release
taskset -c 16 ./build/release/benchmark/exchange/bitget/trading/bitget_order_feedback_parser_benchmark \
  --benchmark_repetitions=3 \
  --benchmark_report_aggregates_only=true \
  --benchmark_out=/home/liuxiang/tmp/bitget_order_feedback_parser_benchmark.json \
  --benchmark_out_format=json
```

Expected: all benchmark cases complete；报告只称为当前机器 release baseline，不宣称相对收益或 live feedback latency。

- [ ] **Step 3: 提交 benchmark target**

```bash
git add benchmark/exchange/bitget/trading/order_feedback_parser_benchmark.cpp \
  benchmark/exchange/bitget/trading/CMakeLists.txt
git commit -m "bench: add Bitget feedback parser baseline"
```

### Task 6: Diagnostic contract、入口文档和最终验证

**Files:**
- Modify: `docs/diagnostic_fields.md`
- Modify: `README.md`
- Modify: `docs/project_onboarding_guide.md`

- [ ] **Step 1: 同步 diagnostic fields**

新增 Bitget OrderFeedbackSession 小节，逐项记录设计要求的 log key 和 stats；明确 `Ready()` 仅表示 login+subscribe、未知 `cancelReason` 不触发 continuity、disconnect/decode continuity 需要外部 reconcile、credential 和完整 login payload 禁止写日志。

- [ ] **Step 2: 更新 README 与 onboarding 摘要**

README 增加 dry-run/login-subscribe-only 命令和边界；onboarding 把“尚未实现”改为实际入口、验证命令、benchmark artifact 和尚缺 REST baseline/reconcile、rate limiter、LeadLag、真实订单 smoke，不复制完整实现历史。

- [ ] **Step 3: 运行 focused 和全量验证**

Run:

```bash
TMPDIR=/home/liuxiang/tmp ./build.sh debug
ctest --test-dir build/debug -R '^bitget_(order_feedback|order|operation)' --output-on-failure
ctest --test-dir build/debug --output-on-failure
rg '#include "evaluation/' core exchange tools
rg 'aquila_evaluation' core exchange tools
git diff --check
git status --short --branch
```

Expected: build/test PASS；两个 evaluation boundary `rg` 无输出；`git diff --check` 无输出；`core/market_data/types.h` 仍是唯一不属于本任务的 user dirty 文件。

- [ ] **Step 4: 提交文档并复核提交边界**

```bash
git add docs/diagnostic_fields.md README.md docs/project_onboarding_guide.md
git commit -m "docs: document Bitget feedback session"
git show --stat --oneline HEAD
git status --short --branch
```

Expected: 文档提交不包含 `core/market_data/types.h`；本任务所有提交均在当前 branch，未自动 push。
