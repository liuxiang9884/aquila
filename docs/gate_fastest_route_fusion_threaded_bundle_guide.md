# Gate / Binance Fastest-Route Fusion Threaded Bundle Guide

**Goal:** 增加一个同进程多线程 fusion bundle，使 N 个 data session thread、1 个 fusion thread 和 1 个统一 log backend thread 在同一进程中运行，同时保留 source SHM 和 canonical SHM 监控边界。

**Architecture:** V1 threaded bundle 仍使用 source SHM 作为 data session 到 fusion 的主通信边界，复用现有 `DataSession`、`BookTickerFusionRunner` 和 analyzer。V2 只作为后续建议：当 V1 证据显示 SHM reader hop 或 tail 已成为瓶颈时，再引入 data session 到 fusion 的 in-process SPSC ring 主路径，并把 source SHM 降级为 monitor mirror。

**Tech Stack:** C++20 / CMake、`std::thread`、`toml++`、`CLI11`、Nova / quill logging、`core/market_data/data_shm.h`、Gate / Binance `DataSession`、`BookTickerFusionRunner`、GTest、Python fusion analyzer。

---

## 当前实现状态

截至 2026-06-15，V1 threaded bundle 已按“fusion 归 core、交易所启动归 tools”的边界落地：

- `BookTickerFusionConfig`、metadata ABI、runner 和 fusion thread wrapper 位于 `core/market_data/`；TOML parser 位于 `core/config/book_ticker_fusion_config.*`。
- `tools/market_data/book_ticker_fusion.cpp` 和 `tools/market_data/binance_book_ticker_fusion.cpp` 仍保留为独立 fusion process 入口。
- 新增 `gate_data_fusion` / `binance_data_fusion`，一个进程内运行 N 个 data session thread、1 个 fusion thread 和 1 个统一 log backend。
- tools 层的启动配置按交易所命名为 `GateDataFusionConfig` / `BinanceDataFusionConfig`；它只描述 data session config 引用和 source SHM override，后续可继续承载其他 data fusion type。
- 示例配置为 `config/market_data_fusion/gate_data_fusion_book_ticker_4sources.toml` 和 `config/market_data_fusion/binance_data_fusion_book_ticker_4sources.toml`。当前示例保留 fusion thread 在 fusion config 的 `bind_cpu_id = 16`，source threads 绑定 `17-20`，log backend 绑定 `31`。
- dry-run 验证入口：

```bash
./build/debug/tools/gate_data_fusion \
  --config config/market_data_fusion/gate_data_fusion_book_ticker_4sources.toml
./build/debug/tools/binance_data_fusion \
  --config config/market_data_fusion/binance_data_fusion_book_ticker_4sources.toml
```

当前事实源以上述实现状态、代码入口和下方验证方式为准；已完成的逐任务实施清单已删除。

## 背景

当前 fastest-route fusion 已落地为多进程 V1：

```text
N 个 data session process -> N 个 source BookTicker SHM -> fusion process -> canonical BookTicker SHM
```

30-symbol Gate / Binance shadow 显示 fusion 对 tail 有明显改善，且现有 fusion hop p99 是 us 级。多进程方案的主要成本是 N 个 data session process 各自初始化一套 logging backend，并带来更多进程管理和 CPU 噪声。threaded bundle 的目标是先减少这些冷路径和运维成本，而不是改变 fusion 算法或 SHM ABI。

## V1 设计约束

V1 threaded bundle 必须满足：

