# Binance USD-M futures bookTicker 行情接手说明

## 当前范围

本 handoff 主要覆盖 Binance USD-M futures public `bookTicker` 行情；当前额外提供 USD-M futures 合约元数据查询脚本，用于启动期 symbol 配置和下单前字段准备。行情实现不覆盖 spot、COIN-M futures、私有频道或运行时动态订阅。

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
exchange/binance/market_data/book_ticker_parser.h
exchange/binance/market_data/client.h
exchange/binance/market_data/data_session.h
exchange/binance/market_data/data_session_config.h
exchange/binance/market_data/data_session_config.cpp
tools/binance/data_session.cpp
tools/binance/futures_book_ticker_probe.cpp
benchmark/exchange/binance/market_data/futures_market_data_benchmark.cpp
scripts/binance/market_data/query_um_futures_contracts.py
scripts/test/binance/market_data/query_um_futures_contracts_test.py
docs/futures_contract_metadata_fields.md
```

共享 JSON helper 已放到：

```text
exchange/common/simdjson_utils.h
```

Gate 旧 include `exchange/gate/common/simdjson_utils.h` 现在只是转发到共享 helper，以保持已有 Gate 代码不大面积改名。

当前 Binance data session 模板签名为：

```cpp
aquila::binance::DataSession<
    Consumer, WebSocketPolicy, DiagnosticsPolicy>
