# Data Session 配置说明

## 范围

data session TOML 描述单个行情进程的启动输入：instrument catalog、一个 data
session、该 session 的 WebSocket 连接和运行策略。策略参数不放在这里；risk control、
order management 和 order execution 归属于 `Strategy` 模块。

第一版配置遵循：

1. 每个行情进程只加载一份 config，配置中只有一个 `[data_session]`，不使用数组。
2. 每个 session 自己持有 WebSocket 配置，不引入 `websocket.profiles` 复用层。
3. TOML 只写连接之间必须不同或最常改的字段。
4. loader 对缺省字段填统一默认值，并在启动冷路径生成 `DataSession` 可直接消费的运行期 config。
5. WebSocket `target` 不写在配置里，由具体交易所的 data session config parser 按协议生成。
6. tools 根据 `enable_tls` 选择 TLS 或 plain WebSocket policy，生产路径不在同一个 binary 中硬编码协议。

## 实现依赖边界

配置相关代码属于冷路径，只在进程启动、重连前配置构建或测试中使用，不进入行情和下单热路径。
当前依赖选择如下：

| 用途 | 依赖 | 当前边界 |
| --- | --- | --- |
| TOML 解析 | `toml++` / `PkgConfig::tomlplusplus` | C++ 配置 loader 使用；当前入口是 `core/config/websocket_config.h` / `core/config/websocket_config.cpp`。 |
| 日志输出 | `nova/utils/log.h` | 项目代码通过 Nova 封装输出，例如解析失败时使用 `NOVA_ERROR`；不在业务代码中直接依赖底层 log 库。 |
| instrument CSV | `vincentlaucsb-csv-parser` | C++ instrument catalog loader 使用该 vcpkg 依赖；`InstrumentInfo` 加载 CSV 的完整字段，data session 当前只消费 `symbol_id`、`exchange`、`symbol`、`exchange_symbol`。 |
| 合约查询脚本输出 | `pandas.DataFrame` | `scripts/gate/query_futures_contracts.py` 和 `scripts/binance/query_um_futures_contracts.py` 使用 Python pandas 生成统一字段表和 CSV。 |

## 进程拆分

生产推荐把 Gate 行情、Binance 行情、策略 / 交易拆成独立进程：

```text
gate-md-process
  GateDataSessionThread
    GateDataSession

binance-md-process
  BinanceDataSessionThread
    BinanceDataSession

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

因此生产 data session 配置也按进程拆分。当前已给出：

```text
config/data_sessions/gate_data_session.toml
config/data_sessions/binance_data_session.toml
```

多个 data session config 可以指向同一个 `instrument_catalog` CSV；CSV 是共享合约元数据来源，
不是进程私有状态。开发期如果需要合并启动多个 session，应由临时工具组合多个 config，而不是把
多个 session 写进同一个生产配置文件。

## Gate 行情进程示例

```toml
[instrument_catalog]
file = "config/instruments/usdt_futures.csv"
schema = "aquila.instrument.v1"

[data_session]
name = "gate_data_session"
subscribe_symbols = ["BTC_USDT", "ETH_USDT", "SOL_USDT"]
settle = "usdt"
wire_format = "sbe"
sbe_schema_id = 1
feed = "book_ticker"

[data_session.websocket.endpoint]
host = "fx-ws.gateio.ws"
enable_tls = true

[data_session.websocket.execution_policy]
bind_cpu_id = 2
```

仓库内示例配置指向 Gate 公网行情 endpoint，语义是
`wss://fx-ws.gateio.ws:443/v4/ws/usdt/sbe?sbe_schema_id=1`。如果生产部署使用
Gate private link / plain WS，需要同时替换为 private host / service，并显式设置
`enable_tls = false`；不要把公网 `fx-ws.gateio.ws:443` 和 `enable_tls = false` 组合使用。

## Binance 行情进程示例

