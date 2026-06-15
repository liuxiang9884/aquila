# BookTicker Fusion Metadata Policy 实施计划

> **给 agentic worker:** 实施本计划时按任务逐项执行；每个任务完成后运行对应验证，再进入下一项。

**目标:** 增加编译期 metadata policy，使 fusion 可以在不改变 canonical SHM 输出的前提下移除 sidecar metadata 记录路径。

**架构:** 新增 CMake cache mode 和编译期 mode header。`BookTickerFusionRunner` 保持原 public 类型名，但内部使用 template policy；`file` build 写 `FusionMetadataRecord`，`off` build 不打开 writer，也不构造 metadata record。

**技术栈:** CMake、C++20、GoogleTest、现有 `BookTickerFusionRunner`、`FusionMetadataWriter`、TOML config parser。

---

### Task 1: 编译期开关

**文件:**

- 修改：`CMakeLists.txt`
- 修改：`core/CMakeLists.txt`
- 新增：`core/common/book_ticker_fusion_metadata_mode.h`
- 新增测试：`test/core/common/book_ticker_fusion_metadata_mode_test.cpp`
- 修改：`test/core/common/CMakeLists.txt`

步骤：

1. 新增 mode 测试，验证 `AQUILA_BOOK_TICKER_FUSION_METADATA_ENABLED` 只可能为 `0` 或 `1`，并暴露
   `aquila::kBookTickerFusionMetadataEnabled`。
2. 运行目标，确认缺少 header 时失败。
3. 在根 `CMakeLists.txt` 增加：

```cmake
set(AQUILA_BOOK_TICKER_FUSION_METADATA_MODE "file"
    CACHE STRING
    "Build BookTicker fusion sidecar metadata mode: file or off")
set_property(CACHE AQUILA_BOOK_TICKER_FUSION_METADATA_MODE PROPERTY STRINGS
             file off)
```

4. 将 mode 映射为 public compile definition：

```cmake
AQUILA_BOOK_TICKER_FUSION_METADATA_ENABLED=${AQUILA_BOOK_TICKER_FUSION_METADATA_ENABLED}
```

5. 新增 `core/common/book_ticker_fusion_metadata_mode.h`，提供编译期布尔常量。
6. 构建并运行：

```bash
cmake --build build/debug --target core_book_ticker_fusion_metadata_mode_test -j8
./build/debug/test/core/common/core_book_ticker_fusion_metadata_mode_test
```

### Task 2: Config Parser 行为

**文件:**

- 修改：`core/config/book_ticker_fusion_config.cpp`
- 修改：`test/config/book_ticker_fusion_config_test.cpp`

步骤：

1. 在 config 测试中增加按编译模式分支的 missing `metadata_bin` case。
2. 在 `AQUILA_BOOK_TICKER_FUSION_METADATA_ENABLED=1` 时，缺少 `fusion.output.metadata_bin` 必须失败。
3. 在 `AQUILA_BOOK_TICKER_FUSION_METADATA_ENABLED=0` 时，缺少 `fusion.output.metadata_bin` 必须成功，且
   `config.output.metadata_bin.empty()`。
4. 修改 parser：

```cpp
if constexpr (aquila::kBookTickerFusionMetadataEnabled) {
  const std::string metadata_bin =
      RequiredString(output["metadata_bin"], "fusion.output.metadata_bin");
  if (!ok_) {
    return;
  }
  config_.output.metadata_bin = metadata_bin;
} else {
  config_.output.metadata_bin =
      OptionalString(output["metadata_bin"], std::string{});
}
```

5. 分别运行默认 build 和 metadata-off build 的 `book_ticker_fusion_config_test`。

### Task 3: Metadata Policy 和 Runner

**文件:**

- 新增：`core/market_data/book_ticker_fusion_metadata_policy.h`
- 修改：`core/market_data/book_ticker_fusion_runner.h`
- 修改：`test/core/market_data/book_ticker_fusion_runner_test.cpp`
- 修改：`test/core/market_data/book_ticker_fusion_thread_test.cpp`

步骤：

