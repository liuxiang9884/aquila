# Gate / Binance 多路最快行情融合第一版实施计划

> 本计划最初用于实现 `gate_book_ticker_fusion` 第一版；当前同一 runner 已复用到 `binance_book_ticker_fusion`。
> 执行时优先使用 `superpowers:test-driven-development`，每个核心模块先写失败测试，再写最小实现。
> 当前不使用 subagent 并行改共享 C++ 接口，避免 ABI、CMake 和测试边界返工。

## Goal

实现一个可配置 N 路 BBO fusion 工具，首轮配置 `N=4`：读取 N 条 source `BookTicker` SHM，
按 `(symbol_id, BookTicker.id)` 单调推进输出一条 canonical `BookTicker` SHM，并写 sidecar metadata bin
支持离线 latency attribution。当前入口包括 `gate_book_ticker_fusion` 和 `binance_book_ticker_fusion`。

## Architecture

第一版采用独立进程架构：N 个 exchange data session process 分别写 source SHM，
exchange-specific fusion process 单 hot thread round-robin 读取 source SHM，输出 canonical SHM。
canonical SHM 的 ABI 与现有 data session SHM 完全一致，`BookTicker.local_ns` 在 fusion 输出中表示
`fusion_publish_ns`。winner attribution 不写进 SHM，而写入 sidecar metadata binary。

## 当前实现状态

截至 2026-06-14，Task 1-5 已分提交落地：fusion core、metadata writer、TOML config parser、`gate_book_ticker_fusion` tool、4-source fusion 示例配置和 5 个 recorder 配置均已实现。Task 6 实际复用并扩展仓库已有 `scripts/market_data/analyze_book_ticker_fusion_latency.py`，没有新增重名 analyzer；该脚本保留旧的 4 路 combination 模式，并新增读取 source / fusion `BookTicker` bin 与 sidecar metadata bin 的 published fusion 模式。后续同一 runner 已复用到 Binance，新增 `binance_book_ticker_fusion` 入口；两者共享 `(symbol_id, BookTicker.id)` first-arrival 语义和 sidecar metadata 分析方式。

2026-06-14 已完成一次 `BTC_USDT` / `ETH_USDT`、`N=4`、30 分钟 release shadow，运行目录为 `/home/liuxiang/tmp/20260614_051319_gate_fusion_btc_eth_4src_30m_release/`。结果显示 canonical fusion latency p99 `379.303us`、p99.9 `686.858us`，fusion hop p99 `1.029us`、p99.9 `1.312us`；fusion winner 与离线 source-bin fastest 差异 `65 / 57656`，约 `0.1127%`。详细结果见 `docs/gate_fastest_route_fusion_shadow_results.md`。

2026-06-14 后续又完成 Gate / Binance 各 30-symbol、`N=4`、30 分钟、release L4 shadow：

| exchange | run directory | fusion p99 | fusion p99.9 | fusion hop p99 | `fusion > 5ms` |
| --- | --- | ---: | ---: | ---: | ---: |
| Gate | `/home/liuxiang/tmp/20260614_133704_gate_fusion_30symbols_4src_30m_l4_outlier_release/` | `0.259ms` | `0.301ms` | `0.794us` | `3 / 1,075,552` |
| Binance | `/home/liuxiang/tmp/20260614_133704_binance_fusion_30symbols_4src_30m_l4_outlier_release/` | `1.754ms` | `1.926ms` | `1.395us` | `0 / 3,367,135` |

对最佳单路 source，Gate fusion p99 改善 `31.71%`、p99.9 改善 `91.97%`；Binance fusion p99 改善
`7.79%`、p99.9 改善 `39.75%`。Gate L4 attribution 显示 `>5ms` source outlier 主导阶段是
`exchange_ns -> kernel_rx_ns`，不是本机 kernel queue、parser 或 SHM publish。Binance 因 TLS 暂时不能
拆出 `kernel_rx_ns`，但可排除 parser / SHM publish 是主要来源。详细结果见
`docs/gate_fastest_route_fusion_shadow_results.md`。

