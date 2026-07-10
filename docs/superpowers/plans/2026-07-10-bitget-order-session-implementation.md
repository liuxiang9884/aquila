# Bitget UTA OrderSession 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现并验证 Bitget UTA v3 单路 `OrderSession`，覆盖 private WebSocket login、limit GTC/IOC place、single cancel、请求关联、operation response、runtime adapter、TOML 配置、login-only probe 和 microbenchmark。

**Architecture:** 新增 `exchange/bitget/trading/*` 专用实现，复用 `core::StrategyOrder`、`core::OrderResponseEvent` 与 `BasicWebSocketClient`，但不抽象或改写 Gate 热路径。`OrderSession` 只拥有连接、登录、请求编码、单连接关联和直接 operation response；订单生命周期事实、rate limit、feedback、reconcile 与真实订单均保持在本计划之外。

**Tech Stack:** C++20、CMake、toml++、simdjson ondemand、OpenSSL HMAC/Base64、Abseil `flat_hash_map`、GoogleTest、Google Benchmark、CLI11、现有 WebSocket runtime。

---

## 文件结构

新增生产文件：

- `exchange/bitget/trading/order_types.h`：Bitget request/response、状态码、统计和 credentials。
- `exchange/bitget/trading/order_codecs.h`：request id 与 `clientOid` 固定格式 codec。
- `exchange/bitget/trading/order_signature.h/.cpp`：WebSocket login HMAC-SHA256/Base64。
- `exchange/bitget/trading/order_request_encoder.h`：login/place/cancel fixed-buffer JSON encoder。
- `exchange/bitget/trading/operation_response_parser.h`：login 与 trade operation response parser、错误分类。
- `exchange/bitget/trading/order_session.h`：连接/login/heartbeat/correlation/cache 状态机。
- `exchange/bitget/trading/order_session_runtime_adapter.h`：Bitget response 到 `core::OrderResponseEvent` 的转换。
- `exchange/bitget/trading/order_session_config.h/.cpp`：单 endpoint TOML parser。
- `tools/bitget/bitget_order_session_probe.cpp`：默认 dry-run，`--connect` 仅 login/heartbeat。
- `config/order_sessions/bitget_order_session.toml`：high availability private endpoint。

新增验证文件：

- `test/exchange/bitget/trading/*_test.cpp`：codec、signature/encoder、parser、config、session、adapter。
- `test/exchange/bitget/trading/CMakeLists.txt`：focused test targets。
- `benchmark/exchange/bitget/trading/order_session_benchmark.cpp`：encoder/parser release 基线。
- `benchmark/exchange/bitget/trading/CMakeLists.txt`：benchmark target。

修改集成文件：

- `exchange/bitget/CMakeLists.txt`
- `test/CMakeLists.txt`
- `benchmark/CMakeLists.txt`
- `tools/CMakeLists.txt`
- `docs/diagnostic_fields.md`

不修改 `core/market_data/types.h`；不修改公共 order ABI。

## Task 1: Codec、签名和 fixed-buffer encoder

**Files:**
- Create: `exchange/bitget/trading/order_types.h`
- Create: `exchange/bitget/trading/order_codecs.h`
- Create: `exchange/bitget/trading/order_signature.h`
- Create: `exchange/bitget/trading/order_signature.cpp`
- Create: `exchange/bitget/trading/order_request_encoder.h`
- Create: `test/exchange/bitget/trading/order_codecs_test.cpp`
- Create: `test/exchange/bitget/trading/order_request_encoder_test.cpp`
- Create: `test/exchange/bitget/trading/CMakeLists.txt`
- Modify: `exchange/bitget/CMakeLists.txt`
- Modify: `test/CMakeLists.txt`

- [ ] **Step 1: 先写 codec 失败测试**

覆盖 `RequestIdCodec::Encode/Decode` 的 place/cancel round-trip、sequence 上限和未知 type；覆盖 `ClientOidCodec::Format/Parse` 的 `a-1`、`UINT64_MAX`、零值、错误 prefix、空 suffix、负数、尾随字符和超长输入。

```cpp
EXPECT_EQ(RequestIdCodec::Decode(
              RequestIdCodec::Encode(OrderRequestType::kPlaceOrder, 9))
              .sequence,
          9U);
EXPECT_EQ(ClientOidCodec::Format(42, buffer), "a-42");
EXPECT_EQ(ClientOidCodec::Parse("a-18446744073709551615").local_order_id,
          std::numeric_limits<std::uint64_t>::max());
EXPECT_FALSE(ClientOidCodec::Parse("x-42").ok);
```

