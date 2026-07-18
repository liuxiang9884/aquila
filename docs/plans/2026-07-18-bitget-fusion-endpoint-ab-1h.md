# Bitget Fusion Endpoint N=4 一小时对比计划

## 目标

同步运行三组 Bitget UTA public SBE `books1` fusion，各组 `N=4`，持续一小时并记录 canonical
`BookTicker` binary：

1. `ha4`：四路均使用 `vip-ws-uta.bitget.com`；
2. `hs4`：四路均使用 `vip-ws-uta-pub-a.bitget.com`；
3. `mixed2x2`：source 0/1 使用 HA，source 2/3 使用 high-speed。

三组统一订阅 `BTCUSDT/ETHUSDT/SOLUSDT`，每组只启动一个 canonical recorder。

## 非目标

- 不连接 private WebSocket、REST 或订单链路；
- 不启动策略或提交订单；
- 启动阶段不声明 endpoint 延迟优劣，结论只基于完整一小时 binary 与 metadata。

## 关键决定

- 三组同时运行，关键 CPU 互斥：`ha4=16-20`、`hs4=21-25`、`mixed2x2=26-30`；
- recorder 和日志后端使用 `4-12`，避免与 fusion/source critical CPU 重叠；
- 每组使用独立 SHM、日志、metadata 和 binary 路径；
- 使用当前 `main` 的 Release binary 冻结副本和 SHA-256；
- fusion 运行 `3600000ms` 后自然退出，外围 supervisor 再以 `SIGTERM` 停止 recorder。

## 执行步骤

1. 精确停止现有 Aquila fusion/recorder 并确认 quiescence；
2. 生成独立 run 目录、配置、binary 副本、哈希和 PID manifest；
3. 对三组 fusion 做 dry-run，对 recorder 配置做 parser/startup smoke；
4. 启动三个 fusion，确认 12 个 source 均连接并持续发布；
5. 启动三个 canonical recorder，确认 binary 文件持续增长；
6. 一小时结束后核对进程退出、日志 summary、metadata/bin 文件和 recorder skipped/overruns；
7. 生成三组同构 latency 分析并把长期结论迁移到 Bitget market-data handoff。

## 验证与失败边界

- 启动成功要求三组 fusion 进程存活、每组 4 个 source active、metadata/bin 文件增长；
- 任一配置错误、订阅失败、SBE decode error、SHM open error 或 recorder overruns 均保留证据并标记该组失败；
- supervisor 只管理 manifest 中绑定的六个 PID，不使用宽泛 `pkill`。

## 回滚

向 manifest 中仍存活的六个绑定 PID 发送 `SIGTERM`，等待退出；必要时仅对同一绑定 PID 使用
`SIGKILL`。独立 SHM 名不会覆盖其他 run。

## 未决风险

- 公网 endpoint 路由和外部网络条件可能随时间变化；
- 单小时、三个 symbol 结果不能外推到其他 symbol、时段或订单 fillability；
- recorder 只保存 canonical BBO；source 级胜出归因依赖 fusion metadata。
