# aquila

`aquila` 是面向 crypto 高频交易系统的 C++20 仓库。当前仓库的主要可运行切片包括低延迟 WebSocket client、Gate futures SBE BBO 行情、Binance USD-M futures JSON bookTicker 行情、data session / strategy `DataReader`、Gate 下单与订单回报第一版、trading runtime、LeadLag binary replay，以及 Gate / Binance 期货合约元数据查询脚本。WebSocket 冷路径负责 DNS / TCP / TLS / WebSocket handshake，热路径由单 owner thread 驱动 `CriticalSession::DriveRead()` / `DriveWrite()` / heartbeat。

## Onboarding

新对话或新接手开发前，先读 `AGENTS.md` 和 `docs/project_onboarding_guide.md`。其中
`docs/project_onboarding_guide.md` 是当前状态、文档索引、代码入口、验证命令和下一步建议的集中入口。
当用户输入“结束对话”时，按 `AGENTS.md` 和 onboarding 中的收尾流程整理文档、更新交接提示、验证并提交。

如果接手 LeadLag replay / ORDI_USDT 数据对账，先读 `strategy/lead_lag/README.md` 和
`docs/lead_lag_ordi_tardis_hdf_signal_pnl_comparison.md`。Tardis 与 HDF 转换后的 `BookTicker` binary 都可通过
`config/data_readers/lead_lag_ordi_binary_replay.toml` 同类配置回放，但两条数据源不是逐 tick 等价输入。

## 构建依赖

项目默认使用本机用户目录下的 vcpkg：

```text
$HOME/vcpkg
```

`cmake/settings.cmake` 会从下面路径加载 toolchain：

```text
$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake
```

Linux 下安装当前依赖：

```bash
$HOME/vcpkg/vcpkg install \
  fmt \
  quill \
  tomlplusplus \
  cli11 \
  gtest \
  magic-enum \
  vincentlaucsb-csv-parser \
  yyjson \
  simdjson \
  nameof \
  drogon \
  fast-float \
  benchmark \
  abseil \
  ftxui \
  --triplet x64-linux
```

## 构建

Debug 构建：

```bash
./build.sh debug
```

Release 构建：

```bash
./build.sh release
```

默认并行度是 8，也可以指定：

```bash
./build.sh --jobs 16 release
```

## 测试

运行 WebSocket 回归测试：

```bash
ctest --test-dir build/debug -R websocket_ --output-on-failure
```

Release 验证：

```bash
./build.sh release
ctest --test-dir build/release -R websocket_ --output-on-failure
```

单个测试二进制也可以直接运行，例如：

```bash
./build/debug/test/websocket/websocket_frame_codec_test
./build/debug/test/websocket/websocket_critical_session_test
```

## WebSocket Benchmarks

先构建 release：

```bash
./build.sh release
```

当前 WebSocket benchmark target：

```text
prepared_write_benchmark
frame_codec_benchmark
third_party_frame_codec_comparison_benchmark
degraded_evaluator_benchmark
active_spin_benchmark
session_write_path_benchmark
session_tls_write_path_benchmark
session_read_path_benchmark
session_mixed_path_benchmark
message_handler_dispatch_benchmark
clock_source_benchmark
runtime_loopback_benchmark
affinity_policy_comparison_benchmark
cold_path_handshake_benchmark
```

当前交易所 benchmark target：

```text
gate_futures_market_data_benchmark
binance_futures_market_data_benchmark
gate_submit_response_parse_benchmark
```

也可以只构建 WebSocket benchmark：

```bash
cmake --build build/release --target \
  prepared_write_benchmark \
  frame_codec_benchmark \
  third_party_frame_codec_comparison_benchmark \
  degraded_evaluator_benchmark \
  active_spin_benchmark \
  session_write_path_benchmark \
  session_tls_write_path_benchmark \
  session_read_path_benchmark \
  session_mixed_path_benchmark \
  message_handler_dispatch_benchmark \
  clock_source_benchmark \
  runtime_loopback_benchmark \
  affinity_policy_comparison_benchmark \
  cold_path_handshake_benchmark \
  -j8
```

P3 推荐 smoke benchmark：

