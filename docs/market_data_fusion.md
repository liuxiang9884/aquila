# Market Data Fastest-Route Fusion

本文是 Gate、Binance 和 Bitget 多路行情 fusion 的当前架构事实源。Fusion 为策略发布单一 canonical
`BookTicker`/`Trade` stream；历史 shadow 结果见 `docs/gate_fastest_route_fusion_shadow_results.md`，与 Tardis 的离线对账见
`docs/fusion_tardis_bbo_comparison.md`。

## 目标与非目标

同一 exchange/symbol 的 N 条 source connection 同时接收行情，fusion 对同一个 exchange update 采用最先处理到的 source：

```text
source[0..N-1] SHM -> fastest-route fusion -> canonical SHM -> strategy
```

Fusion 不是 primary/standby，不在 primary stale 后才切换，也不等待短窗口择优。策略不读取多 source，不承担去重、source health
或 winner attribution。Fusion 只优化 canonical 行情 freshness；它不证明订单 fillability、Ack latency 或 PnL 收益。

## Update identity 与算法

Live `BookTicker.id`：Gate 使用 SBE `bbo.u`，Binance 使用 JSON `u`，Bitget 使用 SBE `seq`。Identity 必须是
`(exchange, symbol_id, id)`；不能只比较 `id`，也不能跨 exchange 比较。Historical converter 可能把 `id` 写为输入行号，
这种数据只能验证流程，不能证明跨 source update identity。

每个 symbol 保存 `last_published_id`。Hot path 规则：

```text
if ticker.id > last_published_id:
  copy the complete winning ticker
  ticker.local_ns = fusion_publish_ns
  publish immediately
  last_published_id = ticker.id
  publish one metadata record
else:
  drop
```

不等待、不回看、不补洞、不比较 duplicate payload、不在 hot path 统计 conflict。Bid/ask/price/size/timestamp 必须来自同一个
winner record，禁止字段级混合。Canonical stream 是最快可发布 latest BBO/Trade stream，不是可修洞的完整 order book。

Trade 使用相同的 per-symbol monotonic-id 思路；batch 内顺序、`batch_index/batch_count` 和 exchange/event/local timestamp 保持原始
contract。任何 exchange 缺少稳定跨 source id 时，必须先验证 identity 再启用 fusion。

## 部署与线程边界

独立进程形态：

```text
data_session_0 process -> source_0 SHM ┐
data_session_1 process -> source_1 SHM ├-> fusion process -> canonical SHM
...                                  │
data_session_N process -> source_N SHM ┘
```

Threaded bundle 形态：

```text
one process
  N data-session owner threads -> N source SHM channels
  one fusion owner thread      -> canonical SHM
  one log backend thread
```

V1 bundle 继续保留 source SHM，因此 recorder、monitor 和 analyzer 可复用。Direct in-process SPSC ring 只有在 benchmark/shadow
证明 source SHM hop 已成为可见 tail 瓶颈后才进入设计；不能凭结构直觉替换。

每个 enabled feed 使用独立 fusion thread。一个 `data_fusion` process 可以按 launch config 启用 `book_ticker`、`trade` 或两者，
只创建 enabled source/canonical channel。Fusion thread 固定 round-robin poll source；`max_events_per_source` 控制公平性，默认低值
避免 burst source 饿死其它 source。

退出顺序：先停止并 join source，再让 fusion drain 到 idle，最后停止 fusion 和 recorder。Source/fusion/log backend CPU、SHM name、
channel 和 override 必须在启动前 fail fast 检查冲突。

## ABI 与时间戳

Canonical `BookTicker`/`Trade` 仍使用现有 typed binary format v1，不增加 winner 字段：

```text
id / symbol_id / exchange  = winner source
exchange_ns / event_ns     = winner source
local_ns                   = fusion_publish_ns
market fields              = complete winner payload
```

延迟口径：