```

其中 `WebSocketPolicy` 统一选择 TLS / plain socket 和编译期 WebSocket 选项；`DiagnosticsPolicy`
统一选择 market data client 诊断和 session 诊断。默认 policy 均为生产无统计路径，probe /
test / text-control benchmark 可显式启用 `SessionOnlyDiagnosticsPolicy` 或
`DataSessionDiagnosticsPolicy`。

当前 `config/data_sessions/binance_data_session.toml` 是单进程单 session 配置。Binance
data session config parser 在启动冷路径加载 `instrument_catalog` 和 `subscribe_symbols`，
按 `Exchange::kBinance` 生成 `DataSessionConfig`、raw stream target 所需的 exchange symbol
列表和 symbol id 列表。`binance_data_session` tool 根据 `connection.enable_tls` 选择 TLS 或
plain WebSocket policy；`--connect` 模式调用 `DataSession::Run()`，由 session 内部安装
SIGINT / SIGTERM stop handler。

`binance_data_session` tool 启动时只 parse 一次 TOML，同一个 parsed table 同时用于
`nova::LogConfig::FromToml(toml["log"])` 和 data session config 生成。仓库示例 log 文件默认写到
`/home/liuxiang/log/`，并用 `binance_data_session` 名称区分 file sink、console sink 和 backend
log thread；Nova file sink 会在实际文件名上追加启动时间。

## Binance 合约元数据脚本

`scripts/binance/market_data/query_um_futures_contracts.py` 查询 USD-M futures `GET /fapi/v1/exchangeInfo`，按输入 symbol 顺序生成 `pandas.DataFrame`；CLI 支持一个或多个 symbol，也支持 `--file`，文件内每行一个 symbol。脚本把 `settle_asset` 映射为 `marginAsset`；当前使用约定是调用方只传 USDT settled symbols。

当前输出字段与 Gate 脚本保持一致，字段语义见 `docs/futures_contract_metadata_fields.md`。Binance 侧主要映射如下：

1. `price_tick` 来自 `PRICE_FILTER.tickSize`。
2. `quantity_step`、`min_quantity`、`max_quantity` 来自 `LOT_SIZE`。
3. `max_market_quantity` 来自 `MARKET_LOT_SIZE.maxQty`。
4. `min_notional` 来自 `MIN_NOTIONAL.notional` 或 `NOTIONAL.minNotional`。
5. `price_limit_up` / `price_limit_down` 来自 `PERCENT_PRICE` 的 `multiplierUp` / `multiplierDown`，表示相对偏离比例。
6. `notional_multiplier` 固定为 `1.0`，因为 Binance USD-M futures quantity 以 base asset 数量表达。

这组字段只覆盖一类下单前必需元数据，不包含手续费、最大杠杆、风险限额档位、保证金模式、账户权限或交易所运行时限流。

## 数据流

```text
DataSession::Handle(text MessageView)
  -> capture local_ns
  -> FuturesMarketDataClient::OnTextPayload
  -> simdjson parse JSON
  -> fast_float parse string prices / quantities
  -> flat_hash_map symbol -> symbol_id
  -> BookTickerUpdate
  -> DataSink::EmplaceBookTickerWith(writer) 或 DataSink::OnBookTicker
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
2. 生产 JSON parser 固定为 `simdjson::ondemand`；如果 `MessageView::readable_tail_bytes >= simdjson::SIMDJSON_PADDING`，使用 zero-copy padded view，否则 fallback 到 `simdjson::padded_string`。
3. raw stream target 已经限定 `<symbol>@bookTicker`，生产 parser 热路径只读取 `u/E/s/b/B/a/A`，不再解析 `e/T`；字段存在、类型和数字格式作为 Binance 协议约束处理，debug 下用 assert 捕获协议漂移。
4. `b/B/a/A` 是 JSON 字符串，使用 `fast_float::from_chars` 转 double，不使用 `std::stod`。
5. client/session 都是模板组合；热路径不引入虚函数或 `std::function`。
6. session diagnostics 默认 no-op；benchmark / probe / test 显式启用。
7. 启用 market data diagnostics 时会记录 `simdjson_padding_fallback_messages`，用于 live probe 判断生产 receive buffer 是否稳定提供 `simdjson::SIMDJSON_PADDING`，默认 production no-op 路径不写 counter。
8. 无效 stream 数量会生成空 target；`DataSession::Start()` 对空 target 直接返回 `false`，避免在 cold path 尝试错误连接。
9. `SymbolBinding::symbol` 的底层字符串存储必须覆盖 client/session 生命周期；生产 symbol lookup 故意保留 `std::string_view` key，不在初始化阶段复制 symbol 文本。
10. symbol config 是启动期不变量；payload 中出现未配置 symbol 时 debug assert，release 主路径不记录 unknown-symbol counter。
11. client/session 构造期使用 symbol span 构建 lookup / raw stream target，构造后不保存无用 `symbols_` span。
12. `BookTickerUpdate::symbol` 固定 copy 到对象内 `symbol_storage`；不要假设 simdjson `get_string()` 返回的 `string_view` 一定指向原 payload 或在 padded fallback 后仍有效。
13. 如果 `DataSink` 支持 `EmplaceBookTickerWith()`，client 会把 `BookTickerUpdate` 字段直接映射到
    sink-owned `BookTicker` slot；普通 sink 仍走 `OnBookTicker(const BookTicker&)` 兼容路径。

## yyjson 对照

当前 main 中 yyjson 只作为 **benchmark-only parser 对照**：

- 生产 Binance 行情代码保持 main 的最小 simdjson 实现，不暴露 yyjson parser policy。
- yyjson bookTicker helper 只放在 `benchmark/exchange/binance/market_data/futures_market_data_benchmark.cpp` 的匿名 namespace 中。
- `aquila_binance` 不链接 `yyjson::yyjson`；只有 Binance market data benchmark target 链接 yyjson。
- client/session benchmark 保持生产 simdjson 路径，作为 parser 以外成本的基线。
- 如果之后要把 yyjson 放回 production，需要重新设计 parser policy 边界、补 production tests，并单独评估 payload 可写性和尾延迟。

实现边界：