```bash
taskset -c 2 ./build/release/benchmark/websocket/session_read_path_benchmark
taskset -c 2 ./build/release/benchmark/websocket/session_write_path_benchmark
taskset -c 2 ./build/release/benchmark/websocket/active_spin_benchmark
taskset -c 2 ./build/release/benchmark/websocket/clock_source_benchmark
taskset -c 2 ./build/release/benchmark/websocket/runtime_loopback_benchmark
```

各 benchmark 定位：

- `prepared_write_benchmark`：prepared-write arena acquire / release 延迟。
- `frame_codec_benchmark`：WebSocket frame encode/decode microbenchmark。
- `third_party_frame_codec_comparison_benchmark`：Aquila / 线性 buffer / Drogon style frame codec 对比。
- `degraded_evaluator_benchmark`：degraded evaluator 单次评估成本。
- `active_spin_benchmark`：active spin loop skeleton 成本。
- `session_write_path_benchmark`：`CommitPreparedWrite()` 到 `DriveWrite()` 写入本地 socket buffer 的路径。
- `session_tls_write_path_benchmark`：本地 TLS write path 基线。
- `session_read_path_benchmark`：本地 socketpair 写入 frame 到 consumer 收到 `MessageView` 的路径。
- `session_mixed_path_benchmark`：read/write 混合、write budget 和 callback flush 对 read latency 的影响。
- `message_handler_dispatch_benchmark`：`MessageCallback` 与 typed message handler dispatch 对照。
- `clock_source_benchmark`：`ClockSource` 三种取时方式成本。
- `runtime_loopback_benchmark`：`ActiveSpinLoop + CriticalSession` 本地 socketpair loopback 延迟。
- `affinity_policy_comparison_benchmark`：不同 affinity / prefault / memory-lock 策略对比。
- `cold_path_handshake_benchmark`：本地 TCP + TLS + WebSocket handshake 冷路径耗时。

这些 benchmark 是组件级或本地 loopback 基准，不是完整交易所 `wss` 链路延迟报告。对性能结论必须记录 CPU、调度策略、affinity、OpenSSL、kernel 和 benchmark 命令。

## Live Probe

Gate private endpoint inventory：

| 用途 | URL | `websocket_probe` 参数 |
|---|---|---|
| 现货 WebSocket v4 | `wss://spotws-private.gateapi.io/ws/v4/` | `--host spotws-private.gateapi.io --port 443 --target /ws/v4/ --tls` |
| 衍生品 WebSocket v4 | `wss://fxws-private.gateapi.io/v4/ws/usdt` | `--host fxws-private.gateapi.io --port 443 --target /v4/ws/usdt --tls` |
| 衍生品 WebSocket v4 明文 | `ws://fxws-private.gateapi.io/v4/ws/usdt` | `--host fxws-private.gateapi.io --port 80 --target /v4/ws/usdt --no-tls` |
| API v4 HTTP | `https://apiv4-private.gateapi.io` | 不适用于 `websocket_probe`；供后续 REST / 交易适配使用 |

GateIO public WebSocket handshake smoke：

```bash
timeout 15s ./build/debug/tools/websocket_probe \
  --host fx-ws.gateio.ws \
  --port 443 \
  --target /v4/ws/usdt \
  --tls \
  --cpu 2
```

Gate private WebSocket handshake smoke 示例：

```bash
timeout 15s ./build/debug/tools/websocket_probe \
  --host fxws-private.gateapi.io \
  --port 443 \
  --target /v4/ws/usdt \
  --tls \
  --cpu 2
```

Gate private 明文 WebSocket handshake smoke 示例：

```bash
timeout 15s ./build/debug/tools/websocket_probe \
  --host fxws-private.gateapi.io \
  --port 80 \
  --target /v4/ws/usdt \
  --no-tls \
  --cpu 2
```

probe 用于验证 cold path 能进入 active state，并输出 state transition、错误码和最终 metrics。它不是长稳健康监控工具。

Data session config dry-run：

```bash
./build/debug/tools/gate_data_session
./build/debug/tools/binance_data_session
```

Trading runtime demo dry-run：

```bash
./build/debug/tools/gate_demo_strategy --config config/strategies/demo_strategy.toml
```

`gate_demo_strategy` 默认只解析 strategy / data reader / Gate order session / demo 策略配置，不连接 WebSocket、不打开 SHM、不下单；必须显式传 `--execute` 才进入实盘链路。

Gate `OrderSession` failure response 诊断工具 dry-run：

