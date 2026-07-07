# Market Data Fusion 配置与工具层统一设计

## 目标

在第一阶段已经统一 `core/market_data` fusion metadata / runner 公共层的基础上，继续把 BookTicker / Trade
在配置解析、工具 support 和编译期开关上的重复实现收敛为 feed-neutral 接口。本轮采用激进迁移：不保留旧
C++ helper 名称；如旧外部命名阻碍统一，也同步迁移到最新 feed-neutral 命名，并更新测试和文档。

## 当前事实

- `core/config/book_ticker_fusion_config.cpp` 和 `core/config/trade_fusion_config.cpp` 基本完全重复，差异只在
  result/config/source 类型、默认 channel、public 函数名和错误前缀。
- `tools/market_data/data_fusion_tool_support.h` 中 metadata output formatting、source lookup、alignment
  validation、source override、dry-run log、run summary log 都各有 BookTicker / Trade 两套实现。
- `tools/market_data/book_ticker_fusion_cli.cpp` 和 `tools/market_data/trade_fusion_cli.cpp` 仍是 standalone
  fusion CLI 主流程的两份近似拷贝，差异主要是 config parser、runner 和 poll stats 类型。
- Gate / Binance `data_fusion` 工具当前在 feed 分支中分别调用 `ValidateBookTickerFusionAlignment()` /
  `ValidateTradeFusionAlignment()`、`ApplyBookTickerSourceOverride()` / `ApplyTradeSourceOverride()` 等旧 helper。
- BookTicker / Trade 已共用 `FusionMetadataRecord`，但编译期开关仍叫
  `AQUILA_BOOK_TICKER_FUSION_METADATA_MODE` / `AQUILA_BOOK_TICKER_FUSION_METADATA_ENABLED`，
  代码常量仍叫 `kBookTickerFusionMetadataEnabled`。
- 本轮目标是冷路径和工具层去重；runner `PollOnce()`、fusion decision、source scan 顺序、drop / publish
  语义和 SHM ABI 不作为重构对象。

## Locked Decisions

- C++ 内部 helper 统一为 feed-neutral 名称，旧的 BookTicker / Trade helper 名称直接删除，所有调用点同步迁移。
- 编译期开关统一为 `AQUILA_FUSION_METADATA_MODE=file|off`，导出的 compile definition 统一为
  `AQUILA_FUSION_METADATA_ENABLED`，C++ 常量统一为 `aquila::kFusionMetadataEnabled`。
- 新 header 使用 `core/common/fusion_metadata_mode.h`；旧 `core/common/book_ticker_fusion_metadata_mode.h`
  不再作为兼容入口保留。
- TOML schema 中 `[fusion]`、`[fusion.output]`、`[[fusion.sources]]` 结构和字段名继续保持不变，因为这些字段已经
  是 feed-neutral；只保留 feed-specific 默认 channel：BookTicker 默认 `book_ticker_channel`，Trade 默认
  `trade_channel`。
- tool support 使用 feed traits 选择 launch source 上的 `book_ticker_*` 或 `trade_*` 字段，不在调用点保留两套
  validate / override / log 函数。
- dry-run / summary 日志统一带 `feed=<name>`，包括 BookTicker；这是可接受的日志字段变更。
- 不在热路径引入虚函数、type erasure、动态分配、mutex、condition variable 或额外 per-record 日志。
- 性能结论必须由现有 fusion benchmark 的改前/改后结果支撑；本轮主要是冷路径，但仍要验证 core / runner
  benchmark 没有可复现退化。

## Scope

本轮包含：

- 新增 `core/config/fusion_config_parser.h`，用模板 parser 处理公共 TOML schema。
- 缩减 `core/config/book_ticker_fusion_config.cpp` 和 `core/config/trade_fusion_config.cpp` 为 feed traits +
  public wrapper。