1. benchmark-local `YyjsonBookTickerParser` 使用对象内固定 4 KiB read pool，避免每条消息走默认堆分配。
2. yyjson pool parse 不依赖 `MessageView::readable_tail_bytes`；它只作为 parser 层对照，不接入 production client/session。
3. `ParseInsitu()` 用 `YYJSON_READ_INSITU` 对照 yyjson padding 模式，当前 `YYJSON_PADDING_SIZE = 4`；它要求 mutable buffer，并会修改输入 payload。
4. `yyjson_insitu_copy` 是保守对照，计入拷贝到 mutable padded scratch；`yyjson_insitu_view` 是 no-copy 原地解析对照，benchmark 用预填好的独立 mutable payload，避免重复解析已经被 yyjson 修改过的 buffer。
5. `yyjson_insitu_view` 使用 `const_cast` 模拟 FrameCodec-backed mutable receive ring。这个假设只用于 benchmark，不代表 production `MessageView` payload 可以被任意调用方修改。
6. yyjson 版本仍然只产出 `BookTickerUpdate`，后续 symbol lookup、`BookTicker` 构造和 consumer 分发没有接入这个分支。

## 验证入口

单测：

```bash
./build/debug/test/exchange/binance/market_data/binance_book_ticker_parser_test
./build/debug/test/exchange/binance/market_data/binance_futures_market_data_client_test
./build/debug/test/exchange/binance/market_data/binance_data_session_test
./build/debug/test/config/data_session_config_test
scripts/test/binance/market_data/query_um_futures_contracts_test.py
```

benchmark：

```bash
taskset -c 2 ./build/release/benchmark/exchange/binance/market_data/binance_futures_market_data_benchmark --benchmark_filter='binance_market_data/(parse_book_ticker(_padded_view|_ordered|_ordered_padded_view|_yyjson_pool|_yyjson_insitu_copy|_yyjson_insitu_view|_then_shm_push|_into_shm_slot)?|client_on_text_payload|client_handle_binary|session_handle_text(_padded_view)?)(/.*)?$' --benchmark_repetitions=10 --benchmark_report_aggregates_only=true
```

2026-05-02 当前 selected mean 结果：

| case | time |
| --- | ---: |
| `parse_book_ticker` | 190ns |
| `parse_book_ticker_padded_view` | 158ns |
| `parse_book_ticker_ordered` | 178ns |
| `parse_book_ticker_ordered_padded_view` | 156ns |
| `client_on_text_payload/1` | 183ns |
| `session_handle_text_padded_view` | 195ns |

2026-05-06 SHM publish 对照：

| case | time |
| --- | ---: |
| `parse_book_ticker_then_shm_push` | 184ns |
| `parse_book_ticker_into_shm_slot` | 184ns |

这组 benchmark 是本机 parser/client/session microbenchmark，不是 Binance 公网链路延迟。

当前对比结论只限这组 bookTicker payload：

- simdjson fallback copy 明显慢于 padded-view；生产路径仍应优先保证 receive buffer 能稳定提供 `simdjson::SIMDJSON_PADDING`。
- benchmark-only ordered `find_field()` 对照略快于 production unordered parser，但 padded-view 主路径差距很小；当前决定不切 production。
- 字段直取 parser 后，simdjson padded-view 是当前 production parser 层最快路径；本轮没有证明 yyjson 足以替换 production simdjson。
- client/session 数值仍是 production simdjson 路径，不包含 yyjson parser policy。
- SHM slot-writer 路径去掉了 `BookTicker` 临时对象到 shm slot 的 64B copy，但 Binance JSON parser 成本占主导，
  本机 parse+publish microbenchmark 中总耗时与 temp+push 路径基本持平。
- 如果之后要继续 yyjson，需要补真实 receive ring 原地解析压测、尾延迟数据和 live probe，再讨论 production 接入。

data session dry-run / connect / live probe：

```bash
./build/release/tools/binance_data_session
./build/release/tools/binance_data_session --connect
./build/release/tools/binance_futures_book_ticker_probe --contract BTCUSDT --symbol-id 1 --duration-ms 10000 --cpu 2
```

如需引用 live 稳定性或真实交易所延迟结论，必须重新运行 probe 并保存原始输出。