- 保留现有多进程 `gate_data_session` / `binance_data_session` / `gate_book_ticker_fusion` / `binance_book_ticker_fusion`，新功能以独立 bundle tool 形式加入。
- data session 到 fusion 之间继续走 source SHM，fusion 到下游继续走 canonical SHM。
- bundle 进程只初始化一次 Nova / quill logging，所有 data session、fusion 和 supervisor 共用一个 log backend。
- data session thread 不调用 `DataSession::Run()`，避免每个 session 覆盖进程级 `SIGINT` / `SIGTERM` handler；worker thread 调用 `DataSession::Start()`，由 supervisor 统一 stop。
- 第一版拒绝同一个 bundle 内混用 TLS 和 plain source，降低模板实例和生命周期复杂度。Gate private plain 和 Binance public TLS 分别由对应 bundle binary 支持。
- CPU 绑定必须来自配置或 source override；shadow 默认使用 `16-31` 测试 core，不占用实盘 hot path `0-15`。
- V1 不引入 in-process ring，不改变 data session hot path 的 `DataShmPublisher` 语义。

## V1 文件结构

| 文件 | 动作 | 责任 |
| --- | --- | --- |
| `core/market_data/fusion/config.h` | 已新增 | exchange-neutral `BookTickerFusionConfig` 纯结构，不依赖 `toml++` |
| `core/config/book_ticker_fusion_config.*` | 已新增 | 解析 fusion TOML，返回 `aquila::market_data::BookTickerFusionConfig` |
| `core/market_data/fusion/metadata.h` | 已迁入 | sidecar metadata record 和 binary writer |
| `core/market_data/fusion/book_ticker.h` | 已迁入 | 从 N 路 source SHM 读，写 canonical SHM 和 metadata |
| `core/market_data/fusion/thread.h` | 已新增 | 封装 fusion runner thread：构造 `BookTickerFusionRunner`，循环 `PollOnce()`，stop 后 `Flush()` |
| `tools/gate/gate_data_fusion_config.*` | 已新增 | Gate data fusion 启动配置，保存 fusion config 路径和 N 个 data session source override |
| `tools/binance/binance_data_fusion_config.*` | 已新增 | Binance data fusion 启动配置，保存 fusion config 路径和 N 个 data session source override |
| `tools/market_data/data_fusion_tool_support.h` | 已新增 | Gate / Binance data fusion CLI 共享启动期校验、source override 和 summary log helper |
| `tools/gate/gate_data_fusion.cpp` | 新增 | Gate data fusion CLI：加载 config、加载 Gate data session config、应用 source override、启动 workers |
| `tools/binance/binance_data_fusion.cpp` | 新增 | Binance data fusion CLI：加载 config、加载 Binance data session config、应用 source override、启动 workers |
| `tools/CMakeLists.txt` | 修改 | 增加 `gate_data_fusion` 和 `binance_data_fusion` target |
| `test/config/book_ticker_fusion_config_test.cpp` | 已迁入 | 覆盖 fusion config parser |
| `test/core/market_data/fusion/book_ticker_metadata_test.cpp` | 已迁入 | 覆盖 metadata writer |
| `test/core/market_data/fusion/book_ticker_runner_test.cpp` | 已迁入 | 覆盖 runner 发布 canonical SHM 和 metadata |
| `test/core/market_data/fusion/book_ticker_thread_test.cpp` | 已新增 | 用临时 SHM 验证 fusion thread 可发布、可 stop、metadata 可 flush |
| `test/tools/gate/gate_data_fusion_config_test.cpp` | 已新增 | 覆盖 Gate data fusion config parser |
| `test/tools/binance/binance_data_fusion_config_test.cpp` | 已新增 | 覆盖 Binance data fusion config parser |
| `config/market_data_fusion/gate_data_fusion_book_ticker_4sources.toml` | 已新增 | Gate 4-source threaded launch 示例配置 |
| `config/market_data_fusion/binance_data_fusion_book_ticker_4sources.toml` | 已新增 | Binance 4-source threaded launch 示例配置 |
| `docs/gate_fastest_route_fusion_threaded_bundle_guide.md` | 维护 | 当前 threaded bundle 入口、验证命令和 V2 条件索引 |

## V1 Config 形状

launch config 不把多个 `[data_session]` 写进现有 data session TOML。它引用一个或多个现有 data session config，并显式覆盖 source 差异：

