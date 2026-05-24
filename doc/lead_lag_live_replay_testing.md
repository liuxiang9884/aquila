# LeadLag 实盘与 Replay 测试说明

## 目的

本文记录 LeadLag 相关实盘测试和 replay 测试的标准流程、输出约定和历史证据。这里的“实盘”默认指连接真实 Gate / Binance 行情
WebSocket 和本机 SHM，但不等于真实下单；是否下单必须由测试项显式说明。

所有新测试默认把临时 config、stdout / stderr、策略 log、CSV、binary 和对比报告写入 `/home/liuxiang/tmp/<run_id>`。不要为了单次
测试修改仓库默认 config，也不要把临时运行产物写回仓库。

## 标准测试名称

### `lead_lag_live_replay_signal_parity`

触发格式：

```text
lead_lag_live_replay_signal_parity <duration>
```

示例：

```text
lead_lag_live_replay_signal_parity 30m
lead_lag_live_replay_signal_parity 1800s
```

当用户只给出这个名字和测试时长时，直接按“测试 1：实盘信号与 replay 信号一致性”执行。默认使用
`config/strategies/lead_lag_requested_11symbols_strategy_20260522.toml` 对应的 requested 12-symbol 配置，除非用户另行指定。

## 测试 1：实盘信号与 Replay 信号一致性

测试名：`lead_lag_live_replay_signal_parity`

### 目标

1. 实盘运行 LeadLag signal-only 策略，生成 live signal CSV。
2. 同时启动独立 `data_reader_recorder` 进程，从同一组 Gate / Binance `BookTicker` SHM 导出 merged replay binary。
3. 使用导出的 binary 运行 `lead_lag_replay`，生成 replay signal CSV。
4. 比较 live / replay signal CSV，分析差异。

### 安全边界

- 不使用 `--execute`。
- 策略保持 signal-only，`execute=false`，不提交真实订单。
- recorder 是只读 SHM consumer，可与 signal-only 策略并行运行。
- 所有临时文件写入 `/home/liuxiang/tmp/<run_id>`。
- 使用唯一 SHM 名称，避免覆盖正在运行的其他 data session。

### 输入配置

默认输入：

```text
config/data_sessions/gate_data_session_requested_20260521.toml
config/data_sessions/binance_data_session_requested_20260521.toml
config/data_readers/strategy_data_reader_requested_20260521.toml
config/strategies/lead_lag_requested_11symbols_strategy_20260522.toml
config/strategies/lead_lag_requested_11symbols_20260522.toml
```

注意：`lead_lag_requested_11symbols_*` 文件名保留历史 `11symbols`，当前内容已覆盖 requested 12 symbols，并已追加 `ETH_USDT`。

### 临时运行目录

运行目录格式：

```text
/home/liuxiang/tmp/lead_lag_live_replay_compare_<YYYYMMDD_HHMMSS>
```

目录内至少包含：

```text
run_metadata.json
run_live_replay_compare.sh
gate_data_session.toml
binance_data_session.toml
data_reader_live_latest.toml
data_reader_recorder_drain.toml
data_reader_recorded_binary.toml
strategy_live.toml
strategy_replay.toml
lead_lag_requested_12symbols.toml
live_signals.csv
replay_signals.csv
recorded_book_ticker.bin
compare_summary.json
compare_summary.md
compare_intent_summary.json
compare_intent_summary.md
*.stdout.log
*.log
exit_codes.tsv
orchestrator.log
```

### 临时配置生成规则

1. 复制 Gate / Binance data session config 到运行目录。
2. 把 data session log sink 改到运行目录。
3. 把 SHM 名称改成唯一名称：

```text
aquila_gate_market_data_live_replay_compare_<YYYYMMDD_HHMMSS>
aquila_binance_market_data_live_replay_compare_<YYYYMMDD_HHMMSS>
```

4. 复制 strategy data reader config 两份：
   - live reader：保留 `read_mode = "latest"`，连接唯一 SHM；
   - recorder reader：改成 `read_mode = "drain"`，`max_events_per_drain = 4096`，连接同一组唯一 SHM。
