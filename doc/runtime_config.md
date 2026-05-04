# Runtime 配置说明

## 范围

runtime TOML 描述单个进程的启动拓扑：instrument catalog、该进程内的 session、每个
session 的 WebSocket 连接和运行策略。生产配置按进程拆分，策略参数暂不放在这里；risk
control、order management 和 order execution 归属于 `Strategy` 模块。

第一版配置遵循：

1. 每个 session 自己持有 WebSocket 配置，不引入 `websocket.profiles` 复用层。
2. TOML 只写连接之间必须不同或最常改的字段。
3. loader 对缺省字段填统一默认值，再生成完整 `websocket::ConnectionConfig`。
4. WebSocket `target` 不写在配置里，由具体 session builder 按交易所协议生成。

## 进程拆分

生产推荐把 Gate 行情、Binance 行情、策略 / 交易拆成独立进程：

```text
gate-md-process
  GateMarketDataThread
    GateFutureMarketDataSession

binance-md-process
  BinanceMarketDataThread
    BinanceFutureMarketDataSession

strategy-trade-process
  StrategyThread
    Strategy
    GateOrderSession
    BinanceOrderSession

  GateOrderFeedbackThread
    GateOrderFeedbackSession

  BinanceOrderFeedbackThread
    BinanceOrderFeedbackSession
```

因此生产 runtime 配置也按进程拆分。当前已给出：

```text
config/runtime/gate_market_data.toml
config/runtime/binance_market_data.toml
```

`config/runtime.toml` 可以保留为开发期合并示例，但不作为生产推荐拓扑。多个
runtime 文件可以指向同一个 `instrument_catalog` CSV；CSV 是共享合约元数据来源，不是进程私有状态。

## Gate 行情进程示例

```toml
[instrument_catalog]
file = "config/instruments/usdt_futures.csv"
schema = "aquila.instrument.v1"

[[data_sessions]]
name = "gate_future_market_data"
type = "GateFutureMarketDataSession"
exchange = "gate"
thread = "GateMarketDataThread"
enabled = true
websocket.endpoint = { host = "fx-ws.gateio.ws", enable_tls = false }
websocket.execution_policy = { bind_cpu_id = 2 }
settle = "usdt"
wire_format = "sbe"
sbe_schema_id = 1
feed = "book_ticker"
subscribe_symbols = ["BTC_USDT", "ETH_USDT", "SOL_USDT"]
```

## Binance 行情进程示例

```toml
[instrument_catalog]
file = "config/instruments/usdt_futures.csv"
schema = "aquila.instrument.v1"

[[data_sessions]]
name = "binance_future_market_data"
type = "BinanceFutureMarketDataSession"
exchange = "binance"
thread = "BinanceMarketDataThread"
enabled = true
websocket.endpoint = { host = "fstream.binance.com" }
websocket.execution_policy = { bind_cpu_id = 3 }
market = "um_futures"
feed = "book_ticker"
subscribe_symbols = ["BTC_USDT", "ETH_USDT", "SOL_USDT"]
```

## Instrument Catalog

| 字段 | 默认值 | 含义 |
| --- | --- | --- |
| `file` | 无，必须显式配置 | instrument CSV 路径。 |
| `schema` | 无，必须显式配置 | Aquila 自定义 CSV 字段版本；当前固定 `aquila.instrument.v1`。 |

`instrument_catalog` 第一版是 CSV 数据源，不表示引入真实数据库。启动期应读取 CSV 并构建
`InstrumentRegistry`。data session 的运行期 symbol pool 由 `instrument_catalog` 和
`subscribe_symbols` 共同生成。

## Data Session

| 字段 | 默认值 | 含义 |
| --- | --- | --- |
| `name` | 无，必须显式配置 | session 实例名。 |
| `type` | 无，必须显式配置 | session 类型，例如 `GateFutureMarketDataSession`。 |
| `exchange` | 无，必须显式配置 | 交易所名，例如 `gate`、`binance`。 |
| `thread` | 无，必须显式配置 | 该 session 运行的系统线程名。 |
| `enabled` | `true` | 是否启动该 session。 |
| `subscribe_symbols` | 无，必须显式配置 | 该 data session 要订阅的人类可读内部 symbol，例如 `BTC_USDT`；不直接写交易所 symbol。 |