Task 7 已完成文档和 onboarding 同步；第一版实现不再缺核心代码。后续工作是多轮 shadow 复测、评估 L4
outlier logging 扰动、确认 Binance `source_3` 这类 source 异常是否稳定，并设计策略切换到 canonical SHM
的安全流程。

## Tech Stack

- C++20 / CMake
- `core/market_data/data_shm.h` 的 `BookTickerShmReader` 和 `DataShmPublisher`
- `core/config/data_reader_config.*` 既有 source TOML 风格作为参考
- `CLI11`、`toml++`、`fmtlib`、`GTest`
- Python analyzer 使用 `/home/liuxiang/dev/pyenv/lx/bin/python`

---

## 文件结构

| 文件 | 动作 | 责任 |
| --- | --- | --- |
| `core/market_data/book_ticker_fusion.h` | 新增 | Header-only fusion core：per-symbol state、accept/drop 决策、canonical ticker 生成 |
| `test/core/market_data/book_ticker_fusion_test.cpp` | 新增 | 验证 `id` 单调推进、drop 旧 id、`local_ns` 改写、不同 symbol 独立 |
| `tools/market_data/book_ticker_fusion_metadata.h` | 新增 | sidecar metadata record 和 binary writer |
| `test/tools/market_data/book_ticker_fusion_metadata_test.cpp` | 新增 | 验证 metadata 二进制写入和 flush |
| `tools/market_data/book_ticker_fusion_config.h` | 新增 | fusion TOML config 结构 |
| `tools/market_data/book_ticker_fusion_config.cpp` | 新增 | fusion TOML parser |
| `test/tools/market_data/book_ticker_fusion_config_test.cpp` | 新增 | 验证 4 source config、重复 source id、缺 output、空 source |
| `tools/market_data/book_ticker_fusion.cpp` | 新增 | CLI tool：打开 N 路 reader、canonical publisher、metadata writer，运行 fusion loop |
| `tools/market_data/binance_book_ticker_fusion.cpp` | 新增 | Binance CLI tool：复用通用 fusion runner，默认加载 Binance 4-source 示例配置 |
| `tools/CMakeLists.txt` | 修改 | 增加 `gate_book_ticker_fusion` / `binance_book_ticker_fusion` target |
| `test/core/market_data/CMakeLists.txt` | 修改 | 增加 core fusion test target |
| `test/tools/market_data/CMakeLists.txt` | 修改 | 增加 config / metadata test target |
| `config/market_data_fusion/gate_book_ticker_fusion_4sources.toml` | 新增 | 4-source 示例配置 |
| `config/market_data_fusion/binance_book_ticker_fusion_4sources.toml` | 新增 | Binance 4-source 示例配置 |
| `scripts/market_data/analyze_book_ticker_fusion_latency.py` | 修改 | 离线读取 source / fusion BookTicker bin 和 metadata bin，输出 latency summary；同时保留旧 4 路 combination 模式 |
| `scripts/test/market_data/analyze_book_ticker_fusion_latency_test.py` | 修改 | Python analyzer 单元测试 |
| `docs/gate_fastest_route_fusion_design.md` | 修改 | 同步最终第一版设计和验证方式 |

## Task 1: Fusion Core

**Files:**
- Create: `core/market_data/book_ticker_fusion.h`
- Create: `test/core/market_data/book_ticker_fusion_test.cpp`
- Modify: `test/core/market_data/CMakeLists.txt`

- [ ] Step 1: 写失败测试 `PublishesOnlyIncreasingIdsPerSymbol`
  - 输入同一 symbol 的 id `100`、`100`、`99`、`101`。
  - 期望只 publish `100` 和 `101`。
  - 期望 publish ticker 的 `local_ns` 等于传入的 `fusion_publish_ns`。
  - 运行：
    ```bash
    cmake --build build/debug --target core_market_data_book_ticker_fusion_test -j8
    ./build/debug/test/core/market_data/core_market_data_book_ticker_fusion_test
    ```
  - 预期：编译失败或测试失败，因为 header / target 尚不存在。

