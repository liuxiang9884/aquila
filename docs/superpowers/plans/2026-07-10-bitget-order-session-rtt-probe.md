# Bitget OrderSession RTT Probe Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 新增 Bitget single / multi-session 真实 IOC 下单 RTT probe，用 operation response 测 Ack RTT，并用 feedback SHM 确认 zero-fill cancelled terminal 和异常成交 safety close。

**Architecture:** 在 `tools/bitget/order_session_rtt_probe/` 做 Bitget 专用垂直实现，复用现有 Bitget `OrderSession`、`RealtimeDataReader`、instrument catalog 和 order feedback SHM，不修改 Gate probe。每条 session 在独立 owner thread 中运行，主 coordinator 顺序授予唯一 dispatch，单个 feedback reader 按编码后的 `local_order_id` 路由到有界 session queue。

**Tech Stack:** C++20、CMake、gtest、CLI11、toml++、csv-parser、fmtlib、Abseil、现有 WebSocket / SHM runtime。

---

## 文件结构

新增 `tools/bitget/order_session_rtt_probe/`：`config.*`、`connection_plan.*`、`run_plan.h`、`session_config_builder.h`、`passive_order_builder.h`、`sample_id_allocator.h`、`sample_flow.h`、`local_feedback_queue.h`、`sequential_coordinator.h`、`sample_csv_writer.*`、`live_runner.h` 和 `main.cpp`。测试集中到 `test/tools/bitget/order_session_rtt_probe/order_session_rtt_probe_test.cpp`；默认配置放到 `config/order_session_rtt_probe/` 和 `config/data_readers/`。

测试文件匿名 namespace 先定义本计划各段共用的 deterministic fixtures：`MakeBitgetTicker(100.0, 100.1)` 返回 exchange=`kBitget`、id=`7`、有效 realtime `local_ns` 的 BBO；`MakeBitgetInstrument()` 返回 `exchange_symbol="BTCUSDT"`、tick=`0.1`、price decimals=`1`、quantity step/min=`0.0001`、quantity decimals=`4`、price limit down=`0.05`；`MakeIds()` 返回 strategy 7 的 place / close IDs；`MakeSent()` / `MakeAck()` / `MakeAckFor(order)` 使用一致的 request sequence 和 local order ID；`MakeCancelled(fill)` / `MakeCancelledFor(id)` / `MakePartialFill(fill)` 填充对应 feedback。`MakeRunnerConfig()`、`MakePlan()` 和 `MakeCatalog()` 返回 one-sample Bitget fixtures。SHM tests 在 `/home/liuxiang/tmp` 创建唯一 name 的 `OrderFeedbackShmManager` fixture，并定义 `encoded_place_id`、`runners` 和 deterministic `NowNs()`。`WriteConnectionsCsv` 和 `ReadFile` 只在 `/home/liuxiang/tmp` 创建或读取 test fixture。

### Task 1: Bitget OrderSession connection observation

**Files:**
- Modify: `exchange/bitget/trading/order_types.h`
- Modify: `exchange/bitget/trading/order_session.h`
- Modify: `test/exchange/bitget/trading/order_session_test.cpp`

- [ ] **Step 1: 写失败的 optional callback test**

```cpp
struct ConnectionRecordingHandler {
  std::vector<bitget::OrderSessionConnectionInfo> connections;
  void OnOrderSessionConnected(
      const bitget::OrderSessionConnectionInfo& info) noexcept {
    connections.push_back(info);
  }
};

TEST(BitgetOrderSessionTest, ReportsConnectionObservationOnActive) {
  ConnectionRecordingHandler handler;
  TestSession session(MakeConnectionConfig(), MakeCredentials(), handler);
  session.OnConnectionPhase(websocket::ConnectionPhase::kActive);
  ASSERT_EQ(handler.connections.size(), 1U);
  EXPECT_GE(handler.connections.front().owner_thread_tid, 0);
}
```

- [ ] **Step 2: 运行 RED**

```bash
cmake --build build/debug --target bitget_order_session_test -j2
./build/debug/test/exchange/bitget/trading/bitget_order_session_test \
  --gtest_filter='BitgetOrderSessionTest.ReportsConnectionObservationOnActive'
```

Expected: compile failure，`OrderSessionConnectionInfo` 尚不存在。

- [ ] **Step 3: 实现 cold-path callback**

