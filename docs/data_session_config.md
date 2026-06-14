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
| 合约查询脚本输出 | `pandas.DataFrame` | `scripts/gate/market_data/query_futures_contracts.py` 和 `scripts/binance/market_data/query_um_futures_contracts.py` 使用 Python pandas 生成统一字段表和 CSV。 |

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

## Log

Gate / Binance data session config 当前支持顶层 `[log]` section。data session tool 启动时先按
Sirius `third_party/sirius/tools/gate/data_center.cpp` 的模式解析 TOML，调用
`nova::LogConfig::FromToml(toml["log"])`，再用 `nova::InitializeLogging(log_config)` 初始化日志。
因此后续 instrument catalog / data session parser 的诊断和 dry-run 输出都会使用该配置。

示例：

```toml
[log]
log_level = "info"
file_sink_name = "/home/liuxiang/log/gate_data_session.log"
console_sink_name = "gate_data_session_console"
backend_thread_name = "gate_data_session_log"
backend_cpu_affinity = 5
format_pattern = "%(log_level_short_code)%(time) %(process_id):%(thread_id) %(file_name):%(caller_function):%(line_number)] %(message)"
timestamp_pattern = "%Y-%m-%d %H:%M:%S.%Qns"
```

字段语义由 `nova/utils/log.h` 中的 `nova::LogConfig` 定义。当前 Gate 和 Binance data
session tools 都会消费这些字段；各 tool 只 parse 一次 TOML，并把同一个 parsed table 同时用于
log 初始化和 data session config 生成。仓库示例把文件日志统一写到 `/home/liuxiang/log/`
下，并用 data session 名称区分 file sink、console sink 和 backend log thread。Nova file sink
当前会在配置文件名上追加启动时间，例如配置为 `/home/liuxiang/log/gate_data_session.log`
时，实际生成文件类似 `/home/liuxiang/log/gate_data_session_YYYYMMDD_HHMMSS.log`。

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
port = "443"
enable_tls = true

[data_session.websocket.execution_policy]
bind_cpu_id = 2

[data_shm_sink]
enabled = true
shm_name = "aquila_gate_market_data"
channel_name = "book_ticker_channel"
create = true
remove_existing = false
```

仓库内示例配置指向 Gate 公网行情 endpoint，语义是
`wss://fx-ws.gateio.ws:443/v4/ws/usdt/sbe?sbe_schema_id=1`。如果生产部署使用
Gate private link / plain WS，需要同时替换为 private host / port，并显式设置
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

[data_shm_sink]
enabled = true
shm_name = "aquila_binance_market_data"
channel_name = "book_ticker_channel"
create = true
remove_existing = false
```

`[data_shm_sink]` 是可选顶层 section。`enabled = true` 时 data session tool 选择
`DataShmPublisher` 作为唯一 data sink；未配置或 `enabled = false` 时选择默认计数 sink。
第一版容量固定在代码常量中，TOML 不支持 `capacity` 或 `expected_capacity`。

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

## Data Session Diagnostics

Gate / Binance data session config 支持可选的 `[data_session.diagnostics.*]` section。该 section
只在启动冷路径解析；默认不启用，且默认 build 使用 `AQUILA_DATA_SESSION_DIAG_LEVEL=0`，保持现有
data session 热路径行为。运行期配置不能超过编译期 level：

- `latency_outlier.enabled = true` 要求 `AQUILA_DATA_SESSION_DIAG_LEVEL >= 1`。
- `timestamping.enabled = true` 要求 `AQUILA_DATA_SESSION_DIAG_LEVEL >= 4`。

第一版 latency outlier 诊断不写 sidecar binary / CSV，只在超过阈值时写当前 data session 的 Nova log，
log key 为 `data_session_book_ticker_latency_outlier`。详细字段见 `docs/diagnostic_fields.md`。

示例：

```toml
[data_session.diagnostics.latency_outlier]
enabled = true
source_id = 0
threshold_ns = 5000000
max_logs_per_second = 1000

