# Bitget UTA SBE 行情接手说明

## 当前范围

本文是 Bitget UTA public SBE `books1` BBO 与 `publicTrade` 的当前事实源，不覆盖 JSON 行情或交易端实现。
Bitget trading 统一见 `docs/bitget_trading.md`；通用 fusion/SHM 语义见 `docs/market_data_fusion.md` 和
`docs/data_session_shm_communication_design.md`。

已落地能力：

1. 单路 `bitget_data_session` 可订阅 `books1` 和 / 或 `publicTrade`，分别解码 SBE template id
   `1002` / `1003`。
2. 单路 session 可在同一个 SHM object 内发布 72-byte `aquila::BookTicker` 和 64-byte
   `aquila::Trade`。
3. `bitget_data_fusion` 可按 launch config 启动 `BookTickerFusionThread`、`TradeFusionThread` 或两者。
4. `DataReader` / `data_reader_recorder` 可通过配置读取 Bitget source 或 canonical BBO / Trade SHM。

## Endpoint 和订阅

默认配置：

```text
config/data_sessions/bitget_data_session.toml
```

使用 endpoint：

```text
wss://vip-ws-uta.bitget.com:443/v3/ws/public/sbe
```

Bitget 还提供 high speed public SBE endpoint。当前仓库没有把它写入默认配置；如需测试，
在 scratch config 中覆盖 `[data_session.websocket.endpoint]`：

```text
wss://vip-ws-uta-pub-a.bitget.com:443/v3/ws/public/sbe
```

订阅 payload 由 `exchange/bitget/market_data/subscription.h` 生成，`books1` 示例：

```json
{"op":"subscribe","args":[{"instType":"usdt-futures","topic":"books1","symbol":"BTCUSDT"}]}
```

`publicTrade` 示例：

```json
{"op":"subscribe","args":[{"instType":"usdt-futures","topic":"publicTrade","symbol":"BTCUSDT"}]}
```

批量订阅时多个 symbol 作为同一个 `args` array 发送。取消订阅只把 `op` 改为
`unsubscribe`。仓库示例配置订阅 `BTCUSDT`、`ETHUSDT`、`SOLUSDT`，instrument catalog 使用
`config/instruments/usdt_futures.csv` 中的 `Exchange::kBitget` 行。

## 字段映射

`books1` SBE header 使用 schema id `1`、template id `1002`。本地 fixture 覆盖 schema version
`2`，2026-07-08 live smoke 观察到 public endpoint 推送 schema version `3`，dispatcher 当前接受
version `2` 和 `3`。入口：

```text
exchange/bitget/sbe/message_header.h
exchange/bitget/sbe/message_dispatcher.h
exchange/bitget/sbe/book_ticker_decoder.h
```

`BookTicker` 映射：

| Bitget `books1` 字段 | `aquila::BookTicker` 字段 |
| --- | --- |
| `seq` | `id` |
| symbol lookup | `symbol_id` |
| 固定 `Exchange::kBitget` | `exchange` |
| `sts` 微秒 | `exchange_ns = sts * 1000` |
| `ts` 微秒 | `event_ns = ts * 1000` |
| data session ingress `CLOCK_REALTIME` | `local_ns` |
| `bidPrice * 10^priceExponent` | `bid_price` |
| `bidSize * 10^sizeExponent` | `bid_volume` |
| `askPrice * 10^priceExponent` | `ask_price` |
| `askSize * 10^sizeExponent` | `ask_volume` |

历史 probe 或 fixture 中可能存在不带 `sts` 的帧；此时 decoder 写
`exchange_ns = event_ns`。生产 live `books1` 期望包含 `sts`。

`publicTrade` SBE header 使用 schema id `1`、template id `1003`。本地 fixture 覆盖 schema
version `2` 和 live-observed version `3`。2026-07-08 live probe 观察到 version `3` payload：
root block length `16`、group entry block length `40`，`sts` 位于 root block，trade entry 内不带
per-entry `sts`。decoder 同时保留 entry-level `sts` 的兼容读取；如果 payload 没有任何 `sts`，
fallback 写 `exchange_ns = event_ns`。

`Trade` 映射：

| Bitget `publicTrade` 字段 | `aquila::Trade` 字段 |
| --- | --- |
| `execId` | `id` |
| symbol lookup | `symbol_id` |
| 固定 `Exchange::kBitget` | `exchange` |
| `side = 0` | `side = OrderSide::kBuy` |
| `side = 1` | `side = OrderSide::kSell` |
| 固定 `0` | `reserved` |
| `sts` 微秒 | `exchange_ns = sts * 1000` |
| `ts` 微秒 | `event_ns = ts * 1000` |
| data session ingress `CLOCK_REALTIME` | `local_ns` |
| `price * 10^priceExponent` | `price` |
| `size * 10^sizeExponent` | `volume` |
| SBE group index / count | `batch_index` / `batch_count` |