- [ ] Step 2: 实现 `BookTickerFusionCore`
  - API 建议：
    ```cpp
    namespace aquila::market_data {

    struct BookTickerFusionDecision {
      bool publish{false};
      std::int32_t source_id{-1};
      std::int32_t symbol_id{-1};
      std::int64_t book_ticker_id{0};
      std::int64_t source_local_ns{0};
      std::int64_t fusion_publish_ns{0};
      BookTicker ticker{};
    };

    class BookTickerFusionCore {
     public:
      explicit BookTickerFusionCore(std::size_t max_symbol_id);
      [[nodiscard]] BookTickerFusionDecision OnBookTicker(
          std::int32_t source_id, const BookTicker& ticker,
          std::int64_t fusion_publish_ns) noexcept;
    };

    }  // namespace aquila::market_data
    ```
  - 内部 `std::vector<SymbolFusionState>`，`symbol_id` 越界时 drop。
  - 不抛异常、不分配 hot path 内存。

- [ ] Step 3: 跑 core fusion 测试并修到通过
  ```bash
  cmake --build build/debug --target core_market_data_book_ticker_fusion_test -j8
  ./build/debug/test/core/market_data/core_market_data_book_ticker_fusion_test
  ```

- [ ] Step 4: 增加测试 `MaintainsIndependentStatePerSymbol`
  - symbol 1 publish id `10` 后，symbol 2 的 id `5` 仍可 publish。
  - symbol 1 的 id `9` drop。

- [ ] Step 5: 再跑 core fusion 测试

## Task 2: Sidecar Metadata Writer

**Files:**
- Create: `tools/market_data/book_ticker_fusion_metadata.h`
- Create: `test/tools/market_data/book_ticker_fusion_metadata_test.cpp`
- Modify: `test/tools/market_data/CMakeLists.txt`

- [ ] Step 1: 写失败测试 `WritesFixedSizeMetadataRecords`
  - 创建临时文件 `/home/liuxiang/tmp/aquila_fusion_metadata_test_<pid>.bin`。
  - 写两条 `FusionMetadataRecord`。
  - 读取文件，验证大小为 `2 * sizeof(FusionMetadataRecord)`，字段逐项一致。

- [ ] Step 2: 实现 metadata record 和 writer
  - record 使用 fixed-size trivially copyable struct：
    ```cpp
    struct FusionMetadataRecord {
      std::int32_t source_id;
      std::int32_t symbol_id;
      std::int64_t book_ticker_id;
      std::int64_t exchange_ns;
      std::int64_t source_local_ns;
      std::int64_t fusion_publish_ns;
    };
    ```
  - writer 构造时创建 parent dir，二进制 truncate 打开。
  - `Write(record)` 只写 binary，不格式化。
  - `Flush()` 返回 bool。

- [ ] Step 3: 跑 metadata 测试
  ```bash
  cmake --build build/debug --target book_ticker_fusion_metadata_test -j8
  ./build/debug/test/tools/market_data/book_ticker_fusion_metadata_test
  ```

## Task 3: Fusion Config Parser

**Files:**
- Create: `tools/market_data/book_ticker_fusion_config.h`
- Create: `tools/market_data/book_ticker_fusion_config.cpp`
- Create: `test/tools/market_data/book_ticker_fusion_config_test.cpp`
- Modify: `test/tools/market_data/CMakeLists.txt`

- [ ] Step 1: 写失败测试 `ParsesFourSources`
  - TOML 包含 `[fusion]`、`[fusion.output]` 和 4 个 `[[fusion.sources]]`。
  - 验证 `max_events_per_source=1`、`bind_cpu_id=16`、4 个 source id 和 SHM name。

- [ ] Step 2: 写失败测试 `RejectsDuplicateSourceId`
  - 两个 source 使用同一个 `source_id`。
  - 期望 `result.ok == false`，error 包含 `source_id`。