- [ ] **Step 2: 验证 codec RED**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target bitget_order_codecs_test -j8
```

Expected: target 或 header 不存在导致构建失败，失败原因仅为待实现 codec。

- [ ] **Step 3: 实现最小 codec 和类型**

`RequestIdCodec` 使用高 8 bit request type、低 56 bit sequence；`ClientOidCodec` 使用 `fmt::format_to_n` 与 `std::from_chars`，不分配字符串。`OrderSendStatus` 必须包含设计中的本地失败；`OrderResponseKind` 只含 `kAck/kRejected/kCancelRejected/kUnknownResult`。

```cpp
static constexpr std::uint64_t kSequenceMask = 0x00FFFFFFFFFFFFFFULL;
return (static_cast<std::uint64_t>(type) << 56) |
       (sequence & kSequenceMask);
```

- [ ] **Step 4: 验证 codec GREEN**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target bitget_order_codecs_test -j8
ctest --test-dir build/debug -R '^bitget_order_codecs_test$' --output-on-failure
```

Expected: build exit 0，1/1 test passed。

- [ ] **Step 5: 先写 signature/encoder 失败测试**

覆盖官方形式的 HMAC-SHA256/Base64 deterministic fixture；login timestamp 使用 seconds；place exact JSON 覆盖 buy/sell、GTC/IOC、`reduceOnly`；cancel 覆盖 `orderId + clientOid` 与只有 `clientOid`；覆盖 market、空 symbol/qty/price、local id 0 和小 buffer。

```cpp
EXPECT_TRUE(GenerateBitgetLoginSignatureBase64(
    "secret", 1700000000, signature));
EXPECT_EQ(encoded.text,
          R"({"op":"trade","id":"144115188075855873","category":"usdt-futures","topic":"place-order","args":[{"symbol":"BTCUSDT","orderType":"limit","qty":"0.001","price":"100000.0","side":"buy","timeInForce":"ioc","reduceOnly":"NO","marginMode":"crossed","clientOid":"a-9"}]})");
```

- [ ] **Step 6: 验证 signature/encoder RED**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target bitget_order_request_encoder_test -j8
```

Expected: signature/encoder 未定义导致失败。

- [ ] **Step 7: 实现最小 signature/encoder**

签名输入严格为 `fmt::format_to_n(..., "{}GET/user/verify", timestamp)`；使用 `HMAC(EVP_sha256())` 和 `EVP_EncodeBlock` 写入固定 buffer。encoder 只接受 `OrderType::kLimit` 与 GTC/IOC，所有 wire 数值使用调用方文本，不自行格式化 `double`。

- [ ] **Step 8: 验证 signature/encoder GREEN 并提交**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target bitget_order_codecs_test bitget_order_request_encoder_test -j8
ctest --test-dir build/debug -R '^bitget_order_(codecs|request_encoder)_test$' --output-on-failure
git diff --check
```

Commit:

```bash
git add exchange/bitget test/exchange/bitget test/CMakeLists.txt
git commit -m "feat: add Bitget order request codecs"
```

## Task 2: Operation response parser 与错误语义

**Files:**
- Create: `exchange/bitget/trading/operation_response_parser.h`
- Create: `test/exchange/bitget/trading/operation_response_parser_test.cpp`
- Modify: `test/exchange/bitget/trading/CMakeLists.txt`

- [ ] **Step 1: 写 parser 失败测试**

fixtures 覆盖 login `code=0` 与 login error；place/cancel `code=0`；明确业务拒绝；`40010/40725/45001`；未分类 code；malformed JSON；缺少 `id/topic/code/ts/clientOid`；wrong type；`ts` overflow；place success 的 `orderId/clientOid`。

```cpp
const auto response = ParseOperationResponse(
    R"({"op":"trade","id":"144115188075855881","topic":"place-order","code":"0","msg":"success","data":{"orderId":"123","clientOid":"a-42"},"ts":1700000000123})");
EXPECT_EQ(response.kind, OperationResponseKind::kAck);
EXPECT_EQ(response.exchange_ns, 1700000000123000000LL);
EXPECT_EQ(response.exchange_order_id, 123U);
```

