# DataReader Live Record Smoke 设计

## 背景

`RealtimeDataReader`、`HistoricalDataReader` 和 `data_reader_recorder` 已完成本地集成测试：
recorder 可以从 Gate / Binance `BookTicker` SHM 读取并写出一个裸 `aquila::BookTicker`
binary 文件。当前缺口不是 reader 接口扩展，而是一次可复现的 live record smoke：确认现有
Gate / Binance data session 写 SHM 后，recorder 能在真实行情窗口内写出可 replay 的 binary，并明确记录
`overruns` / `skipped` 边界。

本设计只覆盖 live smoke 和低风险工具化改进。trade / order book feed 扩展暂不进入本轮。

## 目标

本轮目标：

- 使用临时 `drain` data reader 配置执行 Gate / Binance `BookTicker` live record smoke；
- 输出裸 `aquila::BookTicker` binary 到 `/home/liuxiang/tmp`；
- 验证输出文件非空，且大小是 `sizeof(aquila::BookTicker)` 的整数倍；
- 使用 `HistoricalDataReader` 或等价现有 replay 入口确认输出 binary 可顺序读取；
- 记录 recorder 退出统计，包括 total records、per-exchange records、per-source `skipped` / `overruns`；
- 如需代码改动，只做 `tools/` 或 `scripts/` 级别的复现辅助，不修改 reader hot path。

## 非目标

本轮不做：

- trade feed 或 order book feed；
- `RealtimeDataReader` typed storage / unified scan table；
- `TradingRuntime` 热路径改造；
- 默认仓库 `config/data_readers/strategy_data_reader.toml` 的 read mode 变更；
- record binary header、索引文件或新 replay 文件格式；
- 性能收益声明。

## Smoke 流程

所有临时配置、输出 binary 和日志默认放在 `/home/liuxiang/tmp`。

1. 生成临时 data reader TOML。
   - 基于现有 Gate / Binance `BookTicker` SHM source。
   - `read_mode = "drain"`。
   - `start_position = "latest"` 用于从 recorder 启动后开始录制；需要补读可见窗口时再使用
     `earliest_visible`。
   - 不修改仓库默认 strategy data reader 配置。

2. 启动 Gate / Binance data session。
   - 使用现有 `gate_data_session` 和 `binance_data_session` 工具。
   - live smoke 需要外网可访问交易所 public market data endpoint。
   - 如果 data session 已在运行，可以复用现有 SHM producer。

3. 启动 `data_reader_recorder`。
   - 使用临时 `drain` 配置。
   - 输出到 `/home/liuxiang/tmp/<name>.bin`。
   - 短窗口 smoke 优先使用有限 `timeout` 或 `--max-polls`，避免无人值守长跑。

4. 验证 binary。
   - 检查文件存在且非空。
   - 检查文件大小能被 `sizeof(aquila::BookTicker)` 整除。
   - 使用 `HistoricalDataReader` 相关测试路径或现有 replay 入口读取该文件。
   - 对照 recorder 日志记录 total records、per-exchange records、first / last timestamp 和 source stats。

## 工具化边界

如果手工 smoke 中发现命令容易重复出错，允许新增一个轻量脚本或工具改进：

- 脚本只生成 `/home/liuxiang/tmp` 下的临时 `drain` 配置并打印建议命令；
- 或者只封装 record smoke 的非交易命令；
- 不把临时 live 输出写入仓库；
- 不引入新依赖；
- 不改变 `RealtimeDataReader::Poll()` / `Drain()` 实现。

优先保持实现局部化。如果现有日志已经足够，代码改动可以为零，只更新验证记录或 onboarding。

## 成功标准

一次通过的 live record smoke 至少满足：

- Gate / Binance data session 能成功写入 SHM，或明确说明只验证了可用的一侧；
- `data_reader_recorder` 正常退出；
- 输出 binary 非空且 record size 对齐；
- binary 可由 `HistoricalDataReader` / replay 路径顺序读取；
- recorder source stats 中明确记录 `skipped` 和 `overruns`；
- 如果出现 `overruns > 0` 或 `skipped > 0`，结果按风险记录，不宣称完整 dump 通过。

## 验证命令

本轮最小验证按实际改动选择：

```bash
TMPDIR=/home/liuxiang/tmp ./build/debug/test/tools/market_data/data_reader_recorder_test
./build/debug/test/core/market_data/core_market_data_historical_data_reader_test
./build/debug/tools/data_reader_recorder \
  --config /home/liuxiang/tmp/aquila_strategy_data_reader_drain.toml \
  --output /home/liuxiang/tmp/live_merged_book_ticker.bin \
  --mode truncate
git diff --check
```

如果修改 `evaluation/` 边界，再额外运行：

```bash
rg '#include "evaluation/' core exchange tools
rg 'aquila_evaluation' core exchange tools
```

## 风险和处理

- 网络或交易所 endpoint 不可用：记录 blocker，只保留本地测试和可用 producer 侧证据。
- SHM segment 不存在：先确认 data session 是否启动，不让 recorder 自动 create SHM。
- recorder CPU 抢占交易线程：live smoke 使用短窗口和非关键 CPU；真实交易并行录制时记录 CPU 使用和 source stats。
- `latest` 配置误用于完整 dump：工具日志已有 warning；本轮临时配置必须显式使用 `drain`。