5. 创建 binary replay data reader config：
   - `type = "binary_file"`；
   - `files = ["<run_dir>/recorded_book_ticker.bin"]`；
   - `start_position = "earliest_visible"`；
   - `read_mode = "drain"`；
   - `max_events_per_drain = 4096`。
6. 复制 strategy config 两份：
   - live strategy 指向 live reader；
   - replay strategy 指向 binary replay reader；
   - 两者都把 log sink 改到运行目录；
   - 两者都指向运行目录内复制出的 LeadLag pair config。

### 运行顺序

先做不连网 config validation：

```bash
./build/debug/tools/gate_data_session --config <run_dir>/gate_data_session.toml
./build/debug/tools/binance_data_session --config <run_dir>/binance_data_session.toml
./build/debug/tools/lead_lag_strategy --config <run_dir>/strategy_live.toml
```

期望都返回 `0`。

正式运行：

```bash
./build/debug/tools/gate_data_session \
  --config <run_dir>/gate_data_session.toml \
  --connect

./build/debug/tools/binance_data_session \
  --config <run_dir>/binance_data_session.toml \
  --connect

./build/debug/tools/data_reader_recorder \
  --config <run_dir>/data_reader_recorder_drain.toml \
  --output <run_dir>/recorded_book_ticker.bin \
  --mode truncate

./build/debug/tools/lead_lag_strategy \
  --config <run_dir>/strategy_live.toml \
  --connect-data \
  --duration-sec <duration_sec> \
  --signals-output <run_dir>/live_signals.csv
```

`lead_lag_strategy` 自然退出后，停止 recorder 和两个 data session。随后运行 replay：

```bash
./build/debug/tools/lead_lag_replay \
  --config <run_dir>/strategy_replay.toml \
  --data-reader-config <run_dir>/data_reader_recorded_binary.toml \
  --signals-output <run_dir>/replay_signals.csv
```

基础对比：

```bash
PYTHONPATH=scripts/lead_lag python3 scripts/lead_lag/compare_signal_csv.py \
  <run_dir>/live_signals.csv \
  <run_dir>/replay_signals.csv \
  --json-output <run_dir>/compare_summary.json \
  --markdown-output <run_dir>/compare_summary.md
```

核心交易意图对比：

```bash
PYTHONPATH=scripts/lead_lag python3 scripts/lead_lag/compare_signal_csv.py \
  <run_dir>/live_signals.csv \
  <run_dir>/replay_signals.csv \
  --compare-fields action,side,price,reduce_only,exchange_ns,local_ns,event_ns,lead_raw_event_ns,lead_raw_bid,lead_raw_ask,lag_event_ns,lag_bid,lag_ask,group_id,position_direction,trailing_price \
  --json-output <run_dir>/compare_intent_summary.json \
  --markdown-output <run_dir>/compare_intent_summary.md
```

### 通过判定

最低通过条件：

- `exit_codes.tsv` 中 data session、recorder、live strategy、replay 和 compare 全部为 `0`。
- `recorded_book_ticker.bin` 大小是 `sizeof(aquila::BookTicker)` 的整数倍；当前 ABI size 为 `64` 字节。
- recorder diagnostics 中所有 SHM source 的 `overruns = 0`。
- recorder diagnostics 中 `skipped = 0`；如果出现 skipped / overrun，必须说明差异是否仍可解释。
- `lead_lag_strategy_signal_only_summary` 和 `lead_lag_replay_summary` 都能输出 signal summary。
- `compare_intent_summary` 中 `live_only = 0`、`replay_only = 0`、`mismatched = 0`。

扩展 diagnostics 对比允许出现小幅差异，但必须解释来源。live strategy 使用 `latest` 采样，recorder 使用 `drain` 记录完整事件流；
即使最终 signal key 和交易意图一致，rolling diagnostics 仍可能因 replay 多处理启动 / 收尾窗口或中间 tick 而出现细微数值差异。

### 差异分析优先级

