# Gate / Binance Fastest-Route Fusion Threaded Bundle Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 增加一个同进程多线程 fusion bundle，使 N 个 data session thread、1 个 fusion thread 和 1 个统一 log backend thread 在同一进程中运行，同时保留 source SHM 和 canonical SHM 监控边界。

**Architecture:** V1 threaded bundle 仍使用 source SHM 作为 data session 到 fusion 的主通信边界，复用现有 `DataSession`、`BookTickerFusionRunner` 和 analyzer。V2 只作为后续建议：当 V1 证据显示 SHM reader hop 或 tail 已成为瓶颈时，再引入 data session 到 fusion 的 in-process SPSC ring 主路径，并把 source SHM 降级为 monitor mirror。

**Tech Stack:** C++20 / CMake、`std::thread`、`toml++`、`CLI11`、Nova / quill logging、`core/market_data/data_shm.h`、Gate / Binance `DataSession`、`BookTickerFusionRunner`、GTest、Python fusion analyzer。

---

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
| `tools/market_data/book_ticker_fusion_bundle_config.h` | 新增 | exchange-neutral bundle config 结构，保存 bundle 名称、fusion config 路径和 N 个 source override |
| `tools/market_data/book_ticker_fusion_bundle_config.cpp` | 新增 | 解析 bundle TOML，校验 source id 唯一、source SHM 名称非空、fusion config 路径非空 |
| `test/tools/market_data/book_ticker_fusion_bundle_config_test.cpp` | 新增 | 覆盖 config parser 成功路径、重复 source id、空 sources、缺 fusion config |
| `tools/market_data/book_ticker_fusion_thread.h` | 新增 | 封装 fusion runner thread：构造 `BookTickerFusionRunner`，循环 `PollOnce()`，stop 后 `Flush()` |
| `test/tools/market_data/book_ticker_fusion_thread_test.cpp` | 新增 | 用临时 SHM 验证 fusion thread 可发布、可 stop、metadata 可 flush |
| `tools/market_data/gate_book_ticker_fusion_bundle.cpp` | 新增 | Gate bundle CLI：加载 bundle config、加载 Gate data session config、应用 source override、启动 workers |
| `tools/market_data/binance_book_ticker_fusion_bundle.cpp` | 新增 | Binance bundle CLI：加载 bundle config、加载 Binance data session config、应用 source override、启动 workers |
| `tools/CMakeLists.txt` | 修改 | 增加 `gate_book_ticker_fusion_bundle` 和 `binance_book_ticker_fusion_bundle` target |
| `test/tools/market_data/CMakeLists.txt` | 修改 | 增加 bundle config / fusion thread tests |
| `config/market_data_fusion/gate_book_ticker_fusion_bundle_4sources.toml` | 新增 | Gate 4-source bundle 示例配置 |
| `config/market_data_fusion/binance_book_ticker_fusion_bundle_4sources.toml` | 新增 | Binance 4-source bundle 示例配置 |
| `docs/gate_fastest_route_fusion_threaded_bundle_plan.md` | 修改 | 实现后同步入口、验证命令和 shadow 结果索引 |

## V1 Config 形状

bundle config 不把多个 `[data_session]` 写进现有 data session TOML。它引用一个或多个现有 data session config，并显式覆盖 source 差异：

```toml
[bundle]
name = "gate_book_ticker_fusion_bundle_4sources"
fusion_config = "config/market_data_fusion/gate_book_ticker_fusion_4sources.toml"

[[bundle.sources]]
source_id = 0
data_session_config = "config/data_sessions/gate_data_session_30symbols_private_plain_20260604.toml"
data_session_name = "gate_source_0"
book_ticker_shm_name = "aquila_gate_book_ticker_src_0"
book_ticker_channel_name = "book_ticker_channel"
remove_existing_source_shm = true
bind_cpu_id = 16

[[bundle.sources]]
source_id = 1
data_session_config = "config/data_sessions/gate_data_session_30symbols_private_plain_20260604.toml"
data_session_name = "gate_source_1"
book_ticker_shm_name = "aquila_gate_book_ticker_src_1"
book_ticker_channel_name = "book_ticker_channel"
remove_existing_source_shm = true
bind_cpu_id = 17

[[bundle.sources]]
source_id = 2
data_session_config = "config/data_sessions/gate_data_session_30symbols_private_plain_20260604.toml"
data_session_name = "gate_source_2"
book_ticker_shm_name = "aquila_gate_book_ticker_src_2"
book_ticker_channel_name = "book_ticker_channel"
remove_existing_source_shm = true
bind_cpu_id = 18

[[bundle.sources]]
source_id = 3
data_session_config = "config/data_sessions/gate_data_session_30symbols_private_plain_20260604.toml"
data_session_name = "gate_source_3"
book_ticker_shm_name = "aquila_gate_book_ticker_src_3"
book_ticker_channel_name = "book_ticker_channel"
remove_existing_source_shm = true
bind_cpu_id = 19
```