1. 在 runner/thread 测试中增加按编译模式分支：
   - `file` build：继续校验 metadata 文件存在且大小正确。
   - `off` build：使用空 `metadata_bin`，canonical SHM 仍发布，metadata error count 为 `0`。
2. 运行 metadata-off build，确认当前实现会因空 metadata path 失败。
3. 新增两个 policy：
   - `FileBookTickerFusionMetadataPolicy`：持有 `FusionMetadataWriter`，写 `FusionMetadataRecord`。
   - `NoopBookTickerFusionMetadataPolicy`：不持有 writer，`Flush()` 返回 `true`。
4. 将 runner 改为：

```cpp
template <typename MetadataPolicy>
class BasicBookTickerFusionRunner { ... };

using BookTickerFusionRunner =
    BasicBookTickerFusionRunner<DefaultBookTickerFusionMetadataPolicy>;
```

5. 在 publish 分支使用 `if constexpr (MetadataPolicy::kEnabled)`，确保 off build 不构造
   `FusionMetadataRecord`。
6. 分别运行默认 build 和 metadata-off build 的 runner/thread 测试。

### Task 4: Tool Log 和文档

**文件:**

- 修改：`tools/market_data/data_fusion_tool_support.h`
- 修改：`tools/market_data/book_ticker_fusion_cli.cpp`
- 修改：`docs/diagnostic_fields.md`

步骤：

1. 在 dry-run / summary log 中增加 `metadata_enabled=true|false`。
2. metadata-off build 中输出 `metadata_output=disabled`，避免把空路径误读为文件写入失败。
3. 将 `book_ticker_fusion_cli.cpp` 的输出统一改为 Nova log。
4. 在 `docs/diagnostic_fields.md` 登记 `metadata_enabled`、`metadata_output` 和 metadata write error 字段。
5. 构建默认 build 和 metadata-off build 的 Gate/Binance fusion CLI 目标。

### 最终验证

默认 build：

```bash
git diff --check
cmake --build build/debug --target \
  core_book_ticker_fusion_metadata_mode_test \
  book_ticker_fusion_config_test \
  core_market_data_book_ticker_fusion_runner_test \
  core_market_data_book_ticker_fusion_thread_test \
  data_fusion_tool_support_test \
  gate_book_ticker_fusion \
  binance_book_ticker_fusion -j8
./build/debug/test/core/common/core_book_ticker_fusion_metadata_mode_test
./build/debug/test/config/book_ticker_fusion_config_test
./build/debug/test/core/market_data/core_market_data_book_ticker_fusion_runner_test
./build/debug/test/core/market_data/core_market_data_book_ticker_fusion_thread_test
./build/debug/test/tools/market_data/data_fusion_tool_support_test
```

metadata-off build：

```bash
cmake -S . -B /home/liuxiang/tmp/aquila_build_fusion_metadata_off \
  -DCMAKE_BUILD_TYPE=Debug \
  -DAQUILA_BOOK_TICKER_FUSION_METADATA_MODE=off
cmake --build /home/liuxiang/tmp/aquila_build_fusion_metadata_off --target \
  core_book_ticker_fusion_metadata_mode_test \
  book_ticker_fusion_config_test \
  core_market_data_book_ticker_fusion_runner_test \
  core_market_data_book_ticker_fusion_thread_test \
  data_fusion_tool_support_test \
  gate_book_ticker_fusion \
  binance_book_ticker_fusion -j8
/home/liuxiang/tmp/aquila_build_fusion_metadata_off/test/core/common/core_book_ticker_fusion_metadata_mode_test
/home/liuxiang/tmp/aquila_build_fusion_metadata_off/test/config/book_ticker_fusion_config_test
/home/liuxiang/tmp/aquila_build_fusion_metadata_off/test/core/market_data/core_market_data_book_ticker_fusion_runner_test
/home/liuxiang/tmp/aquila_build_fusion_metadata_off/test/core/market_data/core_market_data_book_ticker_fusion_thread_test
/home/liuxiang/tmp/aquila_build_fusion_metadata_off/test/tools/market_data/data_fusion_tool_support_test
```