1. 先看 `live_only` / `replay_only`，确认是否有信号 key 缺失。
2. 再看核心交易意图字段：`action`、`side`、`price`、`reduce_only`、事件时间、raw BBO、`group_id`、`position_direction`、`trailing_price`。
3. 最后看 diagnostics 字段：drift、threshold、noise、drifted lead BBO。此类字段差异通常需要结合 live `latest` 与 replay `drain` 的输入差异解释。
4. 如果 recorder 记录数大于 live strategy 处理数，优先检查 recorder 是否早于 live strategy 启动或晚于 live strategy 停止。
5. 如果 replay-only 信号集中在开头或末尾，优先按启动 / 收尾窗口偏移解释；如果分布在中间，再检查 live `latest` 是否跳过关键中间 tick。

## 运行记录

### 2026-05-24 `lead_lag_live_replay_signal_parity 30m`

运行目录：

```text
/home/liuxiang/tmp/lead_lag_live_replay_compare_20260524_033401
```

时间窗口：

- data session 启动：2026-05-24 03:34:35 UTC
- recorder 启动：2026-05-24 03:34:50 UTC
- live strategy 启动：2026-05-24 03:34:51 UTC
- live strategy 自然退出：2026-05-24 04:04:51 UTC
- replay / compare 完成：2026-05-24 04:04:52 UTC

退出码：

| process | exit code |
| --- | ---: |
| `lead_lag_strategy_live` | 0 |
| `data_reader_recorder` | 0 |
| `gate_data_session` | 0 |
| `binance_data_session` | 0 |
| `lead_lag_replay` | 0 |
| `compare_signal_csv` | 0 |

Data session 结果：

| source | book_tickers | rx_messages | tx_messages | result |
| --- | ---: | ---: | ---: | --- |
| Gate | 122,027 | 122,028 | 365 | `ok active=true phase=kClosed error=kNone` |
| Binance | 634,266 | 634,266 | 374 | `ok active=true phase=kClosed error=kNone` |

Recorder 结果：

| metric | value |
| --- | ---: |
| output bytes | 47,975,552 |
| `BookTicker` records | 749,618 |
| trailing bytes | 0 |
| Gate records | 120,766 |
| Binance records | 628,852 |
| Gate skipped / overruns | 0 / 0 |
| Binance skipped / overruns | 0 / 0 |

Live strategy summary：

| metric | value |
| --- | ---: |
| book_tickers | 749,292 |
| signals | 34 |
| open | 17 |
| close | 17 |
| stoploss | 0 |
| order_responses | 0 |
| order_feedbacks | 0 |
| recovery_state | `normal` |
| needs_reconcile | `false` |
| manual_intervention | `false` |
| new_entries_paused | `false` |

Replay summary：

| metric | value |
| --- | ---: |
| book_tickers | 749,618 |
| signals | 34 |
| open | 17 |
| close | 17 |
| stoploss | 0 |

Signal CSV action 分布：

| action | live | replay |
| --- | ---: | ---: |
| `kOpenShort` | 11 | 11 |
| `kCloseShort` | 11 | 11 |
| `kOpenLong` | 6 | 6 |
| `kCloseLong` | 6 | 6 |

基础对比结果：

| metric | count |
| --- | ---: |
| live_signals | 34 |
| replay_signals | 34 |
| matched | 34 |
| live_only | 0 |
| replay_only | 0 |
| mismatched | 8 |

核心交易意图对比结果：

| metric | count |
| --- | ---: |
| live_signals | 34 |
| replay_signals | 34 |
| matched | 34 |
| live_only | 0 |
| replay_only | 0 |
| mismatched | 0 |

结论：

- 30 分钟 signal-only live 与 recorded binary replay 的 34 条 signal key 全部匹配，没有 live-only 或 replay-only。
- `action`、`side`、`price`、`reduce_only`、事件时间、raw BBO、`group_id`、`position_direction` 和 `trailing_price` 等核心交易意图字段完全一致。
- 全 diagnostics 对比中有 8 个 key 出现差异，字段集中在 `up_entry`、`down_entry`、`lead_noise`、`lead_drifted_bid` / `lead_drifted_ask`、`drift_mean` 和 `drift_deviation`。
- replay 比 live 多处理 326 条 `BookTicker`，符合 recorder `drain` 覆盖更完整启动 / 收尾窗口，而 live strategy 使用 `latest` 采样的边界；这些差异未影响本轮 signal intent。

本轮通过 `lead_lag_live_replay_signal_parity` 的最低通过条件。
