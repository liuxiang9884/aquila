# Bitget Gateway 单笔实盘 Smoke Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 新增一个 fresh-run、单 route、单笔 BTCUSDT 最小数量被动 IOC 的 Bitget gateway smoke，并用独立 feedback terminal、进程 quiescence 和最终 REST flat 构成完整实盘证据链。

**Architecture:** C++ `bitget_gateway_smoke` 复用 Gate probe 的 `OrderGatewayClient + BookTicker SHM + OrderFeedback SHM` 形状，只实现一次 entry 和至多一次 reduce-only close。Python prepare/pipeline 为 data session、gateway、feedback 和 runner 生成唯一配置与 manifest，在 REST flat preflight 后启动外部进程，绑定 PID/config/credentials，等待 feedback ready，再由现有 guard 执行 runner、quiescence、最终 REST snapshot 和 emergency flatten。

**Tech Stack:** C++20、CMake、GoogleTest、toml++、fmt、magic_enum、nova SHM；Python 3.11、`unittest`、现有 Bitget REST emergency helper 与 LeadLag guard 基础设施。

---

## 文件结构

- Create: `tools/bitget/gateway_smoke/types.h` — 配置、BBO、订单和结果类型。
- Create: `tools/bitget/gateway_smoke/config.h`
- Create: `tools/bitget/gateway_smoke/config.cpp` — TOML 解析和单 route/单订单安全约束。
- Create: `tools/bitget/gateway_smoke/order_math.h` — 最小数量、被动 entry、aggressive reduce-only close 的纯函数。
- Create: `tools/bitget/gateway_smoke/state_machine.h`
- Create: `tools/bitget/gateway_smoke/state_machine.cpp` — Ack 与 independent terminal 的乱序容忍证据状态机。
- Create: `tools/bitget/gateway_smoke/evidence_writer.h`
- Create: `tools/bitget/gateway_smoke/evidence_writer.cpp` — `order_event.csv` 和原子 `summary.json`。
- Create: `tools/bitget/gateway_smoke/main.cpp` — 一次 entry/close runner；只有 `--execute` 发单。
- Create: `config/gateway_smoke/bitget_btcusdt_gateway_smoke.toml` — BTCUSDT 最小量 smoke 基准配置。
- Create: `config/data_sessions/bitget_gateway_smoke.toml` — 只订阅 BTCUSDT book ticker 的 data session 基准配置。
- Create: `test/tools/bitget/gateway_smoke_test.cpp` — C++ 单元与 CLI 安全测试。
- Modify: `tools/CMakeLists.txt` — 新增 executable。
- Modify: `test/tools/bitget/CMakeLists.txt` — 新增 GoogleTest、help 和 validate-only test。
- Create: `scripts/bitget/trading/prepare_gateway_smoke_run.py` — overlay、digest、manifest、PID/config/credentials 绑定。
- Create: `scripts/test/bitget/trading/prepare_gateway_smoke_run_test.py` — fresh-run 与 attestation 测试。
- Create: `scripts/bitget/trading/run_gateway_smoke_with_guard.py` — REST preflight 后启动三进程并调用现有 guard。
- Create: `scripts/test/bitget/trading/run_gateway_smoke_with_guard_test.py` — 启动顺序、ready、失败清理和单次执行测试。
- Modify: `scripts/lead_lag/run_live_with_guard.py` — quiescence 在 manifest 含 data session 时一并停止，LeadLag 行为保持不变。
- Modify: `scripts/test/lead_lag/run_live_with_guard_test.py` — 三进程 quiescence 回归。
- Modify: `docs/diagnostic_fields.md` — 新增 CSV/summary 字段合同。
- Modify: `docs/bitget_trading.md` — gateway smoke runbook 和证据门。
- Modify: `docs/project_onboarding_guide.md` — 实盘后只记录最新证据摘要、入口与剩余 LeadLag 阻断。

### Task 1: C++ 配置、订单数学与状态机

**Files:**
- Create: `tools/bitget/gateway_smoke/types.h`
- Create: `tools/bitget/gateway_smoke/config.h`
- Create: `tools/bitget/gateway_smoke/config.cpp`
- Create: `tools/bitget/gateway_smoke/order_math.h`
- Create: `tools/bitget/gateway_smoke/state_machine.h`
- Create: `tools/bitget/gateway_smoke/state_machine.cpp`
- Create: `test/tools/bitget/gateway_smoke_test.cpp`
- Modify: `test/tools/bitget/CMakeLists.txt`

