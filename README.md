# aquila

`aquila` 是面向 crypto 高频交易系统的 C++20 仓库。当前仓库的主要可运行切片是低延迟 WebSocket client：冷路径负责 DNS / TCP / TLS / WebSocket handshake，热路径由单 owner thread 驱动 `CriticalSession::DriveRead()` / `DriveWrite()` / heartbeat。

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
session_read_path_benchmark
clock_source_benchmark
runtime_loopback_benchmark
affinity_policy_comparison_benchmark
cold_path_handshake_benchmark
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
  session_read_path_benchmark \
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
- `session_read_path_benchmark`：本地 socketpair 写入 frame 到 consumer 收到 `MessageView` 的路径。
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
