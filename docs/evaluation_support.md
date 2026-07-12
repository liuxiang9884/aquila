# Evaluation 辅助代码规范

`evaluation/` 存放只服务对比、benchmark 和测试验证的共享辅助代码。它不是生产路径的一部分，生产 target 不允许依赖它。

本文只定义 placement、namespace、CMake target 和边界检查，不承载具体 benchmark 结论。

## 命名含义

- 对比：保存和生产实现对照的替代实现，例如 parse-ahead ready queue codec。
- 基准：保存 benchmark 复用的 payload builder、codec、fixture 或测量辅助。
- 测试：保存多个 test 或 test + benchmark 共同复用的 fixture。

## 放置规则

1. 只被单个 test 或 benchmark 文件使用的 helper，放在该 `.cpp` 的匿名 namespace。
2. 只被 test 使用、且不会被 benchmark 复用的 helper，放在对应 `test/` 目录。
3. 只被 benchmark 使用、且不会被 test 复用的 helper，放在对应 `benchmark/` 目录。
4. 同时被 test 和 benchmark 使用，或作为生产实现的稳定对照实现，放在 `evaluation/`。
5. `core/`、`exchange/`、`tools/` 不允许 include `evaluation/`。
6. 生产文件如果需要说明对照实现，只写注释指向 `evaluation/`、`test/` 或 `benchmark/` 的具体文件。

## CMake 约定

`evaluation/` 暴露 header-only target：

```cmake
target_link_libraries(<test_or_benchmark_target>
    PRIVATE
        aquila_evaluation
)
```

只有 `test/` 和 `benchmark/` target 应该链接 `aquila_evaluation`。

## 当前入口

```text
evaluation/websocket/queued_frame_codec.h
evaluation/exchange/gate/sbe/book_ticker_payload_builder.h
```

## 边界检查

提交前可以运行：

```bash
rg '#include "evaluation/' core exchange tools
rg 'aquila_evaluation' core exchange tools
```

期望结果为空。
