# Aquila 诊断字段索引

## 目的

本文登记为诊断、延迟分析、运行报告和临时排障新增的字段。它不是完整 report schema；完整 LeadLag CSV
字段说明仍以 `docs/lead_lag_live_report_csv_schema.md` 为准。

维护目标：

- 新增、重命名或删除诊断字段、log key、stats 字段、report CSV 字段时，同步更新本文。
- 临时字段必须写清用途和删除条件，避免长期遗留无归属字段。
- 字段说明按 Aquila 组件分 section，便于后续用字段名反查代码入口和删除依据。
- 性能结论仍必须基于实际测试、benchmark、profile 或 live run 证据，字段本身只提供观测面。

## 字段生命周期

| 生命周期 | 含义 | 删除或保留规则 |
| --- | --- | --- |
| `stable` | 长期可观测性字段，报告、脚本或 runbook 已依赖。 | 删除前必须同步更新 parser、report、文档和历史兼容说明。 |
| `experiment` | 为当前实验或诊断面加入，预计会被验证或调整。 | 实验结束后改为 `stable` 或删除，并更新本文。 |
| `temporary` | 为复现某个问题加入的短期字段。 | 问题关闭或替代观测面落地后删除；删除时在 commit / report 中说明。 |
| `planned` | 已确认需要，但代码尚未落地。 | 实现时把状态改成实际生命周期。 |

## Order Ack outlier 分析层级

`AQUILA_ORDER_ACK_DIAG_LEVEL` 是编译期上限，取值范围为 `0..5`。`Ln` 表示编译进 `L1..Ln` 的能力；
运行期配置只能在这个上限内请求采样、阈值和输出，不能越级开启。若运行期请求高于编译期 level，应 fail fast，
避免把缺字段误读成真实 0 或真实无 outlier。

| Level | 名称 | 能力边界 | 代表字段 |
| --- | --- | --- | --- |
| `L0` | Off | 不编译 Order Ack outlier 阶段归因；仅保留交易正确性所需状态和既有业务日志。Gate Ack JSON header 中的 `x_in_time` / `x_out_time` 会作为普通 response 字段解析和输出，不受 Ack diagnostic level 控制。 | 不采集 diagnostic window、`TCP_INFO`、socket timestamping 或 pcap 对齐字段；保留 `exchange_request_ingress_ns`、`exchange_response_egress_ns`、`exchange_process_ns`。 |
| `L1` | Correlation | 只保留 join key、endpoint、CPU 和 Ack RTT 主口径，用于定位是哪条连接 / 请求 / CPU 出 tail。 | `run`、`session`、`group`、`ip`、`sid`、`order_session_id`、`local_id`、`req_seq`、`request_sequence`、`local_order_id`、`local_ip` / `remote_ip`、`send_cpu`、`ack_cpu`、`ack_rtt_ns`。 |
| `L2` | Runtime / write path | 在 L1 基础上启用 Ack diagnostic window，拆本机 runtime、`DriveRead()`、编码、enqueue 和 write syscall。 | `diag`、`diag_reason`、`send_to_*`、`drive_read_*`、`max_loop_gap_ns`、`order_encode_done_ns`、`write_*`、`pending_write_count_after`。 |
| `L3` | Socket queue / `TCP_INFO` | 在 L2 基础上采集 socket send queue、notsent backlog 和 Linux `TCP_INFO`，判断本机 TCP queue、kernel RTT、未确认包和重传。 | `socket_sendq_available`、`tcp_sendq_bytes`、`tcp_notsent_bytes`、`tcp_info_*`。 |
| `L4` | Socket timestamping | 在 L3 基础上启用 Linux `SO_TIMESTAMPING` software-level 归因，按本机同一时钟域拆出 TX/RX 阶段。 | `ts_available`、`ts_write_complete_ns`、`ts_tx_sched_ns`、`ts_tx_software_ns`、`ts_tx_ack_ns`、`ts_rx_software_ns`、`ts_*_ns` 阶段耗时。 |
| `L5` | Pcap alignment | 在 L4 基础上保留 no TLS pcap 对齐分析能力，把 runtime 已输出的 Gate `x_in_time -> x_out_time` 与本机抓包中的 `pcap request -> Ack response` 对齐，计算 residual 和 Gate share。 | `pcap_request_to_ack_ms`、`gate_x_in_to_x_out_ms`、`residual_ms`、`gate_share`、`pcap_request_ns`、`pcap_ack_response_ns`、`conn_id`。 |

默认 build 使用 `L4`，保持现有 socket timestamping 诊断能力；需要完全关闭 outlier attribution 时显式设
`-DAQUILA_ORDER_ACK_DIAG_LEVEL=0`。旧的 `AQUILA_ENABLE_SOCKET_TIMESTAMPING_ATTRIBUTION` 只作为兼容入口：
未显式设置 `AQUILA_ORDER_ACK_DIAG_LEVEL` 时，旧开关 `OFF` 等价于最高 `L3`，旧开关 `ON` 等价于默认 `L4`。

诊断层级的性能选择必须用当前机器和当前 binary 重新 benchmark；历史 L0-L5 结果不再放在字段索引中。
运行证据、环境边界和分析顺序统一见 `docs/lead_lag_latency_analysis.md`。

## Data Session BookTicker latency outlier

`AQUILA_DATA_SESSION_DIAG_LEVEL` 是 data session 接收路径的编译期诊断上限，取值范围为 `0..4`，默认
`L0`。`L0` 不改变 `MessageView` 布局，不在 data session / client 热路径采集额外时间戳，也不会输出
BookTicker latency outlier log。运行期配置只能在编译期上限内开启诊断；越级开启时 parser fail fast。

| Level | 名称 | 能力边界 | 代表字段 |
| --- | --- | --- | --- |
| `L0` | Off | 默认路径；Gate / Binance / Bitget data session 行为与未开启诊断一致。 | 不输出 `data_session_book_ticker_latency_outlier`。 |
| `L1` | Correlation | 只在 `BookTicker.local_ns - BookTicker.exchange_ns > threshold_ns` 时按限流写 Nova warning，用于定位 exchange / source / symbol / ticker id。 | `exchange`、`source_id`、`symbol_id`、`book_ticker_id`、`latency_ns`、`threshold_ns`、`exchange_ns`、`book_ticker_event_ns`、`book_ticker_local_ns`。 |
| `L2` | Userspace path | 在 L1 基础上从 WebSocket read / dispatch 到交易所 parser / SHM publish 增加本机 userspace 分段时间。 | `drive_read_enter_ns`、`read_return_ns`、`handler_entry_ns`、`parse_done_ns`、`shm_publish_done_ns`、`read_syscall_or_tls_ns`、`ws_dispatch_ns`、`parse_ns`、`shm_publish_ns`、`user_after_read_ns`。 |
| `L3` | Reserved TCP state | 当前只保留 level 名称和配置边界；尚未实现 data session `TCP_INFO` / recv queue 采样。 | 暂无新增字段。 |
| `L4` | RX socket timestamping | 在 L2 基础上复用 WebSocket socket timestamping，plain transport 且内核返回 RX software timestamp 时记录 `kernel_rx_ns`。TLS transport 当前没有 RX software timestamp 提取路径。 | `kernel_rx_available`、`kernel_rx_ns`、`network_or_exchange_ns`、`kernel_queue_ns`。 |

运行期配置位于 `[data_session.diagnostics.latency_outlier]` 和
`[data_session.diagnostics.timestamping]`，详见 `docs/data_session_config.md`。第一版不写 sidecar binary /
CSV；outlier 样本直接写入当前 data session 的 Nova log，log key 为
`data_session_book_ticker_latency_outlier`。默认阈值为 `threshold_ns=5_000_000`，默认限流为
`max_logs_per_second=1000`；`max_logs_per_second=0` 表示完全禁止该 log。

| 字段 | 表面 | 状态 | 单位 / 取值 | 用途 | 删除条件 |
| --- | --- | --- | --- | --- | --- |
| `data_session_book_ticker_latency_outlier` | Nova log key | experiment | warning log | BookTicker source latency 超过阈值时输出一条结构化诊断。 | 诊断方案改为稳定 CSV / binary schema 后重审。 |
| `exchange` / `source_id` / `symbol_id` / `book_ticker_id` | `data_session_book_ticker_latency_outlier` | experiment | enum / integer | 关联交易所、n 路 source、symbol 和交易所 update id。 | 同上。 |
| `latency_ns` / `threshold_ns` | `data_session_book_ticker_latency_outlier` | experiment | ns | 主判定口径；当前 `latency_ns = book_ticker_local_ns - exchange_ns`。 | 同上。 |
| `exchange_ns` / `book_ticker_event_ns` / `book_ticker_local_ns` | `data_session_book_ticker_latency_outlier` | experiment | 交易所 / 本机时间戳 ns | 复核原始 latency 计算输入；Gate `exchange_ns` 使用 SBE `bbo.time` WebSocket server send timestamp，`book_ticker_event_ns` 使用 `bbo.t` engine update timestamp；Binance 分别对应 `E` 和 `T`；Bitget `books1` 分别对应 `sts * 1000` 和 `ts * 1000`，缺少 `sts` 的 probe frame 写 `exchange_ns = event_ns`。 | 同上。 |
| `kernel_rx_available` / `kernel_rx_ns` | `data_session_book_ticker_latency_outlier` | experiment | bool / 本机 Unix epoch ns | L4 RX software timestamp 是否可用及其原始时间；不可用时不要把 `kernel_rx_ns=0` 当真实时间。 | socket timestamping 方案替换或删除后同步更新。 |
| `drive_read_enter_ns` / `read_return_ns` / `handler_entry_ns` | `data_session_book_ticker_latency_outlier` | experiment | 本机 Unix epoch ns | L2 本机 read / WebSocket dispatch 分段输入。 | userspace 分段诊断不再使用时删除。 |
| `parse_done_ns` / `shm_publish_done_ns` | `data_session_book_ticker_latency_outlier` | experiment | 本机 Unix epoch ns | L2 交易所 parser / decoder 完成和 SHM publish 完成时间。 | 同上。 |
| `network_or_exchange_ns` / `kernel_queue_ns` | `data_session_book_ticker_latency_outlier` | experiment | ns，缺失为 `-1` | L4 可用时把 `exchange_ns -> kernel_rx_ns -> drive_read_enter_ns` 拆开；仍受交易所 timestamp 语义和时钟偏移影响。 | 同上。 |
| `read_syscall_or_tls_ns` / `ws_dispatch_ns` / `parse_ns` / `shm_publish_ns` / `user_after_read_ns` | `data_session_book_ticker_latency_outlier` | experiment | ns，缺失为 `-1` | 判断主要延迟是否出在 read/TLS、WebSocket dispatch、交易所 parser、SHM publish 或 read 后 userspace。 | 同上。 |
| `read_bytes` / `transport` | `data_session_book_ticker_latency_outlier` | experiment | bytes / `plain` 或 `tls` | 记录触发 outlier 的 read 批次大小和 transport 类型。 | 同上。 |

### Data Session Tool 运行日志字段

这些字段来自 `tools/gate/data_session.cpp`、`tools/binance/data_session.cpp` 和
`tools/bitget/bitget_data_session.cpp` 的常规 Nova log，不受
`AQUILA_DATA_SESSION_DIAG_LEVEL` 控制，也不是 outlier diagnostic schema。

| 字段 | 表面 | 状态 | 单位 / 取值 | 用途 | 删除条件 |
| --- | --- | --- | --- | --- | --- |
| `book_ticker count` | data session Nova log | stable | count | 每发布 1000 条 `BookTicker` 输出一次采样日志，用于确认 data session 仍在发布 BBO。 | 工具日志 schema 被统一 metrics 取代后重审。 |
| `trade count` | data session Nova log | stable | count | 每发布 1000 条 `Trade` 输出一次采样日志，用于确认 Gate / Bitget SBE `publicTrade` 或 Binance raw trade feed 仍在发布。 | 同上。 |
| `book_tickers` / `trades` | data session result summary Nova log | stable | count | data session 退出时汇总本进程已发布到 sink 的 BBO / trade 数量。 | 同上。 |

## DataReader diagnostics

这些字段来自 `core/market_data/realtime_data_reader.h` 的 `RealtimeDataReaderStats`、
`core/market_data/historical_data_reader.h` 的 `HistoricalDataReaderStats`，以及
`tools/market_data/data_reader_probe.cpp` / `tools/market_data/data_reader_recorder.cpp` 的常规 Nova log。
stats 由编译期 diagnostics policy 启用；生产默认 no-op diagnostics policy 不维护这些计数。

| 字段 | 表面 | 状态 | 单位 / 取值 | 用途 | 删除条件 |
| --- | --- | --- | --- | --- | --- |
| `total_count` | `RealtimeDataReaderStats` / `HistoricalDataReaderStats` / probe summary / recorder summary | stable | count | reader 已交给 handler 的 `BookTicker` + `Trade` 总事件数。 | reader diagnostics schema 替换后同步迁移。 |
| `book_ticker_count` | `RealtimeDataReaderSourceStats` / `HistoricalDataReaderStats` / probe source log / recorder `source_stats` log | stable | count | 单个 realtime source 或 historical reader 已输出的 `BookTicker` 数量。 | 同上。 |
| `trade_count` | `RealtimeDataReaderSourceStats` / `HistoricalDataReaderStats` / probe source log / recorder `source_stats` log | stable | count | 单个 realtime source 或 historical reader 已输出的 `Trade` 数量。 | 同上。 |
| `skipped` | `RealtimeDataReaderSourceStats` / probe source log / recorder `source_stats` log | stable | count | `read_mode=latest` 主动跳过的可见旧消息数量；不等同于 ring overrun。 | 同上。 |
| `overruns` | `RealtimeDataReaderSourceStats` / probe source log / recorder `source_stats` log | stable | count | reader 落后超过 SHM ring capacity 后被拉回可见窗口的次数。 | 同上。 |
| `last_book_ticker_id` | `RealtimeDataReaderSourceStats` / probe source log / recorder `source_stats` log | stable | exchange update id | 单个 source 最近输出的 `BookTicker.id`。 | 同上。 |
| `last_trade_id` | `RealtimeDataReaderSourceStats` / probe source log / recorder `source_stats` log | stable | exchange trade id | 单个 source 最近输出的 `Trade.id`。 | 同上。 |
| `handler_book_tickers` / `handler_trades` | `data_reader_probe` result summary Nova log | stable | count | probe handler 实际收到的 BBO / trade 数量，用于和 reader diagnostics 对账。 | probe summary schema 替换后同步迁移。 |
| `handler_book_tickers` / `handler_trades` | `data_reader_recorder` result summary Nova log | stable | count | recorder 已分别写入 `BookTicker` / `Trade` binary 的记录数。 | recorder summary schema 替换后同步迁移。 |
| `book_ticker_output` / `trade_output` | `data_reader_recorder` result summary Nova log | stable | path | 本次 recorder 实际写出的 BookTicker / Trade typed binary 单文件路径；rotation 模式下作为启动参数和默认派生路径记录，真实 segment 见 manifest。 | recorder output schema 替换后同步迁移。 |
| `recorder_write_error book_ticker_output trade_output` / `recorder_flush_error book_ticker_output trade_output` | `data_reader_recorder` error Nova log | stable | path | recorder 写入或 flush 失败时同时打印两个 binary artifact 路径，避免 BookTicker / Trade 任一路失败时排障路径混淆。 | recorder error schema 替换后同步迁移。 |
| `book_ticker_segments_completed` / `trade_segments_completed` | `data_reader_recorder` result summary Nova log | stable | count | rotation 模式下每个 feed 已完成并写入 manifest 的 segment 数；单文件模式固定为 `0`。 | recorder rotation schema 替换后同步迁移。 |
| `format` / `version` / `feed` / `header_bytes` / `record_size` | recorder rotation manifest JSONL | stable | typed binary metadata | 每个已关闭 segment 的 typed-header metadata；`bytes` 包含 16-byte header，`records` 是 payload record 数。脚本可用这些字段做 manifest preflight，但二进制文件 header 仍是事实源。 | typed binary format v1 被替换后同步迁移。 |
| `recorder_output feed=book_ticker\|trade path` | `data_reader_recorder` startup Nova log | stable | path | 启动阶段记录 BookTicker / Trade 输出路径，便于从统一 recorder log 找到两个 artifact。 | recorder output schema 替换后同步迁移。 |
| `recorder_rotation feed=book_ticker\|trade` | `data_reader_recorder` startup Nova log | stable | config | 启动阶段记录每个 feed 的 rotation `enabled`、`interval_sec`、`output_dir`、`file_prefix` 和 `manifest_path`。 | recorder rotation schema 替换后同步迁移。 |
| `recorder_stats feed=book_ticker\|trade` / `exchange_stats feed=book_ticker\|trade` | `data_reader_recorder` Nova log | stable | count / ns | 按 feed 拆分的 total records、per-exchange records、first / last `exchange_ns` 和 `local_ns`，用于和两个独立 binary artifact 对账。 | recorder stats schema 替换后同步迁移。 |

## Market Data Fusion

BookTicker / Trade fastest-route fusion 当前共用 `AQUILA_FUSION_METADATA_MODE=file|off`
在编译期决定是否启用 sidecar metadata。`file` 模式写 `fusion_metadata.bin`；`off` 模式不打开
metadata 文件，不构造 metadata record，但仍保留基础 read / publish 运行统计。

| 字段 | 表面 | 状态 | 单位 / 取值 | 用途 | 删除条件 |
| --- | --- | --- | --- | --- | --- |
| `metadata_enabled` | `book_ticker_fusion` / `trade_fusion` CLI stdout / stderr、data fusion Nova log | stable | `true` / `false` | 表示当前 build 是否编译启用 fusion sidecar metadata，避免把空 `metadata_output` 误读为写入失败。 | metadata policy 被其他稳定 build metadata 取代后重审。 |
| `metadata_output` | `book_ticker_fusion` / `trade_fusion` CLI stdout / stderr、data fusion dry-run log | stable | path 或 `disabled` | metadata enabled 时记录 sidecar binary 路径；disabled 时固定为 `disabled`。 | 同上。 |
| `fusion_metadata_write_errors` / `metadata_write_errors` | data fusion Nova log / `book_ticker_fusion` / `trade_fusion` CLI stdout | stable | count | metadata enabled 时记录 sidecar write failure 计数；metadata disabled build 中固定为 `0`。 | 同上。 |
| `fusion_total_read_count` / `fusion_total_published_count` | data fusion Nova log | stable | count | fusion thread 停止时汇总 source SHM read 数和 canonical SHM publish 数。 | fusion summary schema 被替换后同步更新。 |
| `source_id` / `symbol_id` / `record_id` / `exchange_ns` / `event_ns` / `source_local_ns` / `fusion_publish_ns` | BookTicker / Trade `FusionMetadataRecord` sidecar binary v2 | stable | id / ns | 记录 canonical record 由哪个 source 首先发布；BookTicker `record_id=BookTicker.id` 且 `event_ns=BookTicker.event_ns`，Trade `record_id=Trade.id` 且 `event_ns=Trade.event_ns`。 | metadata binary schema 被替换后同步更新。 |