```cpp
struct OrderSessionConnectionInfo {
  int owner_thread_cpu{-1};
  int owner_thread_tid{-1};
  bool endpoint_available{false};
  std::string local_ip;
  std::uint16_t local_port{0};
  std::string remote_ip;
  std::uint16_t remote_port{0};
};
```

active cold path 使用 `sched_getcpu()`、`SYS_gettid` 和 `websocket::SnapshotSocketEndpointDiagnostics(client_.Core().NativeFd())` 填充；用 requires-expression 可选调用 `OnOrderSessionConnected(info)`，不改 place / response hot path。

- [ ] **Step 4: GREEN 并提交**

```bash
cmake --build build/debug --target bitget_order_session_test -j2
./build/debug/test/exchange/bitget/trading/bitget_order_session_test
git add exchange/bitget/trading/order_types.h exchange/bitget/trading/order_session.h \
  test/exchange/bitget/trading/order_session_test.cpp
git commit -m "feat: expose Bitget order session connection info"
```

### Task 2: Config、connections CSV 和 run plan

**Files:**
- Create: `tools/bitget/order_session_rtt_probe/config.h`
- Create: `tools/bitget/order_session_rtt_probe/config.cpp`
- Create: `tools/bitget/order_session_rtt_probe/connection_plan.h`
- Create: `tools/bitget/order_session_rtt_probe/connection_plan.cpp`
- Create: `tools/bitget/order_session_rtt_probe/run_plan.h`
- Create: `tools/bitget/order_session_rtt_probe/session_config_builder.h`
- Create: `test/tools/bitget/order_session_rtt_probe/order_session_rtt_probe_test.cpp`
- Modify: `test/tools/bitget/CMakeLists.txt`

- [ ] **Step 1: 写 RED tests**

```cpp
TEST(BitgetRttProbeConfigTest, RejectsZeroSamples) {
  auto parsed = ParseProbeConfig(
      toml::parse("[probe.sampling]\nsamples_per_session=0\n"));
  EXPECT_FALSE(parsed.ok);
  EXPECT_THAT(parsed.error, testing::HasSubstr("samples_per_session"));
}

TEST(BitgetRttProbeConnectionsTest, AllowsEmptyConnectIp) {
  auto parsed = LoadProbeConnectionsCsvFile(WriteConnectionsCsv(
      "name,group,host,connect_ip,port,enable_tls,worker_cpu_id\n"
      "ha-0,ha,vip-ws-uta.bitget.com,,443,true,6\n"));
  ASSERT_TRUE(parsed.ok) << parsed.error;
  EXPECT_TRUE(parsed.connections.front().connect_ip.empty());
}
```

- [ ] **Step 2: 运行 RED**

```bash
cmake --build build/debug --target bitget_order_session_rtt_probe_test -j2
```

Expected: target 或 headers 不存在。

- [ ] **Step 3: 实现固定配置模型**

```cpp
struct ProbeSessionConfig {
  std::uint32_t wait_login_timeout_ms{10000};
  std::uint32_t request_timeout_ms{5000};
};
struct ProbeSamplingConfig {
  std::uint32_t samples_per_session{1};
  std::uint64_t cycle_cooldown_us{500000};
  std::uint64_t order_session_interval_us{500000};
  std::uint32_t max_events_per_drain{128};
  std::uint32_t feedback_queue_capacity{256};
  std::int32_t coordinator_cpu{-1};
};
struct ProbeOrderConfig {
  std::string order_mode{"ioc"};
  double passive_price_limit_fraction{0.5};
  std::uint64_t bbo_freshness_ns{1'000'000'000ULL};
};
struct ProbeFeedbackConfig {
  std::filesystem::path shm_config{
      "config/order_feedback/bitget_order_feedback_session.toml"};
  std::uint8_t strategy_id{7};
  bool force_claim{false};
  std::uint32_t poll_budget{64};
  std::uint32_t terminal_timeout_ms{5000};
};
struct ProbeInputConfig {
  std::filesystem::path order_session_config;
  std::filesystem::path data_reader_config;
  std::filesystem::path connections_file;
};
struct ProbeOutputConfig {
  std::filesystem::path root_dir{
      "/home/liuxiang/tmp/bitget_order_session_rtt_probe"};
};
struct ProbeConfig {
  std::string name{"bitget_order_session_rtt_probe"};
  std::string run_id;
  ProbeInputConfig inputs;
  ProbeSessionConfig sessions;
  ProbeSamplingConfig sampling;
  ProbeOrderConfig order;
  ProbeFeedbackConfig feedback;
  ProbeOutputConfig output;
};
```