- [ ] Step 3: 实现 parser
  - config 结构：
    ```cpp
    struct BookTickerFusionSourceConfig {
      std::int32_t source_id{-1};
      std::string name;
      std::string shm_name;
      std::string channel_name;
    };

    struct BookTickerFusionOutputConfig {
      std::string shm_name;
      std::string channel_name;
      bool remove_existing{false};
      std::filesystem::path metadata_bin;
    };

    struct BookTickerFusionConfig {
      std::string name;
      std::uint32_t max_events_per_source{1};
      std::int32_t bind_cpu_id{-1};
      std::uint32_t max_symbol_id{4096};
      BookTickerFusionOutputConfig output;
      std::vector<BookTickerFusionSourceConfig> sources;
    };
    ```
  - reject 空 sources、缺 output SHM、缺 metadata bin、重复 source id、`max_events_per_source <= 0`。

- [ ] Step 4: 跑 config 测试
  ```bash
  cmake --build build/debug --target book_ticker_fusion_config_test -j8
  ./build/debug/test/tools/market_data/book_ticker_fusion_config_test
  ```

## Task 4: Fusion Tool

**Files:**
- Create: `tools/market_data/book_ticker_fusion.cpp`
- Modify: `tools/CMakeLists.txt`

- [ ] Step 1: 为 tool 设计可退出 smoke
  - CLI 必须支持 `--max-polls`，`--max-polls 1` 在没有可读数据时也能完成一次 loop 后退出。
  - 本 task 不启动真实 Gate connection；真实 source SHM shadow 放到最后的 Shadow Run Checklist。

- [ ] Step 2: 实现 CLI
  - 参数：
    ```text
    --config <path>
    --max-polls <uint64, default 0>
    ```
  - 读取 TOML，打开 source readers 和 output publisher。
  - `SIGINT/SIGTERM` 设置 stop flag。
  - loop 按 source 顺序读取最多 `max_events_per_source` 条；accepted 时 publish canonical ticker 并写 metadata。
  - 无事件时 `std::this_thread::yield()`。

- [ ] Step 3: 支持 heartbeat / summary log
  - 冷路径周期性 `FlushPublishedCount()` / metadata `Flush()`。
  - 退出时输出 source count、published count、metadata path。

- [ ] Step 4: build target
  ```bash
  cmake --build build/debug --target gate_book_ticker_fusion -j8
  ```

## Task 5: 示例配置和 Recorder 配置

**Files:**
- Create: `config/market_data_fusion/gate_book_ticker_fusion_4sources.toml`
- Create: `config/data_readers/gate_book_ticker_fusion_4sources_source0_recorder.toml`
- Create: `config/data_readers/gate_book_ticker_fusion_4sources_source1_recorder.toml`
- Create: `config/data_readers/gate_book_ticker_fusion_4sources_source2_recorder.toml`
- Create: `config/data_readers/gate_book_ticker_fusion_4sources_source3_recorder.toml`
- Create: `config/data_readers/gate_book_ticker_fusion_canonical_recorder.toml`

- [ ] Step 1: 写 4-source fusion 示例配置
  - source SHM names：
    ```text
    aquila_gate_book_ticker_src_0
    aquila_gate_book_ticker_src_1
    aquila_gate_book_ticker_src_2
    aquila_gate_book_ticker_src_3
    ```
  - canonical SHM:
    ```text
    aquila_gate_book_ticker_fusion
    ```
  - metadata:
    ```text
    /home/liuxiang/tmp/gate_book_ticker_fusion_4sources/fusion_metadata.bin
    ```

- [ ] Step 2: 写 5 个 recorder config
  - 每个 recorder config 只有一个 SHM source，`read_mode = "drain"`，`start_position = "latest"`。
  - output 由 recorder CLI `--output` 指定，不写死。