## Bitget REST emergency helper

组件入口：`scripts/bitget/trading/emergency_flatten_futures.py`。

| 字段 | 表面 | 状态 | 单位 / 取值 | 用途 | 删除条件 |
| --- | --- | --- | --- | --- | --- |
| `post_cancel_positions` | emergency helper JSON summary | stable | position snapshot 列表 | 保留撤销初始 open orders 后、提交 reduce-only close 前的 REST position；`initial_positions` 始终保留 mutation 前 snapshot，不再被覆盖。 | helper summary schema 被统一 reconcile artifact 取代后重审。 |
| `orders_cancelled[].phase` / `scope` / `symbol` / `request` / `response` | emergency helper JSON summary | stable | `before_close` / `after_close`、`symbol` / `category`、UTA request/response | 记录每次按 symbol 或 dedicated category 执行的幂等范围撤单动作；它是 mutation audit，不单独证明订单已经全部撤销，最终结果仍只信保守 REST flat snapshot。 | helper summary schema 被统一 reconcile artifact 取代后重审。 |

## Bitget OrderSession

组件入口：

- `exchange/bitget/trading/order_session.h`
- `exchange/bitget/trading/order_session_runtime_adapter.h`
- `tools/bitget/bitget_order_session_probe.cpp`

当前字段只覆盖单路 Bitget UTA v3 `OrderSession` 的 login、place/cancel operation response 和
login-only probe。它们不表示订单已 accepted、filled 或 cancelled，也不能替代后续 feedback / reconcile 事实源。

| 字段 | 表面 | 状态 | 单位 / 取值 | 用途 | 删除条件 |
| --- | --- | --- | --- | --- | --- |
| `bitget_order_session_connected` | Nova log key | experiment | info log | private WebSocket 进入 active 后记录 endpoint 和本地容量。 | Bitget session 连接日志被统一交易 session metrics 取代后重审。 |
| `host` / `port` / `target` | `bitget_order_session_connected` / `bitget_order_session_probe_config` | experiment | endpoint 文本 | 确认当前只使用配置中的 high availability private endpoint。 | 同上。 |
| `inflight` / `request_map_capacity` / `order_id_cache_capacity` | `bitget_order_session_connected` / `bitget_order_send` / config log | experiment | count | 观察单连接 request correlation 和 cancel order-id cache 容量；它们不是 UID/account rate budget。 | gateway 级稳定 metrics 落地后重审。 |
| `bitget_order_session_phase` | Nova log key | experiment | warning log | 断线、reconnect backoff、closing 或 closed 时记录清理前状态和 WebSocket 错误来源。 | reconnect 诊断被统一 session metrics 取代后重审。 |
| `phase` / `last_error` / `reconnect_trigger` / `reconnect_errno` | `bitget_order_session_phase` / `bitget_order_session_summary` | experiment | enum / errno | 区分 heartbeat timeout、协议错误、peer close 和底层 socket 错误。 | 同上。 |
| `active_before` / `login_ready_before` / `inflight_before` | `bitget_order_session_phase` | experiment | bool / count | 说明连接状态变化是否清理了已登录 session 或未完成 operation correlation。 | 同上。 |
| `bitget_order_send` | Nova log key | experiment | info log | 记录 place/cancel request 已提交给 WebSocket 写路径；不是 exchange ACK。 | 稳定 order gateway 发送 metrics 替代逐笔日志后重审。 |
| `request_type` / `request_sequence` / `local_order_id` | `bitget_order_send` / `bitget_order_response` / `bitget_order_response_error` | experiment | enum / id | 关联单连接 operation request、response 和 core order。 | correlation schema 变化时同步迁移。 |
| `client_oid` | `bitget_order_send` / `bitget_order_response` | stable | 固定 29 字符 `a1-<12 Base32>-<13 Base36>` | 保存完整普通订单 wire identity；可按固定 offset 解析 namespace 与 `local_order_id`，再通过 archived manifest 关联 `run_id`。Response log 从当前已验证的 request identity 重建，不借用 simdjson parser storage。 | 普通 Bitget order identity schema 升级时同步迁移，不能单独删除。 |
| `client_oid_run_namespace` | OrderSession / gateway / feedback / RTT config log | stable | 12 字符大写 Crockford Base32；`000000000000` 为保留模板 | 核对一个 run 的所有 producer/consumer namespace；模板只允许 login-only/dry-run，任何订单或 feedback connect 必须拒绝。 | run identity 不再使用 `clientOid` namespace 时同步迁移。 |
| `request_send_local_ns` | `bitget_order_send` / `bitget_order_response` | experiment | 本机 Unix epoch ns | request 提交给 WebSocket 发送路径前的本地时间；不表示 TCP 已完整写出。 | 被更精确 write timestamp 取代后重审。 |
| `request_send_realtime_ns` | `bitget_order_response` | experiment | Unix epoch ns | 记录调用 WebSocket send 前的 realtime 时点；`bitget_order_send` 的同一时点统一使用 `request_send_local_ns`，不再输出重复别名。 | 订单时间分析迁移到稳定二进制诊断或等价 schema 后重审。 |
| `request_send_monotonic_ns` | `bitget_order_send` / `bitget_order_response` | experiment | monotonic ns | 记录调用 WebSocket send 前的 monotonic 时点，本机 duration 使用该时钟。 | 同上。 |
| `order_encode_done_realtime_ns` | `bitget_order_send` | experiment | Unix epoch ns | 记录 place/cancel JSON 编码完成时点；不是 socket write 或交易所 ingress。 | 同上。 |
| `bitget_order_response` | Nova log key | experiment | info log | 记录可关联 operation response；success 仅为通用 ACK，不确认订单终态。 | feedback 与 operation response 日志统一后重审。 |
| `response_kind` | `bitget_order_response` | experiment | `kAck` / `kRejected` / `kCancelRejected` / `kUnknownResult` | 保留 Bitget operation response 语义，防止 cancel ACK 被误读为 cancelled。 | response contract 变化时同步更新。 |
| `exchange_order_id` | `bitget_order_response` / `bitget_order_response_error` | experiment | Bitget order id，缺失为 `0` | place ACK 可用于 session cancel cache；值非零仍不表示 accepted。 | order identity contract 变化时同步更新。 |
| `error_code` | `bitget_order_response` / `bitget_order_response_error` | experiment | Bitget numeric code | 区分明确业务拒绝与 `40010` / `40725` / `45001` 等 `UnknownResult`。 | error classifier 改版时同步更新。 |
| `local_receive_ns` / `exchange_ns` | `bitget_order_response` / `bitget_order_response_error` | experiment | 本机 / Bitget Unix epoch ns | response ingress 本地取时和 Bitget `ts * 1_000_000`；跨机器差值不作为单程网络延迟。 | timestamp contract 变化时同步更新。 |
| `ack_rtt_ns` | `bitget_order_response` | experiment | ns，缺失为 `-1` | 本机同一时钟口径 `local_receive_ns - request_send_local_ns`；包含本机排队、网络和交易所 operation response。 | 更精确 write-to-response RTT 替代后重审。 |
| `write_complete_realtime_ns` / `write_complete_monotonic_ns` | `bitget_order_response` | experiment | Unix epoch ns / monotonic ns，缺失为 `0` | WebSocket 整帧在本机底层 write 完成的时点；连接在完成前中断时保持 unavailable，不用 `SendText` 返回时间代替。 | 同上。 |
| `ack_receive_realtime_ns` / `ack_receive_monotonic_ns` | `bitget_order_response` | experiment | Unix epoch ns / monotonic ns | operation response 进入 handler 时的成对本机时点。 | 同上。 |
| `ack_rtt_monotonic_ns` / `write_complete_to_ack_monotonic_ns` | `bitget_order_response` | experiment | ns，缺失为 `-1` | 使用 monotonic 计算 send→Ack 与完整 write→Ack 的本机 duration；后者不等价于交易所处理时间。 | 同上。 |
| `place_creation_time_ms` / `exchange_message_time_ms` | `bitget_order_response` | experiment | Bitget Unix epoch ms，缺失为 `0` | 原样保留 place response `args[].cTime` 与顶层 `ts`；跨本机/交易所时钟差不能直接解释为单程时延。 | Bitget 不再提供字段或迁移到稳定订单审计 schema 后重审。 |
| `connection_id_hash` | `bitget_order_response` | experiment | FNV-1a uint64，缺失为 `0` | 对 Bitget `connId` 做不可逆 join，避免在日志保存完整 connection id。 | 稳定 connection generation id 落地后重审。 |
| `bitget_order_response_error` | Nova log key | experiment | warning log | runtime adapter 在 dispatch 前记录明确拒绝和 `UnknownResult`；不记录 `msg` 原文。 | 统一 error metrics 落地后重审。 |
| `bitget_order_session_login` | Nova log key | experiment | info / warning log | login-only probe 记录 ready/not-ready 回调次数。 | probe 被稳定 runbook 工具取代后重审。 |
| `bitget_order_session_probe_config` | Nova log key | experiment | info log | dry-run 记录非敏感配置和是否显式 `--connect`。 | probe config 输出 schema 稳定后改为 `stable`。 |
| `bitget_order_session_summary` | Nova log key | experiment | info log | login-only probe 退出时汇总 login、application ping/pong、reconnect 和 WebSocket metrics。 | 稳定监控面替代 probe summary 后重审。 |
| `completed_requested_duration` / `ever_ready` | Bitget order / feedback probe summary | experiment | bool | 只有 probe 由 duration timer 控制停止且曾进入 ready，summary `result` 才可成功；防止短暂 ready 后提前退出被误报。 | probe outcome contract 被统一 runbook 取代后重审。 |
| `login_sent` / `login_accepted` / `login_rejected` | `OrderSessionStats` / summary | experiment | count | 观察 private login state；不证明 UTA trade permission 或账户 position mode。 | 同上。 |
| `pings_sent` / `pongs_received` / `heartbeat_timeouts` | `OrderSessionStats` / summary | experiment | count | 观察 Bitget application-level text `ping` / `pong` 和超时重连。 | 同上。 |
| `text_messages` / `parse_errors` / `ignored_messages` / `responses` / `unknown_request_ids` / `correlation_mismatches` / `local_send_failures` | `OrderSessionStats` | experiment | count | 观察 parser、关联和本地发送失败；unknown/mismatch 不会错误关联其他订单。 | 同上。 |

凭据边界：日志只允许记录 `api_*_env` 的变量名，不记录 API key、secret、passphrase、signature 或完整 login payload。
login-only probe 不提供真实订单命令，也不把 login success 外推为订单正确性、fillability 或交易延迟证据。

### Bitget OrderSession RTT probe

组件入口：`tools/bitget/order_session_rtt_probe/`。该工具的 Bitget CSV schema 与 Gate RTT probe 独立；Bitget 当前拿不到的
Gate write-path、socket timestamping、`x_in` / `x_out` 字段不会写成固定 0。以下字段均为 `experiment`，在 schema 或 live
runbook 稳定后重审。

| 字段 | 表面 | 状态 | 单位 / 取值 | 用途 | 删除条件 |
| --- | --- | --- | --- | --- | --- |
| `bitget_order_session_rtt_probe_start` / `bitget_order_session_rtt_probe_preflight` / `bitget_order_session_rtt_probe_summary` | Nova log key | experiment | 启动 / preflight / summary | 明确运行模式、静态输入和最终结果；dry-run / preflight 固定记录 `orders_sent=0`。 | CLI / runbook 被稳定调度器替代后重审。 |
| `credentials_read` / `shm_attached` / `websocket_created` | preflight log | experiment | 固定 `false` | 证明 `--live-preflight` 没有读取 credential、attach feedback SHM 或创建 WebSocket。 | preflight contract 变化时同步更新。 |
| `rest_guard_implemented` / `flat_proven` | start / preflight / summary / metadata | experiment | 固定 `false` | 防止把 dedicated-account 确认或 safety close 误读为 REST baseline / run-end flat 证据。 | REST guard 与 reconcile 真正落地后替换。 |
| `run_id` / `session_name` / `group` / `host` / `connect_ip` / `port` / `worker_cpu` | Bitget `order_session_rtt_samples.csv` | experiment | join key / endpoint / CPU | 关联 run、CSV connections row、配置 endpoint 与 owner 线程。 | Bitget probe schema 升级后同步迁移。 |
| `owner_thread_cpu` / `owner_thread_tid` | sample / connection observed CSV | experiment | CPU / Linux tid，未知为 `-1` | 核对 session worker 绑核和 sample 所属 owner。 | 同上。 |
| `cycle_index` / `sample_index` / `local_order_id` / `close_local_order_id` | sample CSV | experiment | 0-based index / id | 复核严格串行 cycle 及 probe / safety-close ID 路由。 | 同上。 |
| `request_sequence` / `close_request_sequence` / `exchange_order_id` / `close_exchange_order_id` | sample CSV | experiment | Bitget request / order id | 关联 place 与 safety-close operation response；exchange id 非零仍不表示 terminal。 | 同上。 |
| `symbol` / `side` / `quantity` / `price` / `bbo_ticker_id` / `bbo_local_ns` | sample CSV | experiment | wire 文本 / id / Unix epoch ns | 复核选定 symbol、最低数量、passive IOC 价格和 BBO freshness 锚点。 | 同上。 |
| `request_send_ns` / `response_receive_ns` / `response_exchange_ns` / `ack_rtt_ns` | sample CSV | experiment | Unix epoch ns / ns | 记录 probe place operation response RTT；`ack_rtt_ns` 是本机 request send 到 operation response receive，不是 accepted / fill latency。 | 同上。 |
| `response_kind` / `error_code` / `connection_id_hash` | sample CSV | experiment | enum / code / hash | 保留 Bitget direct response 语义和 connection join；`kAck` 仍不是订单 terminal。 | 同上。 |
| `close_request_send_ns` / `close_response_receive_ns` / `close_response_exchange_ns` / `close_ack_rtt_ns` / `close_response_kind` / `close_error_code` / `close_connection_id_hash` | sample CSV | experiment | ns / enum / code / hash | 审计唯一一次 reduce-only safety-close operation response；缺失不伪造成功。 | 同上。 |
| `terminal_feedback_kind` / `terminal_feedback_local_ns` / `terminal_feedback_exchange_ns` / `terminal_finish_reason` / `cumulative_fill` | sample CSV | experiment | enum / Unix epoch ns / quantity | 以 feedback SHM 事实确认 zero-fill cancelled terminal，或记录意外成交。 | 同上。 |
| `outcome` / `invalid` / `invalid_reason` / `unexpected_fill` / `safety_close_requested` / `safety_close_sent` / `safety_close_confirmed` / `safety_close_filled_quantity` | sample CSV | experiment | enum 文本 / bool / quantity | 排除 reject、unknown、timeout、continuity 和成交样本；`safety_close_confirmed` 只表示 close filled 覆盖已观察 fill，不证明账户 flat。 | 同上。 |
| `sample_csv_schema_version` / `session_count` / `samples_per_session` / `feedback_strategy_id` / `order_mode` / `order_symbol` / `client_oid_schema` / `client_oid_run_namespace` / CSV paths | Bitget `order_session_rtt_run_metadata.json` | experiment | version / count / text / path | 固化 run 级 schema、lane、目标 symbol、`clientOid` namespace 与证据路径；当前 CSV schema version 为 `1`，普通订单 identity schema 为 `a1`。 | schema 迁移完成后重审。 |
| `configured_host` / `configured_connect_ip` / `configured_port` / `connected_at_ns` / `endpoint_available` / `local_ip` / `local_port` / `remote_ip` / `remote_port` | Bitget `order_session_rtt_connections_observed.csv` | experiment | endpoint / Unix epoch ns | 在下单前保存真实 socket endpoint；指定 numeric IP 时必须与 observed remote IP 一致。 | 同上。 |

## Bitget OrderFeedbackSession

组件入口：

- `exchange/bitget/trading/order_feedback_parser.h`
- `exchange/bitget/trading/order_feedback_session.h`
- `tools/bitget/bitget_order_feedback_session.cpp`

`Ready()` 仅表示 private WebSocket login 与 account-wide `order` subscription ACK 均成功，不表示 REST baseline、初始
open-order snapshot、sequence continuity 或 reconcile 已完成。V1 只从 `order` 发布累计 order facts；同一连接 best-effort
订阅 `fast-fill` 仅输出分析日志，不发布 `OrderFeedbackEvent`、不影响 `Ready()`，也不推导交易状态、累计 quantity 或
feedback reject。