```toml
[instrument_catalog]
file = "config/instruments/usdt_futures.csv"
schema = "aquila.instrument.v1"

[data_session]
name = "binance_data_session"
subscribe_symbols = ["BTC_USDT", "ETH_USDT", "SOL_USDT"]
market = "um_futures"
feed = "book_ticker"

[data_session.websocket.endpoint]
host = "fstream.binance.com"

[data_session.websocket.execution_policy]
bind_cpu_id = 3
```

## Instrument Catalog

| 字段 | 默认值 | 含义 |
| --- | --- | --- |
| `file` | 无，必须显式配置 | instrument CSV 路径。仓库示例使用 repo-root relative path，例如 `config/instruments/usdt_futures.csv`。 |
| `schema` | 无，必须显式配置 | Aquila 自定义 CSV 字段版本；当前固定 `aquila.instrument.v1`。 |

`instrument_catalog` 第一版是 CSV 数据源，不表示引入真实数据库。启动期读取 CSV 并构建
`InstrumentCatalog`。`InstrumentInfo` 会加载 CSV 的完整字段；当前 data session 只读取
`symbol_id`、`exchange`、`symbol` 和 `exchange_symbol` 四列；price tick、quantity step 等交易约束字段留给后续交易端 / 下单校验使用。
data session 的运行期 symbol 输入由 `instrument_catalog` 和 `subscribe_symbols` 共同生成。
`LoadDataSessionConfigFile()` 从文件加载仓库内示例配置时，会把 TOML 中的相对 CSV 路径解析到对应仓库路径；生产部署也可以直接写绝对路径。

## Data Session

| 字段 | 默认值 | 含义 |
| --- | --- | --- |
| `name` | 无，必须显式配置 | session 实例名。 |
| `subscribe_symbols` | 无，必须显式配置 | 该 data session 要订阅的人类可读内部 symbol，例如 `BTC_USDT`；不直接写交易所 symbol。 |

`type`、`exchange`、`thread` 不放在配置里：具体 binary 已经决定 session 类型、交易所和线程角色。
例如 `gate_data_session` binary 只会启动 `GateDataSession`。

## Symbol Pool 生成

`subscribe_symbols` 只表示订阅意图，不直接进入行情热路径。启动期 data session config parser 按
具体 binary 对应的 `exchange` 和 `subscribe_symbols` 从 `InstrumentCatalog` 中查找对应记录，
生成该 data session 专属的 symbol pool，并写入最终 `DataSessionConfig`。

生成结果至少包括：

| 字段 | 来源 | 用途 |
| --- | --- | --- |
| `symbol_id` | `instrument_catalog` CSV | 写入 `BookTicker.symbol_id`，供策略热路径使用。 |
| `symbol` | `subscribe_symbols` / CSV 内部 symbol | 诊断、配置校验和构造订阅视图。 |
| `exchange_symbol` | `instrument_catalog` CSV | 构造交易所订阅 payload / stream target，例如 Gate `BTC_USDT`、Binance `BTCUSDT`。 |

约束：

1. `subscribe_symbols` 中的每个 symbol 必须能按 `(exchange, symbol)` 在 catalog 中找到唯一记录。
2. symbol pool 在启动期生成并固定；行情热路径只使用已生成的 `gate::SymbolBinding` / lookup，不查询 CSV，也不解析配置。
3. 每个 data session 只连接一个交易所，运行期 symbol lookup 放在 session / client 内部，当前 Gate 用 `exchange_symbol -> symbol_id` 的 `absl::flat_hash_map`。
4. `symbol_id` 和字符串存储的生命周期必须覆盖 data session 生命周期；`std::string_view` 只能指向稳定存储。

Gate futures 行情字段：