## Symbol Pool 生成

`subscribe_symbols` 只表示订阅意图，不直接进入行情热路径。启动期 loader / session builder 应按
`exchange` 和 `subscribe_symbols` 从 `InstrumentRegistry` 中查找对应记录，生成该 data
session 专属的 symbol pool。

生成结果至少包括：

| 字段 | 来源 | 用途 |
| --- | --- | --- |
| `symbol_id` | `instrument_catalog` CSV | 写入 `BookTicker.symbol_id`，供策略热路径使用。 |
| `symbol` | `subscribe_symbols` / CSV 内部 symbol | 诊断、配置校验和构造订阅视图。 |
| `exchange_symbol` | `instrument_catalog` CSV | 构造交易所订阅 payload / stream target，例如 Gate `BTC_USDT`、Binance `BTCUSDT`。 |

约束：

1. `subscribe_symbols` 中的每个 symbol 必须能按 `(exchange, symbol)` 在 catalog 中找到唯一记录。
2. symbol pool 在启动期生成并固定；行情热路径只使用已生成的 `SymbolBinding` / lookup，不查询 CSV，也不解析配置。
3. `symbol_id` 和字符串存储的生命周期必须覆盖 data session 生命周期；`std::string_view` 只能指向稳定存储。

Gate futures 行情字段：

| 字段 | 默认值 | 含义 |
| --- | --- | --- |
| `settle` | `usdt` | Gate futures settle currency。 |
| `wire_format` | `sbe` | wire 格式；当前 Gate 行情主路径使用 SBE。 |
| `sbe_schema_id` | `1` | Gate SBE schema id；builder 生成 `/v4/ws/usdt/sbe?sbe_schema_id=1`。 |
| `feed` | `book_ticker` | 当前订阅的行情类型。 |

Binance futures 行情字段：

| 字段 | 默认值 | 含义 |
| --- | --- | --- |
| `market` | `um_futures` | Binance USD-M futures。 |
| `feed` | `book_ticker` | 当前订阅的行情类型。 |

## WebSocket Endpoint

配置位置：

```toml
websocket.endpoint = { host = "fx-ws.gateio.ws" }
```

| 字段 | 默认值 | 映射 | 含义 |
| --- | --- | --- | --- |
| `host` | 无，必须显式配置 | `ConnectionConfig.host` | DNS host。 |
| `service` | `"443"` | `ConnectionConfig.service` | 端口或服务名。 |
| `enable_tls` | `true` | `ConnectionConfig.enable_tls` | 是否使用 TLS。 |
| `connect_timeout_ms` | `10000` | `ConnectionConfig.cold_path_total_timeout_ms` | DNS + TCP + TLS + WebSocket handshake 总超时。 |

Gate private link 部署可显式设置 `enable_tls = false`。公网 `wss://` 连接应保留默认
`enable_tls = true`，或显式写成 `true`。

`target` 不属于 endpoint 默认字段，由 session builder 生成。例如 Gate SBE 行情由
`settle`、`wire_format` 和 `sbe_schema_id` 生成；Binance book ticker 由 `subscribe_symbols`
展开后生成 raw stream target。

## WebSocket Execution Policy

配置位置：

```toml
websocket.execution_policy = { bind_cpu_id = 2 }
```