价格和数量在 SBE 中按 mantissa + exponent 表达，decoder 转成 `double`。`BookTicker` ABI
不变，当前 `sizeof(aquila::BookTicker) == 72`；`Trade` ABI 仍为 64 bytes。

## Data Session

入口：

```text
exchange/bitget/market_data/client.h
exchange/bitget/market_data/data_session.h
exchange/bitget/market_data/data_session_config.h
exchange/bitget/market_data/data_session_config.cpp
tools/bitget/bitget_data_session.cpp
```

默认 dry-run：

```bash
./build/debug/tools/bitget_data_session --config config/data_sessions/bitget_data_session.toml
```

实际连接需要显式加 `--connect`：

```bash
./build/debug/tools/bitget_data_session --config config/data_sessions/bitget_data_session.toml --connect
```

Bitget data session 接受 `feeds = ["book_ticker"]`、`feeds = ["trade"]` 或
`feeds = ["book_ticker", "trade"]`。`feed = "book_ticker"` / `feed = "trade"` 仍可作为 legacy
single-feed alias 使用，但不能和 `feeds` 同时配置。

## Data Fusion

入口：

```text
tools/bitget/bitget_data_fusion.cpp
tools/bitget/bitget_data_fusion_config.h
tools/bitget/bitget_data_fusion_config.cpp
core/market_data/fusion/book_ticker.h
core/market_data/fusion/trade.h
core/market_data/fusion/thread.h
```

配置：

```text
config/market_data_fusion/bitget_book_ticker_fusion_4sources.toml
config/market_data_fusion/bitget_data_fusion_book_ticker_4sources.toml
config/market_data_fusion/bitget_trade_fusion_4sources.toml
config/market_data_fusion/bitget_data_fusion_trade_4sources.toml
config/market_data_fusion/bitget_book_ticker_fusion_book_ticker_trade_4sources.toml
config/market_data_fusion/bitget_trade_fusion_book_ticker_trade_4sources.toml
config/market_data_fusion/bitget_data_fusion_book_ticker_trade_4sources.toml
```

dry-run：

```bash
./build/debug/tools/bitget_data_fusion --config config/market_data_fusion/bitget_data_fusion_book_ticker_4sources.toml
```

实际连接：

```bash
./build/debug/tools/bitget_data_fusion --config config/market_data_fusion/bitget_data_fusion_book_ticker_4sources.toml --connect
```

launcher 按 `launch.feeds` 启用 `book_ticker`、`trade` 或两者；每个 enabled feed 一个 fusion thread。

## DataReader 和 Recorder

Bitget recorder 配置：

```text
config/data_readers/bitget_book_ticker_fusion_canonical_recorder.toml
config/data_readers/bitget_book_ticker_fusion_4sources_source0_recorder.toml
config/data_readers/bitget_book_ticker_fusion_4sources_source1_recorder.toml
config/data_readers/bitget_book_ticker_fusion_4sources_source2_recorder.toml
config/data_readers/bitget_book_ticker_fusion_4sources_source3_recorder.toml
config/data_readers/bitget_trade_fusion_canonical_recorder.toml
config/data_readers/bitget_trade_fusion_4sources_source0_recorder.toml
config/data_readers/bitget_trade_fusion_4sources_source1_recorder.toml
config/data_readers/bitget_trade_fusion_4sources_source2_recorder.toml
config/data_readers/bitget_trade_fusion_4sources_source3_recorder.toml
```

这些配置都声明 `exchange = "bitget"`、`read_mode = "drain"`；BBO 配置使用 `feed = "book_ticker"`，
Trade 配置使用 `feed = "trade"`。rotation 路径分别位于
`/home/liuxiang/tmp/bitget_book_ticker_fusion_4sources/` 和
`/home/liuxiang/tmp/bitget_trade_fusion_4sources/`。

## 验证入口

Focused build / tests：

```bash
cmake --build build/debug --target \
  bitget_sbe_book_ticker_decoder_test \
  bitget_sbe_trade_decoder_test \
  bitget_sbe_message_dispatcher_test \
  bitget_futures_market_data_client_test \
  bitget_data_session_config_test \
  bitget_data_session_test \
  bitget_data_fusion_config_test \
  bitget_data_session \
  bitget_data_fusion \
  -j8

ctest --test-dir build/debug -R '^bitget_' --output-on-failure
```