| 字段 | 默认值 | 含义 |
| --- | --- | --- |
| `settle` | `usdt` | Gate futures settle currency。 |
| `wire_format` | `sbe` | wire 格式；当前 Gate 行情主路径使用 SBE。 |
| `sbe_schema_id` | `1` | Gate SBE schema id；Gate config parser 生成 `/v4/ws/usdt/sbe?sbe_schema_id=1`。 |
| `feed` | `book_ticker` | 当前订阅的行情类型。 |

Gate 当前实现入口：

```text
core/config/instrument_catalog.h
core/config/websocket_config.h
exchange/gate/market_data/data_session_config.cpp
exchange/gate/market_data/data_session_config.h
tools/gate/data_session.cpp
```

Data session TOML parser 放在对应交易所的 `exchange/*/market_data/`，因为 Gate 和 Binance 的
data session 字段不完全相同，不把交易所特有字段放入 `core/config`。`core/config` 当前只保留
WebSocket config 和 instrument catalog 这类交易所无关配置。

默认运行下面命令只做 dry-run，验证 TOML、CSV、target 和 symbol 映射生成结果，不连接网络：

```bash
./build/debug/tools/gate_data_session
```

需要实际连接时显式加 `--connect`，进程会一直运行到收到 SIGINT 或 SIGTERM：

```bash
./build/debug/tools/gate_data_session --connect
```

当前 `gate_data_session` tool 会：

1. 调用 `LoadDataSessionConfigFile()` 读取 TOML 和 CSV。
2. 在启动冷路径生成 `DataSessionConfig`，包括 `ConnectionConfig`、target、exchange symbol 列表和 symbol id 列表。
3. 根据 `connection.enable_tls` 选择 `DefaultTlsWebSocketPolicy` 或 `DefaultPlainWebSocketPolicy`。
4. `--connect` 模式调用 `DataSession::Run()`；`Run()` 在 session 内部安装 SIGINT / SIGTERM stop handler，使 active spin loop 通过 `Stop()` 退出。

Binance futures 行情字段：

| 字段 | 默认值 | 含义 |
| --- | --- | --- |
| `market` | `um_futures` | Binance USD-M futures。 |
| `feed` | `book_ticker` | 当前订阅的行情类型。 |

Binance 当前实现入口：

```text
core/config/instrument_catalog.h
core/config/websocket_config.h
exchange/binance/market_data/data_session_config.cpp
exchange/binance/market_data/data_session_config.h
tools/binance/data_session.cpp
```

Binance config parser 读取 `instrument_catalog` 和 `subscribe_symbols` 后按 `Exchange::kBinance`
生成 exchange symbol 列表，例如内部 `BTC_USDT` 对应 Binance `BTCUSDT`，并在冷路径生成
`/public/ws/btcusdt@bookTicker/...` raw stream target。

默认运行下面命令只做 dry-run，验证 TOML、CSV、target 和 symbol 映射生成结果，不连接网络：

```bash
./build/debug/tools/binance_data_session
```

需要实际连接时显式加 `--connect`，进程会一直运行到收到 SIGINT 或 SIGTERM：

```bash
./build/debug/tools/binance_data_session --connect
```

当前 `binance_data_session` tool 与 Gate tool 保持相同的冷路径流程：先解析 TOML 和 CSV，
生成 `DataSessionConfig`，再按 `connection.enable_tls` 选择 WebSocket policy。Binance raw
stream target 在 parser / session 构造阶段由 exchange symbol 列表生成，active 后不发送 runtime
subscribe。

## WebSocket Endpoint

当前 C++ 实现入口是 `core/config/websocket_config.h` / `core/config/websocket_config.cpp`。
`ParseWebSocketConfig()` 解析 `[data_session.websocket]` 节点，`ToConnectionConfig()` 在
冷路径把配置层结构转换为 `websocket::ConnectionConfig`。解析失败时返回错误字符串；如果
nova log 已初始化，会通过 `NOVA_ERROR` 输出诊断。