```text
source_latency_ns = source_local_ns - exchange_ns
fusion_latency_ns = fusion_publish_ns - exchange_ns
fusion_hop_ns     = fusion_publish_ns - source_local_ns
```

`local_ns` 在 canonical stream 中表示 fusion publish，不再表示 source ingress。需要 source attribution 时使用 sidecar metadata，
不能从 canonical payload 反推 winner ingress。

## Sidecar metadata

每次 canonical publish 写一条 metadata，不为 dropped source 写记录。当前 v2 record：

```cpp
struct FusionMetadataRecord {
  std::int32_t source_id;
  std::int32_t symbol_id;
  std::int64_t record_id;
  std::int64_t exchange_ns;
  std::int64_t event_ns;
  std::int64_t source_local_ns;
  std::int64_t fusion_publish_ns;
};
```

Raw size 为 48 bytes；旧 40-byte artifact 需用旧脚本读取或重录。核心离线指标为 source/fusion latency、fusion hop、winner ratio、
metadata/canonical alignment 和 recorder overrun。

## 配置与代码入口

核心实现：

```text
core/market_data/fusion/fastest_route.h
core/market_data/fusion/book_ticker.h
core/market_data/fusion/trade.h
core/market_data/fusion/thread.h
core/market_data/fusion/metadata.h
tools/market_data/data_fusion_tool_support.h
tools/market_data/data_fusion_launch_config_parser.h
tools/gate/gate_data_fusion.cpp
tools/binance/binance_data_fusion.cpp
tools/bitget/bitget_data_fusion.cpp
```

Launch config 支持 feed enable、source config override、canonical SHM、metadata、CPU 和 log。具体 data session/typed SHM contract 见
`docs/data_session_config.md` 与 `docs/data_session_shm_communication_design.md`。

## Shadow 与验证

策略切换前先 shadow：N 路 source、canonical 和 metadata 同时录制，策略仍读原 source。分析至少输出：

- 每路 source latency p50/p95/p99/p99.9/max。
- Canonical fusion latency 与 best single source 对比。
- Fusion hop p50/p99/p99.9/max。
- Winner ratio、metadata alignment、recorder skipped/overrun。
- 按 symbol 的 stale/reject 影响和异常 source 稳定性。

Analyzer：

```bash
/home/liuxiang/dev/pyenv/lx/bin/python \
  scripts/market_data/analyze_book_ticker_fusion_latency.py \
  --source-bin 0=/home/liuxiang/tmp/<run_id>/source_0.bin \
  --source-bin 1=/home/liuxiang/tmp/<run_id>/source_1.bin \
  --fusion-bin /home/liuxiang/tmp/<run_id>/fusion.bin \
  --metadata-bin /home/liuxiang/tmp/<run_id>/fusion_metadata.bin \
  --output-dir /home/liuxiang/tmp/<run_id>/analysis
```

Focused tests：

```bash
ctest --test-dir build/debug \
  -R '(core_market_data_fusion|fusion_config|data_fusion_tool_support|fusion_cli_traits|book_ticker_fusion_cli|trade_fusion_cli)' \
  --output-on-failure
```

## 当前结论与边界

- Gate/Binance N=4 shadow 已观察到 p99/p99.9 tail 改善，常态 fusion hop 为微秒内；详见证据文档。
- Trade 30-symbol/4-route/30-minute smoke 只证明 fusion pipeline；不代表订单收益。
- Bitget normal/high-speed endpoint A/B 只覆盖行情接入与 fusion。2026-07-18 三组 N=4 一小时结果显示
  high speed 的 p50/p95 更低但 p99+ tail 更差，2+2 mixed 位于两者之间，HA 的 p99+ 最稳；详见
  `docs/agent-handoff-bitget-market-data.md`。
- 策略切换必须使用同构 source、固定 CPU、相同 symbols/feed/recorder，并重复采样尾延迟。
- 任何吞吐、tail、fillability 或 PnL 结论必须引用对应 benchmark/profile/live 证据。