拒绝零 timeout / samples / queue capacity、`strategy_id >= 8`、非 `ioc`、fraction 不在 `(0,1]`、空 input 和空 output。

- [ ] **Step 4: 实现 CSV / run plan API**

```cpp
struct ProbeConnectionConfig {
  std::string name;
  std::string group;
  std::string host;
  std::string connect_ip;
  std::string port;
  bool enable_tls{true};
  std::int32_t worker_cpu_id{-1};
};
ProbeConnectionsCsvResult LoadProbeConnectionsCsvFile(
    const std::filesystem::path&);
ProbeRunPlanResult BuildProbeRunPlan(
    const ProbeConfig&, std::vector<ProbeConnectionConfig>);
bitget::OrderSessionConfig BuildPinnedOrderSessionConfig(
    bitget::OrderSessionConfig, const ProbeConnectionConfig&);
```

name 唯一；空 `connect_ip` 走 DNS，非空必须 numeric IP；每个 cycle 保留 CSV row order。

- [ ] **Step 5: GREEN 并提交**

```bash
cmake --build build/debug --target bitget_order_session_rtt_probe_test -j2
./build/debug/test/tools/bitget/bitget_order_session_rtt_probe_test \
  --gtest_filter='BitgetRttProbeConfigTest.*:BitgetRttProbeConnectionsTest.*:BitgetRttProbeRunPlanTest.*'
git add tools/bitget/order_session_rtt_probe test/tools/bitget
git commit -m "feat: add Bitget RTT probe configuration"
```

### Task 3: IOC order math、ID routing 和 sample flow

**Files:**
- Create: `tools/bitget/order_session_rtt_probe/passive_order_builder.h`
- Create: `tools/bitget/order_session_rtt_probe/sample_id_allocator.h`
- Create: `tools/bitget/order_session_rtt_probe/sample_flow.h`
- Modify: `test/tools/bitget/order_session_rtt_probe/order_session_rtt_probe_test.cpp`

- [ ] **Step 1: 写 builder / ID RED tests**

```cpp
TEST(BitgetRttProbeOrderBuilderTest, BuildsPassiveIoc) {
  auto built = BuildPassiveBuyIoc(MakeBitgetTicker(100.0, 100.1),
                                  MakeBitgetInstrument(), 0.5);
  ASSERT_TRUE(built.ok) << built.error;
  EXPECT_EQ(built.order.symbol, "BTCUSDT");
  EXPECT_EQ(built.order.price_text, "97.5");
  EXPECT_EQ(built.order.quantity_text, "0.0001");
}

TEST(BitgetRttProbeIdTest, RoutesPlaceAndCloseToSameSession) {
  ProbeSampleIdAllocator ids(7, 3, 8);
  const auto sample = ids.Next();
  EXPECT_EQ(SessionIndexForLocalOrderId(sample.ioc_local_order_id, 4), 1U);
  EXPECT_EQ(SessionIndexForLocalOrderId(sample.close_local_order_id, 4), 1U);
}
```

- [ ] **Step 2: 写 flow ordering / safety RED tests**

```cpp
TEST(BitgetRttProbeFlowTest, CompletesAfterAckAndZeroFillCancel) {
  ProbeSampleFlow flow(MakeIds());
  EXPECT_EQ(flow.Start().action, ProbeSampleAction::kSubmitIoc);
  ASSERT_TRUE(flow.OnOrderSent(MakeSent()).ok);
  EXPECT_EQ(flow.OnOrderResponse(MakeAck()).action, ProbeSampleAction::kNone);
  EXPECT_EQ(flow.OnOrderFeedback(MakeCancelled(0.0)).action,
            ProbeSampleAction::kComplete);
}

TEST(BitgetRttProbeFlowTest, UnexpectedFillRequestsOneSafetyClose) {
  ProbeSampleFlow flow(MakeIds());
  flow.Start();
  flow.OnOrderSent(MakeSent());
  EXPECT_EQ(flow.OnOrderFeedback(MakePartialFill(0.0001)).action,
            ProbeSampleAction::kSubmitSafetyClose);
  EXPECT_EQ(flow.OnOrderFeedback(MakePartialFill(0.0001)).action,
            ProbeSampleAction::kNone);
}
```