| 字段 | 表面 | 状态 | 单位 / 取值 | 用途 | 删除条件 |
| --- | --- | --- | --- | --- | --- |
| `bitget_order_feedback_session_config` | probe Nova log key | experiment | config summary | dry-run 记录 endpoint、V1 scope、credential env name 和 SHM 参数；不记录 credential value。 | config 输出 schema 稳定后改为 `stable`。 |
| `bitget_order_feedback_session_phase` | session Nova log key | experiment | connection phase | 记录 active / disconnect / reconnect phase、generation 和底层错误来源。 | 被统一 private session metrics 取代后重审。 |
| `bitget_order_feedback_login` | session Nova log key | experiment | accepted bool / code | 记录 login 结果；成功不表示 trade permission 或订单状态连续。 | 同上。 |
| `bitget_order_feedback_subscribe` | session Nova log key | experiment | accepted bool / code | 记录匹配 `instType=UTA, topic=order` 的 subscription ACK 或 error。 | 同上。 |
| `bitget_fast_fill_subscribe` | session Nova log key | experiment | accepted bool / code | 记录 best-effort `fast-fill` subscription 结果；普通失败不回退 authoritative `order` ready，认证失效仍重连整个 private session。 | fast-fill 分析结束并移除订阅，或升级为经过独立 L3 审查的交易 contract 后重审。 |
| `bitget_order_feedback_raw_update` | session Nova log key | experiment | order fact fields | 记录已映射 event 的 local/exchange order id、累计量、剩余量、撤销量、均价、时间戳和 publish 结果。 | 稳定 event metrics 替代逐笔日志后重审。 |
| `bitget_order_feedback_protocol_update` | session Nova log key | experiment | raw order protocol fields | 原样记录 `clientOid`、`orderId`、`orderStatus`、`cancelReason`、顶层 `ts`、`createdTime`、`updatedTime`，并附 `connection_generation`、本地 message sequence、batch index 和成对 receive clocks。 | 当前成交可达性分析结束或迁移到稳定审计 schema 后删除。 |
| `bitget_order_feedback_client_oid_ignored` | session Nova log key | stable | `reason=kForeignRunNamespace` / `kLegacyClientOid`，附完整 `client_oid`、generation、message sequence、batch index、exchange time | 审计合法 foreign namespace 或历史 `a-<decimal>` 被隔离；这两类记录不发布 SHM，也不触发 continuity failure。 | feedback identity schema 升级且历史/foreign 隔离由等价稳定审计面替代时迁移。 |
| `bitget_fast_fill_raw_update` | session Nova log key | experiment | raw fast-fill protocol fields | 原样记录 `execId`、`clientOid`、`orderId`、symbol、side、holdSide、exec price/quantity、tradeScope、顶层 `ts`、`execTime`、`updatedTime` 及本地排序/receive 字段；只供离线分析。 | 当前 fast-fill 快慢分析结束；若未来进入交易链路，必须由新 contract 替代，不能复用本日志作为状态源。 |
| `bitget_order_feedback_validation_error` | session Nova log key | experiment | parser status / count | 记录 envelope、required field、scope、quantity、status 或 timestamp 不能安全映射的错误。 | parser error metrics 稳定后重审。 |
| `bitget_fast_fill_validation_error` | session Nova log key | experiment | parser status / count | 记录已分类 fast-fill envelope 的必需原始字段不能安全解码；不发布 order continuity lost。共享连接上无法识别 topic 的语法损坏 JSON 仍按 order ingress 保守处理。 | fast-fill 分析订阅删除后同步删除。 |
| `bitget_order_feedback_continuity_lost` | session Nova log key | experiment | reason / generation / publish result | 记录 `kSessionDisconnected` 或 `kDecodeUnrecoverable` 全局广播；同 generation decode 错误只广播一次。 | 外部 recovery controller 提供统一 continuity metrics 后重审。 |
| `bitget_order_feedback_session_summary` | probe Nova log key | experiment | count / enum | login/subscribe-only probe 退出时汇总 session、parser、publisher 和 WebSocket metrics。 | 稳定监控面替代 probe summary 后重审。 |
| `text_messages` / `parse_errors` / `ignored_messages` | `OrderFeedbackSessionStats` | experiment | count | 观察 text ingress、控制或 order data 解码错误及明确忽略的 stale/foreign control。 | stats schema 稳定后重审。 |
| `login_sent` / `login_accepted` / `login_rejected` / `subscribe_sent` / `subscribe_acks` / `subscribe_errors` | `OrderFeedbackSessionStats` / summary | experiment | count | 观察 authentication/subscription 状态机，不表示 REST recovery 完成。 | 同上。 |
| `fast_fill_subscribe_sent` / `fast_fill_subscribe_send_failures` / `fast_fill_subscribe_acks` / `fast_fill_subscribe_errors` / `fast_fill_records` / `fast_fill_parse_errors` | `OrderFeedbackSessionStats` / summary | experiment | count | 对账 best-effort fast-fill 订阅、原始记录与解析错误；这些计数不参与交易 ready 或 continuity。 | fast-fill 分析订阅删除后同步删除。 |
| `order_envelopes` / `orders_seen` / `events_emitted` / `foreign_orders_ignored` / `foreign_run_namespace_orders_ignored` / `legacy_client_oid_orders_ignored` / `unroutable_orders_ignored` / `legacy_canceled_statuses` / `validation_errors` | `OrderFeedbackParserStats` / summary | experiment | count | 对账 account-wide order envelope 的 ownership、run namespace 隔离、历史 identity 和 semantic validation。 | 同上。 |
| `fast_fill_foreign_run_namespace_records_ignored` / `fast_fill_legacy_client_oid_records_ignored` / `rest_foreign_run_namespace_records_ignored` / `rest_legacy_client_oid_records_ignored` | Bitget live report stats / `report.md` | stable | count | Report 从 guard runtime isolation 取得当前 namespace，统计并排除 foreign/legacy fast-fill 与 REST fills，防止复用 `local_order_id` 污染本 run 对账。 | report 不再消费 account-wide fast-fill/REST fills，或由等价 run-scoped source 取代时重审。 |
| `events_published` / `publish_failures` / `decode_continuity_lost_events` / `disconnect_continuity_lost_events` / `global_continuity_lost_publish_failures` | `OrderFeedbackSessionStats` / summary | experiment | count | 对账普通 event 与 continuity event 的 SHM 发布结果；publish failure 不阻塞 WebSocket owner thread。 | 同上。 |
| `producer_pid` / `producer_run_id` | `OrderFeedbackShmHeader` | stable | pid / uint64 run id | publisher owner 启动时登记；复用已有非零 run id 的 channel 会先广播全局 `kProducerRestart`，标记 crash/restart 未知窗口。 | SHM producer ownership contract 改版时同步迁移。 |
| `pings_sent` / `pongs_received` / `heartbeat_timeouts` | `OrderFeedbackSessionStats` / summary | experiment | count | 观察 Bitget application-level text heartbeat 和 timeout reconnect。 | 同上。 |

凭据边界：只允许记录 `api_key_env` / `api_secret_env` / `api_passphrase_env` 的变量名；禁止记录 API key、secret、
passphrase、signature 或完整 login payload。未知 `cancelReason` 映射 `kUnknown`，不触发 continuity lost；disconnect / decode
continuity 必须由 session 外的恢复控制器进入 REST reconcile。

## Bitget OrderGateway

组件入口：

- `exchange/bitget/trading/order_gateway_worker.h`
- `tools/bitget/bitget_order_gateway.cpp`
- `config/order_gateways/bitget_order_gateway.toml`

Gateway 复用现有 OrderGateway SHM ABI。以下字段覆盖进程 dry-run、route 装配、启动错误和 CPU affinity 失败；place/cancel 的
逐请求 response 仍由 `bitget_order_send` / `bitget_order_response` 和 SHM event 诊断。日志不表示订单 terminal，也不替代
FeedbackSession 或 REST reconcile。

| 字段 | 表面 | 状态 | 单位 / 取值 | 用途 | 删除条件 |
| --- | --- | --- | --- | --- | --- |
| `bitget_order_gateway_dry_run` | gateway Nova log key | experiment | config summary | 默认或 `--validate-only` 时记录 name、SHM name、route count、queue capacity 和 startup timeout；不创建 SHM、不连接网络。 | gateway config 输出 schema 稳定后改为 `stable`。 |
| `bitget_order_gateway_route` | gateway Nova log key | experiment | per-route config | 记录 route id/name、worker/session CPU、session config path、host、connect IP 和 TLS；不记录 credentials。 | route diagnostics 被统一 gateway metrics 取代后重审。 |
| `bitget_order_gateway_bind_cpu_invalid` | gateway Nova log key | experiment | CPU id | worker CPU 超出 `CPU_SETSIZE` 时拒绝 affinity 设置。 | 统一 runtime affinity diagnostics 替代后重审。 |
| `bitget_order_gateway_bind_cpu_failed` | gateway Nova log key | experiment | CPU id / errno text | `pthread_setaffinity_np` 失败；route 仍按 Gate 当前 best-effort 语义继续启动。 | 同上。 |
| `bitget_order_gateway_bind_cpu_unsupported` | gateway Nova log key | experiment | CPU id | 非 Linux 平台不支持当前 affinity 操作。 | 同上。 |
| `order_gateway_config_error` | gateway Nova log key | experiment | error text | gateway TOML 解析或 schema 校验失败，进程在连接前退出。 | 统一 gateway startup diagnostics 替代后重审。 |
| `order_gateway_route_error` | gateway Nova log key | experiment | error text | route session config、credential 环境变量或 route 一致性校验失败，进程在连接前退出。 | 同上。 |
| `order_gateway_transport_error` | gateway Nova log key | experiment | error text | route 混用 TLS/plain transport，当前进程拒绝启动。 | 支持 per-route transport 或统一 startup diagnostics 后重审。 |
| `order_gateway_shm_error` | gateway Nova log key | experiment | error text | SHM 创建、复用或 ABI 校验失败，网络 worker 不启动。 | 统一 gateway SHM diagnostics 替代后重审。 |

凭据边界：`--connect` 只在缺失 credential 时记录环境变量名；禁止记录 API key、secret、passphrase 或 login payload。
Gateway 当前不输出 Bitget numeric operation `error_code` 到共享 SHM；是否扩展共享诊断 contract 属于未来独立设计，见
`docs/bitget_trading.md`。

### Bitget Gateway Smoke

组件入口：

- `tools/bitget/gateway_smoke/`
- `scripts/bitget/trading/prepare_gateway_smoke_run.py`
- `scripts/bitget/trading/run_gateway_smoke_with_guard.py`

该 one-shot 工具只用于取得 fanout=1 gateway passive IOC 的授权 live 证据。Gateway direct Ack 与 account-wide
`OrderFeedbackSession` terminal 是相互独立的事实面：Ack 不能替代 terminal，terminal 先到也不能替代 Ack。Runner 成功只表示
entry 以及必要的 reduce-only close 都已取得这两类证据；最终账户 flat 必须以外围 guard 的后续 REST snapshot 为准。

| 字段 | 表面 | 状态 | 单位 / 取值 | 用途 | 删除条件 |
| --- | --- | --- | --- | --- | --- |
| `run_id` / `event_source` / `event_kind` / `order_role` | `order_event.csv` | experiment | join key / `gateway`、`feedback` / event enum / `entry`、`close` | 把本轮 gateway response 与独立 feedback 按订单角色串联；跨 run 审计必须包含 `run_id`。 | Gateway smoke retire 或迁移到稳定 live report schema 时同步删除。 |
| `local_order_id` / `group_id` / `route_id` | `order_event.csv` | experiment | uint64 / uint64 / uint16 | 关联 one-shot execution、entry/close 和实际 gateway route；smoke 将本轮 numeric run id 写入 `group_id`，当前 `route_id=0`。 | 同上。 |
| `response_kind` / `feedback_kind` | `order_event.csv` | experiment | OrderGateway response / OrderFeedback enum | 分别保存 direct operation response 和独立生命周期事实，禁止把 `kAck` 解释为 terminal。 | 同上。 |
| `exchange_order_id` / `exchange_ns` / `local_ns` | `order_event.csv` | experiment | Bitget order id / Unix epoch ns / Unix epoch ns | 关联交易所订单并保留 exchange/local event timestamp；跨时钟差不直接解释为单程延迟。 | 同上。 |
| `price` / `quantity` / `cumulative_filled_quantity` / `left_quantity` | `order_event.csv` | experiment | decimal text / BTC quantity | 对账 entry/close 的价格、原始数量和累计生命周期数量；close quantity 必须等于 entry 累计成交量。 | 同上。 |
| `finish_reason` / `reject_reason` | `order_event.csv` | experiment | enum / text | 保存 terminal 或拒绝原因，用于区分零成交取消、明确拒绝和未知结果。 | 同上。 |
| `run_id` / `final_result` / `failure_reason` | runner `summary.json` | experiment | join key / `success` 或失败状态 / text | 记录 one-shot 状态机最终结果；不包含外围 REST flat 证明。 | 同上。 |
| `entry_local_order_id` / `entry_acked` / `entry_terminal` / `entry_filled_quantity` | runner `summary.json` | experiment | id / bool / bool / BTC quantity | 汇总 entry 是否同时取得 direct Ack 和独立 terminal，以及其累计成交量。 | 同上。 |
| `close_required` / `close_local_order_id` / `close_acked` / `close_terminal` / `close_filled_quantity` | runner `summary.json` | experiment | bool / id / bool / bool / BTC quantity | entry 有成交时汇总同 gateway reduce-only close 的双证据和累计平仓量。 | 同上。 |
| `runtime_isolation.processes.<role>` / `quiescence` / final state | `guard_summary.json` | stable | PID binding / process stop result / REST snapshot | 证明 data session、gateway、feedback 属于本轮且在最终 REST 前已停止；该 summary 的 final flat 才是账户级完成证据。 | 外部 supervisor 与统一 report 提供等价闭环后重审。 |

凭据边界：manifest、CSV、summary 和 log 只允许保存 credential 环境变量名，不得保存 API key、secret、passphrase、signature
或完整 login payload。

## Gate OrderSession

组件入口：

- `exchange/gate/trading/order_session.h`
- `exchange/gate/trading/order_latency_diagnostics.h`
- `exchange/gate/trading/order_session_runtime_adapter.h`
- `tools/gate/strategy_order.cpp`
- `tools/gate/order_session_failure_probe.cpp`

### 连接级字段

这些字段用于区分多条 Gate `OrderSession` / WebSocket 连接的实际路径。连接 active 时低频输出在
`gate_order_session_connected` 中；断线 / reconnect / closing / closed 冷路径输出在
`gate_order_session_phase` 中。DNS `resolved_ips` 仍等待 WebSocket 层暴露稳定 resolver 快照。

| 字段 | 表面 | 状态 | 单位 / 取值 | 用途 | 删除条件 |
| --- | --- | --- | --- | --- | --- |
| `order_session_id` | `gate_order_session_connected` / `gate_order_send_ok` / `gate_order_response` / `gate_order_ack_latency_diagnostic` | experiment | 本进程内单调 id | 关联同一条 `OrderSession` 的 send、ack、diagnostic 和 summary。 | 多 session 诊断停止且所有下游 parser 不再依赖。 |
| `endpoint_available` | `gate_order_session_connected` | experiment | `true` / `false` | 表示本地 / 远端 endpoint snapshot 是否可用；不可用时 endpoint 字段为空或 0。 | endpoint snapshot 不再使用时删除。 |
| `local_ip` | `gate_order_session_connected` / report CSV | experiment | IP 文本 | 记录本地 TCP endpoint，用于区分 NAT / source address。 | 被等价 endpoint snapshot 取代。 |
| `local_port` | `gate_order_session_connected` / report CSV | experiment | TCP port | 区分同 remote endpoint 下的不同连接。 | 被等价 endpoint snapshot 取代。 |
| `remote_ip` | `gate_order_session_connected` / report CSV | experiment | IP 文本 | 判断慢 session 是否落到不同 remote IP / gateway。 | 被等价 endpoint snapshot 取代。 |
| `remote_port` | `gate_order_session_connected` / report CSV | experiment | TCP port | 记录远端 TCP endpoint。 | 被等价 endpoint snapshot 取代。 |
| `owner_thread_cpu` | `gate_order_session_connected` | experiment | Linux CPU id，失败为 `-1` | 确认 session 进入 active 时 owner thread 实际运行 CPU。 | 被更完整 thread affinity / sched snapshot 取代。 |
| `owner_thread_tid` | `gate_order_session_connected` / `gate_order_ack_latency_diagnostic` | experiment | Linux thread id，失败为 `-1` | 将 OrderSession owner thread 与 `pidstat -t`、`perf sched` 等外部调度采样直接对齐。 | 被等价 thread identity snapshot 取代。 |
| `resolved_ips` | `gate_order_session_connected` | planned | DNS 结果列表 | 区分 hostname 相同但 DNS / connect 目标不同的情况。 | WebSocket 层无法稳定提供时可留空；若无消费者可删除。 |
| `phase` | `gate_order_session_phase` | experiment | `ConnectionPhase` enum 文本 | 记录触发清理的连接状态，例如 `kReconnectBackoff` / `kClosed`。 | 断线来源诊断结束且不再对齐 reconnect 时删除。 |
| `last_error` | `gate_order_session_phase` | experiment | `ConnectionError` enum 文本 | 保留 WebSocket 层聚合错误，例如 `kPeerClosed` / `kSocketError` / `kHandshakeFailure`。 | 同上。 |
| `reconnect_trigger` | `gate_order_session_phase` | experiment | `ReconnectTrigger` enum 文本 | 将 `last_error=kPeerClosed` 等聚合错误拆成 read EOF、business write EOF、control write EOF、WebSocket close frame、heartbeat timeout、consumer fatal 等来源。 | reconnect 来源诊断稳定后按保留价值重审。 |
| `reconnect_errno` | `gate_order_session_phase` | experiment | errno，非 syscall 来源为 `0` | 当 `reconnect_trigger` 是 read / write error 时保留底层 errno，用于区分 `EPIPE`、`ECONNRESET` 等系统错误。 | 被更完整 transport error snapshot 取代。 |
| `active_before` / `login_ready_before` | `gate_order_session_phase` | experiment | `true` / `false` | 记录清理前 OrderSession 是否 active / login ready，用于判断是否影响实盘下单状态。 | reconnect 状态机诊断结束后删除。 |
| `inflight_before` / `request_map_capacity` | `gate_order_session_phase` | experiment | count | 记录断开前 submit inflight 数和容量，判断是否存在未完成 request 被清理。 | submit continuity 诊断不再需要时删除。 |

`gate_order_session_phase` 会优先使用当前 fd 的 endpoint snapshot；如果从 active 状态进入断线 phase 前 fd 已关闭，则回退到最近一次 active 时缓存的 endpoint。非 active 状态下的后续 reconnect failure 不复用旧 endpoint，避免把新一轮握手 / 解析失败误归因到上一条连接。

热路径边界：`gate_order_session_phase` 只在连接 state hook 的非 active phase 输出；正常 send / ack 热路径不执行日志格式化，也不采集 endpoint snapshot。WebSocket core 仅在已经进入 reconnect 的错误分支保存 `ReconnectTrigger` / `errno`，正常读写成功分支不写日志。

### IP discovery JSONL 字段

这些字段由 `scripts/gate/diagnostics/discover_gate_ws_ips.py` 写入 `candidate_ips.jsonl`，用于为后续 OrderSession RTT probe
准备候选 `connect_ip`。脚本不登录、不下单，也不修改本机 resolver 配置。