- [ ] **Step 1: 写配置与订单数学失败测试**

测试必须固定以下合同：`route_count == 1`、`route_id == 0`、`strategy_id < 8`、`quantity` 等于 catalog `min_quantity`、正 timeout、`side = buy|sell`；buy entry 使用 `floor(bid * (1 - price_limit_down * fraction))`，sell entry 使用对称的 `ceil(ask * (1 + price_limit_up * fraction))`；close 使用对手价加配置 slippage 并设置 `reduce_only`。

```cpp
TEST(BitgetGatewaySmokeConfigTest, RejectsFanoutGreaterThanOne) {
  const auto result = ParseConfig(ParseToml(ValidConfig("route_count = 2")));
  ASSERT_FALSE(result.ok);
  EXPECT_THAT(result.error, testing::HasSubstr("route_count must be 1"));
}

TEST(BitgetGatewaySmokeOrderMathTest, BuildsMinimumPassiveBuyIoc) {
  const BookTicker ticker = ValidTicker(100.0, 100.1);
  const config::InstrumentInfo instrument = ValidBitgetInstrument();
  const auto result = BuildEntryOrder(ticker, instrument, OrderSide::kBuy,
                                      instrument.min_quantity, 0.5);
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.value.price_text, "97.5");
  EXPECT_EQ(result.value.quantity_text, "0.0001");
  EXPECT_EQ(result.value.time_in_force, TimeInForce::kImmediateOrCancel);
  EXPECT_FALSE(result.value.reduce_only);
}

TEST(BitgetGatewaySmokeOrderMathTest, BuildsAggressiveReduceOnlyClose) {
  const auto result = BuildCloseOrder(ValidTicker(100.0, 100.1),
                                      ValidBitgetInstrument(),
                                      OrderSide::kSell, 0.0001, 100);
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.value.price_text, "99.0");
  EXPECT_TRUE(result.value.reduce_only);
}
```

- [ ] **Step 2: 运行测试并确认失败**

Run: `TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target bitget_gateway_smoke_test -j2`

Expected: FAIL，因为 target 与 `ParseConfig`、`BuildEntryOrder`、`BuildCloseOrder` 尚不存在。

- [ ] **Step 3: 实现配置类型和订单纯函数**

暴露下面的稳定接口；所有 decimal text 使用 `fmt::format`，不在热路径使用 iostream。

```cpp
struct GatewaySmokeConfig {
  std::string run_id;
  std::string symbol;
  std::string exchange_symbol;
  std::int32_t symbol_id{0};
  std::uint8_t strategy_id{0};
  OrderSide side{OrderSide::kBuy};
  double quantity{0.0};
  double passive_price_limit_fraction{0.5};
  std::uint32_t close_slippage_bps{100};
  std::uint64_t bbo_freshness_ns{1'000'000'000};
  std::uint32_t ack_timeout_ms{5'000};
  std::uint32_t terminal_timeout_ms{10'000};
  MarketDataConfig market_data;
  OrderGatewayConfig order_gateway;
  FeedbackConfig feedback;
  std::filesystem::path instrument_catalog_file;
  std::filesystem::path run_dir;
};

[[nodiscard]] GatewaySmokeConfigResult LoadConfig(
    const std::filesystem::path& path);
[[nodiscard]] Result<bool> ValidateInstrumentContract(
    const GatewaySmokeConfig&, const config::InstrumentInfo&);
[[nodiscard]] WireOrderResult BuildEntryOrder(
    const BookTicker&, const config::InstrumentInfo&, OrderSide, double,
    double passive_price_limit_fraction);
[[nodiscard]] WireOrderResult BuildCloseOrder(
    const BookTicker&, const config::InstrumentInfo&, OrderSide, double,
    std::uint32_t close_slippage_bps);
```

- [ ] **Step 4: 写状态机失败测试**

覆盖 terminal-before-Ack、Ack-before-terminal、零成交 cancel、部分成交后 close、close 不足、gateway unknown、foreign local order id、duplicate terminal 和 continuity lost。

