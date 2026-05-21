# Gate Account TUI 详细设计

## 目标

第一版 Gate Account TUI 是一个独立的 terminal monitor，用于按 symbol 监控单一 Gate futures 账户的订单、仓位和 PnL。它是只读工具，不参与策略执行、不改变账户状态、不读取交易系统内存状态。

设计重点：

- 账户级视角：显示所有 Gate futures 订单、仓位和 PnL。
- Aquila 订单特殊标记：解析 `text = t-<local_order_id>`，展示 strategy id / local order id。
- 外部订单不隐藏：手工单、其他程序订单、未知 text 订单必须进入同一 symbol 视图。
- 独立运行：TUI 进程自己连接 Gate private WebSocket 和 REST，不消费交易系统 SHM。
- 只读优先：第一版不提供撤单、平仓或暂停策略；界面保留后续操作入口位置。

## 非目标

- 不实现交易操作：cancel、flatten、pause strategy、cancel-all、reduce-only close 都不在第一版。
- 不接入 `TradingRuntime`，不复用 `OrderManager` 作为账户状态 owner。
- 不抢读 `OrderFeedbackShmReader` lane。
- 不把 TUI ledger 作为交易系统恢复事实源；它只服务监控和诊断。
- 不基于未验证数据源宣称 PnL 精度；PnL 口径必须由 fixture、REST 对账或 live sample 验证。

## 技术选择

第一版推荐：

- 语言：C++20。
- UI：FTXUI。
- 构建：沿用仓库 CMake + vcpkg。
- 配置：TOML + CLI override，风格参考 `gate_order_feedback_session` 和 `gate_demo_strategy`。
- 日志：沿用 Nova log 配置。

理由：

- 现有 Gate WebSocket、SBE schema、登录签名、parser、config 和 logging 都在 C++ 侧。
- TUI 虽然独立于交易系统，但需要复用 Gate private WS 和 SBE dispatch；C++ 避免再实现一套协议栈。
- FTXUI 适合 terminal dashboard、表格、tab、滚动列表和鼠标选择。

## 目录建议

建议新增独立顶层目录：

```text
monitor/
  CMakeLists.txt
  gate/
    account_monitor_config.h
    account_monitor_config.cpp
    account_monitor_session.h
    account_monitor_session.cpp
    order_update_parser.h
    rest_snapshot_client.h
    rest_snapshot_client.cpp
  model/
    order_source.h
    monitor_order_book.h
    position_ledger.h
    pnl_ledger.h
    account_monitor_model.h
  tui/
    gate_account_tui.cpp
    symbol_workbench_view.h
```

依赖方向：

```text
monitor/*
  -> core/*
  -> exchange/gate/*
  -> config/*
```

禁止方向：

```text
core/exchange/strategy/tools -> monitor
```

如果后续 `monitor/` 中出现可被 test / benchmark 共享的 helper，按现有 evaluation 规则判断是否放入 `evaluation/`；生产路径仍不能依赖 evaluation。

## 运行方式

预期可执行文件：

```bash
./build/debug/monitor/gate_account_tui --config config/monitor/gate_account_tui.toml
```

典型配置内容：

```toml
[monitor]
name = "gate_account_tui"
settle = "usdt"
contracts = ["BTC_USDT", "ETH_USDT", "ORDI_USDT"]
rest_snapshot_interval_sec = 10
stale_after_sec = 30

[monitor.credentials]
api_key_env = "TEST_KEY"
api_secret_env = "TEST_SECRET"

[monitor.websocket]
target = "/v4/ws/usdt/sbe?sbe_schema_id=1"

[monitor.websocket.endpoint]
host = "fx-ws.gateio.ws"
enable_tls = true

[monitor.ui]
mouse_enabled = true
default_layout = "symbol_workbench"
```

## 数据源

### 启动期 REST Snapshot

启动时查询一次：

- futures open orders。
- futures positions。
- account summary / balance。
- 可选 recent finished orders，用于补齐 TUI 启动前的近期上下文。

REST snapshot 的用途：

- 初始化 symbol 列表、已存在 open orders、初始 position 和 account summary。
- 识别 TUI 启动前已经存在的外部风险。
- 为 `futures.orders` 增量流提供初始基线。

### 运行期 WebSocket 增量

TUI 自己建立 Gate private WS 连接，只为 monitor 订阅 private `futures.orders`。该连接独立于交易系统的 `OrderFeedbackSession`。

现有 `exchange/gate/trading/order_feedback_session.h` 可以作为 login / subscribe / lifecycle 参考，但 TUI 应输出 monitor 专用 update，不发布到 order feedback SHM。

### 运行期 REST 校验