- [ ] **Step 2: 验证 parser RED**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target bitget_operation_response_parser_test -j8
```

Expected: parser header/function 缺失导致失败。

- [ ] **Step 3: 实现最小 ondemand parser**

parser 输出 parse status、envelope kind、topic、decoded request id、numeric error code、`clientOid`、`orderId` 和时间。`code=0` 是 ACK；明确拒绝使用固定 allowlist；`40010/40725/45001` 和未分类 service/system code 是 `kUnknownResult`。parser 不访问 correlation map，不自行决定 local order。

- [ ] **Step 4: 验证 parser GREEN 并提交**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target bitget_operation_response_parser_test -j8
ctest --test-dir build/debug -R '^bitget_operation_response_parser_test$' --output-on-failure
git diff --check
```

Commit:

```bash
git add exchange/bitget/trading/operation_response_parser.h test/exchange/bitget/trading
git commit -m "feat: parse Bitget order operation responses"
```

## Task 3: TOML config 与 high availability 配置

**Files:**
- Create: `exchange/bitget/trading/order_session_config.h`
- Create: `exchange/bitget/trading/order_session_config.cpp`
- Create: `test/exchange/bitget/trading/order_session_config_test.cpp`
- Create: `config/order_sessions/bitget_order_session.toml`
- Modify: `exchange/bitget/CMakeLists.txt`
- Modify: `test/exchange/bitget/trading/CMakeLists.txt`

- [ ] **Step 1: 写 config 失败测试**

覆盖合法 high availability endpoint；缺 name/credential env；错误 category/position mode/margin mode；容量 0；错误 target；TLS false 与 port 443；默认 env name 和明确 endpoint 字段保真。

```cpp
ASSERT_TRUE(result.ok) << result.error;
EXPECT_EQ(result.value.connection.host, "vip-ws-uta.bitget.com");
EXPECT_EQ(result.value.connection.target, "/v3/ws/private");
EXPECT_EQ(result.value.position_mode, "one_way_mode");
```

- [ ] **Step 2: 验证 config RED**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target bitget_order_session_config_test -j8
```

Expected: config parser 缺失导致失败。

- [ ] **Step 3: 实现 fail-fast config parser 和 checked-in TOML**

使用 `config::ParseWebSocketConfig` / `config::ToConnectionConfig`；严格接受 `usdt-futures/one_way_mode/crossed` 和 `/v3/ws/private`；两个 capacity 必须大于 0。TOML 仅包含 `vip-ws-uta.bitget.com:443` 一条 endpoint，不加入 high speed 或 failover。

- [ ] **Step 4: 验证 config GREEN 并提交**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target bitget_order_session_config_test -j8
ctest --test-dir build/debug -R '^bitget_order_session_config_test$' --output-on-failure
./build/debug/test/exchange/bitget/trading/bitget_order_session_config_test
git diff --check
```

Commit:

```bash
git add exchange/bitget/trading/order_session_config.* exchange/bitget/CMakeLists.txt test/exchange/bitget/trading config/order_sessions/bitget_order_session.toml
git commit -m "feat: add Bitget order session config"
```

## Task 4: OrderSession 状态机、correlation、cache 与 heartbeat

**Files:**
- Create: `exchange/bitget/trading/order_session.h`
- Create: `test/exchange/bitget/trading/order_session_test.cpp`
- Modify: `test/exchange/bitget/trading/CMakeLists.txt`

- [ ] **Step 1: 写 session 失败测试**

通过 test hooks 驱动 `OnConnectionPhase` 和 text response，不建立网络。覆盖 active 自动 login；login success ready；login reject、`30033`、disconnect/reconnect not-ready；place/cancel request 写入 prepared slot；success/error 清 correlation；unknown id/topic mismatch/clientOid mismatch 不回调；capacity fail-fast；place ACK 写 cache；cancel ACK 保留 cache；disconnect 清 map/cache 且不回调 reject；ping/pong 与 timeout。

```cpp
session.OnConnectionPhase(websocket::ConnectionPhase::kActive);
EXPECT_FALSE(session.Ready());
session.HandleTextForTest(R"({"event":"login","code":"0","msg":"success"})");
EXPECT_TRUE(session.Ready());
EXPECT_EQ(session.PlaceOrder(order).status, OrderSendStatus::kOk);
```

