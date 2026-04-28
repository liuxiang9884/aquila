# Agent 接手说明：Gate 交易 WebSocket 架构讨论

## 这份文件是给谁的

这份 handoff 给下一轮模型或开发者使用，用来承接 2026-04-28 关于 Gate futures 交易 WebSocket 架构的讨论。

本文不是已批准的实现计划；它记录已经确认的协议事实、实验结果、Sirius 旧实现结论，以及当前推荐的线程 / session 划分方向。

## 当前仓库状态

- 当前主线分支：`main`
- 本文更新前最近相关提交：
  - `99f0f92 websocket: expose readable payload tail bytes`
  - `bc3a197 benchmark: add minimal Gate submit ack parsing`
  - `8c6773a benchmark: add padded Gate submit parse cases`
  - `5d02950 benchmark: compare Gate submit simdjson parsing`
  - `51d7df0 benchmark: compare Gate submit insitu parsing`
  - `d98cab8 benchmark: add Gate submit response parser`
  - `5f099c9 scripts: move Gate probes into subdirectory`
  - `ab3d2d9 scripts: add Gate dual websocket login probe`
- 新接手时先执行：

```bash
git -C /home/liuxiang/dev/aquila status --short
git -C /home/liuxiang/dev/aquila log --oneline -8
```

## Gate 文档结论

参考文档：

- Gate Futures WebSocket v4：https://www.gate.com/docs/developers/futures/ws/zh_CN/
- 订单频道：https://www.gate.com/docs/developers/futures/ws/zh_CN/#%E8%AE%A2%E5%8D%95%E9%A2%91%E9%81%93
- 用户私有成交频道：https://www.gate.com/docs/developers/futures/ws/zh_CN/#%E7%94%A8%E6%88%B7%E7%A7%81%E6%9C%89%E6%88%90%E4%BA%A4%E9%A2%91%E9%81%93
- SBE 数据推送：https://www.gate.com/docs/developers/futures/ws/zh_CN/#sbe-%E6%95%B0%E6%8D%AE%E6%8E%A8%E9%80%81

已经确认：

1. Gate futures WebSocket 交易 API 是 JSON request / response。
2. `futures.login` 用于 WebSocket 交易 API 登录。
3. `futures.order_place` / `futures.order_batch_place` 会出现 `ack=true` 的请求接收确认，以及 `ack=false` 的实际结果。
4. 撤单、改单、查询类 API 返回对应 JSON result / error。
5. `futures.orders`、`futures.usertrades`、`futures.positions` 属于私有推送频道，不是 order API response。
6. SBE endpoint 使用 JSON 做请求和首次响应，使用 SBE 做数据推送；同一条连接上可能混合 JSON text frame 和 SBE binary frame。
7. SBE 文档明确覆盖 `futures.orders`、`futures.usertrades`、`futures.positions`；不要默认假设 `futures.balances` 已覆盖 SBE。

## 当前建议命名

不要继续使用过于抽象的 `OrderEntry` / `PrivateReport`。建议命名为：

```text
GateOrderSubmitWsSession
GateOrderUpdateWsSession
```

业务模块命名：

```text
GateOrderSubmitter
GateOrderUpdateReceiver
```

含义：

| 名称 | 主要职责 | 下行消息格式 |
| --- | --- | --- |
| `GateOrderSubmitWsSession` | 登录、下单、撤单、改单、订单查询；读取轻量 API response / ack / error | JSON |
| `GateOrderUpdateWsSession` | 登录或订阅鉴权，订阅订单 / 成交 / 持仓更新；读取持续回报流 | JSON 控制消息 + SBE binary push |

## 双 WebSocket 登录测试

新增脚本：

```text
scripts/gate/test_gate_ws_dual_login.py
```

运行方式：

```bash
TEST_KEY=... TEST_SECRET=... scripts/gate/test_gate_ws_dual_login.py --timeout 8
```

脚本行为：

```text
1. 建立两个 Gate futures WebSocket 连接
2. 两个连接都发送 futures.login
3. 输出 request_id、uid、conn_id、conn_trace_id
4. 判断是否 both_logged_in_same_account
```

已验证结果：

```text
[OK] ws=A uid=14446887 ... detail=login succeeded
[OK] ws=B uid=14446887 ... detail=login succeeded
result=both_logged_in_same_account
```