- 新增或重写 `core/common/fusion_metadata_mode.h`，同步 CMake cache option、compile definition、测试和文档。
- 用一组 generic helper 替换 `data_fusion_tool_support.h` 中重复的 BookTicker / Trade helper。
- 修改 Gate / Binance `data_fusion` 调用点使用 generic helper。
- 新增 `tools/market_data/fusion_cli.h`，把 standalone `book_ticker_fusion` / `trade_fusion` CLI 主流程收敛到
  shared traits/helper；原 `RunBookTickerFusionCli()` / `RunTradeFusionCli()` 只保留薄 wrapper。
- 更新 `test/config/*_fusion_config_test.cpp`、`test/tools/market_data/data_fusion_tool_support_test.cpp`、
  `test/tools/market_data/fusion_cli_traits_test.cpp`、`test/core/common/*metadata_mode*_test.cpp` 和必要的
  CLI / runner / thread 测试 include / macro。
- 同步 `docs/diagnostic_fields.md`、`docs/project_onboarding_guide.md`、
  `docs/lead_lag_live_operations_pipeline.md`、fusion design 文档中旧开关名和旧 helper 说明。

本轮不包含：

- 改变 `BookTickerFusionConfig` / `TradeFusionConfig` 对外结构体名。
- 合并 `BookTickerFusionThread` / `TradeFusionThread` 或 `BookTickerFusionRunner` / `TradeFusionRunner` public 类型。
- 改变 existing TOML file 的字段结构、source 数量、SHM 名称、channel 名称或 feed 选择语义。
- 改变 standalone `book_ticker_fusion` / `trade_fusion` CLI binary 的命令行参数。
- 删除历史计划文档中的旧名称；已归档 plan/spec 可以保留历史事实。

## 目标接口

编译期开关：

```cmake
set(AQUILA_FUSION_METADATA_MODE "file"
    CACHE STRING
    "Build market data fusion sidecar metadata mode: file or off")

target_compile_definitions(aquila_core
    PUBLIC
        AQUILA_FUSION_METADATA_ENABLED=${AQUILA_FUSION_METADATA_ENABLED}
)
```

C++ 常量：

```cpp
#include "core/common/fusion_metadata_mode.h"

namespace aquila {

inline constexpr bool kFusionMetadataEnabled =
    AQUILA_FUSION_METADATA_ENABLED != 0;

}  // namespace aquila
```

config parser traits：

```cpp
struct BookTickerFusionConfigParseTraits {
  using Config = aquila::market_data::BookTickerFusionConfig;
  using SourceConfig = aquila::market_data::BookTickerFusionSourceConfig;
  using Result = BookTickerFusionConfigResult;

  static constexpr std::string_view kDefaultSourceChannel =
      "book_ticker_channel";
  static constexpr std::string_view kLoadErrorPrefix =
      "failed to load fusion config: ";
};

struct TradeFusionConfigParseTraits {
  using Config = aquila::market_data::TradeFusionConfig;
  using SourceConfig = aquila::market_data::TradeFusionSourceConfig;
  using Result = TradeFusionConfigResult;

  static constexpr std::string_view kDefaultSourceChannel = "trade_channel";
  static constexpr std::string_view kLoadErrorPrefix =
      "failed to load trade fusion config: ";
};
```

tool support traits：

```cpp
struct BookTickerDataFusionFeedTraits {
  using FusionConfig = aquila::market_data::BookTickerFusionConfig;
  using FusionThreadStats = aquila::market_data::BookTickerFusionThreadStats;

  static constexpr DataFusionFeed kFeed = DataFusionFeed::kBookTicker;

  static std::string_view LaunchShmName(const auto& source) {
    return source.book_ticker_shm_name;
  }
  static std::string_view LaunchChannelName(const auto& source) {
    return source.book_ticker_channel_name;
  }
  static auto& DataSessionShm(auto& config) { return config.book_ticker_shm; }
  static void SelectFeed(auto& feeds) {
    feeds.book_ticker = true;
    feeds.trade = false;
  }
  static std::uint64_t PublishedCount(const auto& publisher) {
    return publisher.published_count();
  }
};
```