- [ ] Step 3: 用 config parser smoke 验证
  ```bash
  ./build/debug/tools/data_reader_recorder --config config/data_readers/gate_book_ticker_fusion_canonical_recorder.toml --output /home/liuxiang/tmp/fusion_recorder_smoke.bin --max-polls 1
  ```
  - 如果 SHM 不存在，预期失败在 attach 阶段；parser 不能失败。

## Task 6: 离线 Analyzer

**Files:**
- Modify: `scripts/market_data/analyze_book_ticker_fusion_latency.py`
- Modify: `scripts/test/market_data/analyze_book_ticker_fusion_latency_test.py`

- [ ] Step 1: 写 Python 测试
  - 用 `struct.pack` 写小型 source bin、fusion bin、metadata bin。
  - 验证输出 summary 包含：
    ```text
    source_latency_ns
    fusion_latency_ns
    fusion_hop_ns
    winner_ratio
    ```

- [ ] Step 2: 实现 analyzer
  - 读取 `BookTicker` binary record，格式必须和 C++ `BookTicker` layout 一致。
  - 读取 metadata binary record。
  - 按 source_id 聚合 p50 / p95 / p99 / max。
  - published mode 参数：
    ```text
    --source-bin SOURCE_ID=PATH
    --fusion-bin PATH
    --metadata-bin PATH
    ```
  - 输出 JSON summary，支持 `--summary-output` 写 CSV summary、`--top-output` 写 fusion hop outlier CSV。

- [ ] Step 3: 跑 Python 测试
  ```bash
  /home/liuxiang/dev/pyenv/lx/bin/python scripts/test/market_data/analyze_book_ticker_fusion_latency_test.py
  ```

## Task 7: 文档和验证

**Files:**
- Modify: `docs/gate_fastest_route_fusion_design.md`
- Modify: `docs/project_onboarding_guide.md`（实现后若新增入口或验证命令，再同步）

- [ ] Step 1: 更新设计文档中的实现入口、配置路径、验证命令。
- [ ] Step 2: 运行 focused build / tests：
  ```bash
  ./build.sh debug
  ctest --test-dir build/debug -R '(book_ticker_fusion|core_market_data|data_reader_recorder)' --output-on-failure
  /home/liuxiang/dev/pyenv/lx/bin/python scripts/test/market_data/analyze_book_ticker_fusion_latency_test.py
  git diff --check
  ```
- [ ] Step 3: 提交剩余 analyzer / 文档同步：
  ```bash
  git add scripts docs
  git commit -m "Update Gate fusion analyzer"
  ```

## Shadow Run Checklist

首轮 live shadow 不接策略，所有临时产物写入 `/home/liuxiang/tmp/<run_id>/`，测试 / recorder / analyzer 默认放 `16-31` 测试 core。

1. 启动 4 路 Gate source data session，各自写独立 source SHM。
2. 启动 `gate_book_ticker_fusion`，输出 canonical SHM 和 `fusion_metadata.bin`。
3. 启动 5 个 `data_reader_recorder`，分别录 `source_0.bin` 到 `source_3.bin` 和 `fusion.bin`。
4. 运行 analyzer：
   ```bash
   /home/liuxiang/dev/pyenv/lx/bin/python scripts/market_data/analyze_book_ticker_fusion_latency.py \
     --source-bin 0=/home/liuxiang/tmp/<run_id>/source_0.bin \
     --source-bin 1=/home/liuxiang/tmp/<run_id>/source_1.bin \
     --source-bin 2=/home/liuxiang/tmp/<run_id>/source_2.bin \
     --source-bin 3=/home/liuxiang/tmp/<run_id>/source_3.bin \
     --fusion-bin /home/liuxiang/tmp/<run_id>/fusion.bin \
     --metadata-bin /home/liuxiang/tmp/<run_id>/fusion_metadata.bin \
     --output-dir /home/liuxiang/tmp/<run_id>/fusion_analysis
   ```
5. 只在 shadow 证据显示 canonical latency 改善且 fusion hop 可接受后，再讨论策略切换。