- [ ] **Step 2: 验证 session RED**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target bitget_order_session_test -j8
```

Expected: `OrderSession` 不存在导致失败。

- [ ] **Step 3: 实现最小 session**

状态由 WebSocket phase + `login_ready_` 表示。active 后立即 login；只在 login success ready。place/cancel 发送前先检查 active/login/capacity/encoding/prepared slot；成功写 socket 后才插入 correlation。response 必须同时校验 id、topic、`clientOid`；已关联 response 产生 ACK/reject/unknown 并删除 request。断线清所有 correlation/cache、不重试、不合成 response。

heartbeat 使用 application text `ping/pong`；runtime probe 根据 monotonic clock 发送 ping 并在 deadline 后调用 WebSocket reconnect/stop 能力，测试用显式 clock hook 保证确定性。

- [ ] **Step 4: 验证 session GREEN 并提交**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target bitget_order_session_test -j8
ctest --test-dir build/debug -R '^bitget_order_session_test$' --output-on-failure
git diff --check
```

Commit:

```bash
git add exchange/bitget/trading/order_session.h test/exchange/bitget/trading
git commit -m "feat: add Bitget order session"
```

## Task 5: Runtime adapter 与 OrderManager 边界

**Files:**
- Create: `exchange/bitget/trading/order_session_runtime_adapter.h`
- Create: `test/exchange/bitget/trading/order_session_runtime_adapter_test.cpp`
- Modify: `exchange/bitget/CMakeLists.txt`
- Modify: `test/exchange/bitget/trading/CMakeLists.txt`

- [ ] **Step 1: 写 adapter 失败测试**

覆盖 `kAck/kRejected/kCancelRejected/kUnknownResult` 映射与字段保真；将 adapter 接到真实 `core::OrderManager` test gateway，证明 place ACK 不进入 accepted、cancel ACK 保持 `kCancelSent`、明确 cancel reject 恢复 cancel 前状态。

```cpp
adapter.PushOrderResponseForTest({.kind = OrderResponseKind::kAck,
                                  .local_order_id = order_id});
EXPECT_EQ(manager.FindOrder(order_id)->status, core::OrderStatus::kSent);
```

- [ ] **Step 2: 验证 adapter RED**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target bitget_order_session_runtime_adapter_test -j8
```

Expected: adapter 缺失导致失败。

- [ ] **Step 3: 实现最小 adapter**

adapter 仿照 Gate 用 response handler 转发 `OnOrderSessionLoginReady/NotReady` 与 `OnOrderResponse`，但只映射 Bitget 四种 operation response，不提升 ACK 为 accepted/cancelled。

- [ ] **Step 4: 验证 adapter GREEN 并提交**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target bitget_order_session_runtime_adapter_test -j8
ctest --test-dir build/debug -R '^bitget_order_session_runtime_adapter_test$' --output-on-failure
git diff --check
```

Commit:

```bash
git add exchange/bitget/trading/order_session_runtime_adapter.h exchange/bitget/CMakeLists.txt test/exchange/bitget/trading
git commit -m "feat: adapt Bitget order responses to runtime"
```

## Task 6: Dry-run/login-only probe 和 diagnostics 文档

**Files:**
- Create: `tools/bitget/bitget_order_session_probe.cpp`
- Modify: `tools/CMakeLists.txt`
- Modify: `docs/diagnostic_fields.md`

- [ ] **Step 1: 写 probe CLI 行为检查**

先仅接入 build target，运行默认命令应打印配置摘要并退出 0；`--connect` 才读取 env credentials 并连接。probe 源码不得包含 `PlaceOrder` 或 `CancelOrder` 调用。

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target bitget_order_session_probe -j8
```

Expected: target 不存在，RED。

- [ ] **Step 2: 实现 probe 与日志字段**

probe 使用 config parser；dry-run 不读取 credential value；connect 模式验证三个 env 非空，启动 session，等待 ready/heartbeat/clean stop，并输出 endpoint、phase、login/heartbeat metrics。提供 `--duration-s`，不得提供 place/cancel CLI 参数。新增 `bitget_order_session_*` log key 后在 `docs/diagnostic_fields.md` 用中文登记字段、单位、来源和敏感信息边界。

- [ ] **Step 3: 验证 dry-run 和静态安全边界**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target bitget_order_session_probe -j8
./build/debug/tools/bitget_order_session_probe --config config/order_sessions/bitget_order_session.toml
rg -n 'PlaceOrder|CancelOrder|place-order|cancel-order' tools/bitget/bitget_order_session_probe.cpp
git diff --check
```