Trade traits 使用同一接口，字段切换到 `trade_shm_name`、`trade_channel_name`、`trade_shm`、
`published_trades()`，并选择 `feeds.trade = true`。

替换后的 helper 形态：

```cpp
template <typename FeedTraits>
std::string FormatFusionMetadataOutput(
    const typename FeedTraits::FusionConfig& fusion_config);

template <typename FeedTraits>
const typename FeedTraits::FusionConfig::SourceConfig* FindFusionSource(
    const typename FeedTraits::FusionConfig& fusion_config,
    std::int32_t source_id);

template <typename FeedTraits, typename LaunchConfig>
bool ValidateFusionAlignment(const LaunchConfig& launch_config,
                             const typename FeedTraits::FusionConfig& config,
                             std::string* error);

template <typename FeedTraits, typename SourceConfig,
          typename DataSessionConfig>
void ApplyFusionSourceOverride(const SourceConfig& source,
                               DataSessionConfig* data_session_config);

template <typename FeedTraits, typename LaunchConfig, typename PreparedSources>
void LogDataFusionDryRun(const LaunchConfig& launch_config,
                         const typename FeedTraits::FusionConfig& config,
                         const PreparedSources& sources);

template <typename FeedTraits>
void LogDataFusionRunSummary(std::string_view launch_name,
                             std::size_t source_count,
                             std::uint64_t source_published_count,
                             const typename FeedTraits::FusionThreadStats& stats);
```

## TDD 验收点

先写失败测试，再改生产代码：

- `FusionMetadataModeTest` 改为 include `core/common/fusion_metadata_mode.h`，断言
  `kFusionMetadataEnabled` 和 `AQUILA_FUSION_METADATA_ENABLED`；旧 header / macro 不再出现在 active code。
- `BookTickerFusionConfigTest` 和 `TradeFusionConfigTest` 继续覆盖默认 channel、duplicate source id、empty sources、
  metadata required/off 行为；改动后两套 public wrapper 行为不变。
- `DataFusionToolSupportTest` 改为只调用 `ValidateFusionAlignment<BookTickerDataFusionFeedTraits>()` /
  `ValidateFusionAlignment<TradeDataFusionFeedTraits>()` 等 generic helper，旧 helper 名称不再编译。
- Gate / Binance `data_fusion` CLI dry-run 测试或最小工具层测试需要覆盖 BookTicker dry-run 也带 `feed=book_ticker`
  的新日志字段。

## 验证

最小功能验证：

```bash
ctest --test-dir build/debug -R '(fusion_config|data_fusion_tool_support|fusion_metadata_mode|book_ticker_fusion|trade_fusion)' --output-on-failure
```

构建 / 静态检查：

```bash
git diff --check
rg 'book_ticker_fusion_metadata_mode|AQUILA_BOOK_TICKER_FUSION_METADATA|kBookTickerFusionMetadataEnabled|ValidateBookTickerFusionAlignment|ValidateTradeFusionAlignment|ApplyBookTickerSourceOverride|ApplyTradeSourceOverride|LogBookTickerDataFusion|LogTradeDataFusion' core tools test benchmark docs --glob '!docs/superpowers/**'
```

上述 `rg` 期望 active code / active docs 无命中；归档在 `docs/superpowers/**` 的历史计划不作为阻断。

性能验证：

```bash
cmake --build build/release --target fastest_route_fusion_benchmark -j
/home/liuxiang/dev/aquila/build/release/benchmark/core/market_data/fastest_route_fusion_benchmark \
  --benchmark_repetitions=5 \
  --benchmark_format=json \
  --benchmark_out=/home/liuxiang/tmp/fusion_refactor_perf/config_tooling_unification.json
```

与提交 `5d4fa07` 后的现有 benchmark JSON 对比；如果 core / runner 路径出现可复现退化，先修正实现再提交。