```cpp
TEST(BitgetGatewaySmokeStateMachineTest, RequiresAckAndIndependentTerminal) {
  SmokeStateMachine state;
  state.MarkEntrySubmitted(EntryId(), 1);
  state.OnFeedback(TerminalCancelled(EntryId(), 0.0));
  EXPECT_FALSE(state.done());
  state.OnGatewayResponse(Ack(EntryId()));
  EXPECT_TRUE(state.done());
  EXPECT_EQ(state.result(), SmokeResult::kNoFill);
}

TEST(BitgetGatewaySmokeStateMachineTest, FilledEntryRequiresFlatClose) {
  SmokeStateMachine state;
  state.MarkEntrySubmitted(EntryId(), 1);
  state.OnGatewayResponse(Ack(EntryId()));
  state.OnFeedback(TerminalCancelled(EntryId(), 0.0001));
  ASSERT_TRUE(state.close_required());
  state.MarkCloseSubmitted(CloseId(), 1, 0.0001);
  state.OnGatewayResponse(Ack(CloseId()));
  state.OnFeedback(TerminalFilled(CloseId(), 0.0001));
  EXPECT_TRUE(state.done());
  EXPECT_EQ(state.result(), SmokeResult::kClosed);
}

TEST(BitgetGatewaySmokeStateMachineTest, ContinuityLossFailsClosed) {
  SmokeStateMachine state;
  state.MarkEntrySubmitted(EntryId(), 1);
  state.OnFeedback(ContinuityLost());
  EXPECT_TRUE(state.failed());
  EXPECT_EQ(state.failure(), SmokeFailure::kFeedbackContinuityLost);
}
```

- [ ] **Step 5: 实现乱序容忍证据状态机**

`OrderFeedbackKind::kPartialFilled` 只更新累计量，不算 terminal；只有 `kFilled`、`kCancelled`、`kRejected` 是 terminal。gateway `kRejected` 或 `kUnknownResult` 立即失败。entry terminal 的累计成交决定是否需要 close；close terminal 的累计成交必须覆盖 entry terminal 累计成交。

```cpp
class SmokeStateMachine {
 public:
  void MarkEntrySubmitted(std::uint64_t local_order_id,
                          std::int64_t submit_ns) noexcept;
  void MarkCloseSubmitted(std::uint64_t local_order_id,
                          std::int64_t submit_ns,
                          double quantity) noexcept;
  void OnGatewayResponse(const core::OrderResponseEvent& event) noexcept;
  void OnFeedback(const OrderFeedbackEvent& event) noexcept;
  void CheckTimeout(std::int64_t now_ns, std::int64_t ack_timeout_ns,
                    std::int64_t terminal_timeout_ns) noexcept;

  [[nodiscard]] bool close_required() const noexcept;
  [[nodiscard]] bool done() const noexcept;
  [[nodiscard]] bool failed() const noexcept;
  [[nodiscard]] double entry_filled_quantity() const noexcept;
  [[nodiscard]] SmokeResult result() const noexcept;
  [[nodiscard]] SmokeFailure failure() const noexcept;
};
```

- [ ] **Step 6: 运行单元测试并提交**

Run: `TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target bitget_gateway_smoke_test -j2 && build/debug/test/tools/bitget/bitget_gateway_smoke_test`

Expected: PASS，且所有状态机测试只操作纯内存对象。

```bash
git add tools/bitget/gateway_smoke test/tools/bitget
git commit -m "feat: add Bitget gateway smoke safety core"
```

### Task 2: C++ runner 与结构化证据

**Files:**
- Create: `tools/bitget/gateway_smoke/evidence_writer.h`
- Create: `tools/bitget/gateway_smoke/evidence_writer.cpp`
- Create: `tools/bitget/gateway_smoke/main.cpp`
- Create: `config/gateway_smoke/bitget_btcusdt_gateway_smoke.toml`
- Create: `config/data_sessions/bitget_gateway_smoke.toml`
- Modify: `tools/CMakeLists.txt`
- Modify: `test/tools/bitget/CMakeLists.txt`
- Modify: `test/tools/bitget/gateway_smoke_test.cpp`

- [ ] **Step 1: 写 evidence writer 与 CLI 失败测试**

