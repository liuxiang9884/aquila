# aquila 新对话导读

## 目的

这份文档给新的模型对话或新接手开发者使用。目标是在不重读整个历史对话的前提下，快速理解 `aquila` 当前状态、重要文档、代码入口和下一步应该怎么接手。

## 30 秒速览

- 项目：面向 crypto 高频交易的 C++20 低延迟交易系统。
- 当前重点：WebSocket 内核已经完成 P0/P1/P2/P3 主体，下一阶段进入 Gate 行情 / 交易接入和订单链路设计。
- 构建：CMake + `build.sh`。
- 核心原则：正确性、确定性、最低延迟、尾延迟可控、固定容量、少动态分配、性能结论必须有 benchmark / profile / live probe 证据。
- 当前建议分支入口：`main`。

## 新对话第一步

先运行：

```bash
git -C /home/liuxiang/dev/aquila status --short
git -C /home/liuxiang/dev/aquila log --oneline -8
```

再读：

```text
AGENTS.md
README.md
doc/project_onboarding_guide.md
doc/agent-handoff-gate-trade-architecture.md
doc/websocket_read_write_benchmark_comparison.md
```

如果继续 Gate 交易架构，优先读 `doc/agent-handoff-gate-trade-architecture.md`。如果继续 WebSocket 性能优化，优先读 `doc/websocket_client_future_optimizations.md`。

## 文档索引

| 文档 | 什么时候读 | 关键内容 |
| --- | --- | --- |
| `AGENTS.md` | 每次新会话最先读 | 中文/英文约定、低延迟原则、测试/benchmark/提交规则。 |
| `README.md` | 了解构建和工具入口 | build、ctest、benchmark、probe、latency compare 的运行方式。 |
| `doc/project_structure.md` | 理解目录边界 | `core`、`exchange`、`test`、`benchmark`、`doc` 的职责。 |
| `doc/websocket_client_design_v1.0.md` | 理解当前 WebSocket client 设计 | session、transport、control、metrics、runtime 边界。 |
| `doc/websocket_frame_codec_receive_strategies.md` | 理解 FrameCodec decode 为什么这样设计 | mirrored ring、direct delivery、fast path、QueuedFrameCodec、decode 收口结论。 |
| `doc/websocket_third_party_comparison.md` | 理解 MengRao/websocket 对比 | 为什么三方库不能直接作为生产内核。 |
| `doc/websocket_read_write_benchmark_comparison.md` | 快速看 read/write benchmark 对比 | `aquila`、Drogon-style、`third_party/websocket` read/write 差异和数值。 |
| `doc/websocket_client_future_optimizations.md` | 继续 WebSocket 优化时读 | read/write/active spin/network 的未来优化 backlog。 |
| `doc/agent-handoff-gate-trade-architecture.md` | 继续 Gate 交易架构时读 | Gate 文档结论、Sirius 旧实现、双 WS login 测试、三种线程模型。 |
| `doc/reviews/2026-04-24-websocket-client-gap-analysis.md` | 追溯 P0/P1/P2/P3 证据 | 每个阶段问题、修复、测试和 benchmark 证据。 |

## 代码入口

### WebSocket 内核

| 文件 | 职责 |
| --- | --- |
| `core/websocket/types.h` | 配置、状态、错误码、flush mode 等基础类型。 |
| `core/websocket/frame_codec.h` | WebSocket frame encode/decode、mirrored ring、mask key pool、8B XOR。 |
| `core/websocket/critical_session.h` | read/write pump、pending write、control slot、heartbeat、`kTryFlushOne`。 |
| `core/websocket/websocket_client.h` | plain/TLS client 生命周期、reconnect/backoff、runtime loop 集成。 |
| `core/websocket/active_spin_loop.h` | active spin loop 调度。 |
| `core/websocket/prepared_write.h` | 预分配 write slot / arena。 |

### 工具

| 文件 | 用途 |
| --- | --- |
| `tools/websocket_probe.cpp` | 单连接 live probe，支持 graceful stop 后输出最终 metrics。 |
| `tools/websocket_latency_compare.cpp` | public/private 或多连接 latency compare / warmup selection。 |
| `scripts/gate/test_gate_ws_connect.py` | Gate WS 连接 / login smoke。 |
| `scripts/gate/test_gate_ws_dual_login.py` | 同账号双 WebSocket login 验证。 |