parser 输出结构：

```cpp
struct BookTickerFusionBundleSourceConfig {
  std::int32_t source_id{-1};
  std::filesystem::path data_session_config;
  std::string data_session_name;
  std::string book_ticker_shm_name;
  std::string book_ticker_channel_name{"book_ticker_channel"};
  bool remove_existing_source_shm{true};
  std::int32_t bind_cpu_id{-1};
};

struct BookTickerFusionBundleConfig {
  std::string name;
  std::filesystem::path fusion_config;
  std::vector<BookTickerFusionBundleSourceConfig> sources;
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

## V1 实施任务

### Task 1: Bundle Config Parser

**Files:**
- Create: `tools/market_data/book_ticker_fusion_bundle_config.h`
- Create: `tools/market_data/book_ticker_fusion_bundle_config.cpp`
- Create: `test/tools/market_data/book_ticker_fusion_bundle_config_test.cpp`
- Modify: `test/tools/market_data/CMakeLists.txt`

- [ ] Step 1: 写失败测试 `ParsesFourSourceBundle`
  - 构造包含 `[bundle]` 和四个 `[[bundle.sources]]` 的 TOML。
  - 验证 `fusion_config`、`source_id`、`data_session_config`、`book_ticker_shm_name` 和 `bind_cpu_id`。
  - 运行：
    ```bash
    cmake --build build/debug --target book_ticker_fusion_bundle_config_test -j8
    ./build/debug/test/tools/market_data/book_ticker_fusion_bundle_config_test
    ```
  - 预期：新增实现前编译失败或测试失败。

- [ ] Step 2: 实现 parser 和校验
  - 空 `bundle.name`、空 `bundle.fusion_config`、空 sources、重复 `source_id`、负数 `source_id`、空 `data_session_config`、空 `book_ticker_shm_name` 都返回 `ok=false`。
  - `book_ticker_channel_name` 缺省为 `book_ticker_channel`。

- [ ] Step 3: 跑 parser 测试并提交
  ```bash
  cmake --build build/debug --target book_ticker_fusion_bundle_config_test -j8
  ./build/debug/test/tools/market_data/book_ticker_fusion_bundle_config_test
  git diff --check
  git add tools/market_data/book_ticker_fusion_bundle_config.* \
      test/tools/market_data/book_ticker_fusion_bundle_config_test.cpp \
      test/tools/market_data/CMakeLists.txt
  git commit -m "Add fusion bundle config parser"
  ```

### Task 2: Fusion Thread Wrapper

**Files:**
- Create: `tools/market_data/book_ticker_fusion_thread.h`
- Create: `test/tools/market_data/book_ticker_fusion_thread_test.cpp`
- Modify: `test/tools/market_data/CMakeLists.txt`

- [ ] Step 1: 写失败测试 `FusionThreadPublishesAndStops`
  - 用 `/home/liuxiang/tmp` 下的唯一 SHM name 创建一个 source publisher。
  - source 写入 `BookTicker{id=100, symbol_id=1}`。
  - fusion thread 读取 source SHM，输出 canonical SHM 和 metadata bin。
  - stop 后验证 canonical reader 能读到 id `100`，metadata 文件大小等于 `sizeof(FusionMetadataRecord)`。

- [ ] Step 2: 实现 `BookTickerFusionThread`
  - 构造时接收 `BookTickerFusionConfig`。
  - `Start()` 创建线程，线程内构造 `BookTickerFusionRunner` 并循环 `PollOnce()`。
  - `Stop()` 设置 atomic stop flag。
  - `Join()` join thread，并返回 final stats。
  - loop 在 `read_count == 0` 时调用 `std::this_thread::yield()`。

- [ ] Step 3: 跑 fusion thread 测试并提交
  ```bash
  cmake --build build/debug --target book_ticker_fusion_thread_test -j8
  ./build/debug/test/tools/market_data/book_ticker_fusion_thread_test
  git diff --check
  git add tools/market_data/book_ticker_fusion_thread.h \
      test/tools/market_data/book_ticker_fusion_thread_test.cpp \
      test/tools/market_data/CMakeLists.txt
  git commit -m "Add fusion thread runner"
  ```

### Task 3: Gate Bundle Tool

**Files:**
- Create: `tools/market_data/gate_book_ticker_fusion_bundle.cpp`
- Modify: `tools/CMakeLists.txt`
- Create: `config/market_data_fusion/gate_book_ticker_fusion_bundle_4sources.toml`

- [ ] Step 1: 写 dry-run 行为
  - CLI 支持：
    ```text
    --config <path>
    --connect
    --max-runtime-ms <0 means unlimited>
    ```
  - 不传 `--connect` 时只解析 bundle config、Gate data session configs 和 fusion config，打印 source / fusion 摘要后退出。

- [ ] Step 2: 实现 Gate source override
  - 复用 `aquila::gate::ParseDataSessionConfig()`。
  - 所有 source 的 `connection.enable_tls` 必须一致；不一致时返回配置错误。
  - 根据一致的 `enable_tls` 选择 `DefaultPlainWebSocketPolicy` 或 `DefaultTlsWebSocketPolicy`。
  - worker thread 调用 `session.Start()`，不调用 `session.Run()`。

- [ ] Step 3: 增加 CMake target 并验证 dry-run
  ```bash
  cmake --build build/debug --target gate_book_ticker_fusion_bundle -j8
  ./build/debug/tools/gate_book_ticker_fusion_bundle \
    --config config/market_data_fusion/gate_book_ticker_fusion_bundle_4sources.toml
  ```
  - 预期：不连接网络，输出 `result=ok connect=false source_count=4`。

- [ ] Step 4: 提交 Gate bundle tool
  ```bash
  git diff --check
  git add tools/market_data/gate_book_ticker_fusion_bundle.cpp \
      tools/CMakeLists.txt \
      config/market_data_fusion/gate_book_ticker_fusion_bundle_4sources.toml
  git commit -m "Add Gate fusion bundle tool"
  ```

### Task 4: Binance Bundle Tool

**Files:**
- Create: `tools/market_data/binance_book_ticker_fusion_bundle.cpp`
- Modify: `tools/CMakeLists.txt`
- Create: `config/market_data_fusion/binance_book_ticker_fusion_bundle_4sources.toml`

- [ ] Step 1: 复用 Gate bundle 的 supervisor 形状
  - 复用 shared bundle config parser 和 fusion thread wrapper。
  - 使用 `aquila::binance::ParseDataSessionConfig()`。
  - 所有 source 的 `connection.enable_tls` 必须一致；Binance 示例配置应为 TLS。

- [ ] Step 2: 增加 CMake target 并验证 dry-run
  ```bash
  cmake --build build/debug --target binance_book_ticker_fusion_bundle -j8
  ./build/debug/tools/binance_book_ticker_fusion_bundle \
    --config config/market_data_fusion/binance_book_ticker_fusion_bundle_4sources.toml
  ```
  - 预期：不连接网络，输出 `result=ok connect=false source_count=4`。

- [ ] Step 3: 提交 Binance bundle tool
  ```bash
  git diff --check
  git add tools/market_data/binance_book_ticker_fusion_bundle.cpp \
      tools/CMakeLists.txt \
      config/market_data_fusion/binance_book_ticker_fusion_bundle_4sources.toml
  git commit -m "Add Binance fusion bundle tool"
  ```

### Task 5: Focused Verification

**Files:**
- Modify: `docs/gate_fastest_route_fusion_threaded_bundle_plan.md`
- Modify: `docs/project_onboarding_guide.md`

- [ ] Step 1: 跑 focused C++ tests
  ```bash
  ctest --test-dir build/debug -R '(book_ticker_fusion|core_market_data_book_ticker_fusion)' --output-on-failure
  ```

- [ ] Step 2: 跑 Python analyzer test
  ```bash
  /home/liuxiang/dev/pyenv/lx/bin/python scripts/test/market_data/analyze_book_ticker_fusion_latency_test.py
  ```

- [ ] Step 3: 跑文档和 diff 检查
  ```bash
  git diff --check
  ```

- [ ] Step 4: 提交文档同步
  ```bash
  git add docs/gate_fastest_route_fusion_threaded_bundle_plan.md \
      docs/project_onboarding_guide.md
  git commit -m "Document threaded fusion bundle"
  ```

## V1 Shadow 验证计划

V1 threaded bundle 做 shadow 时，不接策略。运行目录写入 `/home/liuxiang/tmp/<run_id>/`，默认使用 `16-31` 测试 core。

最小 30-symbol Gate shadow：

```bash
timeout --kill-after=10s 1860s \
  ./build/release/tools/gate_book_ticker_fusion_bundle \
  --config config/market_data_fusion/gate_book_ticker_fusion_bundle_4sources.toml \
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