parser 只做启动配置需要的最低限度解析约束：`endpoint.host` 和
`execution_policy.bind_cpu_id` 必须显式配置，`affinity_mode` 必须能映射到已知枚举值。
其他可选字段缺省时使用默认值，不在 parser 中重复做运行期防御性校验。

配置位置：

```toml
[data_session.websocket.endpoint]
host = "fx-ws.gateio.ws"
enable_tls = true
```

| 字段 | 默认值 | 映射 | 含义 |
| --- | --- | --- | --- |
| `host` | 无，必须显式配置 | `ConnectionConfig.host` | DNS host。 |
| `service` | `"443"` | `ConnectionConfig.service` | 端口或服务名。 |
| `enable_tls` | `true` | `ConnectionConfig.enable_tls` | 是否使用 TLS。 |
| `connect_timeout_ms` | `10000` | `ConnectionConfig.cold_path_total_timeout_ms` | DNS + TCP + TLS + WebSocket handshake 总超时。 |

`enable_tls` 表示当前 WebSocket 物理连接是否走 TLS；tool 会用它选择编译期
`DefaultTlsWebSocketPolicy` 或 `DefaultPlainWebSocketPolicy`。典型组合如下：

| 场景 | host / service | `enable_tls` | 协议语义 |
| --- | --- | --- | --- |
| Gate 公网行情 | `fx-ws.gateio.ws` / `443` | `true` | `wss://fx-ws.gateio.ws:443/...` |
| Gate private link plain WS | private host / plain WS port | `false` | `ws://<private-host>:<port>/...` |
| Binance 公网行情 | `fstream.binance.com` / `443` | `true` | `wss://fstream.binance.com:443/...` |

因此 Gate public config 应保留默认 `enable_tls = true`，或显式写成 `true`。Gate private link
部署可以显式设置 `enable_tls = false`，但必须同时使用对应 private endpoint。

`target` 不属于 endpoint 默认字段，由 data session config parser 在冷路径生成。例如 Gate SBE 行情由
`settle`、`wire_format` 和 `sbe_schema_id` 生成；Binance book ticker 由 `subscribe_symbols`
展开后生成 raw stream target。

## WebSocket Execution Policy

配置位置：

```toml
[data_session.websocket.execution_policy]
bind_cpu_id = 2
```

| 字段 | 默认值 | 映射 | 含义 |
| --- | --- | --- | --- |
| `bind_cpu_id` | 无，当前 data session 配置必须显式配置 | `RuntimePolicy.io_cpu_id` | 该 WebSocket session 所在线程绑定的 CPU id。 |
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
[data_session.websocket.read_path]
max_reads_per_drive = 8
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
[data_session.websocket.heartbeat]
interval_ms = 5000
timeout_ms = 15000
```

| 字段 | 默认值 | 映射 | 含义 |
| --- | --- | --- | --- |
| `interval_ms` | `5000` | `ConnectionConfig.heartbeat_interval_ms` | heartbeat / ping 间隔。 |
| `timeout_ms` | `15000` | `ConnectionConfig.heartbeat_timeout_ms` | heartbeat 超时。 |

`timeout_ms` 应大于 `interval_ms`。

## WebSocket Reconnect

配置位置：

```toml
[data_session.websocket.reconnect]
max_backoff_ms = 30000
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
2. 再用 `data_session.websocket.endpoint`、`data_session.websocket.execution_policy`、
   `data_session.websocket.read_path`、`data_session.websocket.heartbeat`、
   `data_session.websocket.reconnect` 覆盖默认值。
3. 最后由具体交易所的 data session config parser 生成 `target`、`ConnectionConfig`、exchange symbol 列表和 symbol id 列表。
4. 生产启动时每个进程只加载自己的 data session TOML，不把其他进程的 session 放入同一份配置。
5. 启动 tool 按最终 `ConnectionConfig.enable_tls` 选择 TLS / plain WebSocket policy；session 本身只消费已经生成好的 config。

生产 tool 的构造输入是：

```text
DataSessionConfig
Consumer&
```