```cpp
TEST(BitgetGatewaySmokeEvidenceWriterTest, PersistsAckAndTerminalSeparately) {
  EvidenceWriter writer(temp_dir.path());
  ASSERT_TRUE(writer.Open().ok);
  writer.Write(GatewayAckRow());
  writer.Write(FeedbackTerminalRow());
  ASSERT_TRUE(writer.WriteSummary(SuccessSummary()).ok);
  EXPECT_THAT(Read(temp_dir.path() / "order_event.csv"),
              testing::HasSubstr("gateway_response,ack"));
  EXPECT_THAT(Read(temp_dir.path() / "order_event.csv"),
              testing::HasSubstr("feedback_terminal"));
  EXPECT_THAT(Read(temp_dir.path() / "summary.json"),
              testing::HasSubstr("\"final_result\": \"no_fill\""));
}
```

在 CMake 添加 `bitget_gateway_smoke_help_test` 和 `bitget_gateway_smoke_validate_only_test`；validate-only 使用 checked-in config 且不得打开 SHM 或读取 credentials。

- [ ] **Step 2: 运行测试并确认失败**

Run: `TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target bitget_gateway_smoke -j2`

Expected: FAIL，因为 executable 和 writer 尚不存在。

- [ ] **Step 3: 实现 writer 与一次性 runner**

runner 必须以 `Exchange::kBitget` 构造 `StrategyOrder`，local order id 使用 `LocalOrderIdCodec::Encode(strategy_id, sequence)`；只允许 sequence 1 为 entry、sequence 2 为 close。`--validate-only` 只解析配置/catalog；`--preflight-only` 可连接 SHM 但不调用 `PlaceOrder`；只有 `--execute` 进入发单分支。

```cpp
CLI::App app{"Bitget gateway one-shot smoke"};
app.add_option("--config", config_path)->required();
auto* validate_flag = app.add_flag("--validate-only", validate_only);
auto* preflight_flag = app.add_flag("--preflight-only", preflight_only);
auto* execute_flag = app.add_flag("--execute", execute);
validate_flag->excludes(preflight_flag)->excludes(execute_flag);
preflight_flag->excludes(validate_flag)->excludes(execute_flag);
execute_flag->excludes(validate_flag)->excludes(preflight_flag);

if (static_cast<int>(validate_only) + static_cast<int>(preflight_only) +
        static_cast<int>(execute) !=
    1) {
  fmt::print(stderr, "exactly one run mode is required\n");
  return 1;
}

if (!execute) {
  return validate_only ? ValidateOnly(context) : PreflightOnly(context);
}
return ExecuteOnce(context);
```

执行循环每次先 drain BBO、gateway response、feedback；entry Ack 与 feedback terminal 都出现后，零成交结束，有成交则只提交一次 close。任何 signal、timeout、continuity loss、unknown、close residual 返回非零，不重试 entry/close。

- [ ] **Step 4: 写 checked-in 安全配置**

```toml
[gateway_smoke]
name = "bitget_gateway_smoke"
run_id = "validate_only"
symbol = "BTC_USDT"
exchange_symbol = "BTCUSDT"
symbol_id = 0
strategy_id = 7
side = "buy"
quantity = 0.0001
passive_price_limit_fraction = 0.5
close_slippage_bps = 100
bbo_freshness_ns = 1000000000
ack_timeout_ms = 5000
terminal_timeout_ms = 10000
route_id = 0
```

data-session 基准配置只保留 `subscribe_symbols = ["BTCUSDT"]`、`feeds = ["book_ticker"]`、`create = true`、`remove_existing = false`。

- [ ] **Step 5: 构建并运行 C++ 测试**

Run: `TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target bitget_gateway_smoke bitget_gateway_smoke_test -j2`

Run: `ctest --test-dir build/debug --output-on-failure -R 'bitget_gateway_smoke'`

Expected: help、validate-only 和 GoogleTest 全部 PASS；测试输出中没有 `--execute`。

- [ ] **Step 6: 提交 runner**

```bash
git add tools/CMakeLists.txt tools/bitget/gateway_smoke config/gateway_smoke \
  config/data_sessions/bitget_gateway_smoke.toml test/tools/bitget
git commit -m "feat: add Bitget gateway one-shot smoke"
```

### Task 3: Fresh-run prepare 与 manifest attestation

**Files:**
- Create: `scripts/bitget/trading/prepare_gateway_smoke_run.py`
- Create: `scripts/test/bitget/trading/prepare_gateway_smoke_run_test.py`

- [ ] **Step 1: 写 fresh-run 与 tamper 失败测试**