| 字段 | 表面 | 状态 | 单位 / 取值 | 用途 | 删除条件 |
| --- | --- | --- | --- | --- | --- |
| `sources` | `candidate_ips.jsonl` | experiment | `dns_system`、`dns_udp_<ip>_<port>`、`history_log`、`candidate_file` 等 | 记录候选 IP 来自 system resolver、explicit UDP resolver、历史连接日志或既有候选文件。 | IP discovery schema 升级并迁移消费者后删除或重命名。 |
| `dns.resolvers` | `candidate_ips.jsonl` | experiment | resolver label 列表，例如 `system`、`1.1.1.1:53` | 记录该 IP 被哪些 resolver 返回。 | 同上。 |
| `dns.resolver_details` | `candidate_ips.jsonl` | experiment | `{kind,address,port,label}` 对象列表 | 区分 system resolver 与 explicit UDP resolver，便于比较不同 DNS 来源的候选集。 | 同上。 |
| `websocket_verify` | `candidate_ips.jsonl` | experiment | `{attempted,ok,remote_ip,local_port,tcp_connect_ns,tls_handshake_ns,websocket_handshake_ns,total_ns,http_status,error}` | 记录 pinned TCP / TLS / WebSocket handshake 验证结果；不代表 private login 成功。 | 同上。 |
| `login_verify` | `candidate_ips.jsonl` | experiment | `{attempted,ok,latency_ns,status,uid,request_id,conn_id,conn_trace_id,error}` | 启用 `--verify-login` 时记录 `futures.login` 验证结果；只有 login 成功的 IP 会写入 `candidate_ips.txt`。 | 同上。 |

### OrderSession RTT probe CSV 字段

这些字段由第一版 `gate_order_session_rtt_probe` 的 sample CSV schema / writer 定义。连接集合由
`probe.inputs.connections_file` 指向的 CSV 定义，字段为 `name,group,host,connect_ip,port,enable_tls,worker_cpu_id`；
其中 `connect_ip` 允许重复，重复行代表不同 `OrderSession` 连接。sample CSV 采用长表，每行对应一个已提交
order action：GTC open、GTC cancel、可选 GTC safety close、IOC open 或 IOC safety close。当前字段集只保留 RTT
分析必须字段和建议诊断字段，并使用短字段名降低 CSV 宽度。连接级 endpoint / owner CPU 信息不重复写入每行 CSV，
而是由低频 `order_session_rtt_connections_observed.csv` 记录；run 级 schema / build level 信息写入
`order_session_rtt_run_metadata.json`。

CSV contract：

- `order_session_rtt_samples.csv` 使用固定 superset schema，避免不同 `L0..L5` build 产生不同 header 后难以 merge。
- `order_session_rtt_run_metadata.json` 记录 `sample_csv_schema_version`、`order_ack_diag_level`、
  `order_ack_diag_level_name`、编译期 capability 和运行期是否请求 `TCP_INFO` / socket timestamping。
- `diag=true` 时，L2 runtime / write path 字段可信；否则对应时间戳为未采集默认值。
- `socket_sendq_available=true` 时，`tcp_sendq_bytes` / `tcp_notsent_bytes` 可信。
- `tcp_info_requested=true && tcp_info_available=true` 时，`tcp_info_*` 可信。
- `ts_available=true` 时，`ts_*` 原始 timestamp 和阶段耗时可信；`ts_available=false` 时，`ts_* = 0` /
  阶段耗时 `-1` 表示缺归因，不是实际 0。

| 字段 | 表面 | 状态 | 单位 / 取值 | 用途 | 删除条件 |
| --- | --- | --- | --- | --- | --- |
| `run` | `order_session_rtt_samples.csv` | experiment | 文本 | 关联同一次 RTT probe run 的连接 log 与 sample CSV。 | RTT probe schema 升级并迁移消费者后重审。 |
| `session` | `order_session_rtt_samples.csv` | experiment | connections CSV 中唯一 `name` | 稳定区分同一个 `connect_ip` 的多条独立连接，是重复 IP 实验的主 join key。 | 同上。 |
| `group` | `order_session_rtt_samples.csv` | experiment | connections CSV 中 `group` | 按连接类别聚合，例如 `public-<ip>`、`private-10.0.1.154` 或同一实验分组。 | 同上。 |
| `ip` | `order_session_rtt_samples.csv` | experiment | IP 文本 | 被测 Gate TCP 直连 IP，是分组统计主 key。 | 同上。 |
| `sid` | `order_session_rtt_samples.csv` | experiment | 本进程内单调 id | 关联 sample、连接 endpoint 和底层 order session log。 | 同上。 |
| `connection_generation` | planned connection log | planned | 同一 `connect_ip` 内从 0 递增 | 区分同一个指定 IP 断开重连前后的 `OrderSession`，用于 reconnect RTT 对比。 | 同上。 |
| `round` / `sample` | `order_session_rtt_samples.csv` | experiment | 0-based integer | 支持 round-robin 采样顺序分析，避免按 IP 连续采样造成时间窗口偏差。 | 同上。 |
| `contract` | `order_session_rtt_samples.csv` | experiment | Gate contract，例如 `ZEC_USDT` | 标记本次行情触发 cycle 的交易合约；第一版由 Gate `BookTicker` 行情事件决定，不固定只测一个 symbol。 | 同上。 |
| `qty` | `order_session_rtt_samples.csv` | experiment | Gate wire 文本 | 复核 instrument catalog 最小下单量是否符合预期。 | 同上。 |
| `price` | `order_session_rtt_samples.csv` | experiment | Gate wire 文本 | 当前 action 使用的价格；`open` / `cancel` 行使用对应 open price，`close` 行使用 close order price。 | 同上。 |
| `type` | `order_session_rtt_samples.csv` | experiment | `gtc` / `ioc` | 标记该行归属的 probe leg；`gtc,close` 表示为 GTC leg 触发的 safety close，即使 wire TIF 为 IOC。 | 同上。 |
| `action` | `order_session_rtt_samples.csv` | experiment | `open` / `cancel` / `close` | 将旧的 place/cancel/close 阶段规范成长表 action；place 统一写作 `open`。 | 同上。 |
| `local_id` / `req_seq` | `order_session_rtt_samples.csv` | experiment | id / request sequence | 直接关联 sample CSV、`gate_order_send_ok`、`gate_order_response` 和 feedback log。 | 同上。 |
| `bbo_id` / `bbo_ns` | `order_session_rtt_samples.csv` | experiment | `BookTicker.id` / 本机 Unix epoch ns | 记录当前 action 对应 open order 使用的行情版本；没有行情锚点的 action 填 0。 | 同上。 |
| `send_ns` | `order_session_rtt_samples.csv` | experiment | 本机 Unix epoch ns | 当前 action 实际写 socket 前的本地发送时间，用于直接从 CSV 计算 Ack RTT 与近似上行时间。 | 同上。 |
| `ack_recv_ns` / `ack_ex_ns` / `ack_ex2local_ns` / `ack_rtt_ns` | `order_session_rtt_samples.csv` | experiment | ns / 本机 Unix epoch ns / Gate timestamp ns | 当前 action Ack timing；`ack_ex2local_ns` 受时钟偏移影响，`ack_rtt_ns` 是本地 send 到本地 Ack receive。 | 同上。 |
| `diag` / `diag_reason` | `order_session_rtt_samples.csv` | experiment | bool / enum 文本 | 标记该 action 是否拿到了 Ack diagnostic window 快照；当 `ack_rtt_threshold_ns=0` 时每 Ack 都会写入，且不依赖 `gate_order_ack_latency_diagnostic` 日志是否被 `max_logs_per_second` 限流。 | 同上。 |
| `send_to_after_hook_ns` / `send_to_drive_read_ns` / `drive_read_ns` / `max_drive_read_ns` | `order_session_rtt_samples.csv` | experiment | ns | 将 runtime hook 返回、首次进入 `DriveRead()`、Ack 所在 read iteration 耗时和诊断窗口最大 read 耗时写入 sample CSV，用于按连接统计本机 owner runtime 是否卡住。 | 同上。 |
| `inflight_at_send` / `max_loop_gap_ns` / `loop_iters_before_ack` / `owner_tid` | `order_session_rtt_samples.csv` | experiment | count / ns / count / tid | 记录发送瞬间 inflight、Ack 前 owner runtime 最大 loop gap、loop 迭代次数和 owner thread id，用于和调度采样、CPU 绑核结果对齐。 | 同上。 |
| `order_encode_done_ns` / `ws_frame_encode_done_ns` / `write_enqueue_ns` / `drive_write_enter_ns` / `write_some_enter_ns` / `write_some_return_ns` / `write_complete_ns` | `order_session_rtt_samples.csv` | experiment | 本机 Unix epoch ns | 将 Gate payload 编码、WebSocket frame 编码、enqueue、进入写泵、`send()` / `SSL_write()` 前后和完整写入 transport 的时间写入 sample CSV，用于直接统计本机写路径分段。 | 同上。 |
| `write_some_bytes` / `write_complete_bytes` / `write_errno` / `write_eagain` / `pending_write_count_after` | `order_session_rtt_samples.csv` | experiment | bytes / errno / bool / count | 校验写路径是否 partial write、EAGAIN、错误或 pending write queue 积压。 | 同上。 |
| `socket_sendq_available` / `tcp_sendq_bytes` / `tcp_notsent_bytes` | `order_session_rtt_samples.csv` | experiment | bool / bytes / bytes | 把 write complete 后 socket send queue snapshot 写入 sample CSV，区分本机 TCP send queue / notsent backlog。 | 同上。 |
| `tcp_info_requested` / `tcp_info_available` / `tcp_info_rtt_us` / `tcp_info_rttvar_us` / `tcp_info_retrans` / `tcp_info_total_retrans` / `tcp_info_unacked` / `tcp_info_snd_cwnd` | `order_session_rtt_samples.csv` | experiment | bool / bool / us / us / counter | 把 Ack 时 `TCP_INFO` 快照写入 sample CSV，用于按连接统计 kernel TCP RTT、variance、重传、未确认包和拥塞窗口。 | 同上。 |
| `ts_available` | `order_session_rtt_samples.csv` | experiment | `true` / `false` | 标记该 sample 是否成功匹配 socket timestamping probe；为 `false` 时不要解释 `ts_*` 的 0 或阶段耗时 `-1`。 | Socket timestamping 诊断删除或字段改名后同步删除。 |
| `ts_write_complete_ns` / `ts_tx_sched_ns` / `ts_tx_software_ns` / `ts_tx_ack_ns` / `ts_rx_software_ns` | `order_session_rtt_samples.csv` | experiment | 本机 Unix epoch ns | Socket timestamping 原始本地时间戳；默认关闭或未采到时为 0。`ts_tx_ack_ns` 是 request bytes 被 TCP ACK 的本地时间，不是 Gate 业务 Ack。 | 同上。 |
| `ts_write_to_tx_software_ns` / `ts_tx_software_to_tx_ack_ns` / `ts_tx_ack_to_rx_software_ns` / `ts_rx_software_to_ack_receive_ns` | `order_session_rtt_samples.csv` | experiment | ns，缺失为 `-1` | RTT probe 中用于把 Ack RTT outlier 分到本机写后出站、TCP/network ACK、远端业务处理/回程、本机 RX/read/parse 几段；只使用本机同一时钟域字段。 | 同上。 |
| `resp_recv_ns` / `resp_ex_ns` / `resp_ex2local_ns` / `resp_rtt_ns` | `order_session_rtt_samples.csv` | experiment | ns / 本机 Unix epoch ns / Gate timestamp ns | 当前 action final response timing；没有 final response 时填 0 / -1。 | 同上。 |
| `status` | `order_session_rtt_samples.csv` | experiment | enum 文本 | 标记当前 action 是 sent、acked、terminal confirmed、rejected、timeout 或 send failed。 | 同上。 |
| `term_fb` | `order_session_rtt_samples.csv` | experiment | enum 文本 | 记录使 action 进入 terminal/invalid 路径的 feedback kind；无 terminal feedback 时为空。 | 同上。 |
| `fill` | `order_session_rtt_samples.csv` | experiment | `true` / `false` | 标记 passive probe 是否意外成交。 | 同上。 |
| `invalid` / `inv_reason` | `order_session_rtt_samples.csv` | experiment | bool / 文本 | 排除 reject、timeout、unexpected fill、safety close timeout 或 run-end REST 需要人工复核的样本。 | 同上。 |
| `rest_guard_phase` / `rest_guard_result` / `rest_guard_json_path` | `order_session_rtt_rest_guard.csv` | planned | enum / 文本 / 路径 | 记录 REST preflight、fatal flatten 和 run-end 整体账户检查结果；REST 不再作为 sample 级 `final_flat` 字段。 | 同上。 |

### OrderSession RTT probe run metadata / connection observed 字段

这些字段不按 sample 重复写入，用于降低 CSV 体积并避免把 run 级 capability 误读成每单数据。

| 字段 | 表面 | 状态 | 单位 / 取值 | 用途 | 删除条件 |
| --- | --- | --- | --- | --- | --- |
| `sample_csv_schema_version` | `order_session_rtt_run_metadata.json` | experiment | integer | 标记 sample CSV schema；当前 `2` 表示包含 `ts_available`。 | schema 迁移完成并停止支持旧版本后重审。 |
| `order_ack_diag_level` / `order_ack_diag_level_name` | `order_session_rtt_run_metadata.json` | experiment | `0..5` / `L0..L5` | 记录该 run 的编译期诊断上限，解释 sample CSV 中哪些字段可能可用。 | 被等价 build metadata 取代。 |
| `tcp_info_compiled` / `socket_timestamping_compiled` / `pcap_gate_header_compiled` | `order_session_rtt_run_metadata.json` | experiment | bool | 记录编译期 capability，避免把字段默认值误读为真实 0。 | 同上。 |
| `tcp_info_runtime_requested` / `socket_timestamping_runtime_requested` | `order_session_rtt_run_metadata.json` | experiment | bool | 记录运行期是否请求 L3 / L4 heavy diagnostics。 | 同上。 |
| `sample_csv_path` / `connection_observed_csv_path` | `order_session_rtt_run_metadata.json` | experiment | path text | 将 run metadata 直接指向主要输出产物。 | 同上。 |
| `run` / `session` / `group` / `ip` / `sid` | `order_session_rtt_connections_observed.csv` | experiment | join key | 与 sample CSV 按 `run + session + sid` 关联。 | connection observed CSV 删除或 schema 升级后重审。 |
| `connected_at_ns` | `order_session_rtt_connections_observed.csv` | experiment | 本机 Unix epoch ns | 记录该 `OrderSession` active callback 被处理的时间。 | 同上。 |
| `endpoint_available` / `local_ip` / `local_port` / `remote_ip` / `remote_port` | `order_session_rtt_connections_observed.csv` | experiment | bool / TCP endpoint | 记录真实 connected endpoint snapshot；不可用时 `endpoint_available=false` 且 endpoint 为空 / 0。 | 同上。 |
| `owner_thread_cpu` / `owner_thread_tid` | `order_session_rtt_connections_observed.csv` | experiment | Linux CPU / tid，失败为 `-1` | 记录连接 active 时 owner thread 所在 CPU / tid，用于和 sched / pidstat 对齐。 | 同上。 |

### OrderSession RTT pcap 对齐 CSV 字段

这些字段由 `scripts/gate/diagnostics/analyze_order_session_rtt_pcap.py` 从 `order_session_rtt_samples.csv`、no TLS pcap
和 Gate Ack response JSON header 对齐生成。当前 runtime 已直接输出 Gate Ack header 的 `x_in_time` / `x_out_time`
等价 ns 字段；pcap 对齐仍用于获得 `pcap request -> Ack response`、`residual_ms` 和 `gate_share`。

| 字段 | 表面 | 状态 | 单位 / 取值 | 用途 | 删除条件 |
| --- | --- | --- | --- | --- | --- |
| `session` / `group` / `ip` / `contract` | `gate_x_time_alignment.csv` | experiment | 文本 | 从 `order_session_rtt_samples.csv` 继承，用于按 private link、session 和合约分组看 tail。 | pcap 对齐脚本废弃或字段改名后删除。 |
| `pcap_request_to_ack_ms` | `gate_x_time_alignment.csv` | experiment | ms | 本机 pcap 看到 request WebSocket frame 到看到 Gate Ack response frame 的时间。 | pcap 对齐脚本废弃或字段改名后删除。 |
| `gate_x_in_to_x_out_ms` | `gate_x_time_alignment.csv` | experiment | ms | Gate Ack response header 中 `x_out_time - x_in_time`，同一 Gate 时钟域内的处理段。 | Gate 不再提供 `x_in_time` / `x_out_time` 或脚本废弃后删除。 |
| `residual_ms` | `gate_x_time_alignment.csv` | experiment | ms | `pcap_request_to_ack_ms - gate_x_in_to_x_out_ms`，近似 pcap 可见链路剩余段；受 pcap 时间精度和抓包点影响。 | 同上。 |
| `gate_share` | `gate_x_time_alignment.csv` | experiment | ratio | `gate_x_in_to_x_out_ms / pcap_request_to_ack_ms`，判断 tail 是否主要由 Gate header duration 解释。 | 同上。 |
| `ack_response_to_ack_receive_us` | `gate_x_time_alignment.csv` | experiment | us | 本机 pcap 看到 Ack response 到 OrderSession `ack_recv_ns` 的时间；用于排除本机 RX / read / parse tail。 | 同上。 |
| `tcp_ack_to_response_us` | `gate_x_time_alignment.csv` | experiment | us | 第一个 ACK request bytes 的远端 TCP packet 到业务 Ack response packet 的间隔。为 0 时表示 TCP ACK 与业务 Ack response 同包或同 pcap 时间戳。 | 同上。 |
| `tcp_ack_same_as_response` | `gate_x_time_alignment.csv` | experiment | `true` / `false` | 标记第一个 ACK request bytes 的远端 TCP packet 是否就是业务 Ack response packet。 | 同上。 |
| `pcap_request_ns` / `tcp_ack_ns` / `pcap_ack_response_ns` | `gate_x_time_alignment.csv` | experiment | pcap timestamp ns | pcap 对齐原始时间戳；pcap 通常只有微秒精度，不等价于 hardware timestamp。 | 同上。 |
| `x_in_time` / `x_out_time` | `gate_x_time_alignment.csv` | experiment | Gate header timestamp | 从 no TLS pcap 还原出的 Gate Ack response 原始 header timestamp；runtime log 中对应 ns 字段为 `exchange_request_ingress_ns` / `exchange_response_egress_ns`。 | 同上。 |
| `conn_id` / `conn_trace_id` / `trace_id` | `gate_x_time_alignment.csv` | experiment | Gate header 文本 | 按 Gate connection / trace 分组，判断 tail 是否集中在特定连接或 Gate trace。 | 同上。 |