| 字段 | 默认值 | 映射 | 含义 |
| --- | --- | --- | --- |
| `bind_cpu_id` | 无，当前 runtime 配置必须显式配置 | `RuntimePolicy.io_cpu_id` | 该 WebSocket session 所在线程绑定的 CPU id。 |
| `affinity_mode` | `"best_effort"` | `RuntimePolicy.affinity_mode` | CPU 绑定失败时的处理方式：`none` / `best_effort` / `required`。 |
| `lock_memory` | `true` | `RuntimePolicy.lock_memory` | 是否尝试 `mlockall`，减少运行中 page fault 风险。 |
| `prefault_stack` | `true` | `RuntimePolicy.prefault_stack` | 启动时预触碰一段栈内存，降低首次栈页缺页风险。 |
| `active_spin` | `true` | `RuntimePolicy.active_spin` | 是否启用 active spin 等待策略。 |
| `spin_iterations_before_clock_check` | `4096` | `RuntimePolicy.spin_iterations_before_clock_check` | active spin 中每多少轮检查一次时钟 / 状态。 |

说明：

- `bind_cpu_id` 是配置层命名；当前 C++ 字段仍叫 `io_cpu_id`。
- `lock_memory = true` 在权限不足的开发环境可能导致 runtime policy 应用失败；生产环境应显式配置 memlock 能力。
- `spin_iterations_before_clock_check` 是性能参数，调整时必须有 benchmark、profile 或 live probe 证据。

## WebSocket Read Path

配置位置：

```toml
websocket.read_path = { max_reads_per_drive = 8 }
```

| 字段 | 默认值 | 映射 | 含义 |
| --- | --- | --- | --- |
| `max_reads_per_drive` | `8` | `ConnectionConfig.max_reads_per_drive` | 一次 `DriveRead()` 最多调用多少次 `ReadSome()`。 |
| `read_until_would_block` | `false` | `ConnectionConfig.read_until_would_block` | 是否在 read budget 内更积极地读到 `EAGAIN` / would-block。 |

read path 的机制对照见 `doc/websocket_read_write_benchmark_comparison.md`。这些字段影响 burst
drain 能力和单轮 loop 占用时间；生产推荐值必须由 benchmark 或 live probe 支撑。

## WebSocket Heartbeat

配置位置：

```toml
websocket.heartbeat = { interval_ms = 5000, timeout_ms = 15000 }
```

| 字段 | 默认值 | 映射 | 含义 |
| --- | --- | --- | --- |
| `interval_ms` | `5000` | `ConnectionConfig.heartbeat_interval_ms` | heartbeat / ping 间隔。 |
| `timeout_ms` | `15000` | `ConnectionConfig.heartbeat_timeout_ms` | heartbeat 超时。 |

`timeout_ms` 应大于 `interval_ms`。

## WebSocket Reconnect

配置位置：

```toml
websocket.reconnect = { max_backoff_ms = 30000 }
```

| 字段 | 默认值 | 映射 | 含义 |
| --- | --- | --- | --- |
| `enabled` | `true` | `ReconnectPolicy.enabled` | 是否自动重连。 |
| `initial_backoff_ms` | `100` | `ReconnectPolicy.initial_backoff_ms` | 首次重连等待时间。 |
| `max_backoff_ms` | `30000` | `ReconnectPolicy.max_backoff_ms` | 最大重连 backoff。 |
| `backoff_shift_bits` | `1` | `ReconnectPolicy.backoff_shift_bits` | backoff 增长倍率为 `1 << shift_bits`。 |
| `jitter_percent` | `25` | `ReconnectPolicy.jitter_percent` | backoff 抖动比例。 |
| `max_attempts` | `0` | `ReconnectPolicy.max_attempts` | 最大重连次数；`0` 表示直到 stop 或 fatal error。 |

## Loader 合并规则

1. 先加载统一默认值。
2. 再用 `websocket.endpoint`、`websocket.execution_policy`、`websocket.read_path`、
   `websocket.heartbeat`、`websocket.reconnect` 覆盖默认值。
3. 最后由具体 session builder 生成 `target` 和 exchange-specific `SymbolBinding`。
4. 生产启动时每个进程只加载自己的 runtime TOML，不把其他进程的 session 放入同一份配置。

最终构造输入仍是：

```text
websocket::ConnectionConfig
std::span<const SymbolBinding>
Consumer&
```