```python
def test_prepare_generates_four_run_specific_configs(self):
    result = prepare.prepare_runtime_configs(
        self.run_id, self.data_source, self.gateway_source,
        self.feedback_source, self.smoke_source, self.config_dir
    )
    self.assertEqual(result.market_data_shm,
                     f"aquila_bitget_market_data_{self.run_id}")
    self.assertEqual(result.gateway_shm,
                     f"aquila_bitget_order_gateway_{self.run_id}")
    self.assertEqual(result.feedback_shm,
                     f"aquila_bitget_order_feedback_{self.run_id}")
    self.assertEqual(result.manifest.parent, self.config_dir)

def test_validate_rejects_config_digest_change(self):
    result = self.prepare()
    result.smoke_config.write_text("[gateway_smoke]\nname='changed'\n")
    with self.assertRaisesRegex(ValueError, "digest mismatch"):
        prepare.validate_manifest(result.manifest, require_applied=False)

def test_mark_applied_binds_data_gateway_feedback_without_secrets(self):
    result = self.prepare()
    self.write_fake_processes(result)
    manifest = prepare.mark_processes_applied(
        result.manifest, self.data_pid, self.gateway_pid, self.feedback_pid,
        proc_root=self.proc_root,
    )
    self.assertEqual(set(manifest["processes"]),
                     {"data_session", "gateway", "feedback"})
    self.assertNotIn("secret-value", json.dumps(manifest))
```

- [ ] **Step 2: 运行测试并确认失败**

Run: `pyenv/lx/bin/python -m unittest scripts.test.bitget.trading.prepare_gateway_smoke_run_test -v`

Expected: FAIL，模块尚不存在。

- [ ] **Step 3: 实现 overlay、digest 与 attestation**

manifest schema 固定为 `aquila.bitget_gateway_smoke_manifest.v1`，必须包含四个 config 的绝对路径和 SHA-256、runner basename `bitget_gateway_smoke`、三个 SHM 名称、`route_count = 1`、`contract = BTC_USDT`、`external_configs_applied`。validator 必须核对 runner command 的 basename、绝对 `--config` 与 manifest smoke config 一致。输出目录必须严格等于 `/home/liuxiang/tmp/<run_id>/configs`，任何目标文件存在都拒绝，禁止覆盖或 resume。

```python
MANIFEST_SCHEMA = "aquila.bitget_gateway_smoke_manifest.v1"
PROCESS_SPECS = {
    "data_session": "bitget_data_session",
    "gateway": "bitget_order_gateway",
    "feedback": "bitget_order_feedback_session",
}

def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(65536), b""):
            digest.update(chunk)
    return digest.hexdigest()
```

复用 `prepare_bitget_live_run._build_process_binding` 的 `/proc` PID/start-time/executable/absolute-config 校验，以及 gateway/feedback credential value 一致性校验；data session 不读取 credentials。

- [ ] **Step 4: 运行 Python 测试并提交**

Run: `pyenv/lx/bin/python -m unittest scripts.test.bitget.trading.prepare_gateway_smoke_run_test -v`

Expected: PASS，且临时文件全部位于 `/home/liuxiang/tmp`。

```bash
git add scripts/bitget/trading/prepare_gateway_smoke_run.py \
  scripts/test/bitget/trading/prepare_gateway_smoke_run_test.py
git commit -m "feat: attest Bitget gateway smoke runs"
```

### Task 4: Guarded 三进程 pipeline

**Files:**
- Create: `scripts/bitget/trading/run_gateway_smoke_with_guard.py`
- Create: `scripts/test/bitget/trading/run_gateway_smoke_with_guard_test.py`
- Modify: `scripts/lead_lag/run_live_with_guard.py`
- Modify: `scripts/test/lead_lag/run_live_with_guard_test.py`

- [ ] **Step 1: 写三进程 quiescence 失败测试**

```python
def test_quiesce_stops_gateway_feedback_and_data_session(self):
    manifest = {"processes": {
        "data_session": binding(101),
        "gateway": binding(102),
        "feedback": binding(103),
    }}
    ok, summary = guard.quiesce_bitget_processes(
        manifest, controller=self.controller, clock=self.clock
    )
    self.assertTrue(ok)
    self.assertEqual(set(summary["processes"]),
                     {"data_session", "gateway", "feedback"})
```

LeadLag 两进程 manifest 的既有测试必须继续通过。

- [ ] **Step 2: 修改通用 quiescer 并验证回归**