另测 feedback-before-Ack、duplicate、reject、unknown result、negative RTT、timeout、continuity、close filled / rejected / cancelled。

- [ ] **Step 3: 实现 order / flow API**

```cpp
struct ProbeWireOrder {
  std::uint64_t local_order_id{0};
  std::string symbol;
  OrderSide side{OrderSide::kBuy};
  OrderType type{OrderType::kLimit};
  TimeInForce time_in_force{TimeInForce::kImmediateOrCancel};
  std::string quantity_text;
  std::string price_text;
  bool reduce_only{false};
};
enum class ProbeSampleAction {
  kNone, kSubmitIoc, kSubmitSafetyClose, kComplete, kFail,
};
struct ProbeSampleStats;
class ProbeSampleFlow {
 public:
  ProbeSampleTransition Start() noexcept;
  ProbeSampleTransition OnOrderSent(const bitget::OrderSendResult&);
  ProbeSampleTransition OnSafetyCloseSent(const bitget::OrderSendResult&);
  ProbeSampleTransition OnOrderResponse(const bitget::OrderResponse&);
  ProbeSampleTransition OnOrderFeedback(const OrderFeedbackEvent&);
  ProbeSampleTransition OnTimeout();
  const ProbeSampleStats& stats() const noexcept;
};
struct ProbeSampleStats {
  bool place_ack_observed{false};
  bool zero_fill_cancelled_observed{false};
  bool unexpected_fill{false};
  bool safety_close_sent{false};
  bool safety_close_confirmed{false};
  bool invalid{false};
  double observed_fill_quantity{0.0};
  std::int64_t ack_rtt_ns{-1};
  std::string invalid_reason;
};
```

passive price 为 `floor(bid * (1 - price_limit_down * fraction) / tick) * tick`。safety close 是 sell reduce-only IOC，filled quantity 按 step 向下对齐且不得低于 min。normal complete 必须 Ack + zero-fill Cancelled；unexpected fill 只触发一次 close，close confirmed 后 sample 仍 invalid。

- [ ] **Step 4: GREEN 并提交**

```bash
cmake --build build/debug --target bitget_order_session_rtt_probe_test -j2
./build/debug/test/tools/bitget/bitget_order_session_rtt_probe_test \
  --gtest_filter='BitgetRttProbeOrderBuilderTest.*:BitgetRttProbeIdTest.*:BitgetRttProbeFlowTest.*'
git add tools/bitget/order_session_rtt_probe test/tools/bitget/order_session_rtt_probe
git commit -m "feat: add Bitget RTT probe sample flow"
```

### Task 4: Bounded queue、sequential coordinator 和 evidence writer

**Files:**
- Create: `tools/bitget/order_session_rtt_probe/local_feedback_queue.h`
- Create: `tools/bitget/order_session_rtt_probe/sequential_coordinator.h`
- Create: `tools/bitget/order_session_rtt_probe/sample_csv_writer.h`
- Create: `tools/bitget/order_session_rtt_probe/sample_csv_writer.cpp`
- Modify: `test/tools/bitget/order_session_rtt_probe/order_session_rtt_probe_test.cpp`

- [ ] **Step 1: 写 RED tests**

```cpp
TEST(BitgetRttProbeQueueTest, RejectsPushWhenFull) {
  LocalFeedbackQueue queue(2);
  EXPECT_TRUE(queue.TryPush(MakeCancelledFor(1)));
  EXPECT_TRUE(queue.TryPush(MakeCancelledFor(3)));
  EXPECT_FALSE(queue.TryPush(MakeCancelledFor(5)));
}
TEST(BitgetRttProbeCoordinatorTest, AllowsOneActiveSession) {
  SequentialCoordinator coordinator(2, 1, 0, 0);
  EXPECT_EQ(coordinator.NextGrant(0), 0U);
  EXPECT_FALSE(coordinator.NextGrant(0).has_value());
  coordinator.MarkSampleFinished(0, true, 1);
  EXPECT_EQ(coordinator.NextGrant(1), 1U);
}
```

CSV test 要求存在 `ack_rtt_ns` / `connection_id_hash`，不存在 Gate `x_in_time` / `exchange_request_ingress_ns`。

