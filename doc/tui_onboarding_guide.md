# Aquila TUI Onboarding

## 目的

本文是接手 Aquila TUI / monitor 方向时的入口文档。它只记录当前已确认的范围、边界、文档索引、代码入口建议和下一步，不替代详细设计。

## 当前决策

- 第一版 TUI 是运行在 terminal 中的 C++20 程序，推荐使用 FTXUI。
- TUI 放在独立顶层目录 `monitor/`，不放进 `tools/gate/`、`strategy/` 或交易 runtime。
- TUI 可复用 `core/`、`exchange/gate/`、`config/` 中已有的 WebSocket、Gate 登录、SBE schema / dispatch、配置和日志代码。
- 依赖方向必须单向：`monitor/* -> core/exchange/config`；`core/`、`exchange/`、`strategy/`、`tools/` 不反向依赖 `monitor/`。
- 第一版是只读监控，不执行撤单、平仓、暂停策略或修改配置；但 UI 和模型保留后续操作入口位置。
- TUI 的所有运行信息独立于交易系统：独立连接 Gate private WebSocket，不消费交易系统的 order feedback SHM lane，不接入 `TradingRuntime`，不读取 `OrderManager` 内存状态。
- 监控对象是单一 Gate futures 账户的全账户订单 / 仓位 / PnL，而不是只看 Aquila 自己创建的订单。
- Aquila 订单通过 `text = t-<local_order_id>` 识别并标记 strategy id / local order id；外部程序订单、手工订单、未知 text 订单必须保留可见。
- 首页布局采用 Symbol Workbench：左侧 symbol 列表，中间当前 symbol 订单表，右侧仓位和 PnL，底部或副页展示事件流和连接状态。
- TUI 以键盘优先设计，同时支持鼠标选择；鼠标不能成为唯一操作路径。

## 文档索引

| 文档 | 什么时候读 | 内容 |
| --- | --- | --- |
| `doc/tui_gate_account_monitor_design.md` | 继续设计或开始实现 TUI | 第一版 Gate account TUI 的详细架构、组件、数据流、错误处理和测试建议 |
| `doc/project_onboarding_guide.md` | 新对话总体接手 | 项目当前事实源、代码入口和全局下一步 |
| `doc/agent-handoff-gate-trade-architecture.md` | 复用 Gate private WS / SBE / order feedback 代码前 | Gate 交易 WebSocket、private feedback、SHM 和线程模型边界 |
| `doc/strategy_order_component_model.md` | 判断 TUI 是否能复用交易组件时 | `OrderManager`、`OrderFeedbackSession`、`TradingRuntime` 的职责边界 |
| `doc/lead_lag_reconcile_design.md` | 处理 REST snapshot / drift / manual intervention 语义时 | read-only reconcile、REST 事实校验和恢复边界 |
| `doc/futures_contract_metadata_fields.md` | 计算合约数量、notional、PnL 单位时 | Gate / Binance futures metadata 字段和单位差异 |

## 当前实现可复用点

- `core/websocket/*`：WebSocket cold / hot path、TLS socket、message view、runtime policy。
- `exchange/gate/trading/order_feedback_session.h`：Gate private WS login / subscribe / connection lifecycle 参考实现。
- `exchange/gate/sbe/message_dispatcher.h` 和 `exchange/gate/sbe/generated/`：Gate SBE schema dispatch 和生成代码。
- `exchange/gate/trading/order_feedback_parser.h`：交易系统窄 feedback parser 的字段位置和 decimal 转换参考。
- `exchange/gate/trading/order_codecs.h`：`OrderTextCodec::Parse()` 用于识别 Aquila `t-<local_order_id>`。
- `scripts/gate/query_gate_account.py`：REST account / order / position read-only 查询语义参考；第一版 TUI 可以先在 C++ 中实现等价查询或通过后续 REST helper 复用其签名规则。
- `core/config/*` 和已有 TOML parser：TUI config 应沿用 TOML + CLI override 风格。

## 不建议复用的点

- 不复用 `TradingRuntime`：TUI 不是策略执行链路，不应该进入交易系统 event loop。
- 不复用 `OrderManager` 作为账户订单事实源：它是策略订单状态 owner，不适合全账户 monitor。
- 不直接消费 `OrderFeedbackShmReader`：该 transport 是交易系统下行事实流，SPSC lane 语义不适合被 TUI 抢读。
- 不直接使用 `OrderFeedbackEvent` 作为 TUI 主事件：该结构依赖 Aquila `t-<local_order_id>`，并缺少 TUI 计算 PnL 需要的 contract、side / signed size、fee、完整 status 等字段。

## 下一步建议

1. 阅读 `doc/tui_gate_account_monitor_design.md`。
2. 确认是否接受新增 vcpkg 依赖 `ftxui`。
3. 先实现 monitor 专用 Gate orders raw parser，覆盖全账户订单，不丢弃非 Aquila text。
4. 再实现独立 Gate account monitor session：只订阅 private `futures.orders`，输出 monitor raw update。
5. 增加启动期 REST snapshot：open orders、positions、account summary；运行中低频校验 drift。
6. 实现 monitor model：按 symbol 聚合 orders、position ledger、realized / unrealized PnL、source classification 和 stale 状态。
7. 最后接 FTXUI：Symbol Workbench 首页、详情页、事件流页、连接状态栏和键盘 / 鼠标选择。

## 当前开放问题

- PnL 口径需要最终确认：是否以 Gate REST account / position 字段为权威，还是 TUI ledger 为主、REST 只做 drift 标记。
- `futures.orders` 是否足够覆盖手续费和成交增量的精确口径，需要用 live sample 或 fixture 固化；如果不够，后续应补 `futures.usertrades` 或 REST fills。
- 合约 metadata 的启动期来源需要确定：读取现有 CSV / script 输出，还是新增 C++ metadata loader。
- 后续操作入口只预留布局和模型状态；真正 cancel / flatten / pause 需要单独设计和确认。