结论：Gate futures WebSocket 允许同一账号同时登录两个物理 WebSocket 连接。这支持 `GateOrderSubmitWsSession` + `GateOrderUpdateWsSession` 的双连接设计。

## Submit WS JSON response 解析结论

已新增 Gate submit response parser 与 benchmark：

```text
exchange/gate/trading/submit_response_parser.h
test/exchange/gate/trading/submit_response_parser_test.cpp
benchmark/exchange/gate/trading/submit_response_parse_benchmark.cpp
```

当前实现区分两类解析路径：

| 路径 | 含义 | 提取字段 |
| --- | --- | --- |
| full business parse | 当前业务完整解析，不等于提取 JSON 全字段 | `request_id`、`ack`、`header.status`、`header.channel`、`data.errs.label`、`data.result.req_id/id/text` |
| minimal ack parse | Submit WS ACK 快路径 | `request_id`、`ack` |

`minimal ack parse` 的定位：

1. 用于 `ack=true` 的轻量请求接收确认。
2. 不读取 `header`、`data.result.req_id`、`data.errs`。
3. 不能替代 result / error 的完整解析。
4. 如果业务需要校验 status、channel、error label 或最终 order id，应走 full business parse 或后续设计分层解析。

### yyjson 与 simdjson 取舍

已接入：

```text
yyjson
simdjson 4.6.2
```

结论：

1. `yyjson` default parse 不要求调用方提供 padding，也不修改 payload；但它会构建 DOM，所以 minimal 只减少字段访问 / hash / 转换，不能跳过整包 JSON parse。
2. `YYJSON_READ_INSITU` 需要 `YYJSON_PADDING_SIZE`，当前通常为 4 字节；它会修改 payload buffer，不适合直接用于共享的 WebSocket frame buffer。
3. `simdjson::ondemand` 需要 `simdjson::SIMDJSON_PADDING`，当前通常为 64 字节；padding 只要求可读，不要求清零。
4. `simdjson::ondemand` 的 `string_view` / document / value 依赖原始 buffer 生命周期，热路径应立即转成 hash、整数、bool 等自有值，不要跨 buffer 复用保存 view。
5. 代码里不要写死 padding 数值，应使用 `YYJSON_PADDING_SIZE` 和 `simdjson::SIMDJSON_PADDING`。

### Submit response parse benchmark

运行命令：

```bash
taskset -c 2 ./build/release/benchmark/exchange/gate/trading/gate_submit_response_parse_benchmark --benchmark_filter='ack_minimal_padded_view|simdjson_ondemand_padded_view|yyjson_default_padded_view'
```

样本 payload 为 Gate `futures.order_place` 的 `ack=true` 订单回声，payload 约 839B；benchmark 使用 padded view，避免把每次 copy 成本混入解析差异。

2026-04-28 当前结果：

| case | p50 | p99 | p99.9 |
| --- | ---: | ---: | ---: |
| `yyjson_default_padded_view` | 391ns | 663ns | 766ns |
| `yyjson_ack_minimal_padded_view` | 359ns | 605ns | 678ns |
| `simdjson_ondemand_padded_view` | 254ns | 363ns | 583ns |
| `simdjson_ack_minimal_padded_view` | 123ns | 129ns | 444ns |

推荐：

```text
Submit WS ack=true 快路径优先使用 simdjson minimal ack parse。
result / error / 低频异常路径可使用 full business parse。
padding 或生命周期条件不满足时，fallback 到 padded scratch copy 或 yyjson default。
```

## FrameCodec readable tail padding 契约

为支持 simdjson zero-copy padded view，`MessageView` 已新增：

```cpp
std::uint32_t readable_tail_bytes{0};
```

语义：

1. `MessageView::payload` 仍然只表示真实 WebSocket payload，不包含 padding。
2. `readable_tail_bytes` 表示 payload 末尾之后，在当前 `MessageView` 生命周期内可安全读取的额外字节数。
3. 这些 tail bytes 不是 payload，内容可能是 ring buffer 中的任意历史或后续数据。
4. 业务层只能用它判断 parser padding 是否满足，例如 `readable_tail_bytes >= simdjson::SIMDJSON_PADDING`。
5. 不允许把 tail bytes 当业务数据读取，不允许写 tail bytes，也不允许把 parser 返回的 view 保存到 frame buffer 生命周期之外。

