# LeadLag fusion 实盘行情实现计划

目标：新增 checked-in 配置和文档，使 LeadLag live smoke 可以消费 Gate / Binance fastest-route fusion `BookTicker` canonical stream。

架构：LeadLag strategy C++ 不改。先启动 Gate / Binance threaded fusion bundle，输出稳定命名的 canonical SHM，再用新的 LeadLag data reader config 读取这些 SHM。有效 universe 使用 31 个 symbol：Gate 会拒绝 `TON_USDT`，因此从原 30-symbol 集合删除 `TON_USDT`，再加入 `BTC_USDT` 和 `ETH_USDT`。

技术栈：C++20 release binary、TOML config、`gate_data_fusion`、`binance_data_fusion`、`lead_lag_strategy`、`ctest` 和 Python config sanity check。

---

### 任务 1：新增 fusion live 配置

文件：
- 新增：`config/data_sessions/gate_data_session_31symbols_no_ton_private_plain_20260616.toml`
- 新增：`config/data_sessions/binance_data_session_31symbols_no_ton_20260616.toml`
- 新增：`config/market_data_fusion/gate_book_ticker_fusion_31symbols_no_ton_4sources.toml`
- 新增：`config/market_data_fusion/binance_book_ticker_fusion_31symbols_no_ton_4sources.toml`
- 新增：`config/market_data_fusion/gate_data_fusion_31symbols_no_ton_book_ticker_4sources.toml`
- 新增：`config/market_data_fusion/binance_data_fusion_31symbols_no_ton_book_ticker_4sources.toml`

- [ ] 步骤 1：创建使用 31-symbol no-TON 列表的 data session config。
- [ ] 步骤 2：创建使用稳定 source / canonical SHM 名称的 fusion config。
- [ ] 步骤 3：按现有 pattern 创建 threaded bundle launch config，使用实盘预留区 CPU 分配。
- [ ] 步骤 4：运行 dry-run parser：
  ```bash
  ./build/release/tools/gate_data_fusion --config config/market_data_fusion/gate_data_fusion_31symbols_no_ton_book_ticker_4sources.toml
  ./build/release/tools/binance_data_fusion --config config/market_data_fusion/binance_data_fusion_31symbols_no_ton_book_ticker_4sources.toml
  ```

### 任务 2：新增 LeadLag fusion strategy 配置

文件：
- 新增：`config/data_readers/strategy_data_reader_31symbols_no_ton_fusion_20260616.toml`
- 新增：`config/strategies/lead_lag_31symbols_no_ton_fusion_live_strategy_20260616.toml`
- 新增：`config/strategies/lead_lag_31symbols_no_ton_2bps_notional_20260616.toml`

- [ ] 步骤 1：创建读取 `aquila_gate_book_ticker_fusion_31symbols_no_ton_20260616` 和 `aquila_binance_book_ticker_fusion_31symbols_no_ton_20260616` 的 strategy data reader。
- [ ] 步骤 2：复制 30-symbol live runtime config，并把 `[strategy.data_reader].config` 指向 fusion data reader。
- [ ] 步骤 3：从 `lead_lag_30symbols_2bps_notional_20260604.toml` 生成 nested pair config，删除 `TON_USDT`，追加 `BTC_USDT` 和 `ETH_USDT`，保留 freshness guard 字段。
- [ ] 步骤 4：运行 strategy dry-run：
  ```bash
  ./build/release/tools/lead_lag_strategy --config config/strategies/lead_lag_31symbols_no_ton_fusion_live_strategy_20260616.toml
  ```

### 任务 3：更新文档

文件：
- 修改：`docs/lead_lag_live_operations_pipeline.md`
- 修改：`docs/project_onboarding_guide.md`
- 修改：`docs/runtime_cpu_allocation.md`

- [ ] 步骤 1：在 live pipeline 中增加 fusion market-data preflight。
- [ ] 步骤 2：在 onboarding 中增加新配置入口和 `TON_USDT` 排除说明。
- [ ] 步骤 3：在 CPU 分配文档中记录 31-symbol no-TON fusion profile。
- [ ] 步骤 4：保持摘要简短，引用 fusion 专题文档，不复制 shadow 原始输出。

### 任务 4：验证并提交

文件：任务 1-3 中的全部文件。

- [ ] 步骤 1：运行 focused CTest：
  ```bash
  ctest --test-dir build/debug -R '(book_ticker_fusion|data_session_config|data_reader_config|lead_lag_config|live_strategy)' --output-on-failure
  ```
- [ ] 步骤 2：运行格式检查：
  ```bash
  git diff --check
  ```
- [ ] 步骤 3：复核改动文件：
  ```bash
  git status --short
  git diff --stat
  ```
- [ ] 步骤 4：提交：
  ```bash
  git add config docs
  git commit -m "Add LeadLag fusion live market data configs"
  ```