按 `gateway -> feedback -> data_session` 顺序停止；只有 manifest 实际含 `data_session` binding 时才加入第三个 role。身份检查仍使用 pidfd 与 start-time，错误信息改为不能证明 bound Bitget processes 已停止。

Run: `pyenv/lx/bin/python -m unittest scripts.test.lead_lag.run_live_with_guard_test -v`

Expected: 新旧 quiescence 测试全部 PASS。

- [ ] **Step 3: 写 pipeline 失败测试**

```python
def test_flat_preflight_happens_before_process_launch(self):
    events = []
    self.pipeline.state_reader = lambda *args: events.append("preflight") or flat()
    self.pipeline.process_launcher = lambda *args: events.append("launch") or processes()
    self.pipeline.run()
    self.assertEqual(events[:2], ["preflight", "launch"])

def test_feedback_ready_timeout_never_runs_smoke_and_flattens(self):
    result = self.pipeline.run(feedback_log="")
    self.assertNotIn("smoke", self.events)
    self.assertIn("quiesce", self.events)
    self.assertIn("flatten", self.events)
    self.assertNotEqual(result.exit_code, 0)

def test_success_launches_smoke_once_and_finishes_flat(self):
    result = self.pipeline.run(feedback_log=(
        "bitget_order_feedback_subscribe accepted=true code=0"
    ))
    self.assertEqual(self.events.count("smoke"), 1)
    self.assertEqual(self.events.count("launch_gateway"), 1)
    self.assertEqual(result.summary["result"], "normal_exit_flat")
```

- [ ] **Step 4: 实现 pipeline**

pipeline 调用 `prepare_runtime_configs` 后构造现有 `GuardConfig`。把外部进程启动放入 `run_guarded_live` 的 `process_runner` closure，使 guard 的 REST preflight 必然先发生；closure 依次启动 data session、feedback、gateway，立即写入 manifest binding，然后等待 feedback log 出现下面的 readiness 证据，再执行一次 runner：

```python
FEEDBACK_READY_MARKER = "bitget_order_feedback_subscribe accepted=true code=0"

smoke_command = [
    str(smoke_binary), "--config", str(runtime.smoke_config), "--execute"
]

def run_smoke_after_launch(_: list[str]) -> guard.ProcessResult:
    processes = launch_bound_processes(runtime)
    prepare.mark_processes_applied(
        runtime.manifest,
        data_session_pid=processes.data.pid,
        gateway_pid=processes.gateway.pid,
        feedback_pid=processes.feedback.pid,
    )
    wait_for_feedback_ready(runtime.feedback_log, processes, ready_timeout_s)
    return guard.run_strategy_process(smoke_command)
```

`process_quiescer` 重新读取已绑定 manifest 并调用 `quiesce_bitget_processes`；若 launch 在绑定前失败，则按保存的 `Popen` PID 逐个 terminate/kill。runner 异常、非零退出、ready timeout、最终 REST 非 flat 都必须进入现有 Bitget emergency flatten。summary 原子写入 run directory。

- [ ] **Step 5: 运行 Python pipeline 测试并提交**

Run: `pyenv/lx/bin/python -m unittest scripts.test.bitget.trading.run_gateway_smoke_with_guard_test scripts.test.lead_lag.run_live_with_guard_test -v`

Expected: PASS；mock 断言 preflight 先于 launch，任何失败路径都执行 quiescence，成功路径 runner 只执行一次。

```bash
git add scripts/bitget/trading/run_gateway_smoke_with_guard.py \
  scripts/test/bitget/trading/run_gateway_smoke_with_guard_test.py \
  scripts/lead_lag/run_live_with_guard.py \
  scripts/test/lead_lag/run_live_with_guard_test.py
git commit -m "feat: guard Bitget gateway smoke pipeline"
```

### Task 5: 文档合同与完整自动验证

**Files:**
- Modify: `docs/diagnostic_fields.md`
- Modify: `docs/bitget_trading.md`

- [ ] **Step 1: 更新诊断字段事实源**

记录 `order_event.csv` 的 `run_id,event_source,event_kind,order_role,local_order_id,parent_id,route_id,response_kind,feedback_kind,exchange_order_id,exchange_ns,local_ns,price,quantity,cumulative_filled_quantity,left_quantity,finish_reason,reject_reason`，以及 `summary.json` 的 Ack、terminal、close、failure 和 final result 字段。说明 gateway Ack 与 feedback terminal 是两份独立证据。