```toml
[launch]
name = "gate_data_fusion_book_ticker_4sources"
fusion_config = "config/market_data_fusion/gate_book_ticker_fusion_4sources.toml"

[[launch.sources]]
source_id = 0
data_session_config = "config/data_sessions/gate_data_session_30symbols_private_plain_20260604.toml"
data_session_name = "gate_source_0"
book_ticker_shm_name = "aquila_gate_book_ticker_src_0"
book_ticker_channel_name = "book_ticker_channel"
remove_existing_source_shm = true
bind_cpu_id = 17

[[launch.sources]]
source_id = 1
data_session_config = "config/data_sessions/gate_data_session_30symbols_private_plain_20260604.toml"
data_session_name = "gate_source_1"
book_ticker_shm_name = "aquila_gate_book_ticker_src_1"
book_ticker_channel_name = "book_ticker_channel"
remove_existing_source_shm = true
bind_cpu_id = 18

[[launch.sources]]
source_id = 2
data_session_config = "config/data_sessions/gate_data_session_30symbols_private_plain_20260604.toml"
data_session_name = "gate_source_2"
book_ticker_shm_name = "aquila_gate_book_ticker_src_2"
book_ticker_channel_name = "book_ticker_channel"
remove_existing_source_shm = true
bind_cpu_id = 19

[[launch.sources]]
source_id = 3
data_session_config = "config/data_sessions/gate_data_session_30symbols_private_plain_20260604.toml"
data_session_name = "gate_source_3"
book_ticker_shm_name = "aquila_gate_book_ticker_src_3"
book_ticker_channel_name = "book_ticker_channel"
remove_existing_source_shm = true
bind_cpu_id = 20
```

Gate parser 输出结构；Binance 使用同形状的 `BinanceDataFusionSourceConfig` / `BinanceDataFusionConfig`：

```cpp
struct GateDataFusionSourceConfig {
  std::int32_t source_id{-1};
  std::filesystem::path data_session_config;
  std::string data_session_name;
  std::string book_ticker_shm_name;
  std::string book_ticker_channel_name{"book_ticker_channel"};
  bool remove_existing_source_shm{true};
  std::int32_t bind_cpu_id{-1};
};

struct GateDataFusionConfig {
  std::string name;
  std::filesystem::path fusion_config;
  std::vector<GateDataFusionSourceConfig> sources;
};
```

source override 应用规则：

- `data_session_config` 先走对应交易所现有 parser。
- `data_session_name` 覆盖 `DataSessionConfig::name`。
- `book_ticker_shm_name` / `book_ticker_channel_name` 覆盖 `DataSessionConfig::book_ticker_shm`。
- `remove_existing_source_shm` 覆盖 source SHM 创建行为。
- `bind_cpu_id >= 0` 时覆盖 `connection.runtime_policy.io_cpu_id`。
- `diagnostics.latency_outlier.source_id` 覆盖为 bundle source id，保持 L4 outlier attribution 可对齐。

## V1 生命周期

启动顺序：

1. main thread 解析 bundle TOML，并用同一个 parsed table 初始化一次 logging。
2. 加载 fusion config，校验 fusion config 的 source id / SHM 与 bundle source override 一致。
3. 逐个加载 data session config，应用 source override，构造 source `DataShmPublisher` 和 data session worker。
4. 所有 source SHM 创建完成后，构造 `BookTickerFusionRunner`，使 fusion reader 可以 attach source SHM。
5. 安装一次进程级 signal handler，设置 bundle stop flag。
6. 启动 N 个 data session threads，再启动 1 个 fusion thread。
7. 收到 stop 后先调用所有 data session `Stop()`，再 stop fusion thread，最后 join threads 并 flush metadata / published count。

退出结果必须包含：

```text
result=ok|failed
bundle=<name>
source_count=<N>
fusion_total_read_count=<count>
fusion_total_published_count=<count>
fusion_metadata_write_errors=<count>
metadata_output=<path>
```

## V1 Shadow 验证计划

V1 threaded bundle 做 shadow 时，不接策略。运行目录写入 `/home/liuxiang/tmp/<run_id>/`，默认使用 `16-31` 测试 core。