### 请求与 Ack 字段

这些字段已经在 Gate order submit / cancel 路径中使用，主要来自 `gate_order_send_ok`、
`gate_order_response` 和 `gate_order_ack_latency_diagnostic`。

| 字段 | 表面 | 状态 | 单位 / 取值 | 用途 | 删除条件 |
| --- | --- | --- | --- | --- | --- |
| `request_sequence` | log / report CSV | stable | `OrderSession` 内请求序号 | 关联 send log、Gate response 和 diagnostic window。 | 不能删除；除非替换 correlation key 并迁移所有 parser。 |
| `encoded_request_id` | log / report CSV | stable | Gate WS payload request id | 排查 request id 编码、Gate response correlation。 | 同 request correlation 迁移。 |
| `local_order_id` | log / report CSV | stable | Aquila 本地订单 id | 关联策略、订单管理、Gate response 和 feedback。 | 不能删除。 |
| `group_id` | Gate/Bitget send、response、Ack diagnostic 与 gateway smoke/fill-probe CSV | experiment | runtime-local uint64；`0` 表示未归组；跨 symbol 需与 `symbol_id` 组合 | SHM v4 唯一归组字段，原样传播 strategy execution group identity；gateway 不自动补写，generic smoke/probe 使用 run/node id。该字段不替代 `local_order_id`，也不单独构成跨 run join key。 | execution group contract 被新稳定标识替代且所有 producer/consumer 同步迁移后删除。 |
| `route_id` | `gate_order_send_ok` / `gate_order_response` / `gate_order_ack_latency_diagnostic` / report CSV | experiment | order gateway route id；unknown / auto 可为 `65535` | 将 send、Ack、diagnostic 和具体 order gateway route / `OrderSession` 对齐。 | 多路 `OrderSession` 诊断停止且所有下游 parser 不再依赖。 |
| `request_send_local_ns` | log / report CSV | stable | 本机 Unix epoch ns | Ack RTT 本地闭环起点。 | 不能删除；性能报告依赖。 |
| `local_receive_ns` | `gate_order_response` | stable | 本机 Unix epoch ns | Ack / result 本地接收时间，用于本地 RTT。 | 不能删除；性能报告依赖。 |
| `exchange_ns` | `gate_order_response` | stable | Gate timestamp ns | 交易所 timestamp 诊断，不可直接当单程网络延迟。 | 只有 Gate 不再提供该字段时删除。 |
| `exchange_request_ingress_ns` | `gate_order_response` / report CSV | stable | Gate header `x_in_time` 转 ns，缺失为 `0` / CSV 空字段 | Gate 收到 request 的 header timestamp；与 `exchange_response_egress_ns` 同一 Gate 时钟域，只用于 Gate 内部处理段诊断，不可与本地时间直接相减。 | Gate 不再提供 `x_in_time` 或 response schema 变更时重审。 |
| `exchange_response_egress_ns` | `gate_order_response` / report CSV | stable | Gate header `x_out_time` 转 ns，缺失为 `0` / CSV 空字段 | Gate 发出 Ack response 的 header timestamp；当前 `exchange_ns` 也优先使用该值。 | 同上。 |
| `exchange_process_ns` | `gate_order_response` / report CSV | stable | ns，缺失为 `0` / CSV 空字段 | `exchange_response_egress_ns - exchange_request_ingress_ns`，Gate 同一时钟域内 request ingress 到 Ack response egress 的处理段。 | 同上。 |
| `exchange_to_local_ns` | log / report CSV | stable | ns，受时钟偏移影响 | 辅助观察 exchange timestamp 到本地 receive 的差值。 | 若被更清晰的 clock-offset 模型取代可删除。 |
| `ack_rtt_ns` | diagnostic / report CSV | stable | ns | 本地发送到本地收到 Ack 的主指标。 | 不能删除；latency report 主字段。 |
| `inflight` | `gate_order_send_ok` | stable | 当前 inflight 数量 | 观察 request map pressure。 | 若被 per-session summary 全面取代，可评估删除。 |
| `latency_diagnostic_inflight_at_send` | diagnostic / report CSV | experiment | 当前 inflight 数量 | Ack outlier 时确认发送瞬间是否存在排队压力。 | Ack outlier 诊断结束且无下游依赖后可删除。 |
| `send_cpu` | `gate_order_send_ok` / report CSV | experiment | Linux CPU id，失败为 `-1` | 确认发送时 owner thread 运行 CPU。 | 被完整 sched trace 或 thread sample 取代。 |
| `ack_cpu` | `gate_order_response` / report CSV | experiment | Linux CPU id，失败为 `-1` | 确认 Ack / submit result 本地处理时 owner thread 运行 CPU；用于对比 send CPU 和 owner CPU。 | 被完整 sched trace 或 thread sample 取代。 |

### 分阶段 Ack latency diagnostic 字段

这些字段由 `OrderAckLatencyDiagnostics` 只在订单后的诊断窗口内采集，正常无订单时不应输出。

| 字段 | 表面 | 状态 | 单位 / 取值 | 用途 | 删除条件 |
| --- | --- | --- | --- | --- | --- |
| `latency_diagnostic_reason` | diagnostic / report CSV | experiment | `kAckRttThreshold`、`kSendToDriveReadThreshold`、`kDriveReadDurationThreshold`、`kDiagnosticTimeout` | 表示触发诊断输出的原因。 | Ack outlier 分析结束且 report 不再使用时删除。 |
| `latency_diagnostic_ack_rtt_ns` | diagnostic / report CSV | experiment | ns | diagnostic 记录到的 Ack RTT。 | 同上。 |
| `send_to_first_after_hook_ns` | diagnostic / report CSV | experiment | ns | 判断订单发送后 runtime hook 是否及时返回。 | 若 runtime hook 不再是疑点可删除。 |
| `send_to_first_drive_read_ns` | diagnostic / report CSV | experiment | ns | 判断 owner thread 是否及时进入 `DriveRead()`。 | 若被更直接 scheduler trace 取代可删除。 |
| `drive_read_duration_ns` | diagnostic / report CSV | experiment | ns | 记录触发时单次 `DriveRead()` 耗时。 | 若 read path tail 已通过其他 profile 覆盖可删除。 |
| `max_observed_drive_read_duration_ns` | diagnostic / report CSV | experiment | ns | 诊断窗口内最大 `DriveRead()` 耗时。 | 同上。 |
| `diagnostic_cpu` | `gate_order_ack_latency_diagnostic` / report CSV | experiment | Linux CPU id，失败为 `-1` | 记录触发 latency diagnostic log 时 owner thread 所在 CPU；Ack RTT 阈值触发时通常等价于 Ack 处理 CPU。 | 被完整 sched trace 或 thread sample 取代。 |
| `max_runtime_loop_gap_ns` | diagnostic / report CSV | experiment | ns | 诊断窗口内 owner runtime loop 两次迭代之间的最大间隔，用于判断是否存在本机 deschedule / 长时间离 CPU。 | 被可靠的外部 sched trace 或更低开销 loop telemetry 取代。 |
| `runtime_loop_iterations_before_ack` | diagnostic / report CSV | experiment | counter | 从请求诊断窗口 armed 到 Ack 处理前经过的 runtime loop 迭代次数，用于辅助区分 owner thread 是否持续运行。 | 同上。 |

### 写路径 Ack latency diagnostic 字段

这些字段挂在现有 `gate_order_ack_latency_diagnostic` 上，只在 Ack RTT 超阈值或 timeout 的诊断窗口内输出。
目标是把 `request_send_local_ns -> ack_local_receive_ns` 拆成本机编码 / enqueue / write syscall / exchange Ack
等待几个阶段。当前 `write_complete_ns` 覆盖 inline `kTryFlushOne` 同步写完成路径；若发生 partial write / EAGAIN，
后续异步 write complete 可能为空，需结合 `pending_write_count_after` 和 `write_eagain` 解读。

| 字段 | 表面 | 状态 | 单位 / 取值 | 用途 | 删除条件 |
| --- | --- | --- | --- | --- | --- |
| `order_encode_done_ns` | diagnostic / report CSV | experiment | 本机 Unix epoch ns | Gate order JSON payload 编码完成时间；用于确认订单编码是否占用 Ack RTT 前段。 | 写路径诊断结束且无下游依赖后删除。 |
| `ws_frame_encode_done_ns` | diagnostic / report CSV | experiment | 本机 Unix epoch ns | WebSocket text frame 编码完成时间；用于区分 exchange payload 编码和 WebSocket frame 编码。 | 同上。 |
| `write_enqueue_ns` | diagnostic / report CSV | experiment | 本机 Unix epoch ns | prepared write 进入 pending queue 的时间；用于判断是否卡在 enqueue 前。 | 同上。 |
| `drive_write_enter_ns` | diagnostic / report CSV | experiment | 本机 Unix epoch ns | 请求 armed 后首次进入 write pump / inline flush 的时间；用于判断 runtime hook 返回后是否及时进入写泵。 | 同上。 |
| `write_some_enter_ns` | diagnostic / report CSV | experiment | 本机 Unix epoch ns | 调用 `send()` / `SSL_write()` 前的时间。private-link 非 TLS 路径对应 `send()`。 | 同上。 |
| `write_some_return_ns` | diagnostic / report CSV | experiment | 本机 Unix epoch ns | `send()` / `SSL_write()` 返回后的时间；与 `write_some_enter_ns` 共同衡量 syscall / TLS write 耗时。 | 同上。 |
| `write_complete_ns` | diagnostic / report CSV | experiment | 本机 Unix epoch ns | 当前 request 对应 WebSocket frame 全部写入 transport 后的时间；若 `request_send_local_ns -> write_complete_ns` 很小，可基本排除本机发送路径。 | 同上。 |
| `write_some_bytes` | diagnostic / report CSV | experiment | bytes | 本次 `send()` / `SSL_write()` 返回的写入字节数；用于识别 partial write。 | 同上。 |
| `write_complete_bytes` | diagnostic / report CSV | experiment | bytes | 当前 request frame 完整写入的总字节数；用于校验 write complete 是否覆盖完整请求。 | 同上。 |
| `write_errno` | diagnostic / report CSV | experiment | errno，成功为 0 | 记录 write syscall / TLS write 失败原因；EAGAIN 路径需保留原始 errno。 | 同上。 |
| `write_eagain` | diagnostic / report CSV | experiment | `true` / `false` | 标记写路径是否遇到 EAGAIN / would-block，用于解释本地 send queue 或 socket backpressure。 | 同上。 |
| `pending_write_count_after` | diagnostic / report CSV | experiment | count | enqueue / write 后 pending business write 数量，用于判断是否存在本地 write queue 积压。 | 同上。 |

### Socket send queue 字段

这些字段在 write complete 后或 Ack outlier diagnostic 输出前采集。Linux 下优先通过 `ioctl(SIOCOUTQ)` /
`ioctl(SIOCOUTQNSD)` 读取，避免直接依赖不同 kernel header 中不稳定的 `tcp_info` 扩展字段。

| 字段 | 表面 | 状态 | 单位 / 取值 | 用途 | 删除条件 |
| --- | --- | --- | --- | --- | --- |
| `tcp_sendq_bytes` | diagnostic / report CSV | experiment | bytes | 记录 socket send queue 中未被远端 ACK 的字节数，用于判断写完后是否仍在内核发送队列中堆积。 | 被更完整 socket queue telemetry 取代。 |
| `tcp_notsent_bytes` | diagnostic / report CSV | experiment | bytes | 记录已进入 TCP 发送队列但尚未发送到网络的字节数；不可用时填 0 并配套 available 字段或 warning。 | 同上。 |
| `socket_send_queue_available` | diagnostic / report CSV | experiment | `true` / `false` | 标记 send queue snapshot 是否成功，避免把平台不支持误判成 0 backlog。 | 同上。 |

### Socket timestamping Ack diagnostic 字段

这些字段由 Linux `SO_TIMESTAMPING` 提供，在 `[order_session.diagnostics.timestamping]` 或
`[probe.sessions.timestamping]` 显式开启时采集；默认关闭。首轮用于 private non-TLS Gate order session RTT
probe，TLS 路径只保留 fd 级配置入口，不作为当前验收目标。所有阶段归因只使用本机同一时钟域；Gate
`exchange_ns` 只能作为远端上下文，不能用于确认本机 / NIC 边界。TX 方向时间戳按 Linux
`SOF_TIMESTAMPING_OPT_ID_TCP` 返回的 `ee_data` byte-stream id 匹配当前 request 写入范围；晚到的旧写入事件或不在当前
request 范围内的事件会被忽略；同一 request 出现多条同类 TX 事件时保留当前 request 范围内最远 byte id 的时间戳。
`CriticalSession` 会按 `request_sequence` 维护多个 active probe；probe 只会在 runtime config 开启且 transport 的
`SO_TIMESTAMPING` apply result 确认为 enabled 后启动，因此 TLS transport 或未成功 apply 的 plain socket 会得到
`ts_available=false`，不会产生假归因。control frame 只推进全局 kernel byte id，不扩展订单 probe 的匹配范围。RX software
timestamp 在处理对应 Ack 时按 `request_sequence` 归属，避免多个 active order probe 之间互相覆盖。`max_active_probes`
控制每条连接可同时保留的 timestamping probe slot，默认 `16384`；耗尽时该 request 没有 socket timestamping 阶段归因，但订单发送本身不受影响。
`AQUILA_ORDER_ACK_DIAG_LEVEL>=4` 时才编译 socket timestamping attribution；低于 `L4` 时 `SO_TIMESTAMPING`
apply、errqueue drain、TX/RX probe 匹配和相关 per-write 状态都会编译为 no-op。运行期配置如果请求
`timestamping.enabled=true` 但编译期低于 `L4`，应启动失败而不是静默输出 0。
字段缺失时原始时间戳为 0，阶段耗时为 `-1`。

| 字段 | 表面 | 状态 | 单位 / 取值 | 用途 | 删除条件 |
| --- | --- | --- | --- | --- | --- |
| `ts_available` | `gate_order_ack_latency_diagnostic` / `order_session_rtt_samples.csv` | experiment | `true` / `false` | 标记该 Ack diagnostic / RTT sample 是否启用了并匹配到 socket timestamping probe。 | Socket timestamping 诊断删除或字段改名后同步删除。 |
| `ts_write_complete_ns` | `gate_order_ack_latency_diagnostic` / `order_session_rtt_samples.csv` | experiment | 本机 Unix epoch ns | 当前 request WebSocket frame 全部写入 transport 后的时间；作为 socket timestamping 阶段的本机起点。 | 同上。 |
| `ts_tx_sched_ns` | `gate_order_ack_latency_diagnostic` / `order_session_rtt_samples.csv` | experiment | 本机 Unix epoch ns | Linux TX scheduler timestamp；用于观察 qdisc / driver / NIC 出站前排队。 | 同上。 |
| `ts_tx_software_ns` | `gate_order_ack_latency_diagnostic` / `order_session_rtt_samples.csv` | experiment | 本机 Unix epoch ns | request bytes 的 kernel software TX timestamp；近似本机 kernel 发送路径完成点。 | 同上。 |
| `ts_tx_ack_ns` | `gate_order_ack_latency_diagnostic` / `order_session_rtt_samples.csv` | experiment | 本机 Unix epoch ns | 本机 TCP 报告 request bytes 已被远端 ACK 的时间；不是 Gate 业务 Ack 时间。 | 同上。 |
| `ts_rx_software_ns` | `gate_order_ack_latency_diagnostic` / `order_session_rtt_samples.csv` | experiment | 本机 Unix epoch ns | 业务 Ack packet 到达本机 kernel 的 software RX timestamp。 | 同上。 |
| `ts_ack_receive_local_ns` | `gate_order_ack_latency_diagnostic` | experiment | 本机 Unix epoch ns | OrderSession 用户态读到并处理 Ack 的本地时间；RTT probe CSV 中对应既有 `ack_recv_ns`。 | 同上。 |
| `ts_write_to_tx_software_ns` | `gate_order_ack_latency_diagnostic` / `order_session_rtt_samples.csv` | experiment | ns，缺失为 `-1` | `ts_write_complete_ns -> ts_tx_software_ns`；变大时本机 kernel / qdisc / driver / NIC 出站前排队嫌疑上升。 | 同上。 |
| `ts_tx_software_to_tx_ack_ns` | `gate_order_ack_latency_diagnostic` / `order_session_rtt_samples.csv` | experiment | ns，缺失为 `-1` | `ts_tx_software_ns -> ts_tx_ack_ns`；变大时偏 private link / network / 远端 TCP ACK 路径。 | 同上。 |
| `ts_tx_ack_to_rx_software_ns` | `gate_order_ack_latency_diagnostic` / `order_session_rtt_samples.csv` | experiment | ns，缺失为 `-1` | `ts_tx_ack_ns -> ts_rx_software_ns`；远端 TCP 已确认 request bytes 后，业务 Ack packet 到达本机 kernel 之前的阶段，偏 Gate 应用处理、远端发送路径或回程网络。 | 同上。 |
| `ts_rx_software_to_ack_receive_ns` | `gate_order_ack_latency_diagnostic` / `order_session_rtt_samples.csv` | experiment | ns，缺失为 `-1` | `ts_rx_software_ns -> ts_ack_receive_local_ns`；变大时偏本机 RX 路径、owner thread 调度、WebSocket frame decode 或 parser。 | 同上。 |

### TCP_INFO 字段

这些字段用于解释多个 `OrderSession` Ack RTT 不同的问题。采集点限制在 `gate_order_response` 和
`gate_order_ack_latency_diagnostic`，由 `order_session.diagnostics.enable_tcp_info` 显式打开；默认关闭时仍输出
`tcp_info_requested=false` / `tcp_info_available=false`，但不调用 `getsockopt(TCP_INFO)`。

