# Tardis S3 下载脚本实现计划

> **给 agentic workers：** 必须使用 `superpowers:subagent-driven-development`（推荐）或 `superpowers:executing-plans` 按任务执行本计划。步骤使用 checkbox（`- [ ]`）语法跟踪。

**目标：** 增加一个 Python 工具，用 AWS CLI 下载 Tardis S3 数据，并在本地保持 `<download_dir>/tardis/<exchange>/<data_type>/<YYYYMMDD>/...` 路径结构。

**架构：** 脚本根据 `exchange`、`data_type`、日期范围和 symbol 过滤条件构造确定性的 S3 URI 与本地路径，再把实际传输交给 `aws s3 cp`。为避免误拉大目录，CLI 强制要求显式日期范围，并要求 `--symbols` 和 `--all-symbols` 二选一。

**技术栈：** Python 标准库、AWS CLI `aws s3 cp`、`unittest`。

---

### 文件结构

- 新建：`scripts/tardis/download_tardis_s3.py`
  - 负责 CLI 解析、日期范围展开、路径构造、AWS CLI 命令构造和 dry-run 执行。
  - `--download-dir` 默认 `~/`，`--prefix` 默认 `tardis`，因此默认本地根目录是 `~/tardis/`。
  - `--all-symbols` 默认只下载 `*.csv.gz`；显式 `--include-empty-marker` 时才下载 marker 对象。
- 新建：`scripts/tardis/download_tardis_s3_test.py`
  - 覆盖参数校验、日期展开、路径布局、命令构造和 dry-run 不写文件系统。

### Task 1: 先写失败测试

**文件：**
- 新建：`scripts/tardis/download_tardis_s3_test.py`

- [ ] **Step 1: 增加公共 helper API 测试**

测试覆盖：
- `expand_date_range("20260415", "20260417")` 返回包含首尾日期的 3 天。
- 反向日期范围抛出 `ValueError`。
- 指定 symbol 下载时，S3 URI 和本地路径保持 `<download_dir>/tardis/<exchange>/<data_type>/<date>/<symbol>-<data_type>-<date>.csv.gz`。
- `--all-symbols` 每个日期使用 `aws s3 cp --recursive`，默认只 include `*.csv.gz`，并支持 `--no-overwrite`。
- `--include-empty-marker` 会移除 `*.csv.gz` 过滤。
- 缺少 `--symbols` / `--all-symbols` 时 CLI 解析失败。
- `dry_run=True` 只打印命令，不创建本地目标目录。

- [ ] **Step 2: 运行测试确认红灯**

运行：

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/tardis/download_tardis_s3_test.py
```

预期：在实现脚本前，因为 `download_tardis_s3.py` 不存在而失败；新增 dry-run 语义测试也应能捕捉旧行为会创建目录的问题。

### Task 2: 实现脚本

**文件：**
- 新建：`scripts/tardis/download_tardis_s3.py`

- [ ] **Step 1: 实现 helpers 和 CLI**

实现：
- `DownloadCommand` dataclass。
- `expand_date_range(start_date, end_date)`。
- `normalize_symbol(symbol)`。
- `build_symbol_downloads(...)`。
- `build_all_symbols_downloads(...)`，默认使用 `aws s3 cp --recursive --exclude "*" --include "*.csv.gz"`；显式 `--include-empty-marker` 时不加 include/exclude 过滤。
- `parse_args(argv)`。
- `main(argv=None)`。

- [ ] **Step 2: 运行单元测试**

运行：

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/tardis/download_tardis_s3_test.py
```

预期：全部测试通过。

### Task 3: 手工 dry-run 验证

- [ ] **Step 1: 运行安全 dry-run**

运行：

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/tardis/download_tardis_s3.py \
  --exchange binance-futures \
  --data-type book_ticker \
  --start-date 20260415 \
  --end-date 20260415 \
  --symbols BTCUSDC \
  --dry-run
```

预期：只打印一条 `aws s3 cp` 命令，目标路径为 `~/tardis/binance-futures/book_ticker/20260415/BTCUSDC-book_ticker-20260415.csv.gz`，不下载数据、不创建默认下载目录。

- [ ] **Step 2: 检查格式**

运行：

```bash
git diff --check
```

预期：无 whitespace 错误。

### Task 4: 提交

- [ ] **Step 1: 查看变更**

运行：

```bash
git status --short
git diff -- doc/superpowers/plans/2026-05-15-tardis-s3-download-script.md scripts/tardis/download_tardis_s3.py scripts/tardis/download_tardis_s3_test.py
```

- [ ] **Step 2: 提交**

运行：

```bash
git add doc/superpowers/plans/2026-05-15-tardis-s3-download-script.md scripts/tardis/download_tardis_s3.py scripts/tardis/download_tardis_s3_test.py
git commit -m "Add Tardis S3 download helper"
```

### 自检

- 覆盖范围：CLI 支持 bucket、prefix、exchange、data type、日期范围、symbols、all-symbols、download directory、dry-run、profile、no-overwrite 和 include-empty-marker。
- 占位符检查：无未展开的 TBD / TODO。
- 类型一致性：测试和计划统一使用 `DownloadCommand.source`、`DownloadCommand.destination` 和 `DownloadCommand.command`。