Expected: build/dry-run exit 0；`rg` 无命中。

- [ ] **Step 4: 提交 probe/docs**

```bash
git add tools/bitget/bitget_order_session_probe.cpp tools/CMakeLists.txt docs/diagnostic_fields.md
git commit -m "feat: add Bitget login-only order session probe"
```

## Task 7: Release microbenchmark

**Files:**
- Create: `benchmark/exchange/bitget/trading/order_session_benchmark.cpp`
- Create: `benchmark/exchange/bitget/trading/CMakeLists.txt`
- Modify: `benchmark/CMakeLists.txt`

- [ ] **Step 1: 添加 benchmark target 并验证 RED**

benchmark 覆盖 fixed-buffer place/cancel encoder 和 padded operation response parser；每次 iteration 使用 `DoNotOptimize`，parser buffer 包含 `simdjson::SIMDJSON_PADDING`。

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/release --target bitget_order_session_benchmark -j8
```

Expected: target 不存在。

- [ ] **Step 2: 实现 benchmark 并建立基线**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/release --target bitget_order_session_benchmark -j8
./build/release/benchmark/exchange/bitget/trading/bitget_order_session_benchmark --benchmark_min_time=0.2s
```

Expected: 所有 case 完成；只记录本机基线，不写性能收益结论。

- [ ] **Step 3: 提交 benchmark**

```bash
git add benchmark/CMakeLists.txt benchmark/exchange/bitget/trading
git commit -m "bench: add Bitget order session baseline"
```

## Task 8: 全量验证、自审与最终修复

**Files:**
- Modify: only files required by findings

- [ ] **Step 1: 格式化新增 C++**

Run:

```bash
git diff --name-only a4ece9a..HEAD -- '*.h' '*.cpp' | xargs clang-format -i
```

- [ ] **Step 2: Debug build 与 focused/full tests**

Run:

```bash
TMPDIR=/home/liuxiang/tmp ./build.sh debug
ctest --test-dir build/debug -R 'bitget_.*order|core_order' --output-on-failure
ctest --test-dir build/debug --output-on-failure
```

Expected: build exit 0，focused/full tests 0 failures。

- [ ] **Step 3: Release build 与 focused tests**

Run:

```bash
TMPDIR=/home/liuxiang/tmp ./build.sh release
ctest --test-dir build/release -R 'bitget_.*order' --output-on-failure
./build/release/benchmark/exchange/bitget/trading/bitget_order_session_benchmark --benchmark_min_time=0.2s
```

Expected: build/tests/benchmark exit 0。

- [ ] **Step 4: 边界和仓库检查**

Run:

```bash
rg '#include "evaluation/' core exchange tools
rg 'aquila_evaluation' core exchange tools
rg -n 'BITGET_(TEST|PROBE)_(KEY|SECRET|PASSPHRASE)|signature|passphrase' exchange/bitget/trading tools/bitget/bitget_order_session_probe.cpp
git diff --check a4ece9a..HEAD
git status --short --branch
```

Expected: evaluation 两条命令无命中；敏感字段只出现在配置/env 读取和 redacted 边界，不进入日志；只有用户原有 `core/market_data/types.h` 保持 dirty。

- [ ] **Step 5: Login-only live 验证条件判断**

仅当 `BITGET_TEST_KEY/BITGET_TEST_SECRET/BITGET_TEST_PASSPHRASE` 都存在、网络可用且无需改账户/权限时运行：

```bash
timeout 30s ./build/release/tools/bitget_order_session_probe \
  --config config/order_sessions/bitget_order_session.toml \
  --connect --duration-s 10
```

不得发送 place/cancel。凭据缺失或网络条件不满足时，不运行并在报告中写明原因。

- [ ] **Step 6: 只读自审**

逐项核对 design 的 current scope、non-goals、ACK boundary、unknown result、disconnect、capacity、heartbeat、config、probe safety 与 tests。按 Critical/Important/Minor 记录发现；修复 Critical/Important 后重新运行受影响验证。由于本任务明确禁止派生 subagent，`requesting-code-review` 的 reviewer dispatch 由本 worker 的独立只读 diff review 和全量验证替代，并在最终报告中说明这一限制。

- [ ] **Step 7: 提交格式化或 review 修复**

如有文件变化：

```bash
git add <only-bitget-order-session-files>
git commit -m "fix: address Bitget order session review"
```

不提交 `core/market_data/types.h`，不 push。
