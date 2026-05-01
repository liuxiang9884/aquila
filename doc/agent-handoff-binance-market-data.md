# Binance USD-M futures bookTicker 行情接手说明

## 当前范围

本轮只实现 Binance USDⓈ-M futures public `bookTicker` 行情，不覆盖 spot、COIN-M futures、私有频道或运行时动态订阅。

官方文档结论：

1. USDⓈ-M futures WebSocket base host 使用 `fstream.binance.com`。
2. 第一版使用 raw stream URL，例如 `/public/ws/btcusdt@bookTicker`；连接成功后交易所直接推送 JSON text frame，不需要 active 后再发送 `SUBSCRIBE`。
3. 单 symbol stream 名称为 `<symbol>@bookTicker`，payload 是 JSON，字段包括 `e/u/E/T/s/b/B/a/A`。
4. 单连接最多 1024 streams。仓库当前先设置更保守的 `kMaxFuturesBookTickerStreamsPerConnection = 200`，不做自动分片；超过上限由上层拆 session。
5. 全市场 `!bookTicker` 是 5s 更新，不作为低延迟主路径默认方案。

参考：

- https://developers.binance.com/docs/derivatives/usds-margined-futures/websocket-market-streams
- https://developers.binance.com/docs/derivatives/usds-margined-futures/websocket-market-streams/Individual-Symbol-Book-Ticker-Streams
- https://developers.binance.com/docs/derivatives/usds-margined-futures/websocket-market-streams/All-Book-Tickers-Stream

## 代码入口

```text
exchange/binance/market_data/types.h
exchange/binance/market_data/stream.h
exchange/binance/market_data/book_ticker_update.h
exchange/binance/market_data/book_ticker_parser.h
exchange/binance/market_data/book_ticker_yyjson_parser.h
exchange/binance/market_data/client.h
exchange/binance/market_data/session.h
tools/binance_futures_book_ticker_probe.cpp
benchmark/exchange/binance/market_data/futures_market_data_benchmark.cpp
```

共享 JSON helper 已放到：

```text
exchange/common/simdjson_utils.h
```

Gate 旧 include `exchange/gate/common/simdjson_utils.h` 现在只是转发到共享 helper，以保持已有 Gate 代码不大面积改名。

## 数据流

```text
FuturesMarketDataSession::Handle(text MessageView)
  -> capture local_ns
  -> FuturesMarketDataClient::OnTextPayload
  -> parser policy parse JSON
  -> fast_float parse string prices / quantities
  -> flat_hash_map symbol -> symbol_id
  -> aquila::BookTicker(exchange=kBinance)
  -> Consumer::OnBookTicker
```

字段映射：

| Binance 字段 | `BookTicker` 字段 |
| --- | --- |
| `u` | `id` |
| `s` | symbol lookup 后的 `symbol_id` |
| `E` 毫秒 | `exchange_ns = E * 1'000'000` |
| local receive clock | `local_ns` |
| `b` / `B` | `bid_price` / `bid_volume` |
| `a` / `A` | `ask_price` / `ask_volume` |

## 性能边界

1. Binance bookTicker 是 JSON text，不能复用 Gate 的 SBE binary 解码路径。
2. 默认 JSON parser 是 `SimdjsonBookTickerParser`，内部使用 `simdjson::ondemand`；如果 `MessageView::readable_tail_bytes >= simdjson::SIMDJSON_PADDING`，使用 zero-copy padded view，否则 fallback 到 `simdjson::padded_string`。
3. `b/B/a/A` 是 JSON 字符串，使用 `fast_float::from_chars` 转 double，不使用 `std::stod`。
4. client/session 都是模板组合；热路径不引入虚函数或 `std::function`。
5. session diagnostics 默认 no-op；benchmark / probe / test 显式启用。

## yyjson 对照分支

`feature/binance-bookticker-yyjson` 增加了只覆盖 Binance futures `bookTicker` 的 yyjson parser policy：

```cpp
using YyjsonClient = aquila::binance::FuturesMarketDataClient<
    Consumer, aquila::binance::NoopFuturesMarketDataDiagnostics,
    aquila::websocket::DefaultWebSocketOptions,
    aquila::binance::YyjsonBookTickerParser>;
```

实现边界：

1. `YyjsonBookTickerParser` 使用 parser 对象内的固定 4 KiB read pool，避免每条消息走默认堆分配。
2. yyjson default parse 不依赖 `MessageView::readable_tail_bytes`；这个参数保留在 parser policy 接口中，用于与 simdjson parser 统一接入 client/session。
3. yyjson 版本仍然只产出 `BookTickerUpdate`，后续 symbol lookup、`BookTicker` 构造和 consumer 分发与 simdjson 路径相同。
4. 当前 yyjson 只作为分支实验和 benchmark 对照；是否替换默认 parser 需要结合 live probe、真实 symbol 集合和更完整尾延迟数据再决定。

## 验证入口

单测：

```bash
./build/debug/test/exchange/binance/market_data/binance_book_ticker_parser_test
./build/debug/test/exchange/binance/market_data/binance_book_ticker_yyjson_parser_test
./build/debug/test/exchange/binance/market_data/binance_futures_market_data_client_test
./build/debug/test/exchange/binance/market_data/binance_futures_market_data_session_test
```

benchmark：

```bash
taskset -c 2 ./build/release/benchmark/exchange/binance/market_data/binance_futures_market_data_benchmark --benchmark_filter='binance_market_data/(parse_book_ticker(_padded_view|_yyjson_pool)?|client_on_text_payload(_yyjson_pool)?|session_handle_text(_padded_view|_yyjson_pool)?)(/.*)?$' --benchmark_repetitions=10
```

2026-05-01 `feature/binance-bookticker-yyjson` 当前 mean 结果：

| case | time |
| --- | ---: |
| `parse_book_ticker` | 223ns |
| `parse_book_ticker_padded_view` | 201ns |
| `parse_book_ticker_yyjson_pool` | 206ns |
| `client_on_text_payload/1` | 237ns |
| `client_on_text_payload/8` | 243ns |
| `client_on_text_payload/32` | 243ns |
| `client_on_text_payload_yyjson_pool/1` | 226ns |
| `client_on_text_payload_yyjson_pool/8` | 228ns |
| `client_on_text_payload_yyjson_pool/32` | 228ns |
| `session_handle_text/1` | 290ns |
| `session_handle_text/8` | 302ns |
| `session_handle_text/32` | 302ns |
| `session_handle_text_yyjson_pool/1` | 263ns |
| `session_handle_text_yyjson_pool/8` | 274ns |
| `session_handle_text_yyjson_pool/32` | 273ns |
| `session_handle_text_padded_view` | 257ns |

这组 benchmark 是本机 parser/client/session microbenchmark，不是 Binance 公网链路延迟。

当前对比结论只限这组 bookTicker payload：

- yyjson pool 比 simdjson fallback 无 padding 路径略快。
- simdjson padded-view 仍是最快的 session text path。
- 如果真实 WebSocket frame buffer 能稳定提供 `simdjson::SIMDJSON_PADDING`，默认 simdjson 路径仍更有优势；如果大量落到 fallback copy，yyjson pool 值得继续压测。

live probe：

```bash
./build/release/tools/binance_futures_book_ticker_probe --contract BTCUSDT --symbol-id 1 --duration-ms 10000 --cpu 2
```

如需引用 live 稳定性或真实交易所延迟结论，必须重新运行 probe 并保存原始输出。