`FrameCodec` 和 `QueuedFrameCodec` 都会填充该字段。当前 mirrored ring 使跨 ring boundary 的 payload 后仍有连续可读虚拟地址空间，因此可以把这个能力显式暴露给上层，而不是把 padding 混进 payload size。

### FrameCodec benchmark 对比

修改前后使用同一条命令：

```bash
taskset -c 2 ./build/release/benchmark/websocket/frame_codec_benchmark
```

关键 decode path 对比：

| case | 修改前 p50 / p99 | 修改后 p50 / p99 |
| --- | ---: | ---: |
| direct contiguous | 5ns / 6ns | 5ns / 6ns |
| queued contiguous | 15ns / 18ns | 15ns / 17ns |
| direct coalesced drain | 3ns / 3ns | 3ns / 3ns |
| queued coalesced drain | 9ns / 14ns | 9ns / 16ns |
| direct mirrored boundary | 50ns / 56ns | 50ns / 56ns |
| queued mirrored boundary | 71ns / 80ns | 72ns / 83ns |

结论：暴露 `readable_tail_bytes` 对 FrameCodec decode 主路径没有可见 p50 回归；p99 的小幅波动在当前 benchmark 噪声范围内。

验证命令：

```bash
cmake --build build/debug -j8
cmake --build build/release -j8
ctest --test-dir build/debug --output-on-failure
ctest --test-dir build/release --output-on-failure
```

2026-04-28 验证结果：debug / release 均为 `17/17` tests passed。

## Sirius 旧实现结论

旧实现位于：

```text
third_party/sirius/exchange/gate
```

它是用户早期设计的一版 Gate 行情和交易实现。该目录目前属于第三方 / 参考代码，不作为当前 `aquila` 主线实现。

Sirius 交易结构：

```text
WebSocketClient 基类
  ├── FutureDataEngine
  └── TradeEngine
```

`TradeEngine` 的交易 WebSocket 同时处理：

```text
futures.login
futures.order_place / futures.order_cancel
futures.orders 私有订单回报
```

也就是说，Sirius 是“单个 trade WS 同时处理 submit 和 update”的设计。

值得借鉴：

- `RequestIdCodec`：把请求类型编码进 `req_id`，方便 API response 分流。
- `OrderTextCodec`：通过 Gate `text = t-{order_id}` 或 `ao-{price_order_id}` 反查内部订单 ID。
- parser 和 engine 有一定分层。
- 回报转成固定事件结构后写入 feedback channel。

不适合直接搬进 `aquila` 热路径：

- 依赖 Drogon WebSocket，frame/read/write buffer 不可控。
- 下单路径使用 `std::string`、`fmt::format`、`std::format` 构造请求，存在动态分配。
- message callback 后还有虚函数和 hash map channel dispatch。
- submit/update 混在同一个 Drogon loop，update burst 可能污染下单路径。
- 心跳、重连、degraded、metrics 都比当前 `core/websocket` 简单。

## 三种交易线程模型

### 方案 A：Strategy + Submit 同线程，Update 独立线程

```text
MarketDataThread
  -> market ring / latest cache
        |
        v
StrategyThread + GateOrderSubmitWsSession
  - poll 行情
  - 策略计算
  - 直接下单 / 撤单 / 改单
  - 轻量 read order API response
        ^
        |
feedback SPSC
        |
GateOrderUpdateThread + GateOrderUpdateWsSession
  - subscribe futures.orders / futures.usertrades / futures.positions
  - decode JSON control + SBE push
  - 写入 feedback SPSC
```

优点：

- 下单路径最短：`行情 -> 策略 -> encode -> send`。
- 不经过 `strategy -> order ring -> trading thread` 这一跳。
- update SBE decode / orders / usertrades burst 不进入策略线程。
- 最符合“行情 burst 时行情最高优先级”。

缺点：

- 策略线程里仍有 `send()` / `SSL_write()` syscall 和少量 submit read。
- 两个物理 trade WS 需要处理登录 epoch、连接健康、重复回报、REST reconcile。

当前低延迟优先推荐：**方案 A**。

### 方案 B：Strategy 独立线程，Submit + Update 合并为 TradingSession 线程