```bash
./build/debug/tools/gate_order_session_failure_probe --probe cancel-rejected
```

该工具只直接测试 Gate `OrderSession` 协议响应，默认不连接 WebSocket；必须显式传 `--execute` 才发送真实请求。

Gate `OrderSession` RTT probe V1a dry-run：

```bash
./build/debug/tools/gate_order_session_rtt_probe \
  --config config/order_session_rtt_probe/gate_order_session_rtt_probe.toml
```

当前 V1a 解析 TOML 和 `probe.inputs.connections_file` 指向的 connections CSV 并生成 run plan；CSV 每行定义一条
`OrderSession` 连接，`connect_ip` 允许重复。pinned session config builder、sample flow、sample executor、
`local_order_id` 分配和 sample CSV writer 已作为 live sample 前置逻辑落地。sample flow 已保存 Ack 接收
时间和 stage status，校验 Ack / final response `local_order_id` 必须匹配当前 stage，并已覆盖 GTC cancel reject 后立即派发
reduce-only close、feedback fill / timeout 后进入 reduce-only close、close Ack 后等待 terminal feedback 的纯状态流转。可用
`probe.order.order_mode` 选择 `ioc`、`gtc` 或默认 `ioc+gtc`；`probe.sampling.order_session_interval_ms=0` 表示同一
cycle 内下一个 order session 等下一条行情触发，非 0 表示发完当前 order session 后设置非阻塞 deadline，到点后用当时最新
BBO 下下一单，不额外等新行情、不 sleep / busy loop。可用
`--connections-file` 临时覆盖连接列表；可用 `--live-preflight` 加载 connections CSV 和 base order session config，构建
single-session 或 multi-session live plan 并打印输出路径；该模式不连接 WebSocket、不下单。

仓库内 Gate data session 示例配置使用公网 `wss://fx-ws.gateio.ws:443`，因此
`enable_tls = true`。如果部署 private link / plain WS，需要使用对应 private endpoint 并显式设置
`enable_tls = false`。

Binance USD-M futures bookTicker live probe：

```bash
timeout 15s ./build/debug/tools/binance_futures_book_ticker_probe \
  --contract BTCUSDT \
  --symbol-id 1 \
  --duration-ms 10000 \
  --cpu 2
```

Binance probe 使用 raw stream URL，例如 `/public/ws/btcusdt@bookTicker`；第一版不发送 runtime `SUBSCRIBE`。

## Futures Contract Metadata

Gate / Binance 合约基础信息脚本输出同一组一类下单前必需字段，字段语义和交易所差异见 `docs/futures_contract_metadata_fields.md`。

Gate USDT futures：

```bash
scripts/gate/query_futures_contracts.py BTC_USDT ETH_USDT --format csv
```

Binance USD-M futures：

```bash
scripts/binance/query_um_futures_contracts.py BTCUSDT ETHUSDT --format csv
```

两个脚本也支持 `--file`，文件内每行一个 symbol；输出可直接转换为 `pandas.DataFrame`。

## Gate REST Read-only Query

Gate REST read-only query 脚本使用 APIv4 authenticated GET 接口查询当前 API key 可访问的账户、订单和
仓位信息。默认读取 `TEST_KEY` / `TEST_SECRET` 环境变量；`--api-key` 和 `--api-secret` 参数表示环境变量名，
不直接传 secret 值。不写子命令时兼容旧行为，等价于 `account`。

```bash
TEST_KEY=... TEST_SECRET=... scripts/gate/query_gate_account.py account \
  --settle usdt \
  --currency USDT \
  --currency-pair BTC_USDT \
  --contract BTC_USDT \
  --allow-partial
```

账户查询默认覆盖：

```text
GET /wallet/total_balance
GET /futures/{settle}/accounts
GET /wallet/fee
GET /futures/{settle}/fee
```

订单查询：

```bash
TEST_KEY=... TEST_SECRET=... scripts/gate/query_gate_account.py orders \
  --contract BTC_USDT \
  --status open \
  --limit 20

TEST_KEY=... TEST_SECRET=... scripts/gate/query_gate_account.py orders \
  --order-id 36028827892199865
```

仓位查询：

```bash
TEST_KEY=... TEST_SECRET=... scripts/gate/query_gate_account.py positions

TEST_KEY=... TEST_SECRET=... scripts/gate/query_gate_account.py positions \
  --contract BTC_USDT
```