- [ ] **Step 2: 实现 queue / coordinator**

queue 构造时一次性分配 `std::vector<OrderFeedbackEvent>(capacity)`，之后以 ring index + mutex 工作；full 时不覆盖并计数，handler 在锁外执行。coordinator 持有唯一 `active_session_index`，按 CSV order、session interval 和 cycle cooldown 调度；任意 failure 停止 grant。

- [ ] **Step 3: 实现 evidence API**

```cpp
struct SampleCsvRow;
class SampleCsvWriter {
 public:
  bool Open(const std::filesystem::path&);
  bool Write(const SampleCsvRow&);
  void Close();
};
struct SampleCsvRow {
  std::string run_id;
  std::string session_name;
  std::string group;
  std::string endpoint;
  int worker_cpu{-1};
  std::uint64_t sample_id{0};
  std::uint64_t local_order_id{0};
  std::uint64_t request_sequence{0};
  std::uint64_t exchange_order_id{0};
  std::string symbol;
  std::string quantity_text;
  std::string price_text;
  std::int64_t request_send_ns{0};
  std::int64_t response_receive_ns{0};
  std::int64_t response_exchange_ns{0};
  std::int64_t ack_rtt_ns{-1};
  std::uint32_t error_code{0};
  std::uint64_t connection_id_hash{0};
  OrderFeedbackKind terminal_feedback_kind{OrderFeedbackKind::kCancelled};
  double cumulative_fill{0.0};
  bool invalid{false};
  bool unexpected_fill{false};
  bool safety_close_confirmed{false};
  std::string invalid_reason;
};
bool WriteRunMetadata(const std::filesystem::path&, const ProbeConfig&,
                      std::size_t session_count);
bool WriteConnectionObservation(
    const std::filesystem::path&, std::string_view run_id,
    std::string_view session_name,
    const bitget::OrderSessionConnectionInfo&);
```

固定 schema 只含 Bitget 可提供的 run/session/endpoint/CPU、IDs、order/BBO、send/receive/exchange/Ack RTT、response/error/connection hash、terminal/fill/cancel reason、invalid/safety status。

- [ ] **Step 4: GREEN 并提交**

```bash
cmake --build build/debug --target bitget_order_session_rtt_probe_test -j2
./build/debug/test/tools/bitget/bitget_order_session_rtt_probe_test \
  --gtest_filter='BitgetRttProbeQueueTest.*:BitgetRttProbeCoordinatorTest.*:BitgetRttProbeCsvTest.*'
git add tools/bitget/order_session_rtt_probe test/tools/bitget/order_session_rtt_probe
git commit -m "feat: record Bitget RTT probe samples"
```

### Task 5: Single-session live runner with fake runtime

**Files:**
- Create: `tools/bitget/order_session_rtt_probe/live_runner.h`
- Modify: `test/tools/bitget/order_session_rtt_probe/order_session_rtt_probe_test.cpp`

- [ ] **Step 1: 写 fake runtime RED tests**

```cpp
TEST(BitgetRttProbeLiveRunnerTest, FinishesAfterAckAndFeedback) {
  FakeSession session;
  FakeDataReader reader(MakeBitgetTicker(100.0, 100.1));
  LocalFeedbackQueue feedback(8);
  FakeWriter writer;
  LiveRunner runner(MakeRunnerConfig(), MakePlan(), MakeCatalog(), reader,
                    feedback, writer);
  runner.BindSession(session);
  runner.OnLoginReady();
  runner.GrantDispatch();
  runner.DriveHookOnce();
  ASSERT_EQ(session.orders.size(), 1U);
  runner.OnOrderResponse(MakeAckFor(session.orders.front()));
  feedback.TryPush(MakeCancelledFor(session.orders.front().local_order_id));
  runner.DriveHookOnce();
  EXPECT_TRUE(runner.SampleFinished());
}
```

补充 stale BBO、login-not-ready、continuity、queue drop、timeout、unexpected fill 后唯一 reduce-only close、unresolved close。

- [ ] **Step 2: 实现 owner-loop runner**