Ack latency diagnostic 的触发阈值在 `[order_session.diagnostics]` 中配置。默认
`ack_rtt_threshold_ns=20000000`，即单笔 Ack RTT 严格大于 `20ms` 才输出
`gate_order_ack_latency_diagnostic`；测试需要每单采样时可把 `ack_rtt_threshold_ns` 设为 `0`，并把
`max_logs_per_second` 设到高于该 order session 的预期下单速率。相关运行期探针阈值默认值为
`send_to_first_drive_read_threshold_ns=3000000`、
`drive_read_duration_threshold_ns=1000000`、`diagnostic_window_timeout_ns=250000000`，
`max_logs_per_second=10`。

热路径边界：每次成功发送 place / cancel 后都会 arm 一个 Ack diagnostic window；收到 Ack 时会计算
`ack_rtt_ns`。Ack RTT 触发判断使用 `ack_rtt_ns > ack_rtt_threshold_ns && AllowLog(...)`，因此默认
`20ms` 阈值下，正常 Ack 不会进入 `AllowLog()`。把 `ack_rtt_threshold_ns` 设为 `0` 后，每笔 Ack 都会进入
`AllowLog()` 并在限流允许时输出 `gate_order_ack_latency_diagnostic`，会增加 Ack 热路径上的整数判断、日志格式化 /
入队成本，以及打开 `enable_tcp_info=true` 时的 `getsockopt(TCP_INFO)` 采样成本。该模式只用于短期诊断测试，不作为
常驻生产默认。`max_logs_per_second=0` 会完全禁止 diagnostic log；非 0 时每个 `OrderSession` 独立维护一个 1 秒本地时间窗口，
窗口内最多输出该数量的 diagnostic log。runtime loop probe 的 send-to-drive-read / drive-read / timeout diagnostic 也共用
同一个限流；但 `OrderSession::OnRuntimeLoopProbe()` 在没有 active diagnostic window 时会直接返回。

| 字段 | 表面 | 状态 | 单位 / 取值 | 用途 | 删除条件 |
| --- | --- | --- | --- | --- | --- |
| `tcp_info_requested` | response / diagnostic | experiment | `true` / `false` | 标记本条 log 是否启用了 TCP_INFO 采集，区分“未请求”和“请求但不可用”。 | TCP_INFO 诊断删除时同步删除。 |
| `tcp_info_available` | response / diagnostic / report CSV | experiment | `true` / `false` | 标记本次 `TCP_INFO` snapshot 是否成功。 | 同上。 |
| `tcp_info_rtt_us` | response / diagnostic / report CSV | experiment | microseconds | kernel TCP RTT 估计，用于区分网络 RTT 与本地调度。 | TCP_INFO 不可用或被外部采样取代。 |
| `tcp_info_rttvar_us` | response / diagnostic / report CSV | experiment | microseconds | kernel TCP RTT variance 估计。 | 同上。 |
| `tcp_info_retrans` | response / diagnostic / report CSV | experiment | counter | Linux `tcp_info.tcpi_retrans`。 | 同上。 |
| `tcp_info_total_retrans` | response / diagnostic / report CSV | experiment | counter | Linux `tcp_info.tcpi_total_retrans`，连接累计重传数。 | 同上。 |
| `tcp_info_unacked` | response / diagnostic / report CSV | experiment | packet count | Linux `tcp_info.tcpi_unacked`，当前未确认 packet 数。 | 同上。 |
| `tcp_info_snd_cwnd` | response / diagnostic / report CSV | experiment | packet count | Linux `tcp_info.tcpi_snd_cwnd`，TCP send congestion window。 | 同上。 |

## Gate OrderFeedbackSession

组件入口：

- `exchange/gate/trading/order_feedback_session.h`
- `exchange/gate/trading/order_feedback_parser.h`
- `core/trading/order_feedback_event.h`
- `core/trading/order_feedback_shm.h`

| 字段 | 表面 | 状态 | 单位 / 取值 | 用途 | 删除条件 |
| --- | --- | --- | --- | --- | --- |
| `local_order_id` | feedback event / log / report CSV | stable | Aquila 本地订单 id | 关联 strategy order、Gate feedback 和 OrderManager 状态。 | 不能删除。 |
| `exchange_order_id` | feedback event / log / report CSV | stable | Gate order id | 对账 REST / private feedback / submit response。 | 不能删除。 |
| `accepted_exchange_ns` | feedback event / report CSV | stable | Gate timestamp ns | 交易所接受订单时间。 | Gate 字段不可用或 schema 变更时重审。 |
| `finish_exchange_ns` | feedback event / report CSV | stable | Gate timestamp ns | 交易所订单终态时间。 | 同上。 |
| `cumulative_filled_quantity` | feedback event / report CSV | stable | contract quantity | 判断 filled / partial filled / terminal 行为。 | 不能删除。 |
| `average_fill_price` | feedback event / report CSV | stable | price | 计算成交价、PnL 和 slippage。 | 不能删除。 |
| `finish_reason` | feedback event / report CSV | stable | Gate / Aquila reason 文本或枚举 | 区分 filled、cancelled、partial-cancel terminal。 | 不能删除。 |
| `continuity_lost` / `kContinuityLost` | SHM control event / log | stable | control event | 标记 private feedback 连续性丢失，触发 stop-and-flat / handoff。 | 不能删除。 |
| `phase` | `order_feedback_session_phase` / `order_feedback_session_disconnect_continuity_lost` | experiment | `ConnectionPhase` enum 文本 | 记录 feedback private WS 的连接状态，定位 continuity lost 发生在 reconnect、closing 还是 closed。 | feedback reconnect 诊断稳定后按保留价值重审。 |
| `last_error` | `order_feedback_session_phase` / `order_feedback_session_disconnect_continuity_lost` | experiment | `ConnectionError` enum 文本 | 保留 WebSocket 层聚合错误，例如 `kPeerClosed` / `kSocketError`。 | 同上。 |
| `reconnect_trigger` | `order_feedback_session_phase` / `order_feedback_session_disconnect_continuity_lost` | experiment | `ReconnectTrigger` enum 文本 | 将 `kPeerClosed` 细分为 read EOF、write EOF、WebSocket close frame、heartbeat timeout 等来源，用于更确切判断 feedback session 断开原因。 | 被更完整 private WS reconnect telemetry 取代。 |
| `reconnect_errno` | `order_feedback_session_phase` / `order_feedback_session_disconnect_continuity_lost` | experiment | errno，非 syscall 来源为 `0` | read / write error 时保留底层 errno；close frame、EOF、heartbeat 等非 errno 来源为 `0`。 | 同上。 |
| `active_before` / `login_ready_before` / `subscribed_before` / `ready_before` | `order_feedback_session_phase` / `order_feedback_session_disconnect_continuity_lost` | experiment | `true` / `false` | 记录断线前 feedback session 是否已经可用；`ready_before=true` 表示 login 和 subscribe 都完成后丢连续性。 | feedback continuity 诊断结束后删除。 |
| `order_feedback_raw_sbe_update` | Nova log key | temporary | info log | 每条 Gate SBE `futures.orders` update 解码后输出一条原始字段诊断，用于判断 terminal feedback 是交易所未发送、parser 丢弃，还是 SHM publish 失败。 | 当前 missing terminal feedback 根因关闭，或替换为稳定 binary / CSV 诊断面后删除或降级。 |
| `update_index` / `result_count` | `order_feedback_raw_sbe_update` | temporary | 0-based index / count | 标记同一个 SBE message 内的 update 位置和总数。 | 同上。 |
| `text` / `local_order_id_valid` / `local_order_id` / `exchange_order_id` | `order_feedback_raw_sbe_update` | temporary | Gate `text` / bool / id | 对齐 Gate client order id、Aquila local order id 和 Gate order id；`local_order_id_valid=false` 表示 parser 无法从 `text` 解出本地订单。 | 同上。 |
| `finish_as` / `role` / `outcome` / `event_emitted` / `emit_kind` / `publish_ok` | `order_feedback_raw_sbe_update` | temporary | Gate raw string / enum / bool | 复核 raw terminal reason、maker/taker、parser drop reason、内部 feedback kind 和 SHM publish 结果。`publish_ok=false` 且 `event_emitted=true` 表示 parser 已发出事件但 publisher 拒绝。 | 同上。 |
| `size_mantissa` / `left_mantissa` / `size_exponent` / `size_quantity` / `left_quantity` | `order_feedback_raw_sbe_update` | temporary | raw decimal / decoded quantity | 对比 Gate SBE 原始数量字段和 parser 计算出的累计成交 / 剩余数量。 | 同上。 |
| `price_exponent` / `fill_price_mantissa` / `fill_price` | `order_feedback_raw_sbe_update` | temporary | raw decimal / decoded price | 对比 Gate SBE 原始价格字段和 parser 计算出的成交均价。 | 同上。 |
| `update_time_us` / `exchange_update_ns` | `order_feedback_raw_sbe_update` | temporary | Gate us / ns | 对比 Gate SBE 原始 update time 和内部 ns 时间戳。 | 同上。 |

热路径边界：feedback 连接字段只在 `OnConnectionPhase()` 的连接状态变化冷路径输出。`order_feedback_raw_sbe_update`
是为当前 terminal feedback 缺失排障新增的临时热路径诊断；开启期间每条 SBE order update 都会增加一次
Nova info log 格式化和写入，不应长期作为最低延迟生产默认观测面。

## Gate BTC Fill Probe

组件入口：

- `tools/gate/fill_probe/main.cpp`
- `tools/gate/fill_probe/csv_writer.h`
- `tools/gate/fill_probe/csv_writer.cpp`
- `tools/gate/fill_probe/state_machine.h`
- `tools/gate/fill_probe/state_machine.cpp`
- `docs/exchange_matching_fillability_notes.md`

该工具是 Gate `BTC_USDT` 最小量成交探针，只在授权 live probe 中产生真实交易行为。未取得 live run
证据前，不应基于 validate-only、preflight-only 或 dry-run 输出宣称成交率、fillability、延迟收益或交易所行为结论。

| 字段 | 表面 | 状态 | 单位 / 取值 | 用途 | 删除条件 |
| --- | --- | --- | --- | --- | --- |
| `fill_probe_config_ok` | stdout log | experiment | log key | 标记 `--validate-config` 已完成 TOML、instrument catalog 和静态安全阈值校验。 | `fill_probe_strategy` 删除或配置校验入口重构时同步删除。 |
| `fill_probe_start` | stdout log | experiment | log key | 标记 market / gateway / feedback SHM 已 attach，并记录 `run_id`、symbol、`max_nodes`、`duration_ms` 和是否 preflight。 | 同上。 |
| `fill_probe_preflight_ok` | stdout log | experiment | log key | 记录 BBO、entry quantity 和名义金额预检结果；preflight-only 到此退出，不会 submit order。 | 同上。 |
| `fill_probe_node_start` | stdout log | experiment | log key | 记录 node 开始时的 `node_id`、side、BBO 和 decision timestamp。 | 同上。 |
| `fill_probe_order_submitted` | stdout log | experiment | log key | 记录 entry / close / cancel command 写入 order gateway SHM 后的本地 id、route、价格、数量和 gateway send status。 | 同上。 |
| `fill_probe_order_event` | stdout log / `order_event.csv` | experiment | log key / CSV row | 记录 order gateway response 或 private feedback event，用于按 `local_order_id`、`group_id`、`route_id` 串联回报；probe 当前以 `node_id` 作为 `group_id`。 | 同上。 |
| `fill_probe_node_done` | stdout log / `node.csv` / `lifecycle.csv` | experiment | log key / CSV row | 记录 node 正常结束或被 freshness gate 跳过后的状态、净仓位和 CSV flush 边界。 | 同上。 |
| `fill_probe_node_unresolved` | stderr log / `node.csv` / `lifecycle.csv` | experiment | log key / CSV row | 标记 node 超时仍未回到 flat；工具退出，不自动 emergency flatten。 | 同上。 |
| `fill_probe_stop` | stdout log | experiment | log key | 标记 probe loop 正常退出并输出 `run_id`。 | 同上。 |
| `node.csv` schema | `node.csv` | experiment | CSV fields | `run_id`、`node_id`、`side`、`bbo_id`、BBO exchange/local timestamp、decision/submit/finish timestamp、freshness、bid/ask、entry quantity/notional、status、skip/unresolved reason。 | CSV schema 变更、probe retire 或迁移到正式 report schema 时同步更新。 |
| `lifecycle.csv` schema | `lifecycle.csv` | experiment | CSV fields | 每个 node 的 GTC / IOC entry 与 close 生命周期：local order id、route、TIF、price、quantity、submit/finish timestamp、entry result、filled qty、avg fill price、close attempts、close attribution、PnL / fee 预留字段。 | 同上。 |
| `order_event.csv` schema | `order_event.csv` | experiment | CSV fields | gateway response / feedback 明细：`local_order_id`、`group_id`、`route_id`、event/response/feedback kind、exchange order id、exchange/local timestamp、price、quantity、fill / left quantity、finish / reject reason。 | 同上。 |

### Gate BTC Fill Probe Cross-Exchange Node CSV

| Field | Source | Stability | Unit / Values | Meaning | Removal condition |
| --- | --- | --- | --- | --- | --- |
| `trigger_mode` | `node.csv` | experiment | `gate_direct` / `binance_trigger_gate_quote` | Node 的触发模式。 | Fill probe CSV schema 删除后同步删除。 |
| `binance_bbo_id` | `node.csv` | experiment | Binance fusion BBO id | 触发 node 的 Binance BTC_USDT BBO id。 | 同上。 |
| `gate_bbo_id` | `node.csv` | experiment | Gate fusion BBO id | 下单 quote 使用的 Gate BTC_USDT BBO id。 | 同上。 |
| `binance_freshness_ns` | `node.csv` | experiment | ns | `decision_ns - binance_local_ns`。 | 同上。 |
| `gate_freshness_ns` | `node.csv` | experiment | ns | `decision_ns - gate_local_ns`。 | 同上。 |
| `gate_exchange_delta_ns` | `node.csv` | experiment | ns | `gate_exchange_ns - binance_exchange_ns`。 | 同上。 |
| `gate_local_delta_ns` | `node.csv` | experiment | ns | `gate_local_ns - binance_local_ns`。 | 同上。 |
| `trigger_to_send_ns` | `node.csv` | experiment | ns | `submit_ns - decision_ns`，用于解释 trigger 到 entry submit 的策略端延迟。 | 同上。 |
| `skip_reason` | `node.csv` | experiment | `stale_binance_trigger` / `stale_gate_quote` / `missing_gate_quote` / `stale_gate_direct_quote` | 记录未提交 entry 的跳过原因；这些 row 不消耗 `max_nodes`。 | 同上。 |

热路径边界：`fill_probe_order_submitted` 和 `fill_probe_order_event` 会在真实 probe 的订单链路中执行 stdout
格式化；该工具只用于短时探针，不应把这些字段作为低延迟生产策略的默认热路径观测面。

## TradingRuntime / Runtime Affinity

组件入口：

- `core/trading/trading_runtime.h`
- `exchange/gate/trading/order_session_runtime_adapter.h`
- `scripts/lead_lag/run_live_with_guard.py`
- `config/runtime_affinity/lead_lag_requested_12symbols_node0.toml`

| 字段 | 表面 | 状态 | 单位 / 取值 | 用途 | 删除条件 |
| --- | --- | --- | --- | --- | --- |
| `credentials.api_key_env` / `credentials.api_secret_env` / `credentials.api_passphrase_env` / `credentials.source` | guard stdout | stable | env 名称 / `order_session_config` / `order_gateway_config` / `explicit` | 记录 guard REST preflight、final check 和 emergency flatten 使用的凭据 env 名称来源；只输出 env 名称，不输出 credential 值。 | guard 不再承担 REST account guard 时重审。 |
| `runtime_isolation.client_oid_schema` / `client_oid_run_namespace` | Bitget manifest / guard stdout | stable | `a1` / 12 字符 Crockford Base32 | 绑定固定宽度 `clientOid` 到本轮 `run_id`；guard 要求 manifest、所有 route OrderSession 和 feedback exact equality，保留模板或缺失时 fail closed。 | Bitget run identity schema 升级时同步迁移。 |
| `runtime_isolation.configs.<key>.path` / `sha256` | Bitget manifest / guard stdout | stable | 绝对路径 / 64 字符 SHA-256 | 证明 strategy、gateway、feedback 和每个 route OrderSession 使用 prepare 生成的不可变配置；prepare 后或启动前篡改会被拒绝。 | 由等价 content-addressed deployment attestation 替代时重审。 |
| `runtime_isolation.strategy_lag_symbols` | Bitget guard stdout | stable | Bitget UTA symbol 列表 | 记录从本轮 strategy overlay 的 LeadLag 配置解析出的全部 Bitget lag symbols；启动前必须全部被 guard `--contract` 覆盖。 | Bitget live 不再使用外围 allowlist guard 时重审。 |
| `runtime_isolation.processes.<role>.pid` / `start_time_ticks` / `executable` / `config` | Bitget guard stdout | stable | Linux PID / `/proc/<pid>/stat` start time / basename / config path | 绑定本轮 `gateway` 与 `feedback` 的进程身份，防止把复用 PID、错误 binary 或其他配置误当成当前交易栈；manifest 和 summary 不保存 credential 值。 | Bitget live 改用可提供等价代次证明的 supervisor 后重审。 |
| `quiescence.ok` / `result` / `processes.<role>.result` / `phase` / `signals` / `error` | Bitget guard stdout | stable | bool / `stopped`、`stop_failed` / `grace`、`term`、`kill` / signal 名称 / 文本 | 记录 strategy 退出后、REST cleanup 前的 mutation barrier；只有 gateway 与 feedback 均证明停止才允许 final REST 或 emergency flatten。`error` 不包含 credential。 | 外部 supervisor 提供同等 fail-closed barrier 且 guard 不再直接停止进程时重审。 |
| `affinity_profile` | guard stdout / report | experiment | config path 或 profile name | 记录本轮是否使用 runtime affinity overlay。 | affinity pipeline 稳定后可改为 stable。 |
| `affinity_core_path` | guard stdout / report | experiment | generated config path | 记录 core-path 临时配置位置，便于复现。 | 如果生成配置不再保留可删除。 |
| `owner_thread_cpu` | log / summary | planned | Linux CPU id | 对齐 `OrderSession` owner thread 和 runtime hook 执行 CPU。 | 被完整 scheduler trace 取代。 |
| `poll_calls` / `empty_polls` | runtime diagnostics | stable | counter | 观察 DataReader / scheduler 空转行为。 | 若 runtime diagnostics schema 重构再迁移。 |