DataReader / recorder focused 验证：

```bash
cmake --build build/debug --target data_reader_config_test data_reader_recorder_test data_reader_recorder data_reader_probe -j8
ctest --test-dir build/debug -R '^(data_reader_config_test|data_reader_recorder_test)$' --output-on-failure
```

全量收尾仍需运行：

```bash
cmake --build build/debug -j8
ctest --test-dir build/debug --output-on-failure
git diff --check
```

Live smoke 只有在网络可用且不会影响其他进程时运行；失败时记录 endpoint、订阅 payload、SBE
header / template、错误日志，不把 dry-run 结果当作 live 通过。

## 2026-07-08 BBO Endpoint A/B 结果

本次只测试 public SBE `books1` / `BookTicker` 行情，不涉及 REST、private feed、order 或交易端。
结果只能说明行情接入和 fusion pipeline 的表现，不能外推为 fillability、PnL 或订单收益。

测试架构：

- normal endpoint：`wss://vip-ws-uta.bitget.com:443/v3/ws/public/sbe`
- high speed endpoint：`wss://vip-ws-uta-pub-a.bitget.com:443/v3/ws/public/sbe`
- 订阅 symbols：`BTCUSDT`、`ETHUSDT`、`SOLUSDT`
- 两边同时启动，各自 1 个 `bitget_data_fusion` 进程、4 路 source session、1 条 BBO fusion、5 个
  BBO recorder（4 个 source recorder + 1 个 canonical recorder）
- normal critical CPUs：`16-20`；high speed critical CPUs：`21-25`
- recorder 使用测试区 CPU，架构两边一致；recorders 在 fusion 启动约 12 秒后以 `latest` 接入
- run dir：`/home/liuxiang/tmp/20260708_125137_bitget_bbo_normal_vs_highspeed_n4_30m_ab`

运行状态：

- 两边 fusion 同时启动于 `2026-07-08T12:52:21Z`，均在 `2026-07-08T13:22:22Z` 以 status `0` 自然退出。
- 10 个 recorder 均在 `2026-07-08T13:23:09Z` 正常退出，`result=ok stop_reason=signal`。
- 两边 fusion 都是 `result=ok`，`fusion_metadata_write_errors=0`，`fusion_flush_ok=true`。
- 所有 source / canonical recorder 均为 `skipped=0 overruns=0`，未观察到错误日志。

关键结果：

| 指标 | normal | high speed |
| --- | ---: | ---: |
| fusion metadata published | 876,555 | 870,282 |
| canonical recorded | 872,736 | 866,464 |
| fusion latency p50 | 0.872 ms | 0.910 ms |
| fusion latency p95 | 2.105 ms | 3.645 ms |
| fusion latency p99 | 2.650 ms | 14.193 ms |
| fusion latency p99.9 | 3.032 ms | 40.289 ms |
| fusion latency max | 5.607 ms | 55.122 ms |
| fusion latency mean | 0.945 ms | 1.401 ms |

Fusion hop 仍为微秒级，不是主要延迟来源：

| 指标 | normal hop | high speed hop |
| --- | ---: | ---: |
| p50 | 0.507 us | 0.520 us |
| p95 | 0.887 us | 1.415 us |
| p99 | 4.161 us | 5.105 us |
| p99.9 | 9.065 us | 11.261 us |

结论：在这次 30 分钟同步 A/B 中，high speed endpoint 没有带来 BBO 延迟收益，消息量也没有明显更多；
normal endpoint 的 p95、p99、p99.9 和 max 明显更稳。后续如需继续评估，应保留同构启动、同 symbols、
BBO-only、同 recorder 结构和独立 CPU 绑定，并把结论限定为行情 pipeline 证据。

## 2026-07-18 BBO Endpoint 三组 N=4 一小时结果

本轮同时运行三组 `books1` fusion：

- `ha4`：4 路 `vip-ws-uta.bitget.com`；
- `hs4`：4 路 `vip-ws-uta-pub-a.bitget.com`；
- `mixed2x2`：source 0/1 为 normal，source 2/3 为 high speed。

三组统一使用大 instrument catalog，订阅 `BTCUSDT/ETHUSDT/SOLUSDT`。每组包含一个
`bitget_data_fusion`、4 个 source session 和一个 canonical recorder；critical CPU 分别固定为
`16-20`、`21-25`、`26-30`，recorder main 分别固定为 `4/7/10`。正式 run 位于
`/home/liuxiang/tmp/bitget_bbo_ha4_hs4_mixed2x2_n4_1h_20260718T024622Z`。