低频 REST 校验用于发现 drift：

- WS continuity lost。
- REST position 与 TUI ledger 不一致。
- REST open orders 与 TUI open order set 不一致。
- REST account summary 更新时间过旧或查询失败。

第一版 drift 策略是标红和展示原因，不自动修正交易系统，也不执行任何账户操作。

## Monitor 专用事件

交易系统当前 `OrderFeedbackEvent` 不适合作为 TUI 主事件。TUI 需要 monitor 专用 raw update，例如：

```cpp
struct GateMonitorOrderUpdate {
  std::string_view contract;
  std::string_view text;
  std::string_view status;
  std::string_view finish_as;
  std::uint64_t exchange_order_id;
  std::uint64_t local_order_id;
  bool text_is_aquila_order;
  std::int64_t signed_size;
  std::int64_t left_size;
  std::int64_t cumulative_filled_size;
  double order_price;
  double fill_price;
  double fee;
  std::string_view role;
  bool reduce_only;
  std::int64_t exchange_update_ns;
  std::int64_t local_receive_ns;
};
```

字段以实际 schema / live sample 为准；如果 `futures.orders` 无法稳定表达成交增量和手续费，后续需要补 `futures.usertrades` 或 REST fills。

事件分类：

| 类别 | 判定 | 展示 |
| --- | --- | --- |
| Aquila | `OrderTextCodec::Parse(text).ok` | 显示 strategy id、local order id、exchange order id |
| External | text 存在但不是 `t-<local_order_id>` | 显示 text 摘要和 exchange order id |
| Manual / Unknown | text 为空或无法识别 | 显示 exchange order id，标记 unknown source |

## 状态模型

### MonitorOrderBook

职责：

- 按 exchange order id / text 管理订单快照。
- 跟踪 open、partial、filled、cancelled、rejected 等状态。
- 记录 source classification。
- 维护 per-symbol open order counts 和最近订单列表。

不负责：

- 下单或撤单。
- 推进交易系统 `OrderManager`。
- 作为恢复事实源写回策略。

### PositionLedger

职责：

- 按 symbol 维护 signed position。
- 从 REST snapshot 初始化。
- 从 WS order updates / fill delta 更新。
- 与 REST position 校验 drift。

约束：

- 如果无法从 update 中可靠得到成交 delta，只更新订单状态，不更新 position，标记 `position_stale`。
- 如果 TUI 启动时 REST position 非零，必须显示为初始仓位，不假设由 Aquila 订单产生。

### PnlLedger

职责：

- 按 symbol 展示 realized PnL、unrealized PnL、fees 和 total PnL。
- realized PnL 可以由成交 ledger 计算。
- unrealized PnL 需要 mark price / last price；第一版优先通过 REST position / ticker snapshot 或 TUI 自己的独立行情订阅获取，不默认依赖交易系统 data session。

约束：

- PnL 页面必须展示口径和更新时间。
- 若只由本地 ledger 推导，显示 `estimated`。
- 若 REST / exchange 直接返回 PnL 字段，显示 `exchange snapshot`。
- 若缺少 mark price 或成交增量，显示 `stale` 或 `incomplete`。

### AccountMonitorModel

职责：

- 汇总 connection state、snapshot state、per-symbol model 和事件流。
- 为 FTXUI view 提供只读快照。
- 在 UI 线程和网络线程之间提供清晰同步边界。

建议第一版采用单 producer / UI consumer 模型：网络线程解析事件后写入 model event queue，UI loop 以固定频率 drain 并刷新。不要让 WebSocket 回调直接操作 FTXUI widget。

## UI 设计

第一版首页采用 Symbol Workbench：

```text
+--------------------------------------------------------------------------------+
| Gate USDT Account | WS ready | REST 4s | drift: none | q: quit r: refresh / search |
+-------------------+-----------------------------------+------------------------+
| SYMBOLS           | ORDERS: BTC_USDT                  | POSITION / PNL         |
| > BTC_USDT +0.5   | source   side qty left fill status| net pos        +0.5    |
|   ETH_USDT flat   | Aquila#4 buy  1   0    1    filled| avg entry      68012.5 |
|   ORDI_USDT -3 !  | Manual   sell 2   2    0    open  | mark           68018.1 |
|                   | External buy  1  .5   .5   partial| realized       +2.8    |
|                   |                                   | unrealized     +2.4    |
+-------------------+-----------------------------------+------------------------+
| EVENTS / WARNINGS                                                               |
| 11:40:09 continuity gap, REST check pending                                     |
+--------------------------------------------------------------------------------+
```

交互：