```cpp
template <typename SessionT, typename DataReaderT,
          typename FeedbackQueueT, typename WriterT>
class LiveRunner {
 public:
  void BindSession(SessionT&) noexcept;
  void OnLoginReady() noexcept;
  void OnLoginNotReady() noexcept;
  void OnOrderSessionConnected(
      const bitget::OrderSessionConnectionInfo&) noexcept;
  void OnOrderResponse(const bitget::OrderResponse&) noexcept;
  void OnBookTicker(const BookTicker&) noexcept;
  void GrantDispatch() noexcept;
  void DriveHookOnce() noexcept;
  bool SampleFinished() const noexcept;
  bool failed() const noexcept;
};
```

`DriveHookOnce` 固定为 drain BBO、poll feedback、timeout、pending transition、在 login ready + grant + fresh BBO 时 start。unexpected fill 动态构造 safety close 并调用 `Session::PlaceOrder`。

- [ ] **Step 3: GREEN 并提交**

```bash
cmake --build build/debug --target bitget_order_session_rtt_probe_test -j2
./build/debug/test/tools/bitget/bitget_order_session_rtt_probe_test \
  --gtest_filter='BitgetRttProbeLiveRunnerTest.*'
git add tools/bitget/order_session_rtt_probe/live_runner.h \
  test/tools/bitget/order_session_rtt_probe/order_session_rtt_probe_test.cpp
git commit -m "feat: run Bitget RTT probe samples"
```

### Task 6: CLI、single / multi-session、SHM integration 和 docs

**Files:**
- Create: `tools/bitget/order_session_rtt_probe/main.cpp`
- Create: `config/order_session_rtt_probe/bitget_order_session_rtt_probe.toml`
- Create: `config/order_session_rtt_probe/bitget_order_session_rtt_connections.csv`
- Create: `config/data_readers/bitget_order_session_rtt_probe.toml`
- Modify: `tools/CMakeLists.txt`
- Modify: `test/tools/bitget/CMakeLists.txt`
- Modify: `test/tools/bitget/order_session_rtt_probe/order_session_rtt_probe_test.cpp`
- Modify: `docs/diagnostic_fields.md`
- Modify: `docs/project_onboarding_guide.md`
- Modify: `docs/superpowers/specs/2026-07-10-bitget-order-session-rtt-probe-design.md`

- [ ] **Step 1: 写 CLI guard / SHM RED tests**

```cpp
TEST(BitgetRttProbeCliTest, ExecuteRequiresDedicatedAccountConfirmation) {
  EXPECT_FALSE(ValidateExecuteGuard({.execute = true}).ok);
  EXPECT_TRUE(ValidateExecuteGuard(
      {.execute = true, .confirm_dedicated_account = true}).ok);
}
TEST(BitgetRttProbeShmTest, RoutesTerminalAndBroadcastsContinuity) {
  auto manager = CreateTemporaryFeedbackShm();
  OrderFeedbackShmPublisher publisher(manager.channel());
  auto reader = OrderFeedbackShmReader::Claim(manager.channel(), 7, 9001);
  ASSERT_TRUE(reader.ok) << reader.error;
  ASSERT_TRUE(publisher.Publish(MakeCancelledFor(encoded_place_id)));
  EXPECT_EQ(RouteFeedback(reader.value, runners).routed, 1U);
  ASSERT_TRUE(publisher.PublishGlobalContinuityLost(
      OrderFeedbackContinuityReason::kSessionDisconnected, NowNs()));
  EXPECT_TRUE(RouteFeedback(reader.value, runners).continuity_broadcast);
}
```

- [ ] **Step 2: 实现 dry-run / live-preflight**

CLI 包含 `--config`、`--connections-file`、`--samples-per-session`、`--duration-sec`、`--live-preflight`、`--execute`、`--confirm-dedicated-account`。dry-run 打印 plan；preflight 验证 config / instrument / SHM path，但不读取 credential、不创建 session、不 attach SHM；两者打印 `orders_sent=0`、`rest_guard_implemented=false`。

```cpp
struct ExecuteGuardInput {
  bool execute{false};
  bool live_preflight{false};
  bool confirm_dedicated_account{false};
};
Result<bool> ValidateExecuteGuard(const ExecuteGuardInput&);
struct RouteFeedbackResult {
  std::size_t routed{0};
  bool continuity_broadcast{false};
  bool failed{false};
};
template <typename Reader, typename Runners>
RouteFeedbackResult RouteFeedback(Reader&, Runners&,
                                  std::size_t poll_budget) noexcept;
```

- [ ] **Step 3: 实现 execute 生命周期**