[data_session.diagnostics.timestamping]
enabled = true
rx_software = true
```

字段：

| 字段 | 默认值 | 编译期要求 | 含义 |
| --- | --- | --- | --- |
| `data_session.diagnostics.latency_outlier.enabled` | `false` | `L1+` | 开启 BookTicker latency outlier log。 |
| `data_session.diagnostics.latency_outlier.source_id` | `0` | `L1+` | 当前 source 的编号，用于 n 路 source / fusion 对齐。 |
| `data_session.diagnostics.latency_outlier.threshold_ns` | `5000000` | `L1+` | 当 `BookTicker.local_ns - BookTicker.exchange_ns` 大于该阈值时触发 log。 |
| `data_session.diagnostics.latency_outlier.max_logs_per_second` | `1000` | `L1+` | 每个 data session client 独立 1 秒窗口限流；`0` 表示禁止 outlier log。 |
| `data_session.diagnostics.timestamping.enabled` | `false` | `L4+` | 请求 WebSocket socket timestamping 能力。当前 data session 只消费 RX software timestamp。 |
| `data_session.diagnostics.timestamping.rx_software` | `false` | `L4+` | plain transport 可用时填充 `kernel_rx_ns` / `kernel_rx_available`。 |

边界：

- `L0` 不改变 `MessageView` 布局，不采集 read / parse / publish 时间戳，也不会输出 outlier log。
- `L1` 只做 correlation 和阈值 log；`parse_done_ns`、`shm_publish_done_ns` 以及分段字段缺失时为 `0` 或 `-1`。
- `L2` 增加 userspace 分段：read enter / return、handler entry、parse done、SHM publish done。
- `L3` 当前保留 level 名称，尚未实现 data session `TCP_INFO` / recv queue 采样。
- `L4` 复用 WebSocket socket timestamping；当前只有实现 `TakeLastRxSoftwareTimestampNs()` 的 plain socket 会提供
  RX software timestamp，TLS transport 不提供 `kernel_rx_ns`。

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

1. 调用 `toml::parse_file()` 解析一次 TOML。
2. 用同一个 parsed table 初始化 Nova log，并调用 `ParseDataSessionConfig()` 读取 CSV。
3. 在启动冷路径生成 `DataSessionConfig`，包括 `ConnectionConfig`、target、exchange symbol 列表和 symbol id 列表。
4. 根据 `connection.enable_tls` 选择 `DefaultTlsWebSocketPolicy` 或 `DefaultPlainWebSocketPolicy`。
5. `--connect` 模式调用 `DataSession::Run()`；`Run()` 在 session 内部安装 SIGINT / SIGTERM stop handler，使 active spin loop 通过 `Stop()` 退出。

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
port = "443"
enable_tls = true
```

| 字段 | 默认值 | 映射 | 含义 |
| --- | --- | --- | --- |
| `host` | 无，必须显式配置 | `ConnectionConfig.host` | WebSocket logical host；始终用于 HTTP Upgrade `Host` header；TLS 开启时也用于 SNI 和证书 hostname verification。 |
| `connect_ip` | `""` | `ConnectionConfig.connect_ip` | 可选 TCP connect IP。为空时 TCP connect 解析 `host`；非空时 TCP connect 直连 `connect_ip:port`，`host` 仍保留为协议层 logical host。 |
| `port` | `"443"` | `ConnectionConfig.port` | TCP 端口。 |
| `enable_tls` | `true` | `ConnectionConfig.enable_tls` | 是否使用 TLS。 |
| `connect_timeout_ms` | `10000` | `ConnectionConfig.cold_path_total_timeout_ms` | DNS + TCP + TLS + WebSocket handshake 总超时。 |

`enable_tls` 表示当前 WebSocket 物理连接是否走 TLS；tool 会用它选择编译期
`DefaultTlsWebSocketPolicy` 或 `DefaultPlainWebSocketPolicy`。典型组合如下：

| 场景 | host / connect_ip / port | `enable_tls` | 协议语义 |
| --- | --- | --- | --- |
| Gate 公网行情 | `fx-ws.gateio.ws` / 空 / `443` | `true` | TCP 解析 `fx-ws.gateio.ws`，TLS SNI / cert verify / WebSocket Host 均为 `fx-ws.gateio.ws`。 |
| Gate 公网行情 IP pinning | `fx-ws.gateio.ws` / `57.181.9.46` / `443` | `true` | TCP 直连 `57.181.9.46:443`，TLS SNI / cert verify / WebSocket Host 仍为 `fx-ws.gateio.ws`。 |
| Gate private link plain WS | private host 或 logical host / 可选 private IP / plain WS port | `false` | TCP 连接 `connect_ip:port` 或解析 `host:port`；WebSocket Host 始终为 `host`。 |
| Binance 公网行情 | `fstream.binance.com` / 空 / `443` | `true` | TCP 解析 `fstream.binance.com`，TLS SNI / cert verify / WebSocket Host 均为 `fstream.binance.com`。 |

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

read path 的机制对照见 `docs/websocket_read_write_benchmark_comparison.md`。这些字段影响 burst
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