## Gate REST Futures Order Test

Gate REST futures 下单测试脚本使用 `POST /futures/{settle}/orders` 创建常规 futures order，默认只
dry-run 打印请求体；必须显式加 `--execute` 才会真实提交。真实提交后默认会立刻调用
`DELETE /futures/{settle}/orders/{order_id}` 撤单；只有加 `--keep-open` 才会保留挂单。脚本内置单次
最大下单手数风控，当前 `MAX_ORDER_SIZE = 5`，超过会在发请求前拒绝。

```bash
scripts/gate/place_futures_order.py \
  --contract BTC_USDT \
  --side buy \
  --size 1 \
  --price 1 \
  --tif gtc
```

真实提交示例：

```bash
TEST_KEY=... TEST_SECRET=... scripts/gate/place_futures_order.py \
  --contract BTC_USDT \
  --side buy \
  --size 1 \
  --price 1 \
  --tif gtc \
  --execute
```

`--api-key` 和 `--api-secret` 参数表示环境变量名，不直接传 secret 值。默认 `price=1` 用于降低误成交
概率，但真实提交仍可能因交易所价格偏离限制被拒绝；如需 accepted order，应显式传入符合当前行情和合约规则的价格。

命令行撤单示例：

```bash
TEST_KEY=... TEST_SECRET=... scripts/gate/place_futures_order.py cancel 36028827891355772
```

`cancel` 参数可以是 Gate 返回的 order id，也可以是下单时传入的 `text` 客户端 ID。

## Public / Private 延迟对比

`websocket_latency_compare` 用于同时连接 Gate public / private 衍生品 WebSocket v4，订阅同一个 `futures.book_ticker` 合约，并按 `symbol:update_id` 匹配同一条行情在两条链路上的本机到达时间。

示例：

```bash
./build/release/tools/websocket_latency_compare \
  --public-host fx-ws.gateio.ws \
  --public-target /v4/ws/usdt \
  --private-host fxws-private.gateapi.io \
  --private-target /v4/ws/usdt \
  --channel futures.book_ticker \
  --contract BTC_USDT \
  --duration-ms 30000
```

可选绑核：

```bash
./build/release/tools/websocket_latency_compare \
  --contract BTC_USDT \
  --duration-ms 60000 \
  --public-cpu 2 \
  --private-cpu 3
```

如果 private 侧需要比较明文 `ws://` endpoint，可分别指定 private port、target 和 TLS 开关：

```bash
./build/release/tools/websocket_latency_compare \
  --public-host fx-ws.gateio.ws \
  --public-port 443 \
  --public-target /v4/ws/usdt \
  --public-tls \
  --private-host fxws-private.gateapi.io \
  --private-port 80 \
  --private-target /v4/ws/usdt \
  --private-no-tls \
  --contract BTC_USDT \
  --duration-ms 30000
```

输出中的 `private_lead_ns = public_arrival_ns - private_arrival_ns`：

- 正数：private 比 public 更早到达。
- 负数：public 比 private 更早到达。
- `matched`：两条链路都收到且按 `symbol:update_id` 匹配成功的行情条数。
- `pending_public` / `pending_private`：采样结束时只在其中一条链路出现、尚未匹配到对端的 update。

这个工具比较的是同机同钟下的行情到达差，不是完整交易策略延迟，也不是交易所单向网络延迟。高可信结论应在独占 CPU、固定 affinity、稳定网络和足够长采样窗口下记录 p50 / p99 / p99.9。

## 长稳验证

合并到 `main` 前，如环境允许，应执行长稳验证并记录最终 metrics：

```bash
timeout 4h ./build/release/tools/websocket_probe \
  --host fxws-private.gateapi.io \
  --port 443 \
  --target /v4/ws/usdt \
  --tls \
  --cpu 2
```

如果验证环境不允许访问外网交易所，则运行本地 loopback integration test 和 release benchmark，并在 P3 验证记录中明确说明没有采集 live exchange 证据。

## 低延迟运行注意事项

- 使用独占或隔离 CPU 运行 owner thread。
- 优先固定 CPU affinity，并记录 `taskset` / scheduler 状态。
- 进入 steady state 前预热和 prefault hot path。
- 只在专用低延迟主机上启用实时调度。
- benchmark 结论默认看 p50 / p99 / p99.9，不只看均值。