- [ ] **Step 2: 更新 Bitget runbook**

写明唯一 live 入口为 `run_gateway_smoke_with_guard.py --execute`，BTCUSDT、0.0001、fanout 1、buy passive IOC；同一 run 不得重启，任何异常进入 stop-and-flat；gateway smoke 成功不等于 LeadLag 授权。

- [ ] **Step 3: 运行完整自动验证**

Run: `TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target bitget_gateway_smoke bitget_gateway_smoke_test bitget_data_session bitget_order_gateway bitget_order_feedback_session -j2`

Run: `ctest --test-dir build/debug --output-on-failure -R 'bitget_gateway_smoke|bitget_order_gateway|bitget_order_feedback_session|bitget_data_session'`

Run: `pyenv/lx/bin/python -m unittest scripts.test.bitget.trading.prepare_gateway_smoke_run_test scripts.test.bitget.trading.run_gateway_smoke_with_guard_test scripts.test.lead_lag.prepare_bitget_live_run_test scripts.test.lead_lag.run_live_with_guard_test -v`

Run: `git diff --check`

Expected: 所有 build/test PASS，diff check 无输出；未执行任何 `--execute`。

- [ ] **Step 4: 提交文档**

```bash
git add docs/diagnostic_fields.md docs/bitget_trading.md
git commit -m "docs: add Bitget gateway smoke runbook"
```

### Task 6: Release 构建、当次实盘与证据归档

**Files:**
- Modify: `docs/bitget_trading.md`
- Modify: `docs/project_onboarding_guide.md`

- [ ] **Step 1: 重新检查 live 前置条件**

Run: `git status --short --branch`

Run: `pgrep -af 'lead_lag_strategy|bitget_(data_session|order_gateway|order_feedback_session|gateway_smoke)' || true`

Run: `pyenv/lx/bin/python scripts/bitget/trading/emergency_flatten_futures.py --symbol BTCUSDT --dry-run --pretty`

Expected: 只有用户已有的 `core/market_data/types.h` dirty；无存活交易进程；REST baseline 显示 open orders、plan orders、position 都为零。

- [ ] **Step 2: Release 构建和 validate-only**

Run: `TMPDIR=/home/liuxiang/tmp cmake --build build/release --target bitget_gateway_smoke bitget_data_session bitget_order_gateway bitget_order_feedback_session -j2`

Run: `build/release/tools/bitget_gateway_smoke --config config/gateway_smoke/bitget_btcusdt_gateway_smoke.toml --validate-only`

Expected: build 与 validate-only PASS，不访问 private trading API，不发单。

- [ ] **Step 3: 消耗已授权的一次 live smoke**

使用全新 UTC run id，并只执行一次：

```bash
pyenv/lx/bin/python scripts/bitget/trading/run_gateway_smoke_with_guard.py \
  --run-id bitget_gateway_smoke_<UTC_TIMESTAMP> \
  --data-session-config config/data_sessions/bitget_gateway_smoke.toml \
  --gateway-config config/order_gateways/bitget_order_gateway.toml \
  --feedback-config config/order_feedback/bitget_order_feedback_session.toml \
  --smoke-config config/gateway_smoke/bitget_btcusdt_gateway_smoke.toml \
  --execute --pretty
```

Expected: entry 只出现一次；gateway Ack 与独立 terminal 均存在；若 entry 有成交则 close Ack/terminal 存在；三个进程 quiescent；最终 REST flat。失败时不得在同一 run 重启或补发 entry。

- [ ] **Step 4: 核对证据并更新专题文档与 onboarding**

核对 manifest digest/process binding、三份进程日志、`order_event.csv`、`summary.json`、guard summary 和最终 REST snapshot。`docs/bitget_trading.md` 记录日期、run directory、关键结果和边界；onboarding 只记录“gateway live evidence 是否通过、入口、仍缺 LeadLag live”，不粘贴原始日志。

- [ ] **Step 5: 最终验证并提交证据文档**

Run: `git diff --check`

Run: `git status --short --branch`

```bash
git add docs/bitget_trading.md docs/project_onboarding_guide.md
git commit -m "docs: record Bitget gateway live evidence"
```

最终提交不得包含用户的 `core/market_data/types.h` 改动。