顺序：load credential；open-only feedback SHM；claim lane；先 drain 已有 lane event，发现 continuity 或 stale own event 时在下单前失败；每行 CSV 创建一条 session / runner thread；等待全部 login ready；主线程运行唯一 coordinator 与 feedback router；停止 / join；merge CSV；summary。continuity 广播全部 runner；普通 event 走 ID router；无法映射的本 strategy event、session failure 或 signal 均停止新 order。`duration_sec` 是 hard deadline；全部 session 完成目标 samples 可提前成功，deadline 到达但样本未完成则失败；active safety close 只获得 bounded terminal drain。

```cpp
struct ProbeResponseHandler {
  void* context{nullptr};
  void (*on_login_ready)(void*) noexcept {nullptr};
  void (*on_login_not_ready)(void*) noexcept {nullptr};
  void (*on_response)(void*, const bitget::OrderResponse&) noexcept {nullptr};
  void (*on_connected)(
      void*, const bitget::OrderSessionConnectionInfo&) noexcept {nullptr};
};
```

- [ ] **Step 4: 新增默认 config / CMake**

```csv
name,group,host,connect_ip,port,enable_tls,worker_cpu_id
ha-00,high-availability,vip-ws-uta.bitget.com,,443,true,-1
```

probe TOML 使用 one sample、strategy 7、output `/home/liuxiang/tmp/bitget_order_session_rtt_probe`；reader config 只读 `aquila_bitget_book_ticker_fusion/book_ticker_channel`。

- [ ] **Step 5: GREEN、docs 和提交**

```bash
cmake --build build/debug --target bitget_order_session_rtt_probe \
  bitget_order_session_rtt_probe_test -j2
./build/debug/test/tools/bitget/bitget_order_session_rtt_probe_test
./build/debug/tools/bitget_order_session_rtt_probe \
  --config config/order_session_rtt_probe/bitget_order_session_rtt_probe.toml
./build/debug/tools/bitget_order_session_rtt_probe \
  --config config/order_session_rtt_probe/bitget_order_session_rtt_probe.toml \
  --live-preflight
git diff --check
git add tools/bitget/order_session_rtt_probe tools/CMakeLists.txt \
  test/tools/bitget config/order_session_rtt_probe \
  config/data_readers/bitget_order_session_rtt_probe.toml \
  docs/diagnostic_fields.md docs/project_onboarding_guide.md \
  docs/superpowers/specs/2026-07-10-bitget-order-session-rtt-probe-design.md
git commit -m "feat: add Bitget order session RTT probe"
```

Expected: tests pass；dry-run / preflight 返回 0 且不需要 credential；docs 记录 log / CSV 字段和未 live 边界。

### Task 7: Final verification

**Files:** no code changes expected.

- [ ] **Step 1: focused Debug / Release**

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target \
  bitget_order_session_test bitget_order_feedback_session_test \
  bitget_order_feedback_shm_integration_test bitget_order_session_rtt_probe \
  bitget_order_session_rtt_probe_test -j2
ctest --test-dir build/debug -R \
  'bitget_order_session|bitget_order_feedback|bitget_order_session_rtt_probe' \
  --output-on-failure
TMPDIR=/home/liuxiang/tmp cmake --build build/release --target \
  bitget_order_session_rtt_probe bitget_order_session_rtt_probe_test -j2
ctest --test-dir build/release -R 'bitget_order_session_rtt_probe' \
  --output-on-failure
```

Expected: selected tests pass。

- [ ] **Step 2: full regression**

```bash
TMPDIR=/home/liuxiang/tmp ./build.sh -n 2 debug
ctest --test-dir build/debug --output-on-failure
TMPDIR=/home/liuxiang/tmp ./build.sh -n 2 release
ctest --test-dir build/release --output-on-failure
```

Expected: both builds and all tests pass。

- [ ] **Step 3: final boundaries**

```bash
git diff --check
rg '#include "evaluation/' core exchange tools
rg 'aquila_evaluation' core exchange tools
git status --short --branch
```

Expected: evaluation commands 无输出；工作区只保留用户原有 `core/market_data/types.h` 草案。最终报告必须明确没有发送真实订单、没有 live RTT、没有 REST flat 证据；下一步才讨论 IP 白名单、account baseline、feedback smoke 和单个 IOC live sample。