## LeadLag Strategy

组件入口：

- `strategy/lead_lag/strategy.h`
- `strategy/lead_lag/execution_state.h`
- `strategy/lead_lag/signal.h`

| 字段 | 表面 | 状态 | 单位 / 取值 | 用途 | 删除条件 |
| --- | --- | --- | --- | --- | --- |
| `lead_lag_parallel_clamped` | strategy Nova warning | stable | warning log | 配置或 programmatic `execute.parallel` 超过编译期上限时，记录 Strategy 已把内部 effective value clamp 到 `16`；该 warning 在初始化阶段输出，不进入交易热路径。 | parallel 配置改为其他显式 normalization contract 时同步迁移。 |
| `configured_parallel` / `effective_parallel` / `max_parallel` | `lead_lag_parallel_clamped` | stable | count | 分别记录调用方请求值、Strategy 实际使用值和编译期硬上限，避免把 effective value 误当成原始配置。 | 同上。 |
| `trigger_exchange` | strategy log / report CSV | stable | `kGate` / `kBinance` 等 | 表示触发信号的行情来源，不表示实际下单交易所。 | 不能删除；若新增 `order_exchange` 也应保留。 |
| `trigger_symbol_id` | strategy log / report CSV | stable | internal symbol id | 关联触发行情 symbol。 | 不能删除。 |
| `trigger_exchange_ns` | `lead_lag_signal_triggered` / `lead_lag_signal_decision` / `lead_lag_order_submitted` / report CSV | experiment | exchange timestamp ns | 记录触发 BBO 的 `BookTicker.exchange_ns`，用于和历史 signal CSV / replay 对齐；Gate SBE `bbo.time` 为 WebSocket server send timestamp，Binance `E` 为 event time。真实 BBO 事件时间另见 signal `event_ns`。 | 若信号时序诊断结束且 report 不再依赖可删除。 |
| `trigger_local_ns` | `lead_lag_signal_triggered` / `lead_lag_signal_decision` / `lead_lag_order_submitted` / report CSV | experiment | data session 本机 Unix epoch ns | 记录触发 BBO 在 data session ingress 处的本机时间，用于计算 BBO 到策略 / 下单的本地闭环。 | 同上。 |
| `on_book_ticker_entry_ns` | `lead_lag_signal_triggered` / `lead_lag_signal_decision` / `lead_lag_order_submitted` / report CSV | experiment | strategy 本机 Unix epoch ns | `Strategy::OnBookTicker()` 入口时间，用于拆分 data session / DataReader 到策略处理的延迟。 | 同上。 |
| `signal_decision_ns` | `lead_lag_signal_triggered` / `lead_lag_signal_decision` / `lead_lag_order_submitted` / report CSV | experiment | strategy 本机 Unix epoch ns | `SignalEngine` 完成并确认 triggered 后的时间，用于计算策略计算和 signal-to-order submit 延迟。 | 同上。 |
| `lead_exchange_ns` / `lag_exchange_ns` | `lead_lag_signal_triggered` / `lead_lag_signal_decision` / `lead_lag_order_submitted` / `lead_lag_order_response` / `lead_lag_order_feedback` / `lead_lag_order_finished` / report CSV | experiment | exchange timestamp ns | signal / submitted / report CSV 中记录 signal 触发时 lead / lag 两侧最新 BBO 的 `BookTicker.exchange_ns`，用于 replay 对齐和解释 raw price 所属行情；order response / feedback / finished 日志中记录处理该 Ack、回报或终态时策略已看到的两侧最新 BBO timestamp。 | 若 signal / order / position CSV 不再做双边行情对账，且回报定位不再需要双边 BBO timestamp，可删除。 |
| `signal_lead_id` / `signal_lag_id` | `lead_lag_signal_triggered` / `lead_lag_signal_decision` / `lead_lag_order_intent_rejected` / `lead_lag_order_submitted` / report CSV | experiment | `BookTicker.id` | 记录 signal 触发时 lead / lag 两侧最新 BBO update id；`signal.csv`、`order_detail.csv` 和 `latency.csv` 保留同名字段，用于从 recorder/fusion bin 定位 signal 时点行情。 | 若 signal 侧不再需要按 BBO id 对齐，可删除。 |
| `group_id` / `route_id` | `lead_lag_order_submitted` / `lead_lag_order_response` / `lead_lag_order_feedback` / `lead_lag_order_finished` / `order_detail.csv` / `latency.csv` | experiment | pair runtime 内单调 execution group id / order gateway route id | `(symbol_id, group_id)` 把同一 execution group 下 fanout child order 归组；`route_id` 标识该 child order 走哪一路 order gateway route。账户级 feedback 本身不带 group / route，LeadLag 在处理 feedback 时按 `local_order_id` 从本地订单表补出这两个字段。跨运行归因还必须加入 run/session identity。 | multi-group 诊断 schema 稳定后重审；若 group identity contract 再升级，应同步迁移 report。 |
| `<stage>_lead_id` / `<stage>_lag_id` | `lead_lag_order_response` / `lead_lag_order_feedback` / `order_detail.csv` / `latency.csv` | experiment | `BookTicker.id` | 记录策略处理 Ack response 或 private order feedback 时已看到的两侧 latest BBO update id；当前 stage 前缀包括 `ack`、`accepted`、`partial_filled`、`filled`、`cancelled`、`rejected`、`unknown_result`、`cancel_accepted`、`cancel_rejected` 和 `continuity_lost`。字段用于把 Ack / cancel / fill / reject 时点和 recorder `BookTicker` bin 精确对齐；旧日志中的 `lead_book_ticker_id` / `lag_book_ticker_id` 仅作为分析脚本兼容输入。 | 若 Ack / feedback 定位不再需要按 BBO id 对齐，可删除。 |
| `lead_local_ns` / `lag_local_ns` | `lead_lag_signal_triggered` / `lead_lag_signal_decision` / `lead_lag_order_intent_rejected` / `lead_lag_order_submitted` / report CSV | experiment | data session 本机 Unix epoch ns | signal / order 日志中记录 signal decision 时策略看到的 lead / lag 最新 BBO `BookTicker.local_ns`，用于检测 data session ingress 到策略之间的本地链路。 | 若 freshness 与本地链路诊断不再需要双边 local timestamp，可删除。 |
| `lead_freshness_ns` / `lag_freshness_ns` | `lead_lag_signal_triggered` / `lead_lag_signal_decision` / `lead_lag_order_intent_rejected` / `lead_lag_order_submitted` / report CSV | experiment | ns | signal decision 时最新 lead / lag BBO 相对交易所时间的 freshness，定义为 `signal_decision_ns - lead_exchange_ns` / `signal_decision_ns - lag_exchange_ns`；机器默认已和交易所对时到约 100us 级别。 | 若不再用 exchange timestamp 做开仓 freshness guard 和延迟分析，可删除。 |
| `max_lead_freshness_ns` / `max_lag_freshness_ns` / `freshness_guard_pass` / `freshness_reject_reason` | `lead_lag_order_intent_rejected` / `lead_lag_order_submitted` / report CSV | experiment | ns / bool / text | open order freshness guard 的阈值、结果和原因；阈值来自每个 `[[lead_lag.pairs]]` 的整数毫秒配置 `max_lead_freshness_ms` / `max_lag_freshness_ms`，输出时转换为 ns；只作用于 `kOpenLong` / `kOpenShort`，close / stoploss 不用该 guard。 | 若 freshness guard 稳定后字段收敛，可保留阈值和结果，删除临时 reason 细分。 |
| `reject_reason` | `lead_lag_order_intent_rejected` / `order_detail.csv` / report CSV | experiment | text | open signal 已触发但未提交订单时的拒绝原因；`parallel_limit` 表示当前 pair 的 active execution group 数量已达到 `execute.parallel`。`order_route_not_ready` 表示没有可用 order gateway route。其他当前值包括 `drift_guard`、`risk_limit`、`stale_lead_quote`、`stale_lag_quote` 和本地下单准备阶段拒绝原因。 | 若 rejected intent schema 收敛到稳定枚举后重审。 |
| `decision` | `lead_lag_signal_decision` | experiment | text | 记录 signal decision 诊断事件状态；当前只在 `execute.taker_buffer` 非 off 时输出，固定为 `sent`，用于把 taker buffer 参考价和实际提交订单对齐。最终真实订单是否提交仍以 `lead_lag_order_submitted` / rejected 日志为准。 | reference taker buffer 进入正式下单路径并完成 report 迁移后重审。 |
| `raw_price` / `current_order_price` / `reference_order_price` | `lead_lag_signal_decision` | experiment | price | `raw_price` 是 signal 触发时用于决策和计算下单价的原始价格；`current_order_price` 是当前实时策略实际准备提交的 limit price；`reference_order_price` 是用启动前生成的 `execute.taker_buffer` 百分比参数计算的参考价格，用于 live shadow 对比。 | reference taker buffer 进入正式下单路径并完成 report 迁移后重审。 |
| `entry_buffer_pct` / `close_buffer_pct` | `lead_lag_signal_decision` | experiment | ratio | 记录启动前生成或手工配置的 `execute.taker_buffer.entry_fixed_pct` / `normal_close_fixed_pct`，按比例值输出，例如 `0.001` 表示 `0.1%`。 | 同上。 |
| `entry_buffer_pct` / `normal_close_buffer_pct` / `price_tick` / `reference_price_method` / `reference_price` / `max_bid_price` / `max_ask_price` / `open_long_slippage_ticks` / `open_short_slippage_ticks` / `close_long_slippage_ticks` / `close_short_slippage_ticks` / `generated_open_slippage_ticks` / `generated_close_slippage_ticks` | `apply_taker_buffer_slippage.py` audit CSV | experiment | ratio / price / ticks | 记录启动前把 taker buffer pct 转成策略 `open_slippage_ticks` / `close_slippage_ticks` 的输入和结果；当前 `reference_price_method=lag_bbo_max`，ticks 取 `ceil(max_price * buffer_pct / price_tick)`。preflight 不覆盖 `stoploss_slippage_ticks`、`close_retry_times` 或 `close_retry_slippage_step_ticks`。 | 若启动前 slippage 生成流程稳定，可把生成配置保留为主事实源并删除临时审计字段或迁移到正式 run report。 |
| `exchange_update_ns` / `local_receive_ns` | `lead_lag_order_feedback` | experiment | exchange timestamp ns / 本机 Unix epoch ns | 记录 `OrderFeedbackEvent` 自带的交易所更新时间和本机接收时间，用于将 fill / cancel / reject 回报与 `fill_price`、finish reason 以及处理时点的 lead / lag BBO 对齐。 | 若 feedback 定位改由交易所 feedback 原始日志提供且策略日志不再承担回报定位，可删除。 |
| `lead_lag_unknown_result_pause` | strategy Nova warning | experiment | warning log | 收到 Gate `5xx` 等 `OrderResponseKind::kUnknownResult` 后，精确标记对应 symbol 暂停新开仓并进入 `needs_reconcile` 时输出。 | `kUnknownResult` 自动恢复语义稳定并迁移到更完整 recovery telemetry 后重审。 |
| `lead_lag_unknown_result_resume` | strategy Nova warning | experiment | warning log | 之前 unknown 的订单收到对应 terminal private feedback，且该 symbol 所有 unknown order 已解决、没有更高等级 degraded 状态时，自动恢复新开仓并输出。 | 同上。 |
| `new_entries_paused` / `needs_reconcile` / `reason` / `feedback_kind` | `lead_lag_unknown_result_pause` / `lead_lag_unknown_result_resume` | experiment | bool / text / enum | 显式记录暂停或恢复后的交易状态、触发原因和恢复所依据的 terminal feedback 类型，便于 live log 中人工确认 stop / resume 边界。 | 同上。 |
| `lead_lag_order_group_mismatch` | strategy Nova error | experiment | structured error log | terminal response / feedback 的 `StrategyOrder.group_index` slot 为空或当前 `active_group_id` 与 `order_group_id` 不一致时输出；该事件禁止扫描 fallback，并使 pair 进入 `needs_reconcile`、暂停新开仓。 | slot reuse 与 reconcile telemetry 被统一 recovery event schema 取代后重审。 |
| `operation` / `order_group_id` / `slot_occupied` / `active_group_id` | `lead_lag_order_group_mismatch` | experiment | text / uint64 / bool / uint64 | 标识发生 mismatch 的处理阶段、订单稳定 group id、runtime-local slot 是否 occupied 及其中当前 group id。`group_index` 不进入日志；`order_group_id` 和 `active_group_id` 是仅有的 group identity 字段。 | 同上。 |
| `signal_role` | strategy log / report CSV | stable | `kLead` / `kLag` | 区分 pair role。 | 不能删除。 |
| `order_role` | strategy log / report CSV | stable | `entry` / `exit` | 关联 position open / close。 | 不能删除。 |
| `position_id` | strategy log / report CSV | stable | strategy position id | position.csv 配对主键之一。 | 不能删除。 |
| `position_event` | strategy log / report CSV | stable | strategy event enum | 判断 entry / exit submit 状态。 | 不能删除。 |
| `entry_local_order_id` | strategy log / report CSV | stable | local order id | 将 exit 订单关联回 entry。 | 不能删除。 |
| `quantity` / `order_price` | `lead_lag_order_submitted` / report CSV | stable | numeric quantity / price | 策略提交订单的数值事实；`quantity_text` / `price_text` 与这两个字段重复，已从 strategy submit log 删除。Gate 的 exact wire decimal text 可读取 `gate_order_send_ok`；Bitget 当前 send log 不含这两个 text。report 在 send log 缺失时从数值字段生成规范化 text。 | 不能删除数值字段；若 report 不再保留 text compatibility columns，可删除 fallback text 生成。 |
| `order_finished_local_ns` | strategy log / report CSV | stable | 本机 Unix epoch ns | send-to-finish / ack-to-finish 本地闭环终点。 | 不能删除。 |
| `ack_rtt_ns` | strategy log / report CSV | stable | ns | 本地 Ack RTT 主指标。 | 不能删除。 |
| `exchange_lifecycle_ns` | strategy log / report CSV | experiment | ns | Gate exchange Ack 到 terminal update 的交易所侧 lifecycle 诊断，不解释 Ack RTT。 | 若 Gate timestamp 语义不稳定或更好 lifecycle 字段落地可重审。 |

### LeadLag cold submit benchmark counters

组件入口：`benchmark/strategy/lead_lag_submit_breakdown_benchmark.cpp`。这些字段只出现在
Google Benchmark 输出，不进入 production binary、Nova log 或 report CSV。`*_p50_ns`、
`*_p95_ns`、`*_p99_ns` 和 `*_max_ns` 都是同一进程内样本分位数；跨进程比较时使用各组
counter 的中位数，不直接拼接样本。

| 字段 | 表面 | 状态 | 单位 / 取值 | 用途 | 删除条件 |
| --- | --- | --- | --- | --- | --- |
| `pairs` / `fanout` | benchmark counters | experiment | count | 证明 cold workload 使用 46 pair、首单只走一个 gateway route。 | cold submit benchmark 删除或参数由 benchmark name 完整表达时删除。 |
| `churn_sweeps` / `churn_updates` | benchmark counters | experiment | count | 记录目标 submit 前确定性 non-triggering 行情扰动规模；一次 sweep 对每个 pair 更新 lag / lead 两侧。 | 同上。 |
| `warm_before_target` | benchmark counters | experiment | `0` / `1` | `1` 表示目标 submit 前立即执行另一个 symbol 的成功 submit，用作 warm 对照。 | 同上。 |
| `stage_trace_enabled` | benchmark counters | experiment | `0` / `1` | 区分完整逐 stage timestamp 与只保留 decision/request 端点的 observer-overhead 对照。 | observer 开销不再需要量化时删除。 |
| `decision_to_signal_log_done_*` / `signal_log_done_to_price_*` / `signal_log_to_group_ready_*` / `group_ready_to_request_timestamp_*` | benchmark counters | experiment | ns | 把 `signal_decision_ns -> OrderGatewayClient::PlaceOrder()` 返回的 `send_local_ns` 按 signal INFO log observer、price-prepared 和 execution-group-ready landmark 拆分；observer 在同步 signal log call 返回后取时钟。 | cold submit 归因完成且 benchmark 不再维护时删除。 |
| `decision_to_request_timestamp_*` / `before_place_to_request_timestamp_*` / `request_timestamp_to_place_return_*` / `decision_to_place_return_*` | benchmark counters | experiment | ns | 分别记录 decision 到 command `owner_enqueue_ns`、进入 `PlaceOrder()` 前到该 timestamp、timestamp 到 `TryPush` 返回、decision 到 `PlaceOrder()` 返回；`request_timestamp` 不是 socket write 时间。 | 同上。 |
| `request_timestamp_to_submitted_log_done_*` / `submitted_log_to_handle_end_*` / `decision_to_handle_end_*` | benchmark counters | experiment | ns | 覆盖 gateway enqueue 后的 submitted log 与本次 `HandleBookTickerForTest()` 返回阶段。 | 同上。 |
| `decision_to_price_*` / `price_to_signal_decision_log_*` / `signal_decision_log_to_freshness_*` / `freshness_to_quantity_*` | benchmark counters | experiment | ns | 生产 submit stage test hook 的前半段：price、可选 signal-decision log、freshness 与 quantity preparation。 | submit stage test hook 删除时同步删除。 |
| `quantity_to_routes_refreshed_*` / `routes_refreshed_to_routes_selected_*` / `routes_selected_to_risk_*` / `risk_to_group_*` | benchmark counters | experiment | ns | route state refresh / selection、strategy risk check 与 execution group 建立阶段。 | 同上。 |
| `group_to_route0_acquire_begin_*` / `route0_acquire_done_to_place_begin_*` | benchmark counters | experiment | ns | 首 route child 准备与 fixed risk slot 前后连接段。 | 同上。 |
| `route0_acquire_text_*` | benchmark counters | experiment | ns | 历史 counter 名称；当前实际语义是首 route 的 fixed risk slot acquire，不是 decimal text preparation。 | counter 完成兼容性改名或 benchmark 删除时删除。 |
| `route0_place_order_*` / `route0_after_place_to_submit_result_*` | benchmark counters | experiment | ns | 首 route `PlaceOrder()` 调用和返回后的 submit-result / submitted-log 处理；前者同时包含多个 timestamp observer，需结合 endpoint-only case 判断测量扰动。 | 同上。 |