三组 fusion 从 `2026-07-18T02:47:10Z` 运行到 `03:47:10Z` 并自然返回 `0`；三个 recorder
随后正常返回 `0`。三组均为 `fusion_metadata_write_errors=0`、`fusion_flush_ok=true`、
`skipped=0`、`overruns=0`。Fusion metadata 数分别为 `320,343 / 319,879 / 320,229`，
canonical recorder 数分别为 `319,598 / 319,134 / 319,484`。Recorder 在 fusion 后约 12 秒以
`latest` 接入，所以每组 metadata 比 canonical 多 `745` 条；canonical 记录全部能按
`symbol_id/record_id/exchange_ns` 对齐 metadata，`fusion_publish_ns` mismatch 为 `0`。

Latency 定义：

- source latency：`source_local_ns - exchange_ns(sts)`；
- fusion latency：`fusion_publish_ns - exchange_ns(sts)`；
- fusion hop：`fusion_publish_ns - source_local_ns`。

完整 metadata 结果：

| group | fusion p50 | p95 | p99 | p99.9 | max | mean | hop p50 | hop p99 | hop p99.9 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `ha4` | 0.959 ms | 2.496 ms | 2.870 ms | 5.526 ms | 8.353 ms | 1.079 ms | 0.578 us | 5.392 us | 12.835 us |
| `hs4` | 0.845 ms | 1.492 ms | 7.264 ms | 12.431 ms | 17.036 ms | 0.996 ms | 0.550 us | 5.451 us | 14.055 us |
| `mixed2x2` | 0.856 ms | 1.948 ms | 4.248 ms | 9.066 ms | 12.064 ms | 0.953 ms | 0.551 us | 5.508 us | 15.197 us |

三组共同的 `278,750` 条记录用于 paired comparison：

| comparison | 左侧更快 | 右侧更快 | delta p50 | paired 左/右 p99 |
| --- | ---: | ---: | ---: | ---: |
| `ha4 - hs4` | 30.26% | 69.74% | +0.060 ms | 2.755 / 6.162 ms |
| `ha4 - mixed2x2` | 34.82% | 65.18% | +0.049 ms | 2.755 / 3.411 ms |
| `hs4 - mixed2x2` | 53.44% | 46.56% | -0.008 ms | 6.162 / 3.411 ms |

负 delta 表示左侧更快。Mixed 组内 high-speed source 胜出 `211,734 / 320,229`（`66.12%`）；
分 symbol 为 ETH `65.82%`、SOL `69.58%`、BTC `62.81%`。5 分钟分桶显示 high-speed p99
在 `1.513-11.332 ms` 间波动，HA 为 `2.366-4.841 ms`，mixed 为 `2.245-7.792 ms`；
mixed 的 high-speed 胜出率也在 `36.48%-89.83%` 间变化。

结论：

- high speed 本轮在约 70% 的共同记录上快于 HA，p50/p95 更低，但 p99、p99.9 和 max 明显更差；
- HA 仍提供三组中最稳的 p99+ tail，本轮只有 HA 的整体 p99 低于 `3ms`；当前策略对 Bitget lag
  使用 `500ms` freshness guard，因此该差异不能直接解释为 guard 拒绝率；
- mixed 的 paired median 只比全 high speed 慢约 `8us`，同时将 paired p99 从 `6.162ms`
  降到 `3.411ms`，是 bulk latency 与 tail 的折中，但 tail 仍不如 HA；
- 三组 fusion hop 的 p50 约 `0.55us`、p99 约 `5.5us`，endpoint 差异主要来自
  `sts -> source_local_ns`，不是 fusion 线程。Mixed 有两个 hop 异常点超过 `100us`，其中 max
  为 `725.050us`，但其 p99.9 为 `15.197us`。

分析产物位于：

```text
/home/liuxiang/tmp/bitget_bbo_ha4_hs4_mixed2x2_n4_1h_20260718T024622Z/analysis/comparison_report.md
/home/liuxiang/tmp/bitget_bbo_ha4_hs4_mixed2x2_n4_1h_20260718T024622Z/analysis/comparison_summary.json
/home/liuxiang/tmp/bitget_bbo_ha4_hs4_mixed2x2_n4_1h_20260718T024622Z/analysis/group_summary.csv
/home/liuxiang/tmp/bitget_bbo_ha4_hs4_mixed2x2_n4_1h_20260718T024622Z/analysis/time_bucket_5m.csv
```

该结果仍只覆盖同一机器、一个小时、三个 symbols 的公网 BBO；不能外推到其他时段、symbol、
订单 fillability 或 PnL，也不单独授权切换策略 endpoint。