```text
StrategyThread
  -> order SPSC
        |
        v
TradingSessionThread
  - GateOrderSubmitWsSession
  - GateOrderUpdateWsSession
  - order state / dedup / report correlation
        |
        v
feedback SPSC -> StrategyThread
```

优点：

- StrategyThread 最干净，不碰 socket syscall。
- 交易状态、去重、request_id correlation 更集中。
- 工程实现更稳妥，调试更简单。

缺点：

- 下单多一跳 SPSC 和一次 trading thread poll。
- 如果 submit/update 在同一 loop 里处理，update burst 仍可能推迟 order send。

适合作为正确性优先的工程基线，但不是最低下单延迟。

### 方案 C：Strategy + Submit + Update 全部同线程

```text
StrategyThread
  - market data
  - strategy
  - order submit
  - order update read / parse
```

优点：

- 实现最简单。
- 订单状态天然单线程一致。

缺点：

- update SBE decode / 私有回报 burst 会污染策略线程。
- trade read syscall、JSON/SBE parse 都在策略 loop 里。
- 很难同时保证行情最高优先级和交易回报及时处理。

不建议作为 `aquila` 的最终低延迟架构。

## 当前推荐方向

如果按性能第一选择：

```text
方案 A：Strategy + GateOrderSubmitWsSession 同线程，
       GateOrderUpdateWsSession 独立线程，
       feedback 通过 SPSC 回到 strategy。
```

关键约束：

1. `GateOrderSubmitWsSession` 只处理 JSON order API response，不订阅 `futures.orders` / `futures.usertrades`。
2. `GateOrderUpdateWsSession` 可连 `/sbe` endpoint，处理 JSON control + SBE push。
3. 策略线程必须保持行情最高优先级；feedback ring 只在合适预算下 poll。
4. submit session 的 read budget 必须很小，只处理 login、ack/result/error、pong/close。
5. update session 的回报解析结果必须变成固定结构事件，再写 SPSC，避免把 JSON/SBE buffer 生命周期暴露给策略。
6. submit read path 里的 `ack=true` 快路径优先走 `simdjson_ack_minimal`，只提取 `request_id` 与 `ack`。
7. `MessageView::payload` 不包含 padding；如果 `readable_tail_bytes >= simdjson::SIMDJSON_PADDING`，submit parser 可以构造 zero-copy padded view。
8. 如果 tail padding 不满足，必须 fallback 到 padded scratch copy 或 yyjson default，不允许越界读取或临时改写 frame buffer。
9. `YYJSON_READ_INSITU` 会修改 buffer，默认不要直接用于共享的 FrameCodec payload。

## 待讨论 / 待验证

下一轮建议按这个顺序继续：

1. 确认 Gate SBE schema 获取方式和 C++ decoder 生成 / 集成方式。
2. 明确 `GateOrderSubmitWsSession` 需要支持哪些 WebSocket API：place、batch place、cancel、cancel ids、cancel cp、amend、status/list。
3. 设计 `RequestIdCodec`、`OrderTextCodec`、`OrderFeedback` 固定结构。
4. 明确 update stream 断线时是否允许继续 submit。
5. 设计 REST reconcile：update WS 断线或 gap 后如何补齐未决订单状态。
6. 将 `MessageView::readable_tail_bytes` 接入未来 `GateOrderSubmitWsSession`，形成 submit read 快路径：`payload view -> padding check -> simdjson minimal ack parse -> request correlation`。
7. 为方案 A / B 写端到端最小 benchmark：`strategy -> submit send`、`submit ack parse -> request correlation`、`update decode -> feedback SPSC`、`feedback poll -> order state update`。

## 相关文件

- `scripts/gate/test_gate_ws_dual_login.py`
- `scripts/gate/test_gate_ws_connect.py`
- `exchange/gate/trading/submit_response_parser.h`
- `test/exchange/gate/trading/submit_response_parser_test.cpp`
- `benchmark/exchange/gate/trading/submit_response_parse_benchmark.cpp`
- `core/websocket/message_view.h`
- `core/websocket/websocket_client.h`
- `core/websocket/critical_session.h`
- `core/websocket/frame_codec.h`
- `core/websocket/queued_frame_codec.h`
- `test/websocket/frame_codec_test.cpp`
- `core/websocket/types.h`
- `doc/websocket_read_write_benchmark_comparison.md`
- `doc/websocket_client_future_optimizations.md`
- `third_party/sirius/exchange/gate`