### LeadLag Lag Vol Guard Audit CSV

组件入口：

- `tools/lead_lag/lag_vol_guard_audit.*`
- `tools/lead_lag/replay.cpp`
- `scripts/lead_lag/summarize_guard_audit.py`

该诊断只在 replay 中显式传 `--lag-vol-guard-audit-output <path>` 时输出 `lag_vol_guard_audit.csv`。它维护独立 Go-like `lag_vol_guard` 状态，并在 open signal 之后写一行“如果启用 guard 是否会阻断”的 snapshot；不改变 replay synthetic accounting，也不进入 live hot path。duration CLI 支持 `ns`、`us`、`ms`、`s`、`m`、`h`。同一 CSV 还复用生产 `DriftGuardState` 输出 `drift_guard` 对照字段；replay 通过 signal observer 在 post-signal guard 执行前记录 triggered signal，因此被 `drift_guard` 拦截的 open signal 也会出现在 `signals.csv` 和 audit CSV 中。

| 字段 | 表面 | 状态 | 单位 / 取值 | 用途 | 删除条件 |
| --- | --- | --- | --- | --- | --- |
| `open_signal_index` | `lag_vol_guard_audit.csv` | experiment | 0-based row id | 标识本次 replay 中 open signal 的 audit 顺序。 | audit CSV schema 删除或字段改名后同步更新。 |
| `symbol` / `symbol_id` / `action` / `side` | `lag_vol_guard_audit.csv` | experiment | text / id / strategy action / side | 标识 open signal 归属合约、方向和策略 action；`action` 参与和 `order_detail.csv` 的 entry order 对齐。 | 同上。 |
| `trigger_exchange_ns` / `lead_exchange_ns` / `lag_exchange_ns` | `lag_vol_guard_audit.csv` | experiment | exchange timestamp ns | 记录 signal 触发时触发行情、lead 最新 BBO、lag 最新 BBO 的 exchange timestamp，用于复盘 guard 判断所处行情时点。 | 同上。 |
| `signal_lead_id` / `signal_lag_id` | `lag_vol_guard_audit.csv` | experiment | `BookTicker.id` | 记录 signal 触发时 lead / lag 两侧最新 BBO id；汇总脚本用 `symbol_id + signal_lag_id + action` 对齐 entry order。 | 若 report 不再保留对应 signal id 或 join key 迁移后更新。 |
| `raw_price` | `lag_vol_guard_audit.csv` | experiment | price | 记录原始 signal price，用于和 signal CSV / order price 对账。 | audit CSV schema 删除或字段改名后同步更新。 |
| `would_block` / `would_block_reason` | `lag_vol_guard_audit.csv` | experiment | bool / `none`、`lag-vol-guard-cooldown`、`lag-vol-guard-trigger` | 表示 Go-like `lag_vol_guard` 如果在 open signal 后执行是否会阻断，以及原因。该字段不是 live 实盘拦截结果。 | 若 guard 进入 live shadow / enforce，需要新增或迁移 live 字段并说明执行顺序。 |
| `lag_vol_jump_count` / `lag_vol_amplitude` / `lag_vol_hot` | `lag_vol_guard_audit.csv` | experiment | count / ratio / bool | 记录 lag mid jump window 内达到阈值的 jump 数、短窗振幅和当前是否处于 hot 状态。 | audit CSV schema 删除或字段改名后同步更新。 |
| `lag_vol_cooldown_active` / `lag_vol_cooldown_until_ns` | `lag_vol_guard_audit.csv` | experiment | bool / event time ns | 记录评估该 open signal 时 cooldown 是否已激活及其结束时间。 | 同上。 |
| `jump_threshold` / `jump_count_threshold` / `jump_window_ns` | `lag_vol_guard_audit.csv` | experiment | ratio / count / ns | 记录本次 replay 使用的 jump guard 参数，避免离线汇总时混淆不同实验配置。 | 同上。 |
| `amplitude_threshold` / `amplitude_window_ns` / `cooldown_ns` | `lag_vol_guard_audit.csv` | experiment | ratio / ns / ns | 记录本次 replay 使用的 amplitude 和 cooldown 参数。 | 同上。 |
| `drift_instant` / `ratio_std` / `drift_mean` / `drift_guard_outcome` | `lag_vol_guard_audit.csv` | experiment | ratio / std / mean / `disabled`、`not_ready`、`pass`、`blocked:instant`、`blocked:ratio_std`、`blocked:drift_mean` | 记录生产 `DriftGuardState` 在该 replay open signal 时的 Go-like 判断。`drift_instant` 是原始 `lag_mid / lead_mid` ratio，`ratio_std` 是 ratio 窗口标准差，`drift_mean` 是 ratio 窗口均值；消费者如需偏离度应自行计算 `abs(value - 1)`。当 guard disabled 或 enabled 但窗口未 ready 时，三个数值字段输出 `nan`，并用 `drift_guard_outcome` 区分 `disabled` / `not_ready`。 | audit CSV schema 删除或字段改名后同步更新；若 drift guard audit 不再随 replay 输出可删除。 |

`scripts/lead_lag/summarize_guard_audit.py` 可把 `lag_vol_guard_audit.csv` 与 `order_detail.csv`、可选 `position.csv` 汇总成 JSON / Markdown。汇总字段同属 replay audit 实验面：

| 字段 | 表面 | 状态 | 单位 / 取值 | 用途 | 删除条件 |
| --- | --- | --- | --- | --- | --- |
| `open_signal_count` / `would_block_count` / `block_rate` | summary JSON / Markdown | experiment | count / ratio string | 总体、按 symbol、按 action 统计 open signal 数和 would-block 比例。 | summary schema 删除或迁移到正式 report 后同步更新。 |
| `unmatched_audit_rows` / `unmatched_order_rows` | summary JSON / Markdown | experiment | count | 标记 audit row 与 entry order 的 join 缺口，避免把缺订单误判为 guard 效果。 | 同上。 |
| `blocked` / `allowed` group metrics | summary JSON / Markdown | experiment | counts / PnL strings | 按 would-block 分组输出 orders、filled、partially_filled、cancelled、zero-fill cancelled、position 状态和 gross / net PnL。 | 同上。 |
| `warnings` | summary JSON / Markdown | experiment | text list | 记录缺 required field、schema drift 等问题；空列表表示本次汇总未发现 schema warning。 | 同上。 |

`lead_lag_replay` 仍保留 CLI stdout / stderr 摘要，便于既有脚本读取；同时把关键运行摘要和 guard audit 配置错误写入 Nova log：

| 字段 | 表面 | 状态 | 单位 / 取值 | 用途 | 删除条件 |
| --- | --- | --- | --- | --- | --- |
| `lead_lag_replay` / `lead_lag_replay_summary` | replay stdout / Nova log | experiment | run summary text | 记录 replay config、data reader、输出 CSV、signal 计数和退出码；用于从 stdout log 或 Nova log 追踪同一轮 replay。 | replay summary schema 迁移或正式 report 覆盖该信息后重审。 |
| `lag_vol_guard_*_error` / `lag_vol_guard_audit_error` | Nova error log / CLI stderr | experiment | error text | 记录 `--lag-vol-guard-*` CLI 参数校验失败或 audit CSV writer 打开失败；对应 CLI 仍输出 `[FAIL]`。 | guard audit CLI 删除、字段改名或迁移到统一 config validation log 后同步删除。 |

### LeadLag Market Calculation CSV

组件入口：

- `strategy/lead_lag/market_calc_diagnostics.h`
- `strategy/lead_lag/market_calc_csv_writer.*`
- `strategy/lead_lag/strategy.h`
- `tools/lead_lag/replay.cpp`
- `tools/lead_lag/live_strategy.cpp`

该诊断只在 `AQUILA_ENABLE_LEAD_LAG_MARKET_CALC_CSV=ON` 时编译。runner 仍是 `lead_lag_replay` / `lead_lag_strategy`，通过 `--diagnostic-mode market_calc --market-calc-output-dir <dir>` 输出 `lead_calc.csv` 和 `lag_calc.csv`。模式语义是只输出行情计算快照，不触发 signal、不执行 synthetic position accounting、不提交外部订单。bool 输出 `true` / `false`，enum 输出名字；尚未初始化或数学无定义的 double 输出 `nan`。

| 字段 | 表面 | 状态 | 单位 / 取值 | 用途 | 删除条件 |
| --- | --- | --- | --- | --- | --- |
| `row_index` | `lead_calc.csv` / `lag_calc.csv` | experiment | process-local monotonic id | 标识策略本次运行中已输出的 market calc row 顺序。 | market_calc CSV 删除时同步删除。 |
| `role` / `exchange` / `symbol` / `symbol_id` / `book_ticker_id` | `lead_calc.csv` / `lag_calc.csv` | experiment | enum name / text / id | 标识当前行来自哪条已路由 BBO；`book_ticker_id` 是交易所 update id，不是本地行号。 | market_calc CSV 删除时同步删除。 |
| `exchange_ns` / `local_ns` / `event_ns` | `lead_calc.csv` / `lag_calc.csv` | experiment | exchange timestamp ns / data session 本机 Unix epoch ns / event ns | 用于按 UTC / exchange timestamp 和 data session ingress 时间对齐每条计算 row。 | 若 market_calc CSV 不再用于行情对齐可删除。 |
| `price_changed` / `both_sides_valid` / `active` | `lead_calc.csv` / `lag_calc.csv` | experiment | bool | 标识当前 tick 是否改变 quote、lead / lag 是否都已有 quote、alignment 是否 active。 | market_calc CSV 删除时同步删除。 |
| `lead_bid` / `lead_ask` / `lag_bid` / `lag_ask` | `lead_calc.csv` / `lag_calc.csv` | experiment | raw price | 输出处理当前 tick 后策略看到的 lead / lag 最新 raw BBO。 | market_calc CSV 删除时同步删除。 |
| `drift_mean` / `drift_std_ema` | `lead_calc.csv` / `lag_calc.csv` | experiment | ratio / ratio std EMA | 输出 alignment 当前 drift 均值和 drift std EMA，用于解释 lead drift 后价格。 | 若 drift 诊断不再需要逐 tick 对账可删除。 |
| `drifted_lead_bid` / `drifted_lead_ask` | `lead_calc.csv` | experiment | drifted price | 输出当前 active lead tick 的 drift 后 lead BBO。 | 若 lead 开仓计算不再需要逐 tick 对账可删除。 |
| `up_entry` / `down_entry` / `up_exit` / `down_exit` | `lead_calc.csv` | experiment | ratio threshold | 输出当前 threshold snapshot，用于复盘 entry / exit 条件。 | 若 threshold 诊断不再需要逐 tick 对账可删除。 |
| `lead_noise` / `lag_noise` / `lag_spread_mean` | `lead_calc.csv` / `lag_calc.csv` | experiment | ratio / price | 输出 recorder 当前 noise 与 lag spread 均值。 | 若 cost / noise 诊断不再需要逐 tick 对账可删除。 |
| `long_lead_move` / `long_price_diff` / `long_lag_part_ratio` / `long_target_space` / `long_required_edge` | `lead_calc.csv` | experiment | ratio | 输出 open long gate 的中间计算值，和 `SignalEngine::TryOpenLong()` 共用计算 helper。 | open long 对账完成且不再需要逐 tick CSV 时删除。 |
| `short_lead_move` / `short_price_diff` / `short_lag_part_ratio` / `short_target_space` / `short_required_edge` | `lead_calc.csv` | experiment | ratio | 输出 open short gate 的中间计算值，和 `SignalEngine::TryOpenShort()` 共用计算 helper。 | open short 对账完成且不再需要逐 tick CSV 时删除。 |
| `lag_spread` / `lag_spread_buffer` / `lag_spread_pct` | `lead_calc.csv` / `lag_calc.csv` | experiment | price / ratio | 输出当前 lag spread、相对 recorder mean 的 spread buffer 和百分比 spread。 | 若 lag spread 对账不再需要逐 tick CSV 时删除。 |

## LeadLag Report / Analyzer

组件入口：

- `scripts/lead_lag/analyze_order_detail.py`
- `scripts/lead_lag/generate_live_report.py`
- `docs/lead_lag_live_report_csv_schema.md`

| 字段 | 表面 | 状态 | 单位 / 取值 | 用途 | 删除条件 |
| --- | --- | --- | --- | --- | --- |
| `order_session_id` | `order_detail.csv` / `latency.csv` | experiment | 本进程内单调 id | 将订单行关联回 Gate `OrderSession`。 | Gate OrderSession 多连接诊断删除后同步删除。 |
| `group_id` / `route_id` | `order_detail.csv` / `latency.csv` | experiment | pair runtime execution group id / order gateway route id | 在 report 明细中直接保留 `(symbol_id, group_id)` fanout 归组与 route attribution，避免事后只靠 `local_order_id` 反查 live log。新 analyzer 不读取旧 LeadLag `parent_id` schema。 | 同 LeadLag Strategy。 |
| `owner_thread_cpu` | `order_detail.csv` / `latency.csv` | experiment | Linux CPU id，失败为 `-1` | 将 session active 时 owner CPU 合并进 report。 | 同上。 |
| `owner_thread_tid` | `order_detail.csv` / `latency.csv` | experiment | Linux thread id，失败为 `-1` | 将 report 行关联到外部 `pidstat -t` / `perf sched` 采样中的具体 owner thread。 | Gate OrderSession thread id 诊断删除后同步删除。 |
| `local_ip` / `local_port` | `order_detail.csv` / `latency.csv` | experiment | TCP endpoint | 将本地 TCP endpoint 合并进 report。 | endpoint 诊断删除后同步删除。 |
| `remote_ip` / `remote_port` | `order_detail.csv` / `latency.csv` | experiment | TCP endpoint | 将远端 TCP endpoint 合并进 report。 | endpoint 诊断删除后同步删除。 |
| `send_cpu` / `ack_cpu` / `diagnostic_cpu` | `order_detail.csv` / `latency.csv` | experiment | Linux CPU id，失败为 `-1`；多 diagnostic CPU 用 `;` 合并 | 将下单发送、Ack 处理和 diagnostic 输出时的 owner CPU 合并进 report。 | CPU 诊断删除后同步删除。 |
| `ack_exchange_request_ingress_ns` / `ack_exchange_response_egress_ns` / `ack_exchange_process_ns` | `order_detail.csv` / `latency.csv` | stable | Gate timestamp ns / ns duration | 将 runtime `gate_order_response.exchange_request_ingress_ns` / `exchange_response_egress_ns` / `exchange_process_ns` 按 Ack 合并进 report；用于不抓包时统计 Gate 同钟域 request ingress 到 Ack response egress 的处理段。 | Gate Ack header 字段不可用或 schema 变更时重审。 |
| `tcp_info_*` | `order_detail.csv` / `latency.csv` | experiment | 见 Gate OrderSession section | 将 `gate_order_response` / `gate_order_ack_latency_diagnostic` 的 TCP_INFO snapshot 合并进 report；数值字段多次出现取最大值。 | TCP_INFO 诊断删除后同步删除。 |
| `latency_diagnostic_*` | `order_detail.csv` / `latency.csv` | experiment | 见 Gate OrderSession section | 将 `gate_order_ack_latency_diagnostic` 合并进 report。 | Gate OrderSession diagnostic 删除后同步删除。 |
| `trigger_*_ns` / `signal_decision_ns` | `signal.csv` / `order_detail.csv` / `latency.csv` | experiment | 本机或交易所 ns | 将触发 BBO、策略入口和 signal decision 时间合并进 report。 | LeadLag signal timing 诊断删除后同步删除。 |
| `bbo_to_strategy_ns` / `strategy_to_signal_ns` / `signal_to_request_send_ns` / `trigger_to_request_send_ns` | `signal.csv` / `latency.csv` | experiment | ns | 计算 BBO ingress 到策略、策略计算、signal 到下单发送、BBO ingress 到下单发送的本地闭环；新 live data session 的 `BookTicker.local_ns` 默认是 `CLOCK_REALTIME`，历史 replay / 旧录制若看起来跨时钟域仍留空并在 `latency.csv.warnings` 写 `cross_clock_*`。 | 同上。 |
| `write_path_*` | `order_detail.csv` / `latency.csv` | experiment | 见 Gate OrderSession 写路径 section | 将 Ack outlier 的 encode / enqueue / write syscall / write complete 阶段合并进 report。 | 写路径诊断删除后同步删除。 |
| `socket_send_queue_*` | `order_detail.csv` / `latency.csv` | experiment | 见 Gate OrderSession socket send queue section | 将 Ack outlier 时的 kernel send queue / notsent snapshot 合并进 report。 | socket send queue 诊断删除后同步删除。 |
| `send_to_ack_local_ns` | `latency.csv` | stable | ns | 本地 send 到 Ack receive。 | 不能删除；等价于主 Ack RTT 校验字段。 |
| `send_to_finish_local_ns` | `latency.csv` | stable | ns | 本地 send 到策略终态处理完成。 | 不能删除；用于区分 Ack path 与 terminal lifecycle。 |
| `ack_to_finish_local_ns` | `latency.csv` | stable | ns | 本地 Ack receive 到策略终态处理完成。 | 不能删除。 |
| `ack_exchange_to_local_ns` | `latency.csv` | stable | ns，受时钟偏移影响 | 辅助定位 exchange timestamp 与本地 receive 差值。 | 被 clock-offset 校正模型取代时重审。 |
| `exchange_lifecycle_ns` | `latency.csv` | experiment | ns | Gate exchange Ack 到 terminal update 的相对间隔。 | 同 LeadLag Strategy。 |
| `warnings` | CSV | stable | `;` 分隔文本 | 标记缺字段、异常 join、数量不一致等分析问题。 | 不能删除；新增 warning 值需更新 CSV schema。 |
| `order_exchange` | CSV | planned | exchange enum | 区分 signal source (`trigger_exchange`) 与实际下单交易所。 | 实现后按使用情况决定 stable / experiment。 |

## 删除字段流程

删除或重命名字段前，至少检查：

```bash
rg '<field_name>' docs strategy exchange core tools scripts test benchmark
rg '<log_event_or_csv_column>' reports scripts docs
```

如果字段出现在历史 report、CSV schema 或 parser 中，删除提交必须说明兼容策略。对 hot path 字段，删除也需要确认不会移除仍在使用的诊断证据。