### Benchmark

| 文件 | 用途 |
| --- | --- |
| `benchmark/websocket/frame_codec_benchmark.cpp` | `FrameCodec` 单点 encode/decode。 |
| `benchmark/websocket/third_party_frame_codec_comparison_benchmark.cpp` | `aquila`、Drogon-style、third-party read codec 对比。 |
| `benchmark/websocket/session_write_path_benchmark.cpp` | write path、masking、Drogon-style、third-party write 对比。 |
| `benchmark/websocket/session_read_path_benchmark.cpp` | session read path socketpair 基线。 |
| `benchmark/websocket/session_mixed_path_benchmark.cpp` | read/write 混合、write budget、callback flush。 |
| `benchmark/websocket/session_tls_write_path_benchmark.cpp` | local TLS write path 基线。 |
| `benchmark/websocket/active_spin_benchmark.cpp` | active spin loop / stop check / clock 相关基线。 |

## 当前重要结论

### WebSocket decode

`FrameCodec` decode 主体已经收口：

- 默认生产路径使用 mirrored receive ring。
- 单帧和 repeated `Poll()` 多帧 drain 走 direct delivery。
- ready metadata ring 已移到 `QueuedFrameCodec` 对照路径。
- data frame fast path 覆盖 `FIN=1`、`RSV=0`、text/binary、server unmasked、payload length `<= 65535`。
- control frame、fragmentation、masked inbound、payload `>= 65536`、RSV / unknown opcode 走 generic path。

后续不要轻易为了几 ns 删除 `MessageView`、capacity/degraded、mirrored ring 或 payload 生命周期。

### WebSocket write

当前 write path 已完成：

- client mask key pool。
- 8-byte chunk XOR。
- dedicated control write slot。
- `CommitPreparedWrite(write, WriteFlushMode::kTryFlushOne)`。
- business write budget 默认 `1`。

下一步 write 优化重点不应是通用 frame encode，而是真实 Gate 订单 payload、签名、序列化、plain/TLS socket write 和交易所 ack。

### Gate 交易架构

当前推荐方向：

```text
StrategyThread + GateOrderSubmitWsSession
GateOrderUpdateThread + GateOrderUpdateWsSession
feedback SPSC -> StrategyThread
```

原因：

- 下单路径最短。
- 行情 burst 时策略线程仍以行情为最高优先级。
- `futures.orders` / `futures.usertrades` / SBE decode 不污染下单路径。
- Gate 已通过脚本验证允许同一账号两个 WS 同时 login。

## 常用验证命令

构建：

```bash
./build.sh debug
./build.sh release
```

WebSocket 测试：

```bash
ctest --test-dir build/debug -R websocket_ --output-on-failure
ctest --test-dir build/release -R websocket_ --output-on-failure
```

核心 benchmark：

```bash
taskset -c 2 ./build/release/benchmark/websocket/frame_codec_benchmark
taskset -c 2 ./build/release/benchmark/websocket/third_party_frame_codec_comparison_benchmark
taskset -c 2 ./build/release/benchmark/websocket/session_write_path_benchmark
taskset -c 2 ./build/release/benchmark/websocket/session_mixed_path_benchmark
```

Gate 双 WS login：

```bash
TEST_KEY=... TEST_SECRET=... scripts/gate/test_gate_ws_dual_login.py --timeout 8
```

## 接手注意事项

- 不要 push，除非用户明确要求。
- 修改完成后默认提交到当前 branch，commit message 用英文。
- 文档中文；代码注释英文。
- 性能结论必须写明 benchmark / live probe / profile 证据。
- `third_party/` 中的 Drogon、Sirius、MengRao websocket 是参考代码，通常不提交改动。
- 如果继续 Gate 交易实现，先补设计 / benchmark 边界，不要直接把 Sirius 的 Drogon 架构搬进主线。

## 下一步建议

如果新对话从 Gate 交易继续，建议顺序：

1. 读取 `doc/agent-handoff-gate-trade-architecture.md`。
2. 确认是否采用 `GateOrderSubmitWsSession` + `GateOrderUpdateWsSession`。
3. 确认 SBE schema 获取和 decoder 集成方式。
4. 设计 `RequestIdCodec`、`OrderTextCodec`、`OrderFeedback` 固定结构。
5. 写最小 benchmark：submit send、update decode、feedback SPSC。
6. 再开始 C++ 实现。
