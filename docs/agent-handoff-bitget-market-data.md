# Bitget UTA SBE BBO 行情接手说明

## 当前范围

本 handoff 覆盖 Bitget UTA public SBE `books1` BBO 行情接入。当前实现只支持
`BookTicker`，不实现 Bitget order/private feed、JSON 行情、`publicTrade` 或交易端。

已落地能力：

1. 单路 `bitget_data_session` 订阅 `books1`，解码 SBE template id `1002`。
2. 单路 session 可发布 72-byte `aquila::BookTicker` 到 SHM。
3. `bitget_data_fusion` 可按 4 路 source 启动现有 `BookTickerFusionThread`，输出 canonical
   Bitget BBO SHM。
4. `DataReader` / `data_reader_recorder` 可通过配置读取 Bitget source 或 canonical BBO SHM。

## Endpoint 和订阅

默认配置：

```text
config/data_sessions/bitget_data_session.toml
```

使用 endpoint：

```text
wss://vip-ws-uta.bitget.com:443/v3/ws/public/sbe
```

订阅 payload 由 `exchange/bitget/market_data/subscription.h` 生成，`books1` 示例：

```json
{"op":"subscribe","args":[{"instType":"usdt-futures","topic":"books1","symbol":"BTCUSDT"}]}
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

价格和数量在 SBE 中按 mantissa + exponent 表达，decoder 转成 `double`。`BookTicker` ABI
不变，当前 `sizeof(aquila::BookTicker) == 72`。

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

Bitget data session 当前只接受 `feeds = ["book_ticker"]`。配置中写 `trade` 会 fail fast；不要把
Bitget `publicTrade` 接入假定为已完成。

## Data Fusion

入口：

```text
tools/bitget/bitget_data_fusion.cpp
tools/bitget/bitget_data_fusion_config.h
tools/bitget/bitget_data_fusion_config.cpp
core/market_data/fusion/book_ticker.h
core/market_data/fusion/thread.h
```

配置：

```text
config/market_data_fusion/bitget_book_ticker_fusion_4sources.toml
config/market_data_fusion/bitget_data_fusion_book_ticker_4sources.toml
```

dry-run：

```bash
./build/debug/tools/bitget_data_fusion --config config/market_data_fusion/bitget_data_fusion_book_ticker_4sources.toml
```

实际连接：

```bash
./build/debug/tools/bitget_data_fusion --config config/market_data_fusion/bitget_data_fusion_book_ticker_4sources.toml --connect
```

launcher 只启用 `BookTickerFusionThread`。若 launch config 中出现 `trade` feed，Bitget parser 会拒绝。

## DataReader 和 Recorder

Bitget recorder 配置：

```text
config/data_readers/bitget_book_ticker_fusion_canonical_recorder.toml
config/data_readers/bitget_book_ticker_fusion_4sources_source0_recorder.toml
config/data_readers/bitget_book_ticker_fusion_4sources_source1_recorder.toml
config/data_readers/bitget_book_ticker_fusion_4sources_source2_recorder.toml
config/data_readers/bitget_book_ticker_fusion_4sources_source3_recorder.toml
```

这些配置都声明 `exchange = "bitget"`、`feed = "book_ticker"`、`read_mode = "drain"`，输出
rotation 路径位于 `/home/liuxiang/tmp/bitget_book_ticker_fusion_4sources/`。`DataReaderConfig`
已支持 `bitget` exchange。

## 验证入口

Focused build / tests：

```bash
cmake --build build/debug --target \
  bitget_sbe_book_ticker_decoder_test \
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