- `up/down`：选择 symbol。
- `tab` / `shift-tab`：切换 pane。
- `/`：搜索 symbol / order id / text。
- `r`：强制 REST refresh。
- `f`：切换只看风险 symbol / 全部 symbol。
- `q`：退出。
- 鼠标：点击 symbol 选中，点击 tab 切页，滚轮滚动订单和事件列表。

页面：

- `Overview`：Symbol Workbench 首页。
- `Orders`：全账户订单表，支持按 symbol / source / status 过滤。
- `Positions`：仓位、PnL、stale / drift 详情。
- `Events`：WS / REST / parser / model 事件流。
- `Help`：快捷键和只读边界。

未来操作入口预留：

- 右上角或 command palette 可以预留 disabled actions，例如 `cancel selected`、`flatten symbol`、`pause strategy`。
- 第一版这些操作显示为 disabled 或不显示；不得绑定实际交易逻辑。

## 错误处理

### WebSocket 断线

- 状态栏显示 `WS disconnected` / `reconnecting`。
- 当前 symbol 和订单表保留最后状态，但标记 `stale`。
- 恢复连接后触发 REST snapshot 校验。
- 未校验前不把 stale 状态清除。

### Parser 错误

- 计数并展示 parser error summary。
- 对无法解析的 order update，写入 event log，不更新 ledger。
- 如果错误影响当前 symbol，symbol 行显示 warning。

### REST 失败

- 保留上一次成功 snapshot。
- 状态栏显示失败 endpoint、错误类别和 elapsed。
- 超过 `stale_after_sec` 后将 position / account summary 标记 stale。

### Drift

drift 示例：

- REST open orders 中存在 TUI 未见订单。
- TUI open order 在 REST 中不存在，且无 terminal update。
- REST position 与 TUI ledger position 不一致。
- account summary pending orders 与 TUI 聚合不一致。

第一版处理：

- 标记 symbol 为 `drift`。
- 展示本地值、REST 值、最近 WS update 时间和 REST snapshot 时间。
- 不自动修正、不发交易指令。

## 并发模型

建议线程模型：

```text
GateMonitorNetworkThread
  Gate private WS session
  REST snapshot timer / client
  parse update
  push MonitorEvent queue

TuiThread
  drain MonitorEvent queue
  update AccountMonitorModel
  render FTXUI
```

第一版可以先用互斥保护 cold monitor model；TUI 不在交易热路径内，不需要把复杂无锁结构放在第一版。若后续高频刷新或多 feed 导致 UI 卡顿，再用 SPSC event queue 优化。

## 测试建议

单元测试：

- monitor order parser：Aquila text、external text、empty text、fee、signed size、finish_as、partial / terminal。
- order source classification：`t-<local_order_id>`、invalid text、manual order。
- position ledger：open long、partial close、flip、fee、启动非零仓位。
- PnL ledger：realized、unrealized、fee、缺 mark price。
- drift detector：REST/TUI open order 不一致、position 不一致、snapshot stale。

集成测试：

- fake Gate order update stream 驱动 monitor model。
- REST snapshot fixture + WS incremental fixture 组合。
- FTXUI view model snapshot 测试，不做脆弱的 terminal pixel 测试。

Live smoke：

```bash
./build/debug/monitor/gate_account_tui --config config/monitor/gate_account_tui.toml
```

验证条件：

- 能登录 Gate private WS 并订阅 `futures.orders`。
- REST snapshot 成功，状态栏显示 snapshot age。
- 无订单账户能显示 flat / empty。
- 有外部 open order 时能按 symbol 显示并标记 source。
- Aquila `t-<local_order_id>` 订单能解析并标记 Aquila。

## 实现顺序建议

1. 新增 `monitor/` CMake skeleton 和 `gate_account_tui` 空工具。
2. 新增 monitor config parser。
3. 实现 monitor 专用 Gate orders raw parser 和 fixture tests。
4. 实现 REST snapshot client 或 C++ read-only query helper。
5. 实现 `MonitorOrderBook`、`PositionLedger`、`PnlLedger`。
6. 实现 independent Gate account monitor session。
7. 接入 FTXUI Symbol Workbench。
8. 增加 live smoke 文档和验证命令。

## 收尾边界

第一版完成时，只能宣称：

- TUI 独立监控 Gate futures 单一账户。
- 订单、仓位和 PnL 按已验证口径展示。
- Aquila / external / manual 订单可区分。
- WS / REST stale 和 drift 可见。

不能宣称：

- TUI 可以作为交易系统恢复事实源。
- PnL 精度已覆盖所有 Gate 费用、资金费和历史启动状态，除非有对应 fixture / live 对账。
- 后续操作入口已经安全可用。