最小 30-symbol Gate shadow：

```bash
timeout --kill-after=10s 1860s \
  ./build/release/tools/gate_data_fusion \
  --config config/market_data_fusion/gate_data_fusion_book_ticker_4sources.toml \
  --connect \
  --max-runtime-ms 1800000
```

同时用现有 recorder 记录 4 路 source SHM 和 canonical fusion SHM，之后复用 analyzer：

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/market_data/analyze_book_ticker_fusion_latency.py \
  --source-bin 0=/home/liuxiang/tmp/<run_id>/bin/source_0.bin \
  --source-bin 1=/home/liuxiang/tmp/<run_id>/bin/source_1.bin \
  --source-bin 2=/home/liuxiang/tmp/<run_id>/bin/source_2.bin \
  --source-bin 3=/home/liuxiang/tmp/<run_id>/bin/source_3.bin \
  --fusion-bin /home/liuxiang/tmp/<run_id>/bin/fusion.bin \
  --metadata-bin /home/liuxiang/tmp/<run_id>/bin/fusion_metadata.bin \
  --output-dir /home/liuxiang/tmp/<run_id>/analysis/published_fusion_bundle
```

对比口径：

- multi-process fusion vs threaded bundle 的 `fusion_hop_ns` p50 / p99 / p99.9 / max。
- source `exchange_ns -> source_local_ns` 是否因共用 log backend 或同进程调度发生回归。
- canonical `exchange_ns -> fusion_publish_ns` 是否保持或改善。
- `winner_ratio` 是否大体稳定。
- source recorder `skipped` / `overruns` 是否为 0。
- `fusion_total_read_count`、`fusion_total_published_count` 和 metadata record count 是否一致。
- L4 outlier logging 开 / 关两组对比，确认单 log backend 是否引入共享扰动。

只有 threaded bundle shadow 明确不回归 source latency、fusion hop 和 canonical tail 后，才讨论策略改读 bundle 输出的 canonical SHM。

## V2 Direct Ring 建议

V2 不作为当前实施范围。进入 V2 的条件是 V1 threaded bundle 的 shadow 证据显示：

- `fusion_hop_ns` 的 p99 / p99.9 或 max 已经是 LeadLag freshness / reject 的可见贡献项；或
- source SHM reader / publisher 在 profile 中成为 fusion thread 的主要 CPU 成本；或
- 同进程 bundle 已经稳定运行，多轮 shadow 显示故障恢复、日志和 recorder 边界可控。

V2 建议结构：

```text
data session thread
  -> direct SPSC ring producer -> fusion thread
  -> source SHM mirror         -> recorder / monitor / DataReader

fusion thread
  -> drains N SPSC rings
  -> BookTickerFusionCore
  -> canonical SHM
  -> sidecar metadata
```

V2 关键约束：

- SPSC ring 是 fusion 主路径，source SHM 是 mirror，不参与 fusion winner selection。
- data session hot path 不阻塞等待 fusion；ring full 时按显式策略 drop newest 或 drop oldest，并记录 per-source drop counter。
- ring capacity 固定、power-of-two、cacheline aligned；push / pop 只使用必要的 acquire / release。
- mirror SHM failure 不得阻塞 direct ring push，但必须计数并通过 log / stats 暴露。
- metadata 需要新增字段或 sidecar stats 来记录 ring drop 情况，否则离线 analyzer 无法解释缺失 update。
- V2 必须和 V1 bundle 做 A/B shadow，验证 direct ring 对 `fusion_hop_ns` 和 canonical tail 的收益真实存在。

V2 如果实施，建议先增加一个 isolated microbenchmark，对比：

```text
DataShmPublisher -> BookTickerShmReader -> fusion core
SPSC producer -> SPSC consumer -> fusion core
```

该 benchmark 必须记录 CPU、affinity、capacity、payload size、p50 / p99 / p99.9 / max 和 drop policy。没有 benchmark 或 shadow 证据时，不把 direct ring 假定为生产收益。
